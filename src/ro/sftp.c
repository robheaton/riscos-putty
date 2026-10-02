/*
 * sftp.c: the RISC OS parts of PSFTP and PSCP - the local file layer, the wait for
 * network activity or a key, and the command line reader.
 *
 * Based on upstream's unix/sftp.c, which has the same job; psftp.c and pscp.c are used
 * as upstream has them apart from the few lines patches/psftp.patch and patches/pscp.patch
 * change (see rosftp.h and docs/DESIGN.md for why).
 *
 * File names.  Local paths are native RISC OS pathnames, exactly as the user typed them
 * (a path dragged to a task window works).  The names of the files inside a directory
 * and the names a server sends are Unix leaf names, and the conversion between the two
 * is done here:
 *
 *   notes.txt        <->   notes/txt        '.' and '/' change places
 *   a:b$c            ->    a_b_c            characters a RISC OS name cannot hold become '_'
 *   my file          <->   my<hard space>file  a space ends a name in a RISC OS call, so names
 *                                            carry the hard space (&A0) instead
 *   pic,aff          <->   pic (type &AFF)  a ",xxx" suffix is the file's filetype, as on
 *                                            NFS and Samba shares and with the FTP clients
 *
 * A downloaded file gets the filetype in the suffix if there is one; else one from its
 * extension if it is a well-known one (txt, html, png, ...); else Text or Data according
 * to whether its first bytes look like text.  An uploaded file's name has ",xxx" added if
 * its type is not Text or Data.  Dates travel both ways.  Set PuTTY$SftpNoTypes to leave
 * ",xxx" off the names of files you upload.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>

#include "putty.h"
#include "ssh.h"
#include "psftp.h"
#include "ro.h"
#include "ronet.h"
#include "rosftp.h"

#define UNIX_TO_RISCOS_SECONDS 2208988800ULL    /* from 1900 to 1970 */

/* ----------------------------------------------------------------------
 * Catalogue information (OS_File 5)
 */

struct cat {
    int obj;                           /* 0 not found, 1 file, 2 directory, 3 image file */
    unsigned load, exec, length, attr;
};

static bool read_cat(const char *path, struct cat *c)
{
    int obj = 0, load = 0, exec = 0, len = 0, attr = 0;

    memset(c, 0, sizeof *c);
    if (_swix(OS_File, _INR(0, 1) | _OUT(0) | _OUTR(2, 5), 5, path,
              &obj, &load, &exec, &len, &attr))
        return false;
    c->obj = obj;
    c->load = (unsigned)load;
    c->exec = (unsigned)exec;
    c->length = (unsigned)len;
    c->attr = (unsigned)attr;
    return obj != 0;
}

/* A typed file's load address is 0xFFFttt dd; otherwise load and exec are real addresses. */
static bool cat_is_stamped(const struct cat *c)
{
    return (c->load & 0xFFF00000u) == 0xFFF00000u;
}

static int cat_filetype(const struct cat *c)
{
    return cat_is_stamped(c) ? (int)((c->load >> 8) & 0xFFF) : -1;
}

/* Seconds since 1970, from the five-byte centisecond stamp since 1900; 0 if there is none. */
static unsigned long cat_unix_time(const struct cat *c)
{
    unsigned long long cs, secs;

    if (!cat_is_stamped(c))
        return 0;
    cs = ((unsigned long long)(c->load & 0xFF) << 32) | c->exec;
    secs = cs / 100;
    return secs > UNIX_TO_RISCOS_SECONDS ? (unsigned long)(secs - UNIX_TO_RISCOS_SECONDS) : 0;
}

static bool exists(const char *path)
{
    struct cat c;
    return read_cat(path, &c);
}

/*
 * A space ends a name in a RISC OS call, so a name with a space in it uses the hard space
 * (&A0) there.  People type ordinary spaces (and servers send them); a name is converted
 * once, as it comes in, and all the calls below use the converted form.
 */
#define HARD_SPACE ((char)0xA0)

