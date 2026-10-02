/*
 * sftpprobe.c - the operating system calls the file transfer tools (psftp, pscp) rely on.
 *
 *  - MimeMap_Translate: which filetype goes with a filename extension (reason 3)?
 *  - OS_FSControl 37 (canonicalise): does it tell a filing system or path variable
 *    ("HostFS:", "Choices:") from a host name ("pi.local:"), as pscp needs to when deciding
 *    whether an argument is a local file or host:path?
 *  - OS_GBPB 12: the layout of the entries that list a directory with load/exec/type.
 *  - the date stamp in the load and execution addresses, against the C library's clock.
 *
 *     Run <path>.sftpprobe [directory]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kernel.h"
#include "swis.h"

#ifndef MimeMap_Translate
#define MimeMap_Translate 0x50B00
#endif

/* Reason codes: R0 = the kind of thing given, R2 = the kind wanted; 0 MIME type (string),
 * 1 RISC OS filetype (number), 2 filename extension (string), 3 filetype name? (tried below) */
static void mime(const char *ext)
{
    char copy[32];
    int type = -1;
    _kernel_oserror *e;
    strncpy(copy, ext, sizeof copy - 1);
    copy[sizeof copy - 1] = 0;
    e = _swix(MimeMap_Translate, _INR(0, 2) | _OUT(3), 2, copy, 1, &type);
    if (e)
        printf("  extension %-6s -> error %d \"%s\"\n", ext, e->errnum, e->errmess);
    else
        printf("  extension %-6s -> &%03X\n", ext, type & 0xFFF);
}

static void mime_ext(int type)
{
    int out = -1;
    _kernel_oserror *e = _swix(MimeMap_Translate, _INR(0, 2) | _OUT(3), 1, type, 2, &out);
    if (e)
        printf("  filetype &%03X -> error %d \"%s\"\n", type, e->errnum, e->errmess);
    else
        printf("  filetype &%03X -> extension \"%s\"\n", type, (const char *)out);
}

static void canon(const char *name)
{
    char buf[512];
    int spare = 0;
    _kernel_oserror *e = _swix(OS_FSControl, _INR(0, 5) | _OUT(5), 37, name, buf, 0, 0,
                               (int)sizeof buf, &spare);
    if (e)
        printf("  %-28s -> error %08X \"%s\"\n", name, e->errnum, e->errmess);
    else
        printf("  %-28s -> \"%s\"\n", name, buf);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "HostFS::HostFS.$.PuttyDev";
    static const char *const exts[] = { "txt", "html", "htm", "png", "jpg", "gif", "zip", "pdf",
                                        "gz", "tar", "c", "h", "py", "sh", "xyz", "TXT", "csv",
                                        "md", "json", "xml", NULL };
    static const char *const names[] = { "ADFS:", "HostFS:", "Choices:", "Resources:", "System:",
                                         "RAM:", "Net:", "pi.local:", "rob@pi.local:",
                                         "10.0.0.44:", "localhost:", "example.com:", "ADFS::4",
                                         "HostFS::HostFS.$.x", "<Wimp$ScrapDir>", "@", "$.x", NULL };
    int i, offset = 0, count;
    char buf[1024];
    time_t now;
    int load = 0, exec = 0, obj = 0;
    _kernel_oserror *e;

    printf("MimeMap_Translate (extension -> filetype, and back):\n");
    for (i = 0; exts[i]; i++)
        mime(exts[i]);
    mime_ext(0xFFF);
    mime_ext(0xFAF);
    mime_ext(0xB60);
    mime_ext(0xFFD);

    printf("OS_FSControl 37 on things that might precede a colon:\n");
    for (i = 0; names[i]; i++)
        canon(names[i]);

    {
        static const int reasons[] = { 9, 10, 11, 12 };
        int r, k;
        for (r = 0; r < 4; r++) {
            printf("OS_GBPB %d on %s:\n", reasons[r], dir);
            offset = 0;
            for (i = 0; i < 3 && offset >= 0; i++) {
                memset(buf, 0, 64);
                e = _swix(OS_GBPB, _INR(0, 6) | _OUT(3) | _OUT(4), reasons[r], dir, buf, 1, offset,
                          (int)sizeof buf, 0, &count, &offset);
                if (e) {
                    printf("  error %s\n", e->errmess);
                    break;
                }
                printf("  read %d, next offset %d:", count, offset);
                for (k = 0; k < 64; k++)
                    printf("%s%02X", k % 4 == 0 ? " " : "", (unsigned char)buf[k]);
                printf("\n");
            }
        }
    }

    now = time(NULL);
    printf("time(NULL) = %lu\n", (unsigned long)now);
    e = _swix(OS_File, _INR(0, 1) | _OUT(0) | _OUTR(2, 3), 5, "HostFS::HostFS.$.PuttyDev.errtest",
              &obj, &load, &exec);
    if (!e && obj == 1) {
        /* the file is typed: load = FFFttt dd, exec = low 32 bits of centiseconds since 1900 */
        unsigned long long cs = ((unsigned long long)(load & 0xFF) << 32) | (unsigned)exec;
        unsigned long long secs = cs / 100;
        printf("errtest load %08X exec %08X: stamp %llu cs -> unix %lld\n", load, exec, cs,
               (long long)secs - 2208988800LL);
    } else {
        printf("OS_File 5: %s (object %d)\n", e ? e->errmess : "no error", obj);
    }
    return 0;
}
