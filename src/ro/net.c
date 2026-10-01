/*
 * net.c: RISC OS networking for PuTTY - the Socket/Plug layer, built on the
 * Internet module's SWIs.
 *
 * This follows upstream's unix/network.c (its IPv4-only, -DNO_IPV6 path) closely
 * so that behaviour matches the platforms PuTTY is tested on.  The differences:
 *
 *  - sockets are Internet module handles driven through _swix(), not file
 *    descriptors, and errors come back as RISC OS error blocks;
 *  - name lookup is asynchronous (Resolver_GetHost returns EINPROGRESS while the
 *    DNS query is outstanding), so a slow or dead name server never freezes the
 *    desktop: sk_namelookup() returns at once with an address that is still
 *    being resolved, and sk_new() connects when the answer arrives;
 *  - there are no Unix-domain sockets and no IPv6.
 *
 * Readiness is found with Socket_Select over the table that uxsel.c (upstream's,
 * used unchanged) keeps for us; ro_net_poll() is the one entry point the event
 * loops call.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>

#include "putty.h"
#include "network.h"
#include "tree234.h"
#include "ro.h"
#include "ronet.h"

/* Constants from the BSD socket API, as the Internet module uses them. */
#define AF_INET_RO       2
#define SOCK_STREAM_RO   1
#define SOL_SOCKET_RO    0xFFFF
#define SO_REUSEADDR_RO  0x0004
#define SO_KEEPALIVE_RO  0x0008
#define SO_OOBINLINE_RO  0x0100
#define SO_ERROR_RO      0x1007
#define IPPROTO_TCP_RO   6
#define TCP_NODELAY_RO   1
#define MSG_OOB_RO       1
#define FIONBIO_RO       0x8004667E
#define SIOCATMARK_RO    0x40047307

#define RESOLVE_TIMEOUT_CS 3000        /* give up on the name server after 30 s */
#define FDSET_WORDS        8           /* room for socket handles 0..255 */

/* sockaddr_in as used by the RISC OS Internet module (BSD 4.3 layout). */
typedef struct {
    short    family;
    unsigned short port;               /* network byte order */
    uint8_t  addr[4];                  /* network byte order */
    uint8_t  zero[8];
} ro_sockaddr_in;

/* struct hostent as returned by Resolver_GetHost. */
typedef struct {
    char *name;
    char **aliases;
    int type;
    int length;
    uint8_t **addrs;
} ro_hostent;

/*
 * Debug trace: if the system variable PuTTY$Trace names a file, socket events
 * are logged to it (and flushed at once, so a hang leaves a record).
 */
static void trace(const char *fmt, ...)
{
    static FILE *fp;
    static bool tried;
    va_list ap;

    if (!tried) {
        const char *name = getenv("PuTTY$Trace");
        tried = true;
        if (name && *name)
            fp = fopen(name, "w");
    }
    if (!fp)
        return;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fflush(fp);
}

/*
 * ----------------------------------------------------------------------
 * Error text.
 */

static const char *bsd_errtext(int en)
{
    switch (en) {
      case RO_EINTR:        return "Interrupted";
      case RO_EINVAL:       return "Invalid argument";
      case RO_EWOULDBLOCK:  return "Operation would block";
      case RO_EINPROGRESS:  return "Operation now in progress";
      case RO_EADDRINUSE:   return "Address already in use";
      case 49:              return "Can't assign requested address";
      case 50:              return "Network is down";
      case RO_ENETUNREACH:  return "Network is unreachable";
      case 52:              return "Network dropped connection on reset";
      case RO_ECONNABORTED: return "Software caused connection abort";
      case RO_ECONNRESET:   return "Connection reset by peer";
      case RO_ENOBUFS:      return "No buffer space available";
      case RO_EISCONN:      return "Socket is already connected";
      case RO_ENOTCONN:     return "Socket is not connected";
      case 58:              return "Can't send after socket shutdown";
      case RO_ETIMEDOUT:    return "Connection timed out";
      case RO_ECONNREFUSED: return "Connection refused";
      case RO_EHOSTDOWN:    return "Host is down";
      case RO_EHOSTUNREACH: return "No route to host";
      default:              return NULL;
    }
}

/* Turn a failed SWI into a dynamically allocated message. */
static char *oserror_text(const _kernel_oserror *e)
{
    const char *t = bsd_errtext(ro_errno(e));
    return dupstr(t ? t : e->errmess);
}

/* The error recorded on a socket via SO_ERROR (0 if none). */
static int socket_pending_error(int s)
{
    int val = 0, len = 4;
    if (_swix(Socket_Getsockopt, _INR(0, 4), s, SOL_SOCKET_RO, SO_ERROR_RO,
              &val, &len))
        return 0;
    return val;
}

static void sock_close_handle(int s)
{
    if (s >= 0) {
        _swix(Socket_Shutdown, _INR(0, 1), s, 2);
        _swix(Socket_Close, _IN(0), s);
    }
}

static void sock_set_nonblocking(int s)
{
    int one = 1;
    _swix(Socket_Ioctl, _INR(0, 2), s, FIONBIO_RO, &one);
}

/*
 * ----------------------------------------------------------------------
 * Addresses.
 */

typedef struct IPv4 { uint8_t b[4]; } IPv4;

struct SockAddr {
    int refcount;
    const char *error;                 /* NULL, or points into errbuf */
    char errbuf[160];
    enum { ADDR_NAMEONLY, ADDR_RESOLVING, ADDR_IP } state;
    IPv4 *addresses;
    int naddresses;
    char hostname[512];
    uint32_t resolve_start;            /* ro_time() when lookup began */
    SockAddr *next_resolving;          /* chain of lookups in progress */
    bool in_resolving_list;
};