static char *to_os(const char *name)
{
    char *r = dupstr(name), *p;

    for (p = r; *p; p++)
        if (*p == ' ')
            *p = HARD_SPACE;
    return r;
}

/* ----------------------------------------------------------------------
 * Names
 */

/* Does NAME end in ",xxx" with three hex digits?  Then BASELEN is the length without it. */
static bool type_suffix(const char *name, size_t *baselen, int *type)
{
    size_t n = strlen(name);

    if (n > 4 && name[n - 4] == ',' && isxdigit((unsigned char)name[n - 3]) &&
        isxdigit((unsigned char)name[n - 2]) && isxdigit((unsigned char)name[n - 1])) {
        if (baselen)
            *baselen = n - 4;
        if (type)
            *type = (int)strtol(name + n - 3, NULL, 16);
        return true;
    }
    return false;
}

/*
 * The name to use for reading a local file: as given if that exists, else without a
 * ",xxx" suffix if that exists (the suffix is how a file's type travels in a name, and
 * the file here has it in its catalogue entry instead).
 */
static char *resolve_read(const char *given)
{
    char *path = to_os(given);
    size_t base;

    if (!exists(path) && type_suffix(path, &base, NULL)) {
        char *shorter = dupprintf("%.*s", (int)base, path);
        if (exists(shorter)) {
            sfree(path);
            return shorter;
        }
        sfree(shorter);
    }
    return path;
}

static bool bad_char(unsigned char c)
{
    return c < 32 || c == 127 || strchr(":$&@^%\\*#\"|", c) != NULL;
}

/* A leaf name from the Unix side as a RISC OS leaf; ",xxx" is left for open_new_file. */
static char *unix_leaf_to_ro(const char *u)
{
    char *r = dupstr(u), *p;

    for (p = r; *p; p++) {
        if (*p == '.')
            *p = '/';
        else if (*p == ' ')
            *p = HARD_SPACE;
        else if (bad_char((unsigned char)*p))
            *p = '_';
    }
    if (!*r) {
        sfree(r);
        r = dupstr("_");
    }
    return r;
}

/* A short-lived string for the callers that cannot free what they are given. */
static char *ring_keep(char *s)
{
    static char *ring[8];
    static int next;

    sfree(ring[next]);
    ring[next] = s;
    next = (next + 1) % 8;
    return s;
}

const char *ro_local_leaf(const char *path)
{
    const char *p = path + strlen(path);

    while (p > path && p[-1] != '.' && p[-1] != ':')
        p--;
    return p;
}

/* A local leaf as the Unix name, with ",xxx" for a filetype that is not Text or Data. */
static char *leaf_with_type(const char *local_leaf, int filetype)
{
    char *u = dupstr(local_leaf), *p;

    for (p = u; *p; p++) {
        if (*p == '/')
            *p = '.';
        else if (*p == HARD_SPACE)
            *p = ' ';
    }
    if (filetype >= 0 && filetype != FILETYPE_TEXT && filetype != FILETYPE_DATA &&
        !getenv("PuTTY$SftpNoTypes")) {
        char *t = dupprintf("%s,%03x", u, filetype);
        sfree(u);
        u = t;
    }
    return u;
}

char *ro_remote_leaf_to_local(const char *leaf)
{
    return ring_keep(unix_leaf_to_ro(leaf));
}

char *ro_local_path_to_remote_leaf(const char *path)
{
    char *real = resolve_read(path);
    struct cat c;
    int type = -1;
    char *r;

    if (read_cat(real, &c) && c.obj == 1)
        type = cat_filetype(&c);
    r = leaf_with_type(ro_local_leaf(real), type);
    sfree(real);
    return ring_keep(r);
}

/*
 * A Unix leaf name joined to a local directory.  If the leaf carries a ",xxx" filetype it
 * stays in the path: open_new_file acts on it when the file is made, and the calls that
 * read a file (resolve_read) look for the file without it when there is no such name.
 */
