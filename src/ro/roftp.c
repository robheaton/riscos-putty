/*
 * roftp.c - an SFTP connection for the file transfer window.  See roftp.h.
 *
 * This is what psftp.c does with its single global connection, written so that waiting for the
 * server keeps the desktop alive and the questions of the connection are boxes.  Upstream's SFTP client
 * (ssh/sftp.c) calls sftp_senddata, sftp_recvdata and sftp_sendbuffer to talk to the server, and keeps
 * the table of outstanding requests in a global, which is why there is one connection ("cur") and
 * one operation at a time.
 *
 * The transfers are psftp's sftp_get_file and sftp_put_file with a progress callback; the local files
 * are the ones of rofile.c, so names, filetypes and dates convert as they do in psftp and pscp.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "putty.h"
#include "ssh.h"
#include "storage.h"
#include "psftp.h"
#include "ssh/sftp.h"
#include "ro.h"
#include "rowimp.h"
#include "rodialog.h"
#include "roapp.h"
#include "roseat.h"
#include "rosftp.h"
#include "roftp.h"

struct FtConn {
    Conf *conf;
    Backend *backend;
    LogContext *logctx;
    Seat seat;
    LogPolicy logpolicy;
    bufchain received;                 /* what the server sent, not yet read as SFTP */
    cmdline_get_passwd_input_state cmdline_state;
    bool sent_eof, dead, aborting;
    char *fatal;                       /* why the connection broke */
    char *home;
    char *host;
    FtStatusFn status;
    void *status_ctx;
};

static FtConn *cur;                    /* the connection the sftp_ hooks below work for */
static FtConn *connecting;             /* the one ft_connect is making, for ft_abort(NULL) */

static const char *dead_reason(const FtConn *c)
{
    return c->fatal ? c->fatal : "The connection was closed.";
}

/* ------------------------------------------------------------------------ */
/* waiting                                                                  */

/* Wait a moment for something to happen; false if the connection is gone or we are told to stop. */
static bool ft_wait(FtConn *c)
{
    if (c->dead || c->aborting) return false;
    if (!roapp_wait()) c->aborting = true;           /* the program is quitting */
    return !c->dead && !c->aborting;
}

/* ------------------------------------------------------------------------ */
/* the hooks ssh/sftp.c calls                                               */

bool sftp_recvdata(char *buf, size_t len)
{
    FtConn *c = cur;

    while (len > 0) {
        size_t got;
        while (bufchain_size(&c->received) == 0) {
            if (c->dead || backend_exitcode(c->backend) >= 0) {
                c->dead = true;
                return false;
            }
            if (!ft_wait(c))
                return false;
        }
        got = bufchain_fetch_consume_up_to(&c->received, buf, len);
        buf += got;
        len -= got;
    }
    return true;
}

bool sftp_senddata(const char *buf, size_t len)
{
    backend_send(cur->backend, buf, len);
    return true;
}

size_t sftp_sendbuffer(void)
{
    return backend_sendbuffer(cur->backend);
}

/* ------------------------------------------------------------------------ */
/* the Seat: how the connection talks to the person                         */

static void set_fatal(FtConn *c, const char *msg)
{
    if (!c->fatal)
        c->fatal = dupstr(msg);
    c->dead = true;
}

static size_t ft_output(Seat *seat, SeatOutputType type, const void *data, size_t len)
{
    FtConn *c = container_of(seat, FtConn, seat);
    if (type == SEAT_OUTPUT_STDOUT)
        bufchain_add(&c->received, data, len);
    return 0;                          /* (what the server says on stderr is of no use here) */
}

static bool ft_eof(Seat *seat)
{
    FtConn *c = container_of(seat, FtConn, seat);
    if (!c->sent_eof)
        set_fatal(c, "Received unexpected end-of-file from the SFTP server.");
    return false;
}

static size_t ft_banner(Seat *seat, const void *data, size_t len)
{
    return len;
}