static SockAddr *resolving_head;

typedef struct NetSocket NetSocket;
struct NetSocket {
    const char *error;                 /* what sk_socket_error() returns */
    char *errstr;                      /* owns the text that error points at */
    int s;                             /* Internet module handle; -1 if none */
    Plug *plug;
    bufchain output_data;
    bool connected;                    /* irrelevant for listening sockets */
    bool writable;
    bool frozen;       /* this causes readability notifications to be ignored */
    bool localhost_only;               /* for listening sockets */
    char oobdata[1];
    size_t sending_oob;
    bool oobpending;                   /* is there OOB data available to read? */
    bool oobinline;
    enum { EOF_NO, EOF_PENDING, EOF_SENT } outgoingeof;
    bool incomingeof;
    char *pending_errmsg;              /* in case send() returns an error */
    bool listener;
    bool nodelay, keepalive;           /* for connect()-type sockets */
    bool privport;
    int port;                          /* and again */
    SockAddr *addr;
    int curraddr;                      /* which of addr's addresses we're on */
    bool waiting_for_dns;              /* addr is still being resolved */
    Socket sock;
};

static tree234 *sktree;

static void uxsel_tell(NetSocket *s);

static int cmpfortree(void *av, void *bv)
{
    NetSocket *a = (NetSocket *) av, *b = (NetSocket *) bv;
    int as = a->s, bs = b->s;
    if (as < bs)
        return -1;
    if (as > bs)
        return +1;
    if (a < b)
        return -1;
    if (a > b)
        return +1;
    return 0;
}

static int cmpforsearch(void *av, void *bv)
{
    NetSocket *b = (NetSocket *) bv;
    int as = *(int *)av, bs = b->s;
    if (as < bs)
        return -1;
    if (as > bs)
        return +1;
    return 0;
}

void sk_init(void)
{
    sktree = newtree234(cmpfortree);
}

void sk_cleanup(void)
{
    NetSocket *s;
    int i;

    if (sktree) {
        for (i = 0; (s = index234(sktree, i)) != NULL; i++) {
            sock_close_handle(s->s);
        }
    }
}

/* Parse a dotted quad; returns true and fills *out on success. */
static bool parse_ipv4(const char *s, IPv4 *out)
{
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4)
        return false;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return false;
    out->b[0] = a; out->b[1] = b; out->b[2] = c; out->b[3] = d;
    return true;
}

static void addr_fail(SockAddr *a, const char *fmt, const char *arg)
{
    a->state = ADDR_NAMEONLY;
    snprintf(a->errbuf, sizeof a->errbuf, fmt, arg);
    a->error = a->errbuf;
}

static void resolving_list_remove(SockAddr *a)
{
    SockAddr **pp;
    if (!a->in_resolving_list)
        return;
    for (pp = &resolving_head; *pp; pp = &(*pp)->next_resolving) {
        if (*pp == a) {
            *pp = a->next_resolving;
            break;
        }
    }
    a->in_resolving_list = false;
    a->next_resolving = NULL;
}

/*
 * Ask the Resolver again about a name whose lookup is in progress.  The
 * multitasking form of Resolver_GetHost returns EINPROGRESS (either as an error
 * or in R0) until the answer is in.
 */
static void resolve_step(SockAddr *a)
{
    int errnum = 0;
    ro_hostent *he = NULL;
    _kernel_oserror *e;
    int i, n;

    /*
     * This is the blocking lookup, as used by UnixLib's gethostbyname().  It
     * is instant for a cached name and otherwise takes as long as the system's
     * own resolver settings allow (Inet$ResolverDelay x Inet$ResolverRetries).
     *
     * The "multitasking" Resolver_GetHost (0x46001) would be better, but with
     * R0 = 0 it answers R0 = -1 forever on RISC OS 5.30 under RPCEmu, and any
     * other R0 gives "Bad parameters to Resolver SWI" (R0 = -1 faults), so its
     * protocol is not understood yet.  See docs/DESIGN.md "Open points".
     * The state machine around this function is ready for it.
     */
    e = _swix(Resolver_GetHostByName, _IN(1) | _OUTR(0, 1), a->hostname,
              &errnum, &he);
    if (e) {
        addr_fail(a, "Cannot look up host name: %s", e->errmess);
        goto done;
    }
    if (errnum == RO_EINPROGRESS)
        goto pending;
    if (errnum == -1 || (errnum == 0 && !he)) {
        addr_fail(a, "Host '%s' not found", a->hostname);
        goto done;
    }
    if (errnum != 0) {
        addr_fail(a, "Host name lookup for '%s' failed; try again later",
                  a->hostname);
        goto done;
    }
    if (he->length != 4 || !he->addrs || !he->addrs[0]) {
        addr_fail(a, "Host '%s' has no IPv4 address", a->hostname);
        goto done;
    }
    for (n = 0; he->addrs[n]; n++)
        ;
    a->addresses = snewn(n, IPv4);
    a->naddresses = n;
    for (i = 0; i < n; i++)
        memcpy(a->addresses[i].b, he->addrs[i], 4);
    a->state = ADDR_IP;
    goto done;

  pending:
    if (ro_time() - a->resolve_start > RESOLVE_TIMEOUT_CS)
        addr_fail(a, "Timed out looking up host '%s'", a->hostname);
    return;

  done:
    resolving_list_remove(a);
}