char *dir_file_cat(const char *dir, const char *file)
{
    char *leaf = unix_leaf_to_ro(file), *r;
    size_t dl = strlen(dir);

    if (dl == 0)
        r = dupstr(leaf);
    else if (dir[dl - 1] == '.' || dir[dl - 1] == ':')
        r = dupcat(dir, leaf);
    else
        r = dupcat(dir, ".", leaf);
    sfree(leaf);
    return r;
}

char *stripslashes(const char *str, bool local)
{
    /*
     * Names handed to this are Unix names - the remote path being worked on, or a name
     * a server sent - so only '/' separates parts, whichever 'local' says.  (A local
     * path is turned into a remote name with ro_local_path_to_remote_leaf instead.)
     */
    char *p = strrchr(str, '/');

    if (p)
        str = p + 1;
    return (char *)str;
}

bool vet_filename(const char *name)
{
    /* Names are made safe by unix_leaf_to_ro, which leaves nothing that is special in a
     * RISC OS path; what is left to refuse is a path and the names of directories. */
    if (strchr(name, '/'))
        return false;
    if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2])))
        return false;
    return true;
}

/*
 * pscp: is SPEC a local file and not host:path?  A colon after a filing system name
 * ("ADFS::4.$.x", "HostFS:") or a path variable ("Choices:PuTTY.x") belongs to a path.
 * A host name or user@host has a dot or an @, which neither of those can have.
 */
bool ro_looks_local(const char *spec)
{
    const char *colon = strchr(spec, ':');
    size_t n;
    char *prefix;
    bool local = false;

    if (!colon)
        return false;
    if (colon[1] == ':')
        return true;                   /* FileSystem::Disc */
    if (strchr(spec, '[') && strchr(spec, '[') < colon)
        return false;                  /* an IPv6 address */
    n = (size_t)(colon - spec);
    if (n == 0)
        return false;
    prefix = dupprintf("%.*s:", (int)n, spec);
    if (!memchr(spec, '.', n) && !memchr(spec, '@', n)) {
        char buf[512];
        int spare = 0;
        _kernel_oserror *e = _swix(OS_FSControl, _INR(0, 5) | _OUT(5), 37, prefix, buf, 0, 0,
                                   (int)sizeof buf, &spare);
        if (!e)
            local = true;              /* a path variable: it expanded */
        else if (strstr(e->errmess, "must be given a filename"))
            local = true;              /* a filing system that exists */
    }
    sfree(prefix);
    return local;
}

/* ----------------------------------------------------------------------
 * Filetypes of files we create
 */

static const struct {
    const char *ext;
    int type;
} extension_types[] = {
    { "txt", 0xFFF }, { "text", 0xFFF }, { "log", 0xFFF }, { "md", 0xFFF }, { "rst", 0xFFF },
    { "c", 0xFFF }, { "h", 0xFFF }, { "cpp", 0xFFF }, { "py", 0xFFF }, { "sh", 0xFFF },
    { "pl", 0xFFF }, { "rb", 0xFFF }, { "js", 0xFFF }, { "css", 0xFFF }, { "json", 0xFFF },
    { "xml", 0xFFF }, { "yaml", 0xFFF }, { "yml", 0xFFF }, { "ini", 0xFFF }, { "cfg", 0xFFF },
    { "conf", 0xFFF }, { "tex", 0xFFF }, { "tsv", 0xFFF }, { "diff", 0xFFF }, { "patch", 0xFFF },
    { "csv", 0xDFE }, { "html", 0xFAF }, { "htm", 0xFAF }, { "jpg", 0xC85 }, { "jpeg", 0xC85 },
    { "gif", 0x695 }, { "png", 0xB60 }, { "pdf", 0xADF }, { "zip", 0xA91 },
};

