/*
 * halfclose.c - how does the Internet module treat a half-closed connection?
 *
 * Plink with its input redirected from a file sends its data and then shuts down its
 * sending side; PuTTY (or anything) on the far end must still be able to answer.  This
 * checks that on the loopback, both ends being in this one program.
 *
 * Every socket is non-blocking and every wait is a bounded delay: a blocking Socket_Recv
 * on the Internet module does not let the stack run and hangs the whole machine.
 *
 *   halfclose srv PORT    (run this in a task window) accept one connection on 127.0.0.1:PORT,
 *                         send a banner, read until EOF, then send two replies and wait
 *   halfclose cli PORT    connect, send "hello", shut down writing, then watch what arrives
 *                         (the two together are what plink and PuTTY's tunnel do)
 *   halfclose [mode]
 *     mode 0  client sends "hello" and shuts down writing; server reads to EOF, then
 *             sends two replies; client reads them                     (the usual case)
 *     mode 1  as 0 but the client does not shut down                    (the control)
 *     mode 2  as 0 but the server replies before it has read the EOF
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "swis.h"

typedef struct {
    short family;
    unsigned short port;               /* network byte order */
    unsigned char addr[4];
    unsigned char zero[8];
} sockaddr_in_ro;

static void delay(int cs)
{
    int until = 0, now = 0;
    _swix(OS_ReadMonotonicTime, _OUT(0), &now);
    until = now + cs;
    while (now < until) _swix(OS_ReadMonotonicTime, _OUT(0), &now);
}

static int mk(void)
{
    int s = -1, one = 1;
    _kernel_oserror *e = _swix(Socket_Creat, _INR(0, 2) | _OUT(0), 2, 1, 0, &s);
    if (e) { printf("socket: %s\n", e->errmess); exit(1); }
    _swix(Socket_Ioctl, _INR(0, 2), s, 0x8004667E, &one);       /* FIONBIO */
    return s;
}

static sockaddr_in_ro addr(unsigned a, unsigned b, unsigned c, unsigned d, int port)
{
    sockaddr_in_ro sa;
    memset(&sa, 0, sizeof sa);
    sa.family = 2;
    sa.port = (unsigned short)(((port & 255) << 8) | ((port >> 8) & 255));
    sa.addr[0] = (unsigned char)a; sa.addr[1] = (unsigned char)b;
    sa.addr[2] = (unsigned char)c; sa.addr[3] = (unsigned char)d;
    return sa;
}

static void send_(const char *who, int s, const char *data)
{
    int n = 0;
    _kernel_oserror *e = _swix(Socket_Send, _INR(0, 3) | _OUT(0), s, data, (int)strlen(data), 0, &n);
    printf("  %s send(%d bytes) -> %s n=%d\n", who, (int)strlen(data), e ? e->errmess : "ok", n);
}

static int recv_(const char *who, int s)
{
    char buf[256];
    int n = -99;
    _kernel_oserror *e = _swix(Socket_Recv, _INR(0, 3) | _OUT(0), s, buf, (int)sizeof buf - 1, 0, &n);
    if (e) { printf("  %s recv -> %s\n", who, e->errmess); return -1; }
    if (n > 0) { buf[n] = 0; printf("  %s recv -> %d bytes \"%s\"\n", who, n, buf); }
    else printf("  %s recv -> %d%s\n", who, n, n == 0 ? " (EOF)" : "");
    return n;
}

static void shut(const char *who, int s, int how)
{
    _kernel_oserror *e = _swix(Socket_Shutdown, _INR(0, 1), s, how);
    printf("  %s shutdown(%d) -> %s\n", who, how, e ? e->errmess : "ok");
}

static int stamp(void)
{
    int cs = 0;
    _swix(OS_ReadMonotonicTime, _OUT(0), &cs);
    return cs;
}

