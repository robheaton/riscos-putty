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

static int object_type(const char *path, unsigned *length);

void ro_trace(const char *var, const char *fmt, ...)
{
    static char started[4][32];
    const char *name = getenv(var);
    bool first = true;
    FILE *fp;
    va_list ap;
    int i;

    if (!name || !*name) return;
    for (i = 0; i < 4; i++) {
        if (!strcmp(started[i], var)) { first = false; break; }
        if (!started[i][0]) { snprintf(started[i], sizeof started[i], "%s", var); break; }
    }
    fp = fopen(name, first ? "w" : "a");
    if (!fp) return;
    fprintf(fp, "%u ", (unsigned)ro_time());
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

/*
 * Everything PuTTY writes through f_open - logs, key files, exported keys, the text
 * puttygen prints - is text, but the C library makes a file opened "wb" Data (&FFD).
 * So a file this call creates or empties is typed Text straight away (setting the
 * type of an open file is allowed and survives the writing and the close; see
 * tests/ro/ftype.c); one that is appended to keeps the type it already has.
 */
FILE *f_open(const Filename *filename, char const *mode, bool is_private)
{
    /* RISC OS has no file permissions, so is_private means nothing here. */
    bool existed = object_type(filename->path, NULL) == 1;
    FILE *fp = fopen(filename->path, mode);

    if (fp && (strchr(mode, 'w') || (strchr(mode, 'a') && !existed)))
        ro_set_filetype(filename->path, FILETYPE_TEXT);
    return fp;
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

char *ro_temp_sibling(const char *path)
{
    char full[1024];
    const char *name = path, *dot, *colon;
    int spare = -1, i;
    size_t dirlen = 0;
    uint32_t start = ro_time();

    /* Spell the name out in full first, so that "@.key" or "<Keys$File>" end up in the
     * directory they really name. */
    if (!_swix(OS_FSControl, _INR(0, 5) | _OUT(5), 37, path, full, 0, 0,
               (int)sizeof full, &spare) && spare >= 0)
        name = full;

    /* The directory part is everything up to the last '.' or ':'. */
    dot = strrchr(name, '.');
    colon = strrchr(name, ':');
    if (dot && colon)
        dirlen = (size_t)((dot > colon ? dot : colon) - name) + 1;
    else if (dot || colon)
        dirlen = (size_t)((dot ? dot : colon) - name) + 1;

    for (i = 0; i < 4096; i++) {
        char *tmp = dupprintf("%.*sPgTmp%03X", (int)dirlen, name,
                              (unsigned)(start + i) & 0xFFF);
        if (object_type(tmp, NULL) == 0)
            return tmp;
        sfree(tmp);
    }
    /* Every name is taken: let whoever uses this one report the problem. */
    return dupprintf("%.*sPgTmp", (int)dirlen, name);
}

int ro_test_key(void)
{
    static bool loaded, in_use;
    static unsigned char *data;
    static size_t len, pos;

    if (!loaded) {
        const char *name = getenv("PuTTY$KeyFile");

        loaded = true;
        if (name && *name) {
            FILE *fp = fopen(name, "rb");
            size_t cap = 4096, n;

            in_use = true;
            data = snewn(cap, unsigned char);
            while (fp && (n = fread(data + len, 1, cap - len, fp)) > 0) {
                len += n;
                if (len == cap) {
                    cap *= 2;
                    data = sresize(data, cap, unsigned char);
                }
            }
            if (fp)
                fclose(fp);
        }
    }
    if (!in_use)
        return -2;
    return pos < len ? data[pos++] : -1;
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

/*
 * ----------------------------------------------------------------------
 * Error messages (see platform.h).  tests/ro/errtest.c shows what the C library leaves
 * behind after a failed call: errno -1 (or 0, after rename), with the message in the
 * operating system's last error.
 */
#undef strerror
#undef perror

char *ro_strerror(int errnum)
{
    static char last[256];

    if (errnum == -1 || errnum == EOPSYS) {
        const _kernel_oserror *err = _kernel_last_oserror();   /* reading it clears it */
        if (err) {
            strncpy(last, err->errmess, sizeof last - 1);
            last[sizeof last - 1] = '\0';
        }
        if (last[0])                   /* asked again for the same failure: say it again */
            return last;
    }
    return strerror(errnum);
}

void ro_perror(const char *prefix)
{
    const char *msg;

    if (errno == 0) {                  /* rename() and remove() fail without setting errno */
        const _kernel_oserror *err = _kernel_last_oserror();
        msg = err ? err->errmess : "unknown error";
    } else {
        msg = ro_strerror(errno);
    }
    if (prefix && *prefix)
        fprintf(stderr, "%s: %s\n", prefix, msg);
    else
        fprintf(stderr, "%s\n", msg);
}