/* The extension is written after a '/' in a RISC OS name: notes/txt. */
static int type_from_name(const char *name)
{
    const char *leaf = ro_local_leaf(name), *dot = strrchr(leaf, '/');
    size_t i;

    if (!dot)
        return -1;
    for (i = 0; i < sizeof extension_types / sizeof extension_types[0]; i++)
        if (!strcasecmp(dot + 1, extension_types[i].ext))
            return extension_types[i].type;
    return -1;
}

/* Text unless the first bytes have a NUL or a lot of control characters. */
static int sniff_type(const unsigned char *b, size_t n)
{
    size_t i, odd = 0;

    for (i = 0; i < n; i++) {
        unsigned char c = b[i];
        if (c == 0)
            return FILETYPE_DATA;
        if (c < 32 && c != '\n' && c != '\r' && c != '\t' && c != '\f' && c != 27 && c != 8)
            odd++;
    }
    return odd * 50 > n ? FILETYPE_DATA : FILETYPE_TEXT;
}

/* ----------------------------------------------------------------------
 * Files
 */

struct RFile {
    FILE *fp;
};

RFile *open_existing_file(const char *name, uint64_t *size, unsigned long *mtime,
                          unsigned long *atime, long *perms)
{
    char *real = resolve_read(name);
    struct cat c;
    FILE *fp;
    RFile *f;

    if (!read_cat(real, &c) || c.obj == 2) {
        sfree(real);
        return NULL;
    }
    fp = fopen(real, "rb");
    sfree(real);
    if (!fp)
        return NULL;
    f = snew(RFile);
    f->fp = fp;
    if (size)
        *size = c.length;
    if (mtime)
        *mtime = cat_unix_time(&c);
    if (atime)
        *atime = cat_unix_time(&c);
    if (perms)
        *perms = -1;                   /* RISC OS has no POSIX permissions */
    return f;
}

int read_from_file(RFile *f, void *buffer, int length)
{
    size_t n = fread(buffer, 1, (size_t)length, f->fp);

    if (n == 0 && ferror(f->fp))
        return -1;
    return (int)n;
}

void close_rfile(RFile *f)
{
    fclose(f->fp);
    sfree(f);
}

#define SNIFF_BYTES 512

struct WFile {
    FILE *fp;
    char *name;                        /* the real name, without any ",xxx" */
    int type;                          /* -1: not decided yet */
    bool is_new;                       /* we made it: it needs a type */
    bool have_times;
    unsigned long mtime;
    unsigned char sniff[SNIFF_BYTES];
    size_t nsniff;
};

WFile *open_new_file(const char *name, long perms)
{
    size_t base;
    int type = -1;
    char *real;
    FILE *fp;
    WFile *f;

    real = to_os(name);
    if (type_suffix(real, &base, &type))
        real[base] = '\0';
    fp = fopen(real, "wb");
    if (!fp) {
        sfree(real);
        return NULL;
    }
    f = snew(WFile);
    memset(f, 0, sizeof *f);
    f->fp = fp;
    f->name = real;
    f->type = type;
    f->is_new = true;
    return f;
}

WFile *open_existing_wfile(const char *name, uint64_t *size)
{
    char *real = resolve_read(name);
    struct cat c;
    FILE *fp;
    WFile *f;

    if (!read_cat(real, &c) || c.obj != 1) {
        sfree(real);
        return NULL;
    }
    fp = fopen(real, "ab");
    if (!fp) {
        sfree(real);
        return NULL;
    }
    f = snew(WFile);
    memset(f, 0, sizeof *f);
    f->fp = fp;
    f->name = real;
    f->type = -1;
    if (size)
        *size = c.length;
    return f;
}

int write_to_file(WFile *f, void *buffer, int length)
{
    size_t n = fwrite(buffer, 1, (size_t)length, f->fp);

    if (f->nsniff < SNIFF_BYTES) {
        size_t take = SNIFF_BYTES - f->nsniff;
        if (take > n)
            take = n;
        memcpy(f->sniff + f->nsniff, buffer, take);
        f->nsniff += take;
    }
    if (n == 0 && length > 0)
        return -1;
    return (int)n;
}