/* the server half, as a separate program from the client */
static int server(int port)
{
    int one = 1, L, S = -1, i, got_eof = 0, t0;
    sockaddr_in_ro la = addr(127, 0, 0, 1, port), peer;
    int peerlen = sizeof peer;
    _kernel_oserror *e;

    L = mk();
    _swix(Socket_Setsockopt, _INR(0, 4), L, 0xFFFF, 4, &one, 4);
    e = _swix(Socket_Bind, _INR(0, 2), L, &la, (int)sizeof la);
    if (e) { printf("bind: %s\n", e->errmess); return 1; }
    _swix(Socket_Listen, _INR(0, 1), L, 5);
    printf("srv: listening\n");
    for (i = 0; i < 600 && S < 0; i++) {            /* up to 30 s */
        delay(5);
        e = _swix(Socket_Accept, _INR(0, 2) | _OUT(0), L, &peer, &peerlen, &S);
        if (e) S = -1;
    }
    if (S < 0) { printf("srv: nobody came\n"); return 1; }
    _swix(Socket_Ioctl, _INR(0, 2), S, 0x8004667E, &one);
    t0 = stamp();
    printf("srv: accepted\n");
    send_("srv", S, "echo server ready (raw)\r\n> ");
    for (i = 0; i < 200 && !got_eof; i++) {         /* up to 10 s */
        char buf[64];
        int n = -99;
        delay(5);
        e = _swix(Socket_Recv, _INR(0, 3) | _OUT(0), S, buf, (int)sizeof buf, 0, &n);
        if (e) continue;
        printf("srv: +%d cs recv -> %d\n", stamp() - t0, n);
        if (n == 0) got_eof = 1;
    }
    send_("srv", S, "\r\n");
    delay(10);
    send_("srv", S, "got: hello via -L\r\n> ");
    delay(100);
    _swix(Socket_Close, _IN(0), S);
    _swix(Socket_Close, _IN(0), L);
    return 0;
}

static int client(int port)
{
    int C, i;
    sockaddr_in_ro la = addr(127, 0, 0, 1, port);
    _kernel_oserror *e;
    int t0;

    C = mk();
    e = _swix(Socket_Connect, _INR(0, 2), C, &la, (int)sizeof la);
    if (e) printf("cli: connect: %s\n", e->errmess);
    delay(20);
    t0 = stamp();
    send_("cli", C, "hello via -L\r");
    shut("cli", C, 1);
    for (i = 0; i < 100; i++) {                     /* 5 s */
        char buf[128];
        int n = -99;
        delay(5);
        e = _swix(Socket_Recv, _INR(0, 3) | _OUT(0), C, buf, (int)sizeof buf - 1, 0, &n);
        if (e) continue;
        buf[n > 0 ? n : 0] = 0;
        printf("cli: +%d cs recv -> %d \"%s\"\n", stamp() - t0, n, buf);
        if (n == 0) break;
    }
    _swix(Socket_Close, _IN(0), C);
    return 0;
}

int main(int argc, char **argv)
{
    int mode;
    if (argc > 2 && !strcmp(argv[1], "srv")) return server(atoi(argv[2]));
    if (argc > 2 && !strcmp(argv[1], "cli")) return client(atoi(argv[2]));
    mode = argc > 1 ? atoi(argv[1]) : 0;
    int one = 1, L, C, S = -1, i;
    sockaddr_in_ro la = addr(127, 0, 0, 1, 9100), peer;
    int peerlen = sizeof peer;
    _kernel_oserror *e;

    printf("mode %d\n", mode);
    L = mk();
    _swix(Socket_Setsockopt, _INR(0, 4), L, 0xFFFF, 4, &one, 4);
    e = _swix(Socket_Bind, _INR(0, 2), L, &la, (int)sizeof la);
    if (e) { printf("bind: %s\n", e->errmess); return 1; }
    _swix(Socket_Listen, _INR(0, 1), L, 5);
    C = mk();
    e = _swix(Socket_Connect, _INR(0, 2), C, &la, (int)sizeof la);
    if (e) printf("connect: %s (expected: in progress)\n", e->errmess);
    for (i = 0; i < 20 && S < 0; i++) {
        delay(10);
        e = _swix(Socket_Accept, _INR(0, 2) | _OUT(0), L, &peer, &peerlen, &S);
        if (e) S = -1;
    }
    if (S < 0) { printf("accept: never got the connection\n"); return 1; }
    _swix(Socket_Ioctl, _INR(0, 2), S, 0x8004667E, &one);

    send_("C", C, "hello");
    delay(20);
    if (mode != 1) { shut("C", C, 1); delay(20); }
    if (mode == 2) {
        send_("S", S, "early1");
        delay(20);
        send_("S", S, "early2");
        delay(20);
    }
    recv_("S", S);
    if (mode != 1) recv_("S", S);                   /* the EOF */
    if (mode != 2) {
        send_("S", S, "reply1");
        delay(20);
        send_("S", S, "reply2");
        delay(20);
    }
    recv_("C", C);
    recv_("C", C);
    delay(20);
    recv_("C", C);
    _swix(Socket_Close, _IN(0), S);
    _swix(Socket_Close, _IN(0), C);
    _swix(Socket_Close, _IN(0), L);
    return 0;
}