static void ft_connection_fatal(Seat *seat, const char *msg)
{
    set_fatal(container_of(seat, FtConn, seat), msg);
}

static void ft_nonfatal(Seat *seat, const char *msg)
{
    FtConn *c = container_of(seat, FtConn, seat);
    char *title = dupcat(appname, " Error");
    ro_notice(c, title, msg);
    sfree(title);
}

/* ---- prompts: one box for each, in turn ---- */

typedef struct {
    FtConn *c;
    prompts_t *p;
    size_t i;
} PromptJob;

static void prompt_finish(PromptJob *j, SeatPromptResult spr)
{
    prompts_t *p = j->p;
    p->spr = spr;
    p->data = NULL;
    sfree(j);
    queue_toplevel_callback(p->callback, p->callback_ctx);
}

static void prompt_ask(PromptJob *j);

static void prompt_answered(void *ctx, const char *answer)
{
    PromptJob *j = (PromptJob *)ctx;

    if (!answer) {
        prompt_finish(j, SPR_USER_ABORT);
        return;
    }
    prompt_set_result(j->p->prompts[j->i], answer);
    if (++j->i < j->p->n_prompts)
        prompt_ask(j);
    else
        prompt_finish(j, SPR_OK);
}

static void prompt_ask(PromptJob *j)
{
    prompts_t *p = j->p;
    strbuf *text = strbuf_new();
    char *title;

    if (j->i == 0 && p->instruction && *p->instruction)
        put_fmt(text, "%s\n", p->instruction);
    put_datapl(text, ptrlen_from_asciz(p->prompts[j->i]->prompt));
    title = p->name && *p->name ? dupstr(p->name) : dupprintf("%s - %s", appname, j->c->host);
    ro_prompt(j->c, title, text->s, !p->prompts[j->i]->echo, "", prompt_answered, j);
    sfree(title);
    strbuf_free(text);
}

static SeatPromptResult ft_get_userpass_input(Seat *seat, prompts_t *p)
{
    FtConn *c = container_of(seat, FtConn, seat);
    SeatPromptResult spr = cmdline_get_passwd_input(p, &c->cmdline_state, false);
    PromptJob *j;
    size_t i;

    if (spr.kind != SPRK_INCOMPLETE)
        return spr;
    if (p->spr.kind != SPRK_INCOMPLETE)
        return p->spr;                 /* already finished: the same answer again */
    if (p->data)
        return SPR_INCOMPLETE;         /* still asking */

    for (i = 0; i < p->n_prompts; i++)
        prompt_set_result(p->prompts[i], "");
    if (p->n_prompts == 0)
        return SPR_OK;                 /* only something to read (an instruction): nothing to answer */

    j = snew(PromptJob);
    j->c = c;
    j->p = p;
    j->i = 0;
    p->data = j;
    p->spr = SPR_INCOMPLETE;
    prompt_ask(j);
    return SPR_INCOMPLETE;
}

static SeatPromptResult ft_confirm_ssh_host_key(
    Seat *seat, const char *host, int port, const char *keytype, char *keystr,
    SeatDialogText *text, HelpCtx helpctx,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    FtConn *c = container_of(seat, FtConn, seat);
    return roseat_confirm_host_key(c, seat, host, port, keytype, keystr, text, callback, ctx);
}

static SeatPromptResult ft_confirm_weak_crypto_primitive(
    Seat *seat, SeatDialogText *text, void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    return roseat_confirm_yesno(container_of(seat, FtConn, seat), text, callback, ctx);
}

static SeatPromptResult ft_confirm_weak_cached_hostkey(
    Seat *seat, SeatDialogText *text, void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    return roseat_confirm_yesno(container_of(seat, FtConn, seat), text, callback, ctx);
}

static const SeatDialogPromptDescriptions *ft_prompt_descriptions(Seat *seat)
{
    return roseat_prompt_descriptions();
}