void set_file_times(WFile *f, unsigned long mtime, unsigned long atime)
{
    /* The file system stamps a file when it is closed, so this is applied then. */
    f->have_times = true;
    f->mtime = mtime;
}

void close_wfile(WFile *f)
{
    int type = f->type;

    fclose(f->fp);
    if (f->is_new && type < 0) {
        type = type_from_name(f->name);
        if (type < 0)
            type = sniff_type(f->sniff, f->nsniff);
    }
    if (f->have_times && f->mtime > 0) {
        struct cat c;
        unsigned long long cs = ((unsigned long long)f->mtime + UNIX_TO_RISCOS_SECONDS) * 100;
        unsigned load, exec;
        int t = type;

        read_cat(f->name, &c);
        if (t < 0)
            t = cat_filetype(&c);
        if (t < 0)
            t = FILETYPE_TEXT;
        load = 0xFFF00000u | ((unsigned)t << 8) | (unsigned)((cs >> 32) & 0xFF);
        exec = (unsigned)(cs & 0xFFFFFFFFu);
        _swix(OS_File, _INR(0, 3) | _IN(5), 1, f->name, load, exec, c.attr);
    } else if (type >= 0) {
        ro_set_filetype(f->name, type);
    }
    sfree(f->name);
    sfree(f);
}

int seek_file(WFile *f, uint64_t offset, int whence)
{
    int w;

    switch (whence) {
      case FROM_START: w = SEEK_SET; break;
      case FROM_CURRENT: w = SEEK_CUR; break;
      case FROM_END: w = SEEK_END; break;
      default: return -1;
    }
    return fseek(f->fp, (long)offset, w) == 0 ? 0 : -1;
}

uint64_t get_file_posn(WFile *f)
{
    long p = ftell(f->fp);
    return p < 0 ? 0 : (uint64_t)p;
}

void get_file_times(char *filename, unsigned long *mtime, unsigned long *atime)
{
    char *real = resolve_read(filename);
    struct cat c;

    read_cat(real, &c);
    sfree(real);
    *mtime = *atime = cat_unix_time(&c);
}

int file_type(const char *name)
{
    char *real = resolve_read(name);
    struct cat c;
    bool found = read_cat(real, &c);

    sfree(real);
    if (!found)
        return FILE_TYPE_NONEXISTENT;
    return c.obj == 2 ? FILE_TYPE_DIRECTORY : FILE_TYPE_FILE;
}

bool create_directory(const char *name)
{
    char *real = to_os(name);
    _kernel_oserror *e = _swix(OS_File, _INR(0, 1) | _IN(4), 8, real, 0);

    sfree(real);
    return !e;
}

char *psftp_getcwd(void)
{
    char buf[1024];
    int spare = 0;

    if (_swix(OS_FSControl, _INR(0, 5) | _OUT(5), 37, "@", buf, 0, 0, (int)sizeof buf, &spare))
        return dupstr("[cwd unavailable]");
    return dupstr(buf);
}

char *psftp_lcd(char *dir)
{
    char *real = to_os(dir);
    _kernel_oserror *e = _swix(OS_FSControl, _INR(0, 1), 0, real);

    sfree(real);
    if (e)
        return dupprintf("%s: %s", dir, e->errmess);
    return NULL;
}

/* ----------------------------------------------------------------------
 * Directories and wildcards, with OS_GBPB 10: for each entry load, exec, length,
 * attributes, object type, and then the name.
 */

static bool gbpb_next(const char *dir, int *offset, const char *wildcard, struct cat *c,
                      char *name, size_t namesize)
{
    static unsigned buf[128];          /* a word buffer, for the unsigned fields */

    while (*offset >= 0) {
        int count = 0;

        if (_swix(OS_GBPB, _INR(0, 6) | _OUT(3) | _OUT(4), 10, dir, buf, 1, *offset,
                  (int)sizeof buf, wildcard, &count, offset)) {
            *offset = -1;
            return false;
        }
        if (count > 0) {
            c->load = buf[0];
            c->exec = buf[1];
            c->length = buf[2];
            c->attr = buf[3];
            c->obj = (int)buf[4];
            strncpy(name, (const char *)(buf + 5), namesize - 1);
            name[namesize - 1] = '\0';
            return true;
        }
    }
    return false;
}

