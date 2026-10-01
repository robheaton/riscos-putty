/*
 * dnstest.c - how do the Resolver's two lookup SWIs behave?
 *   dnstest -cache N     Resolver_CacheControl N (1 flush everything, 3 reread Inet$Resolvers), then exit
 *   dnstest name ...     for each name: Resolver_GetHostByName (blocking), then
 *                        Resolver_GetHost (multitasking) called repeatedly until it
 *                        gives an answer other than "in progress".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "swis.h"

#define Resolver_GetHostByName 0x46000
#define Resolver_GetHost       0x46001

typedef struct { char *name; char **aliases; int type; int length; unsigned char **addrs; } hostent_ro;

static int now(void)
{
    int cs = 0;
    _swix(OS_ReadMonotonicTime, _OUT(0), &cs);
    return cs;
}

static void show(const char *what, _kernel_oserror *e, int r0, hostent_ro *he, int t0)
{
    printf("  %-14s +%4d cs  ", what, now() - t0);
    if (e) { printf("error %d \"%s\"\n", e->errnum, e->errmess); return; }
    printf("R0=%d R1=%p", r0, (void *)he);
    if (r0 == 0 && he && he->addrs && he->addrs[0])
        printf("  -> %d.%d.%d.%d", he->addrs[0][0], he->addrs[0][1], he->addrs[0][2], he->addrs[0][3]);
    printf("\n");
}

int main(int argc, char **argv)
{
    int a, flags = 0, blocking = 1;
    if (argc > 2 && !strcmp(argv[1], "-cache")) {
        _kernel_oserror *e = _swix(0x46003, _IN(0), atoi(argv[2]));
        printf("Resolver_CacheControl %d: %s\n", atoi(argv[2]), e ? e->errmess : "ok");
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "-r0")) { flags = atoi(argv[2]); argc -= 2; argv += 2; }
    if (argc > 1 && !strcmp(argv[1], "-n")) { blocking = 0; argc--; argv++; }
    printf("GetHost with R0 = %d\n", flags);
    for (a = 1; a < argc; a++) {
        const char *name = argv[a];
        int r0 = 0, i, t0;
        hostent_ro *he = NULL;
        _kernel_oserror *e;

        printf("%s\n", name);
        if (blocking) {
            t0 = now();
            e = _swix(Resolver_GetHostByName, _IN(1) | _OUTR(0, 1), name, &r0, &he);
            show("GetHostByName", e, r0, he, t0);
        }

        t0 = now();
        for (i = 0; i < 40; i++) {
            r0 = 0; he = NULL;
            e = _swix(Resolver_GetHost, _IN(0) | _IN(1) | _OUTR(0, 1), flags < 0 ? (int)name : flags,
                      flags == -2 ? 0 : (int)name, &r0, &he);
            if (i < 3 || e || r0 != 36) show("GetHost", e, r0, he, t0);
            if (e || r0 != 36) break;
            {   int until = now() + 25;
                while (now() < until) ;
            }
        }
    }
    return 0;
}