static StripCtrlChars *ft_stripctrl_new(Seat *seat, BinarySink *bs_out, SeatInteractionContext sic)
{
    return stripctrl_new(bs_out, false, 0);
}

static const SeatVtable ft_seat_vt = {
    .output = ft_output,
    .eof = ft_eof,
    .sent = nullseat_sent,
    .banner = ft_banner,
    .get_userpass_input = ft_get_userpass_input,
    .notify_session_started = nullseat_notify_session_started,
    .notify_remote_exit = nullseat_notify_remote_exit,
    .notify_remote_disconnect = nullseat_notify_remote_disconnect,
    .connection_fatal = ft_connection_fatal,
    .nonfatal = ft_nonfatal,
    .update_specials_menu = nullseat_update_specials_menu,
    .get_ttymode = nullseat_get_ttymode,
    .set_busy_status = nullseat_set_busy_status,
    .confirm_ssh_host_key = ft_confirm_ssh_host_key,
    .confirm_weak_crypto_primitive = ft_confirm_weak_crypto_primitive,
    .confirm_weak_cached_hostkey = ft_confirm_weak_cached_hostkey,
    .prompt_descriptions = ft_prompt_descriptions,
    .is_utf8 = nullseat_is_never_utf8,
    .echoedit_update = nullseat_echoedit_update,
    .get_display = nullseat_get_display,
    .get_windowid = nullseat_get_windowid,
    .get_window_pixel_size = nullseat_get_window_pixel_size,
    .stripctrl_new = ft_stripctrl_new,
    .set_trust_status = nullseat_set_trust_status,
    .can_set_trust_status = nullseat_can_set_trust_status_yes,
    .has_mixed_input_stream = nullseat_has_mixed_input_stream_no,
    .verbose = nullseat_verbose_yes,
    .interactive = nullseat_interactive_yes,
    .get_cursor_position = nullseat_get_cursor_position,
};

/* ---- the log: the last event is what the window shows while connecting ---- */

static void ft_lp_eventlog(LogPolicy *lp, const char *string)
{
    FtConn *c = container_of(lp, FtConn, logpolicy);
    if (c->status)
        c->status(c->status_ctx, string);
}

static int ft_lp_askappend(LogPolicy *lp, Filename *filename, void (*callback)(void *ctx, int result), void *ctx)
{
    return 2;                          /* (logging is switched off; but if it were on: start afresh) */
}

static void ft_lp_logging_error(LogPolicy *lp, const char *event)
{
}

static const LogPolicyVtable ft_logpolicy_vt = {
    .eventlog = ft_lp_eventlog,
    .askappend = ft_lp_askappend,
    .logging_error = ft_lp_logging_error,
    .verbose = null_lp_verbose_yes,
};

/* ------------------------------------------------------------------------ */
/* requests                                                                 */

/* Wait for the reply to `req`.  NULL if the connection broke. */
static struct sftp_packet *wait_reply(FtConn *c, struct sftp_request *req)
{
    struct sftp_packet *pktin;
    struct sftp_request *rreq;

    cur = c;
    sftp_register(req);
    pktin = sftp_recv();
    if (!pktin) {
        set_fatal(c, "No reply came from the SFTP server.");
        return NULL;
    }
    rreq = sftp_find_request(pktin);
    if (rreq != req) {
        set_fatal(c, "The SFTP server's reply could not be understood.");
        sftp_pkt_free(pktin);
        return NULL;
    }
    return pktin;
}

static char *failure(FtConn *c, const char *what, const char *path)
{
    if (c->dead || c->aborting)
        return dupstr(dead_reason(c));
    return path ? dupprintf("%s: %s: %s", path, what, fxp_error()) : dupprintf("%s: %s", what, fxp_error());
}

/* Close a remote handle, ignoring how it went. */
static void close_handle(FtConn *c, struct fxp_handle *fh)
{
    struct sftp_request *req = fxp_close_send(fh);
    struct sftp_packet *pktin = wait_reply(c, req);
    if (pktin)
        fxp_close_recv(pktin, req);
}