struct DirHandle {
    char *path;
    int offset;
};

DirHandle *open_directory(const char *name, const char **errmsg)
{
    char *real = resolve_read(name);
    struct cat c;
    DirHandle *dir;

    if (!read_cat(real, &c)) {
        *errmsg = "no such directory";
        sfree(real);
        return NULL;
    }
    if (c.obj != 2) {
        *errmsg = "not a directory";
        sfree(real);
        return NULL;
    }
    dir = snew(DirHandle);
    dir->path = real;
    dir->offset = 0;
    return dir;
}

char *read_filename(DirHandle *dir)
{
    struct cat c;
    char name[256];

    if (!gbpb_next(dir->path, &dir->offset, NULL, &c, name, sizeof name))
        return NULL;
    return leaf_with_type(name, c.obj == 1 ? cat_filetype(&c) : -1);
}

void close_directory(DirHandle *dir)
{
    sfree(dir->path);
    sfree(dir);
}

/* Split PATH into its directory (with no trailing separator) and its last part. */
static void split_path(const char *path, char **dir, char **leaf)
{
    const char *l = ro_local_leaf(path);
    size_t dl = (size_t)(l - path);

    *leaf = dupstr(l);
    if (dl > 1 && path[dl - 1] == '.')
        dl--;                          /* (a trailing ':' belongs to the directory's name) */
    *dir = dl > 0 ? dupprintf("%.*s", (int)dl, path) : dupstr("@");
}

static bool has_wildcard(const char *leaf)
{
    return strpbrk(leaf, "*#") != NULL;
}

int test_wildcard(const char *name, bool cmdline)
{
    char *os = to_os(name), *dir, *leaf;
    int ret;

    split_path(os, &dir, &leaf);
    sfree(os);
    if (has_wildcard(leaf)) {
        struct cat c;
        char found[256];
        int offset = 0;

        ret = gbpb_next(dir, &offset, leaf, &c, found, sizeof found) ? WCTYPE_WILDCARD
                                                                     : WCTYPE_NONEXISTENT;
    } else {
        char *real = resolve_read(name);
        ret = exists(real) ? WCTYPE_FILENAME : WCTYPE_NONEXISTENT;
        sfree(real);
    }
    sfree(dir);
    sfree(leaf);
    return ret;
}

struct WildcardMatcher {
    char *dir, *leaf, *prefix;
    int offset;
};

WildcardMatcher *begin_wildcard_matching(const char *name)
{
    WildcardMatcher *wc = snew(WildcardMatcher);
    char *os = to_os(name);
    const char *l = ro_local_leaf(os);

    split_path(os, &wc->dir, &wc->leaf);
    wc->prefix = dupprintf("%.*s", (int)(l - os), os);       /* up to and including the '.' */
    wc->offset = 0;
    sfree(os);
    return wc;
}

char *wildcard_get_filename(WildcardMatcher *wc)
{
    struct cat c;
    char name[256];

    if (!gbpb_next(wc->dir, &wc->offset, wc->leaf, &c, name, sizeof name))
        return NULL;
    return dupcat(wc->prefix, name);
}

void finish_wildcard_matching(WildcardMatcher *wc)
{
    sfree(wc->dir);
    sfree(wc->leaf);
    sfree(wc->prefix);
    sfree(wc);
}

/* ----------------------------------------------------------------------
 * Waiting for the network or a key, and reading a command
 */

static bool stdin_tty;
static int pending_key = -1;
static uint32_t test_deadline;         /* ro_time() to give up at, from PuTTY$Timeout (for tests) */

