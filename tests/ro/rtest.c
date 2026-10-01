/*
 * rtest.c - dev tool: show exactly what the Resolver SWIs return.
 *   rtest <hostname>
 * Tries Resolver_GetHost with a range of R0 values, polling each for a few
 * seconds, then the blocking Resolver_GetHostByName for comparison.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "swis.h"

#define Resolver_GetHostByName 0x46000
#define Resolver_GetHost       0x46001

typedef struct { char *name; char **aliases; int type; int length; unsigned char **addrs; } hostent_ro;

static void show(const char *what, _kernel_oserror *e, int r0, int r1)
{
    printf("%-26s ", what);
    if (e) printf("ERROR %08x '%s'\n", (unsigned)e->errnum, e->errmess);
    else {
        printf("R0=%d (0x%x) R1=0x%08x", r0, (unsigned)r0, (unsigned)r1);
        if (r1 > 0x8000) {
            hostent_ro *he = (hostent_ro *)(long)r1;
            printf("  name=%s len=%d", he->name ? he->name : "(null)", he->length);
            if (he->addrs && he->addrs[0])
                printf(" addr0=%u.%u.%u.%u", he->addrs[0][0], he->addrs[0][1], he->addrs[0][2], he->addrs[0][3]);
        }
        printf("\n");
    }
}

static void sleep_cs(int cs)
{
    int t0 = 0, t = 0;
    _swi(OS_ReadMonotonicTime, _OUT(0), &t0);
    do { _swi(OS_ReadMonotonicTime, _OUT(0), &t); } while (t - t0 < cs);
}

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "www.iana.org";
    static const int flags[] = { 0, 1, 2, 3, 4, 8, 16, 256, -1 };
    unsigned k;
    for (k = 0; k < sizeof flags / sizeof flags[0]; k++) {
        int i, r0 = -7, r1 = -7;
        _kernel_oserror *e = NULL;
        char tag[64];
        for (i = 0; i < 30; i++) {
            r0 = -7; r1 = -7;
            e = _swix(Resolver_GetHost, _INR(0, 1) | _OUTR(0, 1), flags[k], name, &r0, &r1);
            if (i == 0) { sprintf(tag, "GetHost R0in=%d first", flags[k]); show(tag, e, r0, r1); }
            if (e || (r0 != -1 && r0 != 36)) break;
            sleep_cs(10);
        }
        sprintf(tag, "GetHost R0in=%d after %d", flags[k], i);
        show(tag, e, r0, r1);
    }
    {
        int r0 = -7, r1 = -7;
        _kernel_oserror *e = _swix(Resolver_GetHostByName, _IN(1) | _OUTR(0, 1), name, &r0, &r1);
        show("GetHostByName (blocking)", e, r0, r1);
    }
    return 0;
}