SockAddr *sk_namelookup(const char *host, char **canonicalname,
                        int address_family)
{
    SockAddr *addr = snew(SockAddr);
    memset(addr, 0, sizeof(SockAddr));
    addr->refcount = 1;
    addr->state = ADDR_NAMEONLY;
    *canonicalname = dupstr(host);

    {
        char *trimmed = host_strduptrim(host);
        strncpy(addr->hostname, trimmed, sizeof addr->hostname - 1);
        sfree(trimmed);
    }

    /* An IPv4 literal needs no lookup. */
    {
        IPv4 ip;
        if (parse_ipv4(addr->hostname, &ip)) {
            addr->addresses = snew(IPv4);
            addr->addresses[0] = ip;
            addr->naddresses = 1;
            addr->state = ADDR_IP;
            return addr;
        }
    }

    if (address_family == ADDRTYPE_IPV6) {
        addr_fail(addr, "IPv6 is not supported on RISC OS%s", "");
        return addr;
    }

    /* Start the lookup.  It may well finish immediately (cache hit). */
    addr->state = ADDR_RESOLVING;
    addr->resolve_start = ro_time();
    addr->next_resolving = resolving_head;
    resolving_head = addr;
    addr->in_resolving_list = true;
    resolve_step(addr);
    return addr;
}

SockAddr *sk_nonamelookup(const char *host)
{
    SockAddr *addr = snew(SockAddr);
    memset(addr, 0, sizeof(SockAddr));
    addr->refcount = 1;
    addr->state = ADDR_NAMEONLY;
    strncpy(addr->hostname, host, sizeof addr->hostname - 1);
    return addr;
}

static bool sk_nextaddr(SockAddr *addr, int *curraddr)
{
    if (*curraddr + 1 < addr->naddresses) {
        (*curraddr)++;
        return true;
    }
    return false;
}