/* OS_Byte 129 with no timeout: the key waiting, or -1. */
static int read_key(void)
{
    int key = 0, status = 255;

    if (_swix(OS_Byte, _INR(0, 2) | _OUTR(1, 2), 129, 0, 0, &key, &status))
        return -1;
    if (status != 0)
        return -1;
    return key & 255;
}

/*
 * Wait for something to happen: network data (or a timer, or a callback) - then return 0
 * - or, if include_stdin and the keyboard is the input, a key, which is left in
 * pending_key and gives 1.  Returns -1 if there is no connection left to wait for.
 */
static int wait_for_activity(bool include_stdin, bool no_fds_ok)
{
    for (;;) {
        unsigned long now, next;
        int timeout_cs, n, state, rwx;
        bool ran;

        if (test_deadline && (int32_t)(ro_time() - test_deadline) > 0) {
            fprintf(stderr, "%s: gave up after PuTTY$Timeout seconds\n", appname);
            cleanup_exit(3);
        }
        if (!no_fds_ok && !toplevel_callback_pending() && !ro_net_busy() &&
            first_fd(&state, &rwx) < 0)
            return -1;

        now = GETTICKCOUNT();
        if (toplevel_callback_pending()) {
            timeout_cs = 0;
        } else if (run_timers(now, &next)) {
            long ticks = (long)(next - now);
            timeout_cs = ticks <= 0 ? 0 : (int)(ticks / 10) + 1;
        } else {
            timeout_cs = 100;
        }
        if (timeout_cs > 100)
            timeout_cs = 100;
        if (include_stdin && timeout_cs > 5)
            timeout_cs = 5;
        if (ro_net_busy() && timeout_cs > 2)
            timeout_cs = 2;

        n = ro_net_poll(timeout_cs);
        ran = run_toplevel_callbacks();
        if (include_stdin) {
            if (pending_key < 0)
                pending_key = read_key();
            if (pending_key >= 0)
                return 1;
        }
        if (n > 0 || ran)
            return 0;
    }
}

int ssh_sftp_loop_iteration(void)
{
    return wait_for_activity(false, false);
}

/* Commands typed at the keyboard are kept, newest last, for the cursor keys to bring back. */
#define HISTORY 32
static char *history[HISTORY];
static int nhistory;

static void remember(const char *line)
{
    if (!*line)
        return;
    if (nhistory > 0 && !strcmp(history[nhistory - 1], line))
        return;
    if (nhistory == HISTORY) {
        sfree(history[0]);
        memmove(history, history + 1, (HISTORY - 1) * sizeof history[0]);
        nhistory--;
    }
    history[nhistory++] = dupstr(line);
}

static void erase_echo(size_t n)
{
    while (n--)
        fputs("\b \b", stdout);
}

static char *read_line_from_keyboard(bool no_fds_ok)
{
    size_t len = 0, cap = 256;
    char *line = snewn(cap, char);
    int hist = nhistory;

    line[0] = '\0';
    for (;;) {
        int r = wait_for_activity(true, no_fds_ok), c;

        if (r < 0) {
            printf("connection died\n");
            sfree(line);
            return NULL;
        }
        if (r == 0)
            continue;
        c = pending_key;
        pending_key = -1;
        noise_ultralight(NOISE_SOURCE_KEY, (unsigned long)c);

        if (c == 13 || c == 10) {
            fputs("\n", stdout);
            fflush(stdout);
            remember(line);
            line[len++] = '\n';
            line[len] = '\0';
            return line;
        } else if (c == 4 && len == 0) {           /* Ctrl-D at the start of a line: end of input */
            sfree(line);
            return NULL;
        } else if (c == 8 || c == 127) {
            if (len > 0) {
                line[--len] = '\0';
                erase_echo(1);
            }
        } else if (c == 21) {                      /* Ctrl-U */
            erase_echo(len);
            len = 0;
            line[0] = '\0';
        } else if (c == 139 || c == 138) {         /* up, down: the history */
            if (c == 139 && hist > 0)
                hist--;
            else if (c == 138 && hist < nhistory)
                hist++;
            else
                continue;
            erase_echo(len);
            len = 0;
            if (hist < nhistory) {
                size_t n = strlen(history[hist]);
                while (len + n + 2 > cap)
                    cap *= 2, line = sresize(line, cap, char);
                memcpy(line, history[hist], n + 1);
                len = n;
                fputs(line, stdout);
            } else {
                line[0] = '\0';
            }
        } else if (c >= 32 && c != 127 && c < 256 && !(c >= 128 && c < 160)) {
            if (len + 3 > cap)
                cap *= 2, line = sresize(line, cap, char);
            line[len++] = (char)c;
            line[len] = '\0';
            fputc(c, stdout);
        }
        fflush(stdout);
    }
}