char *ft_join(const char *dir, const char *leaf)
{
    size_t n = strlen(dir);
    return n > 0 && dir[n - 1] == '/' ? dupcat(dir, leaf) : dupcat(dir, "/", leaf);
}

bool ft_alive(const FtConn *c)
{
    if (!c || c->dead || c->aborting)
        return false;
    if (c->backend && backend_exitcode(c->backend) >= 0) {      /* the server hung up while we were idle */
        FtConn *w = (FtConn *)c;
        w->dead = true;
        return false;
    }
    return true;
}

const char *ft_why_dead(const FtConn *c)
{
    return dead_reason(c);
}

const char *ft_home(const FtConn *c)
{
    return c->home ? c->home : "/";
}

const char *ft_host(const FtConn *c)
{
    return c->host;
}

void ft_abort(FtConn *c)
{
    if (!c)
        c = connecting;
    if (c)
        c->aborting = true;
}

char *ft_realpath(FtConn *c, const char *base, const char *path, char **err)
{
    struct sftp_request *req;
    struct sftp_packet *pktin;
    char *full, *canon;

    cur = c;
    full = path[0] == '/' || !base ? dupstr(path) : ft_join(base, path);
    req = fxp_realpath_send(full);
    pktin = wait_reply(c, req);
    canon = pktin ? fxp_realpath_recv(pktin, req) : NULL;
    if (!canon && err)
        *err = failure(c, "no such directory", full);
    sfree(full);
    return canon;
}

