/*
 * ftype.c - what filetype does the C library give a new file, and how can we choose one?
 *
 * puttygen writes key files, and a key file should be Text (&FFF) and not whatever the C
 * library happens to pick.  This checks what fopen() does by itself, whether the type can
 * be set while the file is still open (so that f_open() can do it in one place), whether
 * it survives writing and closing, what rename() does when the target exists, and what
 * OS_FSControl 37 makes of the sort of names a user might type.  Run it with the current
 * directory somewhere writable:
 *
 *     Run <path>.ftype
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "kernel.h"
#include "swis.h"

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

/* Filetype of a file via OS_File 5: &xxx, -1 error, -2 no such object, -3 not typed. */
static int filetype(const char *path)
{
    int obj = 0, load = 0;
    if (_swix(OS_File, _INR(0, 1) | _OUT(0) | _OUT(2), 5, path, &obj, &load))
        return -1;
    if (obj == 0)
        return -2;
    if (((unsigned)load & 0xFFF00000u) != 0xFFF00000u)
        return -3;
    return (load >> 8) & 0xFFF;
}

static const char *describe(int t)
{
    static char buf[32];
    if (t == -1) return "error";
    if (t == -2) return "does not exist";
    if (t == -3) return "untyped";
    sprintf(buf, "&%03X", t);
    return buf;
}

static void fill(FILE *fp)
{
    fputs("line one\nline two\n", fp);
}

static void canonical(const char *name)
{
    char buf[512];
    int spare = 0;
    _kernel_oserror *e = _swix(OS_FSControl, _INR(0, 5) | _OUT(5), 37, name, buf, 0, 0,
                               (int)sizeof buf, &spare);
    if (e)
        printf("  canonicalise \"%s\": error %s\n", name, e->errmess);
    else
        printf("  canonicalise \"%s\" -> \"%s\" (%d spare)\n", name, buf, spare);
}

int main(void)
{
    const char *a = "ftA", *b = "ftB", *c = "ftC", *d = "ftD", *e2 = "ftE";
    FILE *fp;
    _kernel_oserror *err;
    int t;

    remove(a); remove(b); remove(c); remove(d); remove(e2); remove("ftA2");

    printf("A: fopen \"w\" on a new file\n");
    fp = fopen(a, "w");
    fill(fp);
    fclose(fp);
    t = filetype(a);
    printf("  type %s\n", describe(t));

    printf("B: fopen \"wb\" on a new file\n");
    fp = fopen(b, "wb");
    fill(fp);
    fclose(fp);
    t = filetype(b);
    printf("  type %s\n", describe(t));

    printf("C: set the type to Text while the file is open, then write and close\n");
    fp = fopen(c, "wb");
    err = _swix(OS_File, _INR(0, 2), 18, c, 0xFFF);
    printf("  OS_File 18 on the open file: %s\n", err ? err->errmess : "no error");
    check(!err, "setting the type on an open file is allowed");
    fill(fp);
    fclose(fp);
    t = filetype(c);
    printf("  type after close %s\n", describe(t));
    check(t == 0xFFF, "the type survives the writing and the close");

    printf("D: create an empty Text file first, then fopen \"wb\" over it\n");
    err = _swix(OS_File, _INR(0, 2) | _IN(4) | _IN(5), 11, d, 0xFFF, 0, 0);
    printf("  OS_File 11: %s\n", err ? err->errmess : "no error");
    fp = fopen(d, "wb");
    fill(fp);
    fclose(fp);
    t = filetype(d);
    printf("  type after the overwrite %s\n", describe(t));
    check(t == 0xFFF, "fopen over an existing file keeps its type");

    printf("E: fopen \"ab\" on a typed file\n");
    fp = fopen(c, "ab");
    fputs("line three\n", fp);
    fclose(fp);
    t = filetype(c);
    printf("  type %s\n", describe(t));
    check(t == 0xFFF, "appending keeps the type");

    printf("F: rename\n");
    check(rename(c, "ftA2") == 0, "rename to a new name works");
    t = filetype("ftA2");
    printf("  type after rename %s\n", describe(t));
    check(t == 0xFFF, "the type goes with it");
    errno = 0;
    check(rename("ftA2", a) != 0, "rename onto an existing file fails");
    printf("  (errno %d: %s)\n", errno, strerror(errno));
    check(remove(a) == 0 && rename("ftA2", a) == 0, "remove then rename replaces it");
    check(filetype(a) == 0xFFF, "and the result is Text");
    check(remove("ftNoSuchFile") != 0, "remove of a missing file reports failure");

    printf("G: canonical names\n");
    canonical("ftA");
    canonical("@.ftA");
    canonical("<Wimp$ScrapDir>");
    canonical("Choices:PuTTY.SSHHostKeys");
    canonical("&.ftA");
    canonical("$.ftA");

    remove(a); remove(b); remove(c); remove(d); remove(e2); remove("ftA2");
    printf("%s (%d failures)\n", failures ? "FAILED" : "done", failures);
    return failures ? 1 : 0;
}