static char *read_line_from_file(void)
{
    size_t len = 0, cap = 256;
    char *line = snewn(cap, char);
    int c;

    while ((c = fgetc(stdin)) != EOF) {
        if (len + 2 > cap)
            cap *= 2, line = sresize(line, cap, char);
        line[len++] = (char)c;
        if (c == '\n')
            break;
    }
    if (len == 0) {
        sfree(line);
        return NULL;
    }
    line[len] = '\0';
    return line;
}

/*
 * Read a command.  Like upstream's the result ends in a newline, so that an empty line is
 * not mistaken for the end of the input; NULL means end of input or a dead connection.
 */
char *ssh_sftp_get_cmdline(const char *prompt, bool no_fds_ok)
{
    fputs(prompt, stdout);
    fflush(stdout);
    if (stdin_tty)
        return read_line_from_keyboard(no_fds_ok);
    return read_line_from_file();
}

/* ----------------------------------------------------------------------
 * Odds and ends upstream's Unix file has
 */

char *x_get_default(const char *key)
{
    return NULL;
}

char *platform_default_s(const char *name)
{
    return NULL;
}

bool platform_default_b(const char *name, bool def)
{
    return def;
}

int platform_default_i(const char *name, int def)
{
    return def;
}

FontSpec *platform_default_fontspec(const char *name)
{
    return fontspec_new_default();
}

Filename *platform_default_filename(const char *name)
{
    if (!strcmp(name, "LogFileName"))
        return filename_from_str("putty.log");
    else
        return filename_from_str("");
}

SeatPromptResult filexfer_get_userpass_input(Seat *seat, prompts_t *p)
{
    /* The file transfer tools don't support Restart Session, so we can just have a
     * single static cmdline_get_passwd_input_state that's never reset */
    static cmdline_get_passwd_input_state cmdline_state =
        CMDLINE_GET_PASSWD_INPUT_STATE_INIT;
    SeatPromptResult spr;

    spr = cmdline_get_passwd_input(p, &cmdline_state, false);
    if (spr.kind == SPRK_INCOMPLETE)
        spr = console_get_userpass_input(p);
    return spr;
}

void frontend_net_error_pending(void)
{
}

void platform_psftp_pre_conn_setup(LogPolicy *lp)
{
    if (ro_entropy_is_weak() && !getenv("PuTTY$NoEntropyWarning"))
        fprintf(stderr,
                "%s: warning: the CryptRandom module is not loaded and could not be found in\n"
                "%s: System:Modules, so this system has no strong source of random numbers.\n"
                "%s: Session keys will be weaker than they should be. Install the CryptRandom\n"
                "%s: package with !PackMan (System section).\n",
                appname, appname, appname, appname);
}

const bool buildinfo_gtk_relevant = false;

int main(int argc, char **argv)
{
    const char *t = getenv("PuTTY$Timeout");

    enable_dit();
    stdin_tty = isatty(0);
    if (t && atoi(t) > 0)
        test_deadline = ro_time() + (uint32_t)atoi(t) * 100;
    uxsel_init();
    CmdlineArgList *arglist = cmdline_arg_list_from_argv(argc, argv);
    return psftp_main(arglist);
}