void sk_getaddr(SockAddr *addr, char *buf, int buflen)
{
    if (addr->state != ADDR_IP) {
        strncpy(buf, addr->hostname, buflen);
        buf[buflen-1] = '\0';
    } else {
        const uint8_t *b = addr->addresses[0].b;
        snprintf(buf, buflen, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    }
}

/*
 * This constructs a SockAddr that points at one specific sub-address of a
 * parent SockAddr.  The returned SockAddr does not own all its own memory: it
 * points into the old one's data structures, so it MUST NOT be used after the
 * old one is freed, and it MUST NOT be passed to sk_addr_free.
 */
static SockAddr sk_extractaddr_tmp(SockAddr *addr, int curraddr)
{
    SockAddr toret = *addr;            /* structure copy */
    toret.refcount = 1;
    if (addr->state == ADDR_IP) {
        toret.addresses += curraddr;
        toret.naddresses = 1;
    }
    return toret;
}

bool sk_addr_needs_port(SockAddr *addr)
{
    return addr->state != ADDR_NAMEONLY;
}

bool sk_hostname_is_local(const char *name)
{
    return !strcmp(name, "localhost") ||
           !strcmp(name, "::1") ||
           !strncmp(name, "127.", 4);
}

bool sk_address_is_local(SockAddr *addr)
{
    if (addr->state != ADDR_IP)
        return false;                  /* we don't know; assume not */
    return addr->addresses[0].b[0] == 127;
}

bool sk_address_is_special_local(SockAddr *addr)
{
    return false;
}

int sk_addrtype(SockAddr *addr)
{
    return addr->state == ADDR_IP ? ADDRTYPE_IPV4 : ADDRTYPE_NAME;
}

void sk_addrcopy(SockAddr *addr, char *buf)
{
    assert(addr->state == ADDR_IP);
    memcpy(buf, addr->addresses[0].b, 4);
}

void sk_addr_free(SockAddr *addr)
{
    if (--addr->refcount > 0)
        return;
    resolving_list_remove(addr);
    sfree(addr->addresses);
    sfree(addr);
}

SockAddr *sk_addr_dup(SockAddr *addr)
{
    addr->refcount++;
    return addr;
}

const char *sk_addr_error(SockAddr *addr)
{
    return addr->error;
}

/*
 * ----------------------------------------------------------------------
 * The NetSocket vtable.
 */

static Plug *sk_net_plug(Socket *sock, Plug *p)
{
    NetSocket *s = container_of(sock, NetSocket, sock);
    Plug *ret = s->plug;
    if (p)
        s->plug = p;
    return ret;
}

static void sk_net_close(Socket *s);
static size_t sk_net_write(Socket *s, const void *data, size_t len);
static size_t sk_net_write_oob(Socket *s, const void *data, size_t len);
static void sk_net_write_eof(Socket *s);
static void sk_net_set_frozen(Socket *s, bool is_frozen);
static SocketEndpointInfo *sk_net_endpoint_info(Socket *s, bool peer);
static const char *sk_net_socket_error(Socket *s);

static const SocketVtable NetSocket_sockvt = {
    .plug = sk_net_plug,
    .close = sk_net_close,
    .write = sk_net_write,
    .write_oob = sk_net_write_oob,
    .write_eof = sk_net_write_eof,
    .set_frozen = sk_net_set_frozen,
    .socket_error = sk_net_socket_error,
    .endpoint_info = sk_net_endpoint_info,
};

static NetSocket *netsocket_new(void)
{
    NetSocket *s = snew(NetSocket);
    memset(s, 0, sizeof *s);
    s->sock.vt = &NetSocket_sockvt;
    bufchain_init(&s->output_data);
    s->s = -1;
    return s;
}

/* Record a setup error on a socket (copying the text). */
static void set_error(NetSocket *s, const char *msg)
{
    sfree(s->errstr);
    s->errstr = dupstr(msg);
    s->error = s->errstr;
}

static Socket *sk_net_accept(accept_ctx_t ctx, Plug *plug)
{
    int sockfd = ctx.i;
    NetSocket *s = netsocket_new();

    s->plug = plug;
    s->writable = true;                /* to start with */
    s->frozen = true;
    s->connected = true;
    s->s = sockfd;

    if (s->s < 0) {
        set_error(s, "Could not accept the connection");
        return &s->sock;
    }

    uxsel_tell(s);
    add234(sktree, s);

    return &s->sock;
}

/*
 * Make one attempt to connect to the current candidate address.  Returns 0 if
 * the attempt is under way or complete, else an errno-ish value; the message
 * is in *errmsg (to be freed).
 */
static int try_connect(NetSocket *sock, char **errmsg)
{
    int s;
    ro_sockaddr_in sa;
    int err = 0, one = 1;
    _kernel_oserror *e;
    const IPv4 *ip = &sock->addr->addresses[sock->curraddr];

    *errmsg = NULL;

    /*
     * Remove the socket from the tree before we overwrite its internal socket
     * id, because that forms part of the tree's sorting criterion.  We'll add
     * it back before exiting this function, whether we changed anything or not.
     */
    del234(sktree, sock);

    if (sock->s >= 0) {
        uxsel_del(sock->s);
        sock_close_handle(sock->s);
        sock->s = -1;
    }

    {
        SockAddr thisaddr = sk_extractaddr_tmp(sock->addr, sock->curraddr);
        plug_log(sock->plug, &sock->sock, PLUGLOG_CONNECT_TRYING,
                 &thisaddr, sock->port, NULL, 0);
    }

    s = -1;
    e = _swix(Socket_Creat, _INR(0, 2) | _OUT(0),
              AF_INET_RO, SOCK_STREAM_RO, 0, &s);
    if (e) {
        err = ro_errno(e) ? ro_errno(e) : -1;
        *errmsg = oserror_text(e);
        goto ret;
    }
    sock->s = s;

    if (sock->oobinline) {
        e = _swix(Socket_Setsockopt, _INR(0, 4), s, SOL_SOCKET_RO,
                  SO_OOBINLINE_RO, &one, 4);
        if (e) {
            err = ro_errno(e) ? ro_errno(e) : -1;
            *errmsg = oserror_text(e);
            sock_close_handle(s);
            sock->s = -1;
            goto ret;
        }
    }

    if (sock->nodelay)
        _swix(Socket_Setsockopt, _INR(0, 4), s, IPPROTO_TCP_RO,
              TCP_NODELAY_RO, &one, 4);

    if (sock->keepalive)
        _swix(Socket_Setsockopt, _INR(0, 4), s, SOL_SOCKET_RO,
              SO_KEEPALIVE_RO, &one, 4);

    if (sock->privport) {
        /* Count down from port 1023 looking for a free local port. */
        int localport;
        for (localport = 1023; localport > 0; localport--) {
            ro_sockaddr_in la;
            memset(&la, 0, sizeof la);
            la.family = AF_INET_RO;
            la.port = (unsigned short)(((localport & 255) << 8) |
                                       ((localport >> 8) & 255));
            e = _swix(Socket_Bind, _INR(0, 2), s, &la, (int)sizeof la);
            if (!e)
                break;
            if (ro_errno(e) != RO_EADDRINUSE) {
                err = ro_errno(e) ? ro_errno(e) : -1;
                *errmsg = oserror_text(e);
                sock_close_handle(s);
                sock->s = -1;
                goto ret;
            }
        }
    }

    memset(&sa, 0, sizeof sa);
    sa.family = AF_INET_RO;
    sa.port = (unsigned short)(((sock->port & 255) << 8) |
                               ((sock->port >> 8) & 255));
    memcpy(sa.addr, ip->b, 4);

    sock_set_nonblocking(s);

    e = _swix(Socket_Connect, _INR(0, 2), s, &sa, (int)sizeof sa);
    if (e) {
        int en = ro_errno(e);
        if (en != RO_EINPROGRESS && en != RO_EWOULDBLOCK && en != RO_EALREADY) {
            err = en ? en : -1;
            *errmsg = oserror_text(e);
            sock_close_handle(s);
            sock->s = -1;
            goto ret;
        }
    } else {
        /*
         * The connect completed at once.  Set the socket as connected and
         * writable.
         */
        SockAddr thisaddr;
        sock->connected = true;
        sock->writable = true;

        thisaddr = sk_extractaddr_tmp(sock->addr, sock->curraddr);
        plug_log(sock->plug, &sock->sock, PLUGLOG_CONNECT_SUCCESS,
                 &thisaddr, sock->port, NULL, 0);
    }

    uxsel_tell(sock);

  ret:

    /* No matter what happened, put the socket back in the tree. */
    add234(sktree, sock);

    if (err) {
        SockAddr thisaddr = sk_extractaddr_tmp(sock->addr, sock->curraddr);
        plug_log(sock->plug, &sock->sock, PLUGLOG_CONNECT_FAILED,
                 &thisaddr, sock->port, *errmsg, err);
    }
    return err;
}

/* Try each candidate address in turn until one is under way. */
static void start_connecting(NetSocket *s)
{
    int err;
    char *errmsg = NULL;

    do {
        sfree(errmsg);
        err = try_connect(s, &errmsg);
    } while (err && sk_nextaddr(s->addr, &s->curraddr));

    if (err)
        set_error(s, errmsg);          /* kept for sk_socket_error() */
    sfree(errmsg);
}

Socket *sk_new(SockAddr *addr, int port, bool privport, bool oobinline,
               bool nodelay, bool keepalive, Plug *plug)
{
    NetSocket *s = netsocket_new();

    s->plug = plug;
    s->addr = addr;
    s->curraddr = 0;
    s->oobinline = oobinline;
    s->nodelay = nodelay;
    s->keepalive = keepalive;
    s->privport = privport;
    s->port = port;

    if (addr->error) {
        set_error(s, addr->error);
        add234(sktree, s);
        return &s->sock;
    }

    switch (addr->state) {
      case ADDR_IP:
        start_connecting(s);
        break;
      case ADDR_RESOLVING:
        /* Connect when the name server has answered; see ro_net_poll(). */
        s->waiting_for_dns = true;
        add234(sktree, s);
        break;
      default:
        set_error(s, "Cannot connect to an address that has not been resolved");
        add234(sktree, s);
        break;
    }

    return &s->sock;
}

static void listener_error(NetSocket *s, const _kernel_oserror *e)
{
    char *m = oserror_text(e);
    set_error(s, m);
    sfree(m);
}

Socket *sk_newlistener(const char *srcaddr, int port, Plug *plug,
                       bool local_host_only, int orig_address_family)
{
    int fd = -1, on = 1;
    ro_sockaddr_in sa;
    NetSocket *s = netsocket_new();
    _kernel_oserror *e;
    IPv4 ip = { { 0, 0, 0, 0 } };

    s->plug = plug;
    s->localhost_only = local_host_only;
    s->listener = true;

    if (orig_address_family == ADDRTYPE_IPV6) {
        set_error(s, "IPv6 is not supported on RISC OS");
        return &s->sock;
    }

    e = _swix(Socket_Creat, _INR(0, 2) | _OUT(0),
              AF_INET_RO, SOCK_STREAM_RO, 0, &fd);
    if (e) {
        listener_error(s, e);
        return &s->sock;
    }

    e = _swix(Socket_Setsockopt, _INR(0, 4), fd, SOL_SOCKET_RO,
              SO_REUSEADDR_RO, &on, 4);
    if (e) {
        listener_error(s, e);
        sock_close_handle(fd);
        return &s->sock;
    }

    if (srcaddr != NULL && parse_ipv4(srcaddr, &ip)) {
        /* An explicit listen address overrides local_host_only. */
        s->localhost_only = (ip.b[0] == 127);
    } else if (local_host_only) {
        ip.b[0] = 127; ip.b[3] = 1;
    }

    memset(&sa, 0, sizeof sa);
    sa.family = AF_INET_RO;
    sa.port = (unsigned short)(((port & 255) << 8) | ((port >> 8) & 255));
    memcpy(sa.addr, ip.b, 4);

    e = _swix(Socket_Bind, _INR(0, 2), fd, &sa, (int)sizeof sa);
    if (e) {
        listener_error(s, e);
        sock_close_handle(fd);
        return &s->sock;
    }

    e = _swix(Socket_Listen, _INR(0, 1), fd, 5);
    if (e) {
        listener_error(s, e);
        sock_close_handle(fd);
        return &s->sock;
    }

    sock_set_nonblocking(fd);
    s->s = fd;

    uxsel_tell(s);
    add234(sktree, s);

    return &s->sock;
}

static void sk_net_close(Socket *sock)
{
    NetSocket *s = container_of(sock, NetSocket, sock);

    bufchain_clear(&s->output_data);

    del234(sktree, s);
    if (s->s >= 0) {
        uxsel_del(s->s);
        sock_close_handle(s->s);
    }
    if (s->addr)
        sk_addr_free(s->addr);
    sfree(s->errstr);
    sfree(s->pending_errmsg);
    delete_callbacks_for_context(s);
    sfree(s);
}

void *sk_getxdmdata(Socket *sock, int *lenp)
{
    return NULL;                       /* no XDM-AUTHORIZATION on RISC OS */
}

void plug_closing_errno(Plug *plug, int error)
{
    plug_closing(plug, PLUGCLOSE_ERROR, strerror(error));
}

/*
 * Deal with socket errors detected in try_send().
 */
static void socket_error_callback(void *vs)
{
    NetSocket *s = (NetSocket *)vs;

    /*
     * Just in case other socket work has caused this socket to vanish or
     * become somehow non-erroneous before this callback arrived...
     */
    if (!find234(sktree, s, NULL) || !s->pending_errmsg)
        return;

    /* An error has occurred on this socket.  Pass it to the plug. */
    plug_closing(s->plug, PLUGCLOSE_ERROR, s->pending_errmsg);
}

/*
 * The function which tries to send on a socket once it's deemed writable.
 */
static void try_send(NetSocket *s)
{
    while (s->sending_oob || bufchain_size(&s->output_data) > 0) {
        int nsent = 0;
        const void *data;
        size_t len;
        int urgentflag;
        _kernel_oserror *e;

        if (s->sending_oob) {
            urgentflag = MSG_OOB_RO;
            len = s->sending_oob;
            data = &s->oobdata;
        } else {
            ptrlen bufdata = bufchain_prefix(&s->output_data);
            urgentflag = 0;
            data = bufdata.ptr;
            len = bufdata.len;
        }
        e = _swix(Socket_Send, _INR(0, 3) | _OUT(0), s->s, data, (int)len,
                  urgentflag, &nsent);
        if (!e)
            noise_ultralight(NOISE_SOURCE_IOLEN, nsent);
        if (e || nsent <= 0) {
            int en = e ? ro_errno(e) : 0;
            if (e && (en == RO_EWOULDBLOCK || en == RO_EAGAIN ||
                      en == RO_ENOBUFS)) {
                /* Perfectly normal: we've sent all we can for the moment. */
                s->writable = false;
                return;
            } else {
                /*
                 * We unfortunately can't just call plug_closing(), because
                 * it's quite likely that we're currently _in_ a call from the
                 * code we'd be calling back to, so we'd have to make half the
                 * SSH code reentrant.  Instead we flag a pending error on the
                 * socket, to be dealt with (by calling plug_closing()) at some
                 * suitable future moment.
                 */
                sfree(s->pending_errmsg);
                s->pending_errmsg = e ? oserror_text(e)
                                      : dupstr("Connection closed by peer");
                /* Stop selecting on this socket, so that we don't tight-loop
                 * repeatedly trying to do whatever it was that went wrong. */
                uxsel_tell(s);
                /* Arrange to be called back from the top level to deal with
                 * the error condition on this socket. */
                queue_toplevel_callback(socket_error_callback, s);
                return;
            }
        } else {
            if (s->sending_oob) {
                if ((size_t)nsent < len) {
                    memmove(s->oobdata, s->oobdata+nsent, len-nsent);
                    s->sending_oob = len - nsent;
                } else {
                    s->sending_oob = 0;
                }
            } else {
                bufchain_consume(&s->output_data, nsent);
            }
        }
    }

    /* If we reach here, we've finished sending everything we might have
     * needed to send.  Send EOF, if we need to. */
    if (s->outgoingeof == EOF_PENDING) {
        _swix(Socket_Shutdown, _INR(0, 1), s->s, 1);
        s->outgoingeof = EOF_SENT;
    }

    /* Also update the select status, because we don't need to select for
     * writing any more. */
    uxsel_tell(s);
}

static size_t sk_net_write(Socket *sock, const void *buf, size_t len)
{
    NetSocket *s = container_of(sock, NetSocket, sock);

    assert(s->outgoingeof == EOF_NO);

    /* Add the data to the buffer list on the socket. */
    bufchain_add(&s->output_data, buf, len);

    /* Now try sending from the start of the buffer list. */
    if (s->writable)
        try_send(s);

    /* Update the select() status to correctly reflect whether or not we
     * should be selecting for write. */
    uxsel_tell(s);

    return bufchain_size(&s->output_data);
}

static size_t sk_net_write_oob(Socket *sock, const void *buf, size_t len)
{
    NetSocket *s = container_of(sock, NetSocket, sock);

    assert(s->outgoingeof == EOF_NO);

    /* Replace the buffer list on the socket with the data. */
    bufchain_clear(&s->output_data);
    assert(len <= sizeof(s->oobdata));
    memcpy(s->oobdata, buf, len);
    s->sending_oob = len;

    /* Now try sending from the start of the buffer list. */
    if (s->writable)
        try_send(s);

    uxsel_tell(s);

    return s->sending_oob;
}

static void sk_net_write_eof(Socket *sock)
{
    NetSocket *s = container_of(sock, NetSocket, sock);

    assert(s->outgoingeof == EOF_NO);

    /* Mark the socket as pending outgoing EOF. */
    s->outgoingeof = EOF_PENDING;

    /* Now try sending from the start of the buffer list. */
    if (s->writable)
        try_send(s);

    uxsel_tell(s);
}

static void net_select_result(int fd, int event)
{
    int ret;
    static char buf[16384];            /* nice big buffer for plenty of speed */
    NetSocket *s;
    bool atmark = true;
    _kernel_oserror *e;

    /* Find the Socket structure */
    s = find234(sktree, &fd, cmpforsearch);
    trace("select_result fd=%d event=%d found=%d", fd, event, s != NULL);
    if (!s)
        return;                /* boggle */

    noise_ultralight(NOISE_SOURCE_IOID, fd);

    switch (event) {
      case SELECT_X:                   /* exceptional */
        if (!s->oobinline) {
            /*
             * On a non-oobinline socket, this indicates that we can
             * immediately perform an OOB read and get back OOB data, which we
             * will send to the back end with type==2 (urgent data).
             */
            ret = 0;
            e = _swix(Socket_Recv, _INR(0, 3) | _OUT(0), s->s, buf,
                      (int)sizeof buf, MSG_OOB_RO, &ret);
            if (!e)
                noise_ultralight(NOISE_SOURCE_IOLEN, ret);
            if (!e && ret == 0) {
                plug_closing_error(s->plug, "Internal networking trouble");
            } else if (e) {
                char *m = oserror_text(e);
                plug_closing_error(s->plug, m);
                sfree(m);
            } else {
                /* Receiving actual data on a socket means we can stop
                 * falling back through the candidate addresses to connect to. */
                if (s->addr) {
                    sk_addr_free(s->addr);
                    s->addr = NULL;
                }
                plug_receive(s->plug, 2, buf, ret);
            }
            break;
        }

        /*
         * If we reach here, this is an oobinline socket, which means we should
         * set s->oobpending and then deal with it when we get called for the
         * readability event (which should also occur).
         */
        s->oobpending = true;
        break;
      case SELECT_R:                   /* readable; also acceptance */
        if (s->listener) {
            /*
             * On a listening socket, the readability event means a connection
             * is ready to be accepted.
             */
            ro_sockaddr_in su;
            int addrlen = sizeof su;
            accept_ctx_t actx;
            int t = -1;                /* socket of connection */

            memset(&su, 0, sizeof su);
            e = _swix(Socket_Accept, _INR(0, 2) | _OUT(0), s->s, &su,
                      &addrlen, &t);
            if (e || t < 0)
                break;

            sock_set_nonblocking(t);
            actx.i = t;

            if (s->localhost_only && su.addr[0] != 127) {
                sock_close_handle(t);  /* someone let nonlocal through?! */
            } else if (plug_accepting(s->plug, sk_net_accept, actx)) {
                sock_close_handle(t);  /* denied or error */
            }
            break;
        }

        /*
         * If we reach here, this is not a listening socket, so readability
         * really means readability.
         */

        /* In the case the socket is still frozen, we don't even bother */
        if (s->frozen)
            break;

        /*
         * We have received data on the socket.  For an oobinline socket, this
         * might be data _before_ an urgent pointer, in which case we send it
         * to the back end with type==1 (data prior to urgent).
         */
        if (s->oobinline && s->oobpending) {
            int atmark_from_ioctl = 0;
            if (!_swix(Socket_Ioctl, _INR(0, 2), s->s, SIOCATMARK_RO,
                       &atmark_from_ioctl)) {
                atmark = atmark_from_ioctl;
                if (atmark)
                    s->oobpending = false; /* clear this indicator */
            }
        } else
            atmark = true;

        ret = 0;
        e = _swix(Socket_Recv, _INR(0, 3) | _OUT(0), s->s, buf,
                  s->oobpending ? 1 : (int)sizeof buf, 0, &ret);
        trace("  recv: %s ret=%d", e ? e->errmess : "ok", ret);
        if (e) {
            int en = ro_errno(e);
            if (en == RO_EWOULDBLOCK || en == RO_EAGAIN)
                break;
            {
                char *m = oserror_text(e);
                plug_closing_error(s->plug, m);
                sfree(m);
            }
            break;
        }
        noise_ultralight(NOISE_SOURCE_IOLEN, ret);
        if (0 == ret) {
            s->incomingeof = true;     /* stop trying to read now */
            uxsel_tell(s);
            plug_closing_normal(s->plug);
        } else {
            /* Receiving actual data on a socket means we can stop falling
             * back through the candidate addresses to connect to. */
            if (s->addr) {
                sk_addr_free(s->addr);
                s->addr = NULL;
            }
            plug_receive(s->plug, atmark ? 0 : 1, buf, ret);
        }
        break;
      case SELECT_W:                   /* writable */
        if (!s->connected) {
            /*
             * select reports a socket as _writable_ when an asynchronous
             * connect() attempt either completes or fails.  So first we must
             * find out which.
             */
            int err = socket_pending_error(s->s);
            if (err) {
                /*
                 * The asynchronous connection attempt failed.  Report the
                 * problem via plug_log, and try again with the next candidate
                 * address, if we have more than one.
                 */
                SockAddr thisaddr;
                const char *t = bsd_errtext(err);
                char fallback[40];
                char *errmsg;
                assert(s->addr);

                if (!t) {
                    snprintf(fallback, sizeof fallback, "Network error %d",
                             err);
                    t = fallback;
                }
                errmsg = dupstr(t);

                thisaddr = sk_extractaddr_tmp(s->addr, s->curraddr);
                plug_log(s->plug, &s->sock, PLUGLOG_CONNECT_FAILED,
                         &thisaddr, s->port, errmsg, err);

                while (err && s->addr && sk_nextaddr(s->addr, &s->curraddr)) {
                    char *m2 = NULL;
                    err = try_connect(s, &m2);
                    if (err) {
                        sfree(errmsg);
                        errmsg = m2;
                    } else {
                        sfree(m2);
                    }
                }
                if (err) {
                    plug_closing(s->plug, PLUGCLOSE_ERROR, errmsg);
                    sfree(errmsg);
                    return;      /* socket is now presumably defunct */
                }
                sfree(errmsg);
                if (!s->connected)
                    return;      /* another async attempt in progress */
            } else {
                /* The connection attempt succeeded. */
                SockAddr thisaddr = sk_extractaddr_tmp(s->addr, s->curraddr);
                plug_log(s->plug, &s->sock, PLUGLOG_CONNECT_SUCCESS,
                         &thisaddr, s->port, NULL, 0);
            }

            /* If we get here, we've managed to make a connection. */
            if (s->addr) {
                sk_addr_free(s->addr);
                s->addr = NULL;
            }
            s->connected = true;
            s->writable = true;
            uxsel_tell(s);
        } else {
            size_t bufsize_before, bufsize_after;
            s->writable = true;
            bufsize_before = s->sending_oob + bufchain_size(&s->output_data);
            try_send(s);
            bufsize_after = s->sending_oob + bufchain_size(&s->output_data);
            if (bufsize_after < bufsize_before)
                plug_sent(s->plug, bufsize_after);
        }
        break;
    }
}

static const char *sk_net_socket_error(Socket *sock)
{
    NetSocket *s = container_of(sock, NetSocket, sock);
    return s->error;
}

static void sk_net_set_frozen(Socket *sock, bool is_frozen)
{
    NetSocket *s = container_of(sock, NetSocket, sock);
    if (s->frozen == is_frozen)
        return;
    s->frozen = is_frozen;
    uxsel_tell(s);
}

static SocketEndpointInfo *sk_net_endpoint_info(Socket *sock, bool peer)
{
    NetSocket *s = container_of(sock, NetSocket, sock);
    ro_sockaddr_in addr;
    int addrlen = sizeof addr;
    SocketEndpointInfo *pi;
    _kernel_oserror *e;

    if (s->s < 0)
        return NULL;
    memset(&addr, 0, sizeof addr);
    e = _swix(peer ? Socket_Getpeername : Socket_Getsockname, _INR(0, 2),
              s->s, &addr, &addrlen);
    if (e || addr.family != AF_INET_RO)
        return NULL;

    pi = snew(SocketEndpointInfo);
    pi->addressfamily = ADDRTYPE_IPV4;
    memcpy(pi->addr_bin.ipv4, addr.addr, 4);
    pi->port = ((addr.port & 255) << 8) | ((addr.port >> 8) & 255);
    pi->addr_text = dupprintf("%u.%u.%u.%u", addr.addr[0], addr.addr[1],
                              addr.addr[2], addr.addr[3]);
    pi->log_text = dupprintf("%s:%d", pi->addr_text, pi->port);
    return pi;
}

int sk_net_get_fd(Socket *sock)
{
    /* This function is not fully general: it only works on NetSocket */
    if (sock->vt != &NetSocket_sockvt)
        return -1;                     /* failure */
    NetSocket *s = container_of(sock, NetSocket, sock);
    return s->s;
}

static void uxsel_tell(NetSocket *s)
{
    int rwx = 0;
    if (s->s < 0)
        return;
    if (!s->pending_errmsg) {
        if (s->listener) {
            rwx |= SELECT_R;           /* read == accept */
        } else {
            if (!s->connected)
                rwx |= SELECT_W;       /* write == connect */
            if (s->connected && !s->frozen && !s->incomingeof)
                rwx |= SELECT_R | SELECT_X;
            if (bufchain_size(&s->output_data))
                rwx |= SELECT_W;
        }
    }
    uxsel_set(s->s, rwx, net_select_result);
}

int net_service_lookup(const char *service)
{
    return 0;                          /* no getservbyname here */
}

char *get_hostname(void)
{
    return NULL;
}

/*
 * ----------------------------------------------------------------------
 * The glue to the event loops.
 */

/* The loops re-check uxsel's table every time round, so there is nothing to do
 * when an fd is added or removed. */
uxsel_id *uxsel_input_add(int fd, int rwx) { return NULL; }
void uxsel_input_remove(uxsel_id *id) { }

/* Advance name lookups, and connect sockets whose lookups have finished. */
static void step_dns(void)
{
    SockAddr *a, *next;
    NetSocket *s;
    int i;

    for (a = resolving_head; a; a = next) {
        next = a->next_resolving;
        resolve_step(a);               /* may unlink a */
    }

    for (i = 0; (s = index234(sktree, i)) != NULL; i++) {
        if (!s->waiting_for_dns)
            continue;
        if (s->addr->state == ADDR_RESOLVING)
            continue;                  /* not yet */

        /* i may be invalidated by what follows (the socket moves in the tree
         * when it gets a handle), so deal with it and start again. */
        s->waiting_for_dns = false;
        if (s->addr->state == ADDR_IP) {
            s->curraddr = 0;
            start_connecting(s);
            if (s->error) {
                /* Every address failed; the caller of sk_new() has long since
                 * gone away, so say so through the plug. */
                sfree(s->pending_errmsg);
                s->pending_errmsg = dupstr(s->error);
                queue_toplevel_callback(socket_error_callback, s);
            }
        } else {
            sfree(s->pending_errmsg);
            s->pending_errmsg = dupstr(s->addr->error ? s->addr->error
                                       : "Host name lookup failed");
            queue_toplevel_callback(socket_error_callback, s);
        }
        i = -1;
    }
}

bool ro_net_busy(void)
{
    NetSocket *s;
    int i;

    if (resolving_head)
        return true;
    for (i = 0; sktree && (s = index234(sktree, i)) != NULL; i++)
        if (s->waiting_for_dns || (!s->listener && !s->connected && s->s >= 0))
            return true;
    return false;
}

int ro_net_poll(int timeout_cs)
{
    uint32_t rd[FDSET_WORDS], wr[FDSET_WORDS], ex[FDSET_WORDS];
    int fdlist[FDSET_WORDS * 32];
    int nfds = 0, maxfd = -1, i, state, rwx, fd, nready = 0, found = 0;
    struct { int sec; int usec; } tv;
    _kernel_oserror *e;

    if (!sktree)
        return 0;

    step_dns();

    memset(rd, 0, sizeof rd);
    memset(wr, 0, sizeof wr);
    memset(ex, 0, sizeof ex);

    for (fd = first_fd(&state, &rwx); fd >= 0; fd = next_fd(&state, &rwx)) {
        if (fd >= FDSET_WORDS * 32 || nfds >= FDSET_WORDS * 32)
            continue;
        fdlist[nfds] = fd;
        nfds++;
        if (rwx & SELECT_R) rd[fd >> 5] |= 1u << (fd & 31);
        if (rwx & SELECT_W) wr[fd >> 5] |= 1u << (fd & 31);
        if (rwx & SELECT_X) ex[fd >> 5] |= 1u << (fd & 31);
        if (fd > maxfd) maxfd = fd;
    }

    if (timeout_cs < 0)
        timeout_cs = 0;
    tv.sec = timeout_cs / 100;
    tv.usec = (timeout_cs % 100) * 10000;

    e = _swix(Socket_Select, _INR(0, 4) | _OUT(0), maxfd + 1, rd, wr, ex,
              &tv, &nready);
    if (e)
        trace("select error: %s", e->errmess);
    else if (nready > 0)
        trace("select: nfds=%d nready=%d rd=%08x wr=%08x ex=%08x", maxfd + 1,
              nready, rd[0], wr[0], ex[0]);
    if (e || nready <= 0)
        return 0;

    for (i = 0; i < nfds; i++) {
        fd = fdlist[i];
        /*
         * We must process exceptional notifications before ordinary
         * readability ones, or we may go straight past the urgent marker.
         */
        if (ex[fd >> 5] & (1u << (fd & 31))) {
            select_result(fd, SELECT_X);
            found++;
        }
        if (rd[fd >> 5] & (1u << (fd & 31))) {
            select_result(fd, SELECT_R);
            found++;
        }
        if (wr[fd >> 5] & (1u << (fd & 31))) {
            select_result(fd, SELECT_W);
            found++;
        }
    }
    return found;
}