bool ft_stat(FtConn *c, const char *path, FtEntry *out)
{
    struct sftp_request *req;
    struct sftp_packet *pktin;
    struct fxp_attrs attrs;

    cur = c;
    memset(out, 0, sizeof *out);
    req = fxp_stat_send(path);
    pktin = wait_reply(c, req);
    if (!pktin || !fxp_stat_recv(pktin, req, &attrs))
        return false;
    out->has_perms = (attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0;
    out->perms = attrs.permissions;
    out->dir = out->has_perms && (attrs.permissions & 0170000) == 0040000;
    out->has_size = (attrs.flags & SSH_FILEXFER_ATTR_SIZE) != 0;
    out->size = attrs.size;
    out->has_mtime = (attrs.flags & SSH_FILEXFER_ATTR_ACMODTIME) != 0;
    out->mtime = attrs.mtime;
    return true;
}

bool ft_list(FtConn *c, const char *dir, FtEntry **out, size_t *nout, char **err)
{
    struct sftp_request *req;
    struct sftp_packet *pktin;
    struct fxp_handle *dh;
    FtEntry *list = NULL;
    size_t n = 0, cap = 0;
    bool ok = true;

    cur = c;
    *out = NULL;
    *nout = 0;
    req = fxp_opendir_send(dir);
    pktin = wait_reply(c, req);
    dh = pktin ? fxp_opendir_recv(pktin, req) : NULL;
    if (!dh) {
        *err = failure(c, "unable to open directory", dir);
        return false;
    }

    for (;;) {
        struct fxp_names *names;
        int i;

        req = fxp_readdir_send(dh);
        pktin = wait_reply(c, req);
        names = pktin ? fxp_readdir_recv(pktin, req) : NULL;
        if (!names) {
            if (pktin && fxp_error_type() == SSH_FX_EOF)
                break;
            *err = failure(c, "reading the directory", dir);
            ok = false;
            break;
        }
        if (names->nnames == 0) {
            fxp_free_names(names);
            break;
        }
        for (i = 0; i < names->nnames; i++) {
            const struct fxp_name *nm = &names->names[i];
            FtEntry *e;

            if (!strcmp(nm->filename, ".") || !strcmp(nm->filename, "..") || !vet_filename(nm->filename))
                continue;
            sgrowarray(list, cap, n);
            e = &list[n++];
            memset(e, 0, sizeof *e);
            e->name = dupstr(nm->filename);
            e->has_perms = (nm->attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0;
            e->perms = nm->attrs.permissions;
            e->has_size = (nm->attrs.flags & SSH_FILEXFER_ATTR_SIZE) != 0;
            e->size = nm->attrs.size;
            e->has_mtime = (nm->attrs.flags & SSH_FILEXFER_ATTR_ACMODTIME) != 0;
            e->mtime = nm->attrs.mtime;
            e->dir = e->has_perms && (e->perms & 0170000) == 0040000;
            e->link = e->has_perms && (e->perms & 0170000) == 0120000;
        }
        fxp_free_names(names);
    }
    close_handle(c, dh);

    if (ok) {
        size_t i;
        /* A symbolic link is shown as what it points to. */
        for (i = 0; i < n && ok; i++) {
            if (list[i].link) {
                char *p = ft_join(dir, list[i].name);
                FtEntry t;
                if (ft_stat(c, p, &t)) {
                    list[i].dir = t.dir;
                    if (t.has_size) { list[i].has_size = true; list[i].size = t.size; }
                }
                sfree(p);
                if (c->dead || c->aborting) {
                    *err = dupstr(dead_reason(c));
                    ok = false;
                }
            }
        }
    }
    if (!ok) {
        ft_free_entries(list, n);
        return false;
    }
    *out = list;
    *nout = n;
    return true;
}

void ft_free_entries(FtEntry *e, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        sfree(e[i].name);
    sfree(e);
}

bool ft_mkdir(FtConn *c, const char *path, char **err)
{
    struct sftp_request *req = fxp_mkdir_send(path, NULL);
    struct sftp_packet *pktin;

    cur = c;
    pktin = wait_reply(c, req);
    if (pktin && fxp_mkdir_recv(pktin, req))
        return true;
    *err = failure(c, "unable to create the directory", path);
    return false;
}

bool ft_rename(FtConn *c, const char *from, const char *to, char **err)
{
    struct sftp_request *req = fxp_rename_send(from, to);
    struct sftp_packet *pktin;

    cur = c;
    pktin = wait_reply(c, req);
    if (pktin && fxp_rename_recv(pktin, req))
        return true;
    *err = failure(c, "unable to rename", from);
    return false;
}

bool ft_remove(FtConn *c, const char *path, bool is_dir, char **err)
{
    struct sftp_request *req = is_dir ? fxp_rmdir_send(path) : fxp_remove_send(path);
    struct sftp_packet *pktin;
    bool ok;

    cur = c;
    pktin = wait_reply(c, req);
    ok = pktin && (is_dir ? fxp_rmdir_recv(pktin, req) : fxp_remove_recv(pktin, req));
    if (!ok)
        *err = failure(c, is_dir ? "unable to remove the directory" : "unable to delete", path);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* transfers                                                                */

/* True if the person (or the system) has had enough: Cancel, a closed window, Quit. */
static bool stop_wanted(FtConn *c, const FtProgress *pr, uint64_t done, uint64_t total)
{
    if (roapp_yield())
        c->aborting = true;
    if (pr && pr->update && !pr->update(pr->ctx, done, total))
        return true;
    return c->aborting || c->dead;
}

bool ft_download(FtConn *c, const char *remote, const char *local, int local_type, const FtProgress *pr,
                 char **err)
{
    struct sftp_request *req;
    struct sftp_packet *pktin;
    struct fxp_handle *fh;
    struct fxp_xfer *xfer;
    struct fxp_attrs attrs;
    WFile *file;
    char *lp;
    uint64_t done = 0, total;
    bool ok = true, cancelled = false;

    cur = c;
    memset(&attrs, 0, sizeof attrs);
    req = fxp_stat_send(remote);
    pktin = wait_reply(c, req);
    if (!pktin) {
        *err = failure(c, "no answer", remote);
        return false;
    }
    if (!fxp_stat_recv(pktin, req, &attrs))
        attrs.flags = 0;
    total = (attrs.flags & SSH_FILEXFER_ATTR_SIZE) ? attrs.size : 0;

    req = fxp_open_send(remote, SSH_FXF_READ, NULL);
    pktin = wait_reply(c, req);
    fh = pktin ? fxp_open_recv(pktin, req) : NULL;
    if (!fh) {
        *err = failure(c, "unable to open it", remote);
        return false;
    }

    lp = local_type >= 0 ? dupprintf("%s,%03x", local, local_type) : dupstr(local);
    file = open_new_file(lp, (attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) ? (long)attrs.permissions : -1L);
    sfree(lp);
    if (!file) {
        *err = dupprintf("%s: unable to create the file here", local);
        close_handle(c, fh);
        return false;
    }

    xfer = xfer_download_init(fh, 0);
    while (!xfer_done(xfer)) {
        void *vbuf;
        int retd, len;

        xfer_download_queue(xfer);
        pktin = sftp_recv();
        if (!pktin) {
            if (ok)
                *err = failure(c, "the connection failed", remote);
            ok = false;
            break;
        }
        retd = xfer_download_gotpkt(xfer, pktin);
        if (retd <= 0) {
            if (ok)
                *err = dupprintf("%s: error while reading: %s", remote, fxp_error());
            ok = false;
            if (retd == INT_MIN)       /* the packet was not even freed */
                sfree(pktin);
        }
        while (xfer_download_data(xfer, &vbuf, &len)) {
            unsigned char *buf = (unsigned char *)vbuf;
            int wpos = 0;

            while (wpos < len) {
                int w = write_to_file(file, buf + wpos, len - wpos);
                if (w <= 0)
                    break;
                wpos += w;
            }
            if (wpos < len) {
                if (ok)
                    *err = dupprintf("%s: unable to write to the file (is the disc full?)", local);
                ok = false;
                xfer_set_error(xfer);
            }
            done += (uint64_t)len;
            sfree(vbuf);
        }
        if (ok && stop_wanted(c, pr, done, total)) {
            cancelled = true;
            ok = false;
            xfer_set_error(xfer);
            *err = dupstr(c->dead || c->aborting ? dead_reason(c) : "Cancelled.");
        }
    }
    xfer_cleanup(xfer);

    if (ok && (attrs.flags & SSH_FILEXFER_ATTR_ACMODTIME))
        set_file_times(file, attrs.mtime, attrs.atime);
    close_wfile(file);
    if (!c->dead)
        close_handle(c, fh);
    if (!ok) {
        char *os = dupstr(local);
        char *p;
        for (p = os; *p; p++)
            if (*p == ' ') *p = (char)0xA0;
        remove(os);                    /* no half-copied file is left behind */
        sfree(os);
    } else if (pr && pr->update) {
        pr->update(pr->ctx, done, total);
    }
    (void)cancelled;
    return ok;
}

bool ft_upload(FtConn *c, const char *local, const char *remote, const FtProgress *pr, char **err)
{
    struct sftp_request *req;
    struct sftp_packet *pktin;
    struct fxp_handle *fh;
    struct fxp_xfer *xfer;
    struct fxp_attrs attrs;
    RFile *file;
    uint64_t size = 0, done = 0;
    unsigned long mtime = 0, atime = 0;
    long perms = -1;
    bool ok = true, eof = false;

    cur = c;
    file = open_existing_file(local, &size, &mtime, &atime, &perms);
    if (!file) {
        *err = dupprintf("%s: unable to read the file", local);
        return false;
    }
    memset(&attrs, 0, sizeof attrs);
    PUT_PERMISSIONS(attrs, perms);
    req = fxp_open_send(remote, SSH_FXF_WRITE | SSH_FXF_CREAT | SSH_FXF_TRUNC, &attrs);
    pktin = wait_reply(c, req);
    fh = pktin ? fxp_open_recv(pktin, req) : NULL;
    if (!fh) {
        *err = failure(c, "unable to create it", remote);
        close_rfile(file);
        return false;
    }

    xfer = xfer_upload_init(fh, 0);
    while ((ok && !eof) || !xfer_done(xfer)) {
        char buffer[16384];
        int len;

        while (xfer_upload_ready(xfer) && ok && !eof) {
            len = read_from_file(file, buffer, sizeof buffer);
            if (len < 0) {
                *err = dupprintf("%s: error while reading the file", local);
                ok = false;
            } else if (len == 0) {
                eof = true;
            } else {
                xfer_upload_data(xfer, buffer, len);
                done += (uint64_t)len;
            }
        }
        if (toplevel_callback_pending() && ok && !eof) {
            run_toplevel_callbacks();
            continue;
        }
        if (!xfer_done(xfer)) {
            int ret;
            pktin = sftp_recv();
            if (!pktin) {
                if (ok)
                    *err = failure(c, "the connection failed", remote);
                ok = false;
                break;
            }
            ret = xfer_upload_gotpkt(xfer, pktin);
            if (ret <= 0) {
                if (ret == INT_MIN)
                    sfree(pktin);
                if (ok)
                    *err = dupprintf("%s: error while writing: %s", remote, fxp_error());
                ok = false;
            }
        }
        if (ok && stop_wanted(c, pr, done, size)) {
            ok = false;
            *err = dupstr(c->dead || c->aborting ? dead_reason(c) : "Cancelled.");
        }
    }
    xfer_cleanup(xfer);

    if (!c->dead) {
        req = fxp_close_send(fh);
        pktin = wait_reply(c, req);
        if (!pktin || !fxp_close_recv(pktin, req)) {
            if (ok)
                *err = failure(c, "error while closing", remote);
            ok = false;
        }
    }
    close_rfile(file);

    if (ok && mtime > 0 && !c->dead) {                 /* the file keeps its date */
        struct fxp_attrs t;
        memset(&t, 0, sizeof t);
        t.flags = SSH_FILEXFER_ATTR_ACMODTIME;
        t.atime = mtime;
        t.mtime = mtime;
        req = fxp_setstat_send(remote, t);
        pktin = wait_reply(c, req);
        if (pktin)
            fxp_setstat_recv(pktin, req);              /* (it does not matter if the server will not) */
    }
    if (ok && pr && pr->update)
        pr->update(pr->ctx, done, size);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* connecting and disconnecting                                             */

static void free_conn(FtConn *c)
{
    if (c->logctx) log_free(c->logctx);
    if (c->conf) conf_free(c->conf);
    bufchain_clear(&c->received);
    sfree(c->fatal);
    sfree(c->home);
    sfree(c->host);
    sfree(c);
}

static FtConn *connect_inner(Conf *conf_in, FtStatusFn status, void *ctx, char **err);

FtConn *ft_connect(Conf *conf_in, FtStatusFn status, void *ctx, char **err)
{
    FtConn *c = connect_inner(conf_in, status, ctx, err);
    connecting = NULL;
    return c;
}

static FtConn *connect_inner(Conf *conf_in, FtStatusFn status, void *ctx, char **err)
{
    FtConn *c = snew(FtConn);
    const struct BackendVtable *vt;
    char *error, *realhost = NULL, *key;
    struct sftp_packet *pktin;
    struct sftp_request *req;

    memset(c, 0, sizeof *c);
    connecting = c;
    c->seat.vt = &ft_seat_vt;
    c->logpolicy.vt = &ft_logpolicy_vt;
    bufchain_init(&c->received);
    c->cmdline_state = cmdline_get_passwd_input_state_new;
    c->status = status;
    c->status_ctx = ctx;
    c->conf = conf_copy(conf_in);
    c->host = dupprintf("%s", conf_get_str(c->conf, CONF_host));

    /* Everything a terminal session has that SFTP does not want: as psftp does. */
    if (!backend_vt_from_proto(conf_get_int(c->conf, CONF_protocol))) {
        conf_set_int(c->conf, CONF_protocol, PROT_SSH);
        conf_set_int(c->conf, CONF_port, 22);
    }
    if ((conf_get_int(c->conf, CONF_sshprot) & ~1) != 2)
        conf_set_int(c->conf, CONF_sshprot, 2);
    conf_set_bool(c->conf, CONF_x11_forward, false);
    conf_set_bool(c->conf, CONF_agentfwd, false);
    conf_set_bool(c->conf, CONF_ssh_simple, true);
    while ((key = conf_get_str_nthstrkey(c->conf, CONF_portfwd, 0)) != NULL)
        conf_del_str_str(c->conf, CONF_portfwd, key);
    conf_set_str(c->conf, CONF_remote_cmd, "sftp");
    conf_set_bool(c->conf, CONF_ssh_subsys, true);
    conf_set_bool(c->conf, CONF_nopty, true);
    conf_set_str(c->conf, CONF_remote_cmd2,
                 "test -x /usr/lib/sftp-server && exec /usr/lib/sftp-server\n"
                 "test -x /usr/local/lib/sftp-server && exec /usr/local/lib/sftp-server\n"
                 "exec sftp-server");
    conf_set_bool(c->conf, CONF_ssh_subsys2, false);
    conf_set_int(c->conf, CONF_logtype, LGTYP_NONE);

    c->logctx = log_init(&c->logpolicy, c->conf);
    vt = backend_vt_from_proto(conf_get_int(c->conf, CONF_protocol));
    cur = c;
    error = backend_init(vt, &c->seat, &c->backend, c->logctx, c->conf,
                         conf_get_str(c->conf, CONF_host), conf_get_int(c->conf, CONF_port),
                         &realhost, 0, conf_get_bool(c->conf, CONF_tcp_keepalives));
    if (error) {
        *err = dupprintf("Unable to connect to %s: %s", conf_get_str(c->conf, CONF_host), error);
        sfree(error);
        c->backend = NULL;
        connecting = NULL;
        free_conn(c);
        return NULL;
    }
    sfree(realhost);

    while (!backend_sendok(c->backend)) {
        if (c->dead || backend_exitcode(c->backend) >= 0 || !ft_wait(c)) {
            *err = c->aborting && !c->dead ? dupstr("Cancelled.")
                                           : dupprintf("Unable to connect to %s:\n%s", c->host, dead_reason(c));
            connecting = NULL;
            ft_close(c);
            return NULL;
        }
    }
    if (!fxp_init()) {
        *err = dupprintf("Unable to start SFTP: %s", fxp_error());
        connecting = NULL;
        ft_close(c);
        return NULL;
    }
    req = fxp_realpath_send(".");
    pktin = wait_reply(c, req);
    c->home = pktin ? fxp_realpath_recv(pktin, req) : NULL;
    if (!c->home && (c->dead || c->aborting)) {
        *err = dupstr(dead_reason(c));
        connecting = NULL;
        ft_close(c);
        return NULL;
    }
    if (!c->home)
        c->home = dupstr("/");
    return c;
}

void ft_close(FtConn *c)
{
    if (!c)
        return;
    ro_msgbox_drop_owner(c);
    if (c->backend) {
        cur = c;
        if (!c->dead && backend_exitcode(c->backend) < 0) {
            /* say goodbye and give the server a moment to hang up */
            const uint32_t until = ro_time() + 100;
            backend_special(c->backend, SS_EOF, 0);
            c->sent_eof = true;
            c->aborting = false;
            while (!c->dead && backend_exitcode(c->backend) < 0 && (int32_t)(until - ro_time()) > 0)
                if (!roapp_wait())
                    break;
        }
        backend_free(c->backend);
        c->backend = NULL;
        sftp_cleanup_request();
    }
    free_conn(c);
    if (cur == c)
        cur = NULL;
}
