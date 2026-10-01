/*
 * util.c: the small platform hooks PuTTY expects every port to supply -
 * Filename, the tick counter, the user name, debug output, directory
 * creation, and so on.  (Upstream's unix/utils/ directory has the equivalents.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "putty.h"
#include "ro.h"

/*
 * ----------------------------------------------------------------------
 * Filename.  A Filename is just a RISC OS pathname string.
 */

Filename *filename_from_str(const char *str)
{
    Filename *fn = snew(Filename);
    fn->path = dupstr(str);
    return fn;
}

Filename *filename_copy(const Filename *fn)
{
    return filename_from_str(fn->path);
}

const char *filename_to_str(const Filename *fn)
{
    return fn->path;
}

bool filename_equal(const Filename *f1, const Filename *f2)
{
    return !strcmp(f1->path, f2->path);
}

bool filename_is_null(const Filename *fn)
{
    return !fn->path[0];
}

void filename_free(Filename *fn)
{
    sfree(fn->path);
    sfree(fn);
}

void filename_serialise(BinarySink *bs, const Filename *f)
{
    put_asciz(bs, f->path);
}

Filename *filename_deserialise(BinarySource *src)
{
    return filename_from_str(get_asciz(src));
}

/*
 * Used when text such as a host name is substituted into a log file name:
 * anything that means something in a RISC OS pathname becomes an underscore.
 */
char filename_char_sanitise(char c)
{
    if ((unsigned char)c < 32 || strchr(".:$&@^%\\*#\"|/", c))
        return '_';
    return c;
}

FILE *f_open(const Filename *filename, char const *mode, bool is_private)
{
    /* RISC OS has no file permissions, so is_private means nothing here. */
    return fopen(filename->path, mode);
}

/*
 * ----------------------------------------------------------------------
 * Time.
 */

unsigned long getticks(void)
{
    /*
     * OS_ReadMonotonicTime counts centiseconds in 32 bits; PuTTY wants
     * milliseconds in 32 bits.  Scaling the raw counter would make the result
     * jump when the counter wraps (after 497 days), so extend it to 64 bits
     * first and truncate after scaling.
     */
    static uint32_t last_cs, wraps;
    uint32_t cs = ro_time();
    uint64_t total;

    if (cs < last_cs)
        wraps++;
    last_cs = cs;
    total = ((uint64_t)wraps << 32) | cs;
    return (unsigned long)(uint32_t)(total * 10);
}

/*
 * ----------------------------------------------------------------------
 * Miscellany.
 */

char *get_username(void)
{
    /* RISC OS has no notion of a login name; let the user set one. */
    static const char *const vars[] = {
        "PuTTY$User", "Username", "USER", "LOGNAME", NULL
    };
    int i;
    for (i = 0; vars[i]; i++) {
        const char *v = getenv(vars[i]);
        if (v && *v)
            return dupstr(v);
    }
    return NULL;
}

void dputs(const char *buf)
{
    fputs(buf, stderr);
    fflush(stderr);
}

/* Object type of a pathname via OS_File 17: 0 none, 1 file, 2 dir, 3 image. */
static int object_type(const char *path, unsigned *length)
{
    int type = 0, load = 0, exec = 0, len = 0;
    if (_swix(OS_File, _INR(0, 1) | _OUT(0) | _OUTR(2, 4), 17, path,
              &type, &load, &exec, &len))
        return 0;
    if (length)
        *length = (unsigned)len;
    return type;
}

bool open_for_write_would_lose_data(const Filename *fn)
{
    unsigned len = 0;
    /* Only a file with something in it is destroyed by opening it for write. */
    return object_type(fn->path, &len) == 1 && len > 0;
}

/*
 * Create a directory and any missing parents.  Returns NULL on success or a
 * dynamically allocated error message.
 */
char *make_dir_path(const char *path, mode_t mode)
{
    char *tmp = dupstr(path);
    char *p;
    char *err = NULL;

    for (p = tmp; ; p++) {
        if (*p == '.' || *p == '\0') {
            char save = *p;
            *p = '\0';
            if (p > tmp) {
                int type = object_type(tmp, NULL);
                if (type == 1) {
                    err = dupprintf("'%s' is a file, not a directory", tmp);
                    break;
                } else if (type == 0) {
                    _kernel_oserror *e =
                        _swix(OS_File, _INR(0, 4), 8, tmp, 0, 0, 0);
                    if (e) {
                        err = dupprintf("unable to create directory '%s': %s",
                                        tmp, e->errmess);
                        break;
                    }
                }
            }
            *p = save;
            if (!save)
                break;
        }
    }

    sfree(tmp);
    return err;
}

static void spr_errno_errfn(SeatPromptResult spr, BinarySink *bs)
{
    put_fmt(bs, "%s: %s", spr.errdata_lit, strerror(spr.errdata_u));
}

SeatPromptResult make_spr_sw_abort_errno(const char *prefix, int errno_value)
{
    SeatPromptResult spr;
    spr.kind = SPRK_SW_ABORT;
    spr.errfn = spr_errno_errfn;
    spr.errdata_lit = prefix;
    spr.errdata_u = errno_value;
    return spr;
}
