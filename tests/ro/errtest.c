/*
 * errtest.c - what do errno and strerror() say after the C library's file calls fail?
 *
 * PuTTY's file code reports a failure with strerror(errno).  The Shared C Library knows
 * few error numbers, and for a file that cannot be opened it may not set errno at all, so
 * the message could be "Unknown Error" where the user needs "File not found".  This shows
 * what is available (errno, strerror, and the operating system's own last error) for the
 * failures that matter: a missing file, a directory, an unknown disc, a locked file.
 *
 *     Run <path>.errtest
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "kernel.h"
#include "swis.h"

static void show(const char *what, int failed)
{
    _kernel_oserror *e = _kernel_last_oserror();
    printf("%-34s %s  errno %d \"%s\"; OS error %s\n", what, failed ? "failed" : "worked", errno,
           strerror(errno), e ? e->errmess : "(none)");
}

static void try_open(const char *what, const char *path, const char *mode)
{
    FILE *fp;
    errno = 0;
    _kernel_last_oserror();                         /* reading it clears it */
    fp = fopen(path, mode);
    show(what, fp == NULL);
    if (fp) fclose(fp);
}

int main(void)
{
    int r;

    try_open("read a missing file", "NoSuchFileHere", "r");
    try_open("read a directory", "HostFS::HostFS.$", "r");
    try_open("read on an unknown disc", "NoSuchFS::Disc.$.x", "r");
    try_open("write in a missing directory", "NoSuchDir.file", "w");
    try_open("write on an unknown disc", "NoSuchFS::Disc.$.x", "w");
    {
        FILE *fp = fopen("errLocked", "w");
        if (fp) { fputs("x", fp); fclose(fp); }
        _swix(OS_File, _INR(0, 3), 4, "errLocked", 0, 0);     /* attributes: none, then ... */
        _swix(OS_CLI, _IN(0), "Lock errLocked");
        try_open("write to a locked file", "errLocked", "w");
        _swix(OS_CLI, _IN(0), "Unlock errLocked");
        remove("errLocked");
    }
    errno = 0;
    r = rename("NoSuchFileHere", "AnotherName");
    show("rename a missing file", r != 0);
    errno = 0;
    r = remove("NoSuchFileHere");
    show("remove a missing file", r != 0);
    printf("strerror: 0 \"%s\", 1 \"%s\", 2 \"%s\", 5 \"%s\", 6 \"%s\", 13 \"%s\", 22 \"%s\"\n", strerror(0),
           strerror(1), strerror(2), strerror(5), strerror(6), strerror(13), strerror(22));
#ifdef EOPSYS
    printf("EOPSYS is %d\n", EOPSYS);
#endif
#ifdef ENOENT
    printf("ENOENT is %d\n", ENOENT);
#endif
    return 0;
}
