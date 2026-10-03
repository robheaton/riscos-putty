/*
 * storage.c: RISC OS implementation of the interface defined in storage.h.
 *
 * Follows upstream's unix/storage.c, which keeps everything in plain files:
 *
 *   <dir>.Sessions.<name>   one saved session, as key=value lines
 *   <dir>.SSHHostKeys       known host keys, one per line
 *   <dir>.RandomSeed        the PRNG seed
 *   <dir>.SSHHostCAs.<name> trusted host certification authorities
 *
 * where <dir> is the system variable PuTTY$Choices if set, else
 * <Choices$Write>.PuTTY.  Session names are turned into leafnames by escaping
 * every character that RISC OS filing systems dislike as =XX.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>

#include "putty.h"
#include "storage.h"
#include "tree234.h"
#include "ro.h"

enum {
    INDEX_DIR, INDEX_HOSTKEYS, INDEX_HOSTKEYS_TMP, INDEX_RANDSEED,
    INDEX_SESSIONDIR, INDEX_SESSION, INDEX_HOSTCADIR, INDEX_HOSTCA, INDEX_WINPOS, INDEX_AGENTKEYS
};

static const char hex[16] = "0123456789ABCDEF";

static void make_session_filename(const char *in, strbuf *out)
{
    if (!in || !*in)
        in = "Default Settings";

    while (*in) {
        unsigned char c = (unsigned char)*in;
        /*
         * Opt-in for safe characters rather than opt-out for unsafe ones: '.',
         * ':', '$', '&', '@', '^', '%', '\', '*', '#', '"' and '|' all mean
         * something in a RISC OS pathname, spaces are a nuisance on the command
         * line, and the rest of the punctuation is best avoided too.
         */
        if (c != '+' && c != '-' && c != '_' &&
            !(c >= '0' && c <= '9') &&
            !(c >= 'A' && c <= 'Z') &&
            !(c >= 'a' && c <= 'z')) {
            put_byte(out, '=');
            put_byte(out, hex[c >> 4]);
            put_byte(out, hex[c & 15]);
        } else
            put_byte(out, c);
        in++;
    }
}

static void decode_session_filename(const char *in, strbuf *out)
{
    while (*in) {
        if (*in == '=' && in[1] && in[2]) {
            int i, j;

            i = in[1] - '0';
            i -= (i > 9 ? 7 : 0);
            j = in[2] - '0';
            j -= (j > 9 ? 7 : 0);

            put_byte(out, (i << 4) + j);
            in += 3;
        } else {
            put_byte(out, *in++);
        }
    }
}

static char *data_dir(void)
{
    const char *env = getenv("PuTTY$Choices");
    if (env && *env)
        return dupstr(env);
    env = getenv("Choices$Write");
    if (env && *env)
        return dupstr("<Choices$Write>.PuTTY");
    return dupstr("<PuTTY$Dir>.Choices");
}

static char *make_filename(int index, const char *subname)
{
    char *tmp, *ret;

    if (index == INDEX_DIR)
        return data_dir();
    if (index == INDEX_SESSIONDIR) {
        tmp = data_dir();
        ret = dupprintf("%s.Sessions", tmp);
        sfree(tmp);
        return ret;
    }
    if (index == INDEX_SESSION) {
        strbuf *sb = strbuf_new();
        tmp = make_filename(INDEX_SESSIONDIR, NULL);
        put_fmt(sb, "%s.", tmp);
        sfree(tmp);
        make_session_filename(subname, sb);
        return strbuf_to_str(sb);
    }
    if (index == INDEX_HOSTKEYS) {
        tmp = data_dir();
        ret = dupprintf("%s.SSHHostKeys", tmp);
        sfree(tmp);
        return ret;
    }
    if (index == INDEX_HOSTKEYS_TMP) {
        tmp = data_dir();
        ret = dupprintf("%s.SSHHostKeyT", tmp);
        sfree(tmp);
        return ret;
    }
    if (index == INDEX_RANDSEED) {
        tmp = data_dir();
        ret = dupprintf("%s.RandomSeed", tmp);
        sfree(tmp);
        return ret;
    }
    if (index == INDEX_AGENTKEYS) {
        tmp = data_dir();
        ret = dupprintf("%s.AgentKeys", tmp);
        sfree(tmp);
        return ret;
    }
    if (index == INDEX_WINPOS) {
        tmp = data_dir();
        ret = dupprintf("%s.WindowPos", tmp);
        sfree(tmp);
        return ret;
    }
    if (index == INDEX_HOSTCADIR) {
        tmp = data_dir();
        ret = dupprintf("%s.SSHHostCAs", tmp);
        sfree(tmp);
        return ret;
    }
    if (index == INDEX_HOSTCA) {
        strbuf *sb = strbuf_new();
        tmp = make_filename(INDEX_HOSTCADIR, NULL);
        put_fmt(sb, "%s.", tmp);
        sfree(tmp);
        make_session_filename(subname, sb);
        return strbuf_to_str(sb);
    }
    tmp = data_dir();
    ret = dupprintf("%s.ERROR", tmp);
    sfree(tmp);
    return ret;
}

struct settings_w {
    FILE *fp;
    char *filename;
};

settings_w *open_settings_w(const char *sessionname, char **errmsg)
{
    char *filename, *err;
    FILE *fp;

    *errmsg = NULL;

    /* Start by making sure the data directory and its Sessions subdirectory
     * actually exist. */
    filename = make_filename(INDEX_DIR, NULL);
    if ((err = make_dir_path(filename, 0700)) != NULL) {
        *errmsg = dupprintf("Unable to save session: %s", err);
        sfree(err);
        sfree(filename);
        return NULL;
    }
    sfree(filename);

    filename = make_filename(INDEX_SESSIONDIR, NULL);
    if ((err = make_dir_path(filename, 0700)) != NULL) {
        *errmsg = dupprintf("Unable to save session: %s", err);
        sfree(err);
        sfree(filename);
        return NULL;
    }
    sfree(filename);

    filename = make_filename(INDEX_SESSION, sessionname);
    fp = fopen(filename, "w");
    if (!fp) {
        *errmsg = dupprintf("Unable to save session: open(\"%s\") "
                            "returned '%s'", filename, strerror(errno));
        sfree(filename);
        return NULL;                   /* can't open */
    }

    settings_w *toret = snew(settings_w);
    toret->fp = fp;
    toret->filename = filename;
    return toret;
}

void write_setting_s(settings_w *handle, const char *key, const char *value)
{
    fprintf(handle->fp, "%s=%s\n", key, value);
}

void write_setting_i(settings_w *handle, const char *key, int value)
{
    fprintf(handle->fp, "%s=%d\n", key, value);
}

void close_settings_w(settings_w *handle)
{
    fclose(handle->fp);
    ro_set_filetype(handle->filename, FILETYPE_TEXT);
    sfree(handle->filename);
    sfree(handle);
}

/* ----------------------------------------------------------------------
 * Main code for reading settings from a disk file.
 */

struct skeyval {
    const char *key;
    const char *value;
};

static int keycmp(void *av, void *bv)
{
    struct skeyval *a = (struct skeyval *)av;
    struct skeyval *b = (struct skeyval *)bv;
    return strcmp(a->key, b->key);
}

struct settings_r {
    tree234 *t;
};

settings_r *open_settings_r(const char *sessionname)
{
    char *filename;
    FILE *fp;
    char *line;
    settings_r *toret;

    filename = make_filename(INDEX_SESSION, sessionname);
    fp = fopen(filename, "r");
    sfree(filename);
    if (!fp)
        return NULL;                   /* can't open */

    toret = snew(settings_r);
    toret->t = newtree234(keycmp);

    while ( (line = fgetline(fp)) ) {
        char *value = strchr(line, '=');
        struct skeyval *kv;

        if (!value) {
            sfree(line);
            continue;
        }
        *value++ = '\0';
        value[strcspn(value, "\r\n")] = '\0';   /* trim trailing NL */

        kv = snew(struct skeyval);
        kv->key = dupstr(line);
        kv->value = dupstr(value);
        add234(toret->t, kv);

        sfree(line);
    }

    fclose(fp);

    return toret;
}

char *read_setting_s(settings_r *handle, const char *key)
{
    struct skeyval tmp, *kv;

    tmp.key = key;
    if (handle != NULL &&
        (kv = find234(handle->t, &tmp, NULL)) != NULL) {
        assert(kv->value != NULL);
        return dupstr(kv->value);
    }
    return NULL;
}

int read_setting_i(settings_r *handle, const char *key, int defvalue)
{
    struct skeyval tmp, *kv;

    tmp.key = key;
    if (handle != NULL &&
        (kv = find234(handle->t, &tmp, NULL)) != NULL) {
        assert(kv->value != NULL);
        return atoi(kv->value);
    }
    return defvalue;
}

FontSpec *read_setting_fontspec(settings_r *handle, const char *name)
{
    char *tmp = read_setting_s(handle, name);
    if (tmp) {
        FontSpec *fs = fontspec_new(tmp);
        sfree(tmp);
        return fs;
    }
    return NULL;
}

Filename *read_setting_filename(settings_r *handle, const char *name)
{
    char *tmp = read_setting_s(handle, name);
    if (tmp) {
        Filename *ret = filename_from_str(tmp);
        sfree(tmp);
        return ret;
    } else
        return NULL;
}

void write_setting_fontspec(settings_w *handle, const char *name, FontSpec *fs)
{
    write_setting_s(handle, name, fs->name);
}

void write_setting_filename(settings_w *handle,
                            const char *name, Filename *result)
{
    write_setting_s(handle, name, result->path);
}

void close_settings_r(settings_r *handle)
{
    struct skeyval *kv;

    if (!handle)
        return;

    while ( (kv = index234(handle->t, 0)) != NULL) {
        del234(handle->t, kv);
        sfree((char *)kv->key);
        sfree((char *)kv->value);
        sfree(kv);
    }

    freetree234(handle->t);
    sfree(handle);
}

void del_settings(const char *sessionname)
{
    char *filename;
    filename = make_filename(INDEX_SESSION, sessionname);
    remove(filename);
    sfree(filename);
}

/* ----------------------------------------------------------------------
 * Enumerating a directory (sessions, host CAs): OS_GBPB 9 hands back the
 * leafnames, which we collect up front.
 */

typedef struct DirEnum {
    char *names;                       /* NUL-separated, all of them */
    size_t len, pos;
} DirEnum;

static DirEnum *dir_enum_start(const char *dirpath)
{
    DirEnum *de = snew(DirEnum);
    strbuf *all = strbuf_new();
    char buf[2048];
    int offset = 0;

    while (offset != -1) {
        int nread = 0, i;
        char *p = buf;
        if (_swix(OS_GBPB, _INR(0, 6) | _OUTR(3, 4), 9, dirpath, buf, 100,
                  offset, (int)sizeof buf, 0, &nread, &offset))
            break;
        for (i = 0; i < nread; i++) {
            size_t n = strlen(p) + 1;
            put_data(all, p, n);
            p += n;
        }
    }

    de->len = all->len;
    de->names = strbuf_to_str(all);    /* keeps the NULs; len says how much */
    de->pos = 0;
    return de;
}

static bool dir_enum_next(DirEnum *de, strbuf *out)
{
    if (de->pos >= de->len)
        return false;
    decode_session_filename(de->names + de->pos, out);
    de->pos += strlen(de->names + de->pos) + 1;
    return true;
}

static void dir_enum_finish(DirEnum *de)
{
    sfree(de->names);
    sfree(de);
}

struct settings_e {
    DirEnum *de;
};

settings_e *enum_settings_start(void)
{
    char *filename = make_filename(INDEX_SESSIONDIR, NULL);
    settings_e *toret = snew(settings_e);
    toret->de = dir_enum_start(filename);
    sfree(filename);
    return toret;
}

bool enum_settings_next(settings_e *handle, strbuf *out)
{
    return dir_enum_next(handle->de, out);
}

void enum_settings_finish(settings_e *handle)
{
    dir_enum_finish(handle->de);
    sfree(handle);
}

struct host_ca_enum {
    DirEnum *de;
};

host_ca_enum *enum_host_ca_start(void)
{
    char *filename = make_filename(INDEX_HOSTCADIR, NULL);
    host_ca_enum *handle = snew(host_ca_enum);
    handle->de = dir_enum_start(filename);
    sfree(filename);
    return handle;
}

bool enum_host_ca_next(host_ca_enum *handle, strbuf *out)
{
    return dir_enum_next(handle->de, out);
}

void enum_host_ca_finish(host_ca_enum *handle)
{
    dir_enum_finish(handle->de);
    sfree(handle);
}

host_ca *host_ca_load(const char *name)
{
    char *filename = make_filename(INDEX_HOSTCA, name);
    FILE *fp = fopen(filename, "r");
    sfree(filename);
    if (!fp)
        return NULL;

    host_ca *hca = host_ca_new();
    hca->name = dupstr(name);

    char *line;
    CertExprBuilder *eb = NULL;

    while ( (line = fgetline(fp)) ) {
        char *value = strchr(line, '=');

        if (!value) {
            sfree(line);
            continue;
        }
        *value++ = '\0';
        value[strcspn(value, "\r\n")] = '\0';   /* trim trailing NL */

        if (!strcmp(line, "PublicKey")) {
            hca->ca_public_key = base64_decode_sb(ptrlen_from_asciz(value));
        } else if (!strcmp(line, "MatchHosts")) {
            if (!eb)
                eb = cert_expr_builder_new();
            cert_expr_builder_add(eb, value);
        } else if (!strcmp(line, "Validity")) {
            hca->validity_expression = strbuf_to_str(
                percent_decode_sb(ptrlen_from_asciz(value)));
        } else if (!strcmp(line, "PermitRSASHA1")) {
            hca->opts.permit_rsa_sha1 = atoi(value);
        } else if (!strcmp(line, "PermitRSASHA256")) {
            hca->opts.permit_rsa_sha256 = atoi(value);
        } else if (!strcmp(line, "PermitRSASHA512")) {
            hca->opts.permit_rsa_sha512 = atoi(value);
        }

        sfree(line);
    }

    fclose(fp);

    if (eb) {
        if (!hca->validity_expression) {
            hca->validity_expression = cert_expr_expression(eb);
        }
        cert_expr_builder_free(eb);
    }

    return hca;
}

char *host_ca_save(host_ca *hca)
{
    char *filename, *err;

    if (!*hca->name)
        return dupstr("CA record must have a name");

    filename = make_filename(INDEX_DIR, NULL);
    if ((err = make_dir_path(filename, 0700)) != NULL) {
        char *msg = dupprintf("Unable to save CA record: "
                              "making directory '%s': %s", filename, err);
        sfree(err);
        sfree(filename);
        return msg;
    }
    sfree(filename);

    filename = make_filename(INDEX_HOSTCADIR, NULL);
    if ((err = make_dir_path(filename, 0700)) != NULL) {
        char *msg = dupprintf("Unable to save CA record: "
                              "making directory '%s': %s", filename, err);
        sfree(err);
        sfree(filename);
        return msg;
    }
    sfree(filename);

    filename = make_filename(INDEX_HOSTCA, hca->name);
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        err = dupprintf("Unable to save CA record: opening file '%s': %s",
                        filename, strerror(errno));
        sfree(filename);
        return err;
    }

    fprintf(fp, "PublicKey=");
    base64_encode_fp(fp, ptrlen_from_strbuf(hca->ca_public_key), 0);
    fprintf(fp, "\n");

    fprintf(fp, "Validity=");
    percent_encode_fp(fp, ptrlen_from_asciz(hca->validity_expression), NULL);
    fprintf(fp, "\n");

    fprintf(fp, "PermitRSASHA1=%d\n", (int)hca->opts.permit_rsa_sha1);
    fprintf(fp, "PermitRSASHA256=%d\n", (int)hca->opts.permit_rsa_sha256);
    fprintf(fp, "PermitRSASHA512=%d\n", (int)hca->opts.permit_rsa_sha512);

    bool bad = ferror(fp);
    if (fclose(fp) < 0)
        bad = true;
    ro_set_filetype(filename, FILETYPE_TEXT);

    err = NULL;
    if (bad)
        err = dupprintf("Unable to save CA record: writing file '%s': %s",
                        filename, strerror(errno));

    sfree(filename);
    return err;
}

char *host_ca_delete(const char *name)
{
    if (!*name)
        return dupstr("CA record must have a name");
    char *filename = make_filename(INDEX_HOSTCA, name);
    bool bad = remove(filename) < 0;

    char *err = NULL;
    if (bad)
        err = dupprintf("Unable to delete file '%s'", filename);

    sfree(filename);
    return err;
}

/*
 * Lines in the host keys file are of the form
 *
 *   type@port:hostname keydata
 *
 * e.g.
 *
 *   rsa@22:foovax.example.org 0x23,0x293487364395345345....2343
 */
int check_stored_host_key(const char *hostname, int port,
                          const char *keytype, const char *key)
{
    FILE *fp;
    char *filename;
    char *line;
    int ret;

    filename = make_filename(INDEX_HOSTKEYS, NULL);
    fp = fopen(filename, "r");
    sfree(filename);
    if (!fp)
        return 1;                      /* key does not exist */

    ret = 1;
    while ( (line = fgetline(fp)) ) {
        int i;
        char *p = line;
        char porttext[20];

        line[strcspn(line, "\n")] = '\0';   /* strip trailing newline */

        i = strlen(keytype);
        if (strncmp(p, keytype, i))
            goto done;
        p += i;

        if (*p != '@')
            goto done;
        p++;

        sprintf(porttext, "%d", port);
        i = strlen(porttext);
        if (strncmp(p, porttext, i))
            goto done;
        p += i;

        if (*p != ':')
            goto done;
        p++;

        i = strlen(hostname);
        if (strncmp(p, hostname, i))
            goto done;
        p += i;

        if (*p != ' ')
            goto done;
        p++;

        /*
         * Found the key.  Now just work out whether it's the right one or not.
         */
        if (!strcmp(p, key))
            ret = 0;                   /* key matched OK */
        else
            ret = 2;                   /* key mismatch */

      done:
        sfree(line);
        if (ret != 1)
            break;
    }

    fclose(fp);
    return ret;
}

bool have_ssh_host_key(const char *hostname, int port,
                       const char *keytype)
{
    /*
     * If we have a host key, check_stored_host_key will return 0 or 2.  If we
     * don't have one, it'll return 1.
     */
    return check_stored_host_key(hostname, port, keytype, "") != 1;
}

void store_host_key(Seat *seat, const char *hostname, int port,
                    const char *keytype, const char *key)
{
    FILE *rfp, *wfp;
    char *newtext, *line;
    int headerlen;
    char *filename, *tmpfilename;

    /*
     * Make sure the data directory exists, then open both the old file and a
     * new file.
     */
    {
        char *dir = make_filename(INDEX_DIR, NULL), *errmsg;
        if ((errmsg = make_dir_path(dir, 0700)) != NULL) {
            seat_nonfatal(seat, "Unable to store host key: %s", errmsg);
            sfree(errmsg);
            sfree(dir);
            return;
        }
        sfree(dir);
    }

    tmpfilename = make_filename(INDEX_HOSTKEYS_TMP, NULL);
    wfp = fopen(tmpfilename, "w");
    if (!wfp) {
        seat_nonfatal(seat, "Unable to store host key: open(\"%s\") "
                      "returned '%s'", tmpfilename, strerror(errno));
        sfree(tmpfilename);
        return;
    }
    filename = make_filename(INDEX_HOSTKEYS, NULL);
    rfp = fopen(filename, "r");

    newtext = dupprintf("%s@%d:%s %s\n", keytype, port, hostname, key);
    headerlen = 1 + strcspn(newtext, " ");   /* count the space too */

    /*
     * Copy all lines from the old file to the new one that _don't_ involve the
     * same host key identifier as the one we're adding.
     */
    if (rfp) {
        while ( (line = fgetline(rfp)) ) {
            if (strncmp(line, newtext, headerlen))
                fputs(line, wfp);
            sfree(line);
        }
        fclose(rfp);
    }

    /* Now add the new line at the end. */
    fputs(newtext, wfp);

    fclose(wfp);
    ro_set_filetype(tmpfilename, FILETYPE_TEXT);

    /* RISC OS rename() will not replace an existing file. */
    remove(filename);
    if (rename(tmpfilename, filename) < 0) {
        seat_nonfatal(seat, "Unable to store host key: rename(\"%s\",\"%s\")"
                      " returned '%s'", tmpfilename, filename,
                      strerror(errno));
    }

    sfree(tmpfilename);
    sfree(filename);
    sfree(newtext);
}

void read_random_seed(noise_consumer_t consumer)
{
    FILE *fp;
    char *fname;

    fname = make_filename(INDEX_RANDSEED, NULL);
    fp = fopen(fname, "rb");
    sfree(fname);
    if (fp) {
        char buf[512];
        size_t ret;
        while ( (ret = fread(buf, 1, sizeof(buf), fp)) > 0)
            consumer(buf, (int)ret);
        fclose(fp);
    }
}

void write_random_seed(void *data, int len)
{
    FILE *fp;
    char *fname, *dir, *errmsg;

    fname = make_filename(INDEX_RANDSEED, NULL);

    dir = make_filename(INDEX_DIR, NULL);
    if ((errmsg = make_dir_path(dir, 0700)) != NULL) {
        nonfatal("Unable to write random seed: %s", errmsg);
        sfree(errmsg);
        sfree(fname);
        sfree(dir);
        return;
    }
    sfree(dir);

    /*
     * Don't truncate the random seed file if it already exists; if something
     * goes wrong half way through writing it, it would be better to leave the
     * old data there than to leave it empty.
     */
    fp = fopen(fname, "r+b");
    if (!fp)
        fp = fopen(fname, "wb");
    if (!fp) {
        nonfatal("Unable to write random seed: open(\"%s\") "
                 "returned '%s'", fname, strerror(errno));
        sfree(fname);
        return;
    }

    if (fwrite(data, 1, len, fp) != (size_t)len)
        nonfatal("Unable to write random seed: write returned '%s'",
                 strerror(errno));

    fclose(fp);
    ro_set_filetype(fname, FILETYPE_DATA);
    sfree(fname);
}

/*
 * Where each session's window was when it was closed, so that the next one for the same
 * host and port opens there.  One line per host:port - "host:port", a tab, x, a tab, y (the
 * window's top left, in OS units) - the most recently used last, at most WINPOS_MAX of them.
 */
#define WINPOS_MAX 64

struct winpos {
    char key[160];
    int x, y;
};

static int winpos_load(struct winpos *wp, int max)
{
    char *fname = make_filename(INDEX_WINPOS, NULL), *line;
    FILE *fp = fopen(fname, "r");
    int n = 0;

    sfree(fname);
    if (!fp)
        return 0;
    while (n < max && (line = fgetline(fp)) != NULL) {
        char *tab1 = strchr(line, '\t'), *tab2 = tab1 ? strchr(tab1 + 1, '\t') : NULL;

        if (tab2 && (size_t)(tab1 - line) < sizeof wp[n].key) {
            memcpy(wp[n].key, line, (size_t)(tab1 - line));
            wp[n].key[tab1 - line] = '\0';
            wp[n].x = atoi(tab1 + 1);
            wp[n].y = atoi(tab2 + 1);
            n++;
        }
        sfree(line);
    }
    fclose(fp);
    return n;
}

bool ro_winpos_get(const char *key, int *x, int *y)
{
    struct winpos wp[WINPOS_MAX + 1];
    int n = winpos_load(wp, WINPOS_MAX + 1), i;

    for (i = n - 1; i >= 0; i--) {
        if (!strcmp(wp[i].key, key)) {
            *x = wp[i].x;
            *y = wp[i].y;
            return true;
        }
    }
    return false;
}

void ro_winpos_put(const char *key, int x, int y)
{
    struct winpos wp[WINPOS_MAX + 1];
    int n = winpos_load(wp, WINPOS_MAX + 1), i, j = 0;
    char *fname, *dir, *errmsg;
    FILE *fp;

    if (strlen(key) >= sizeof wp[0].key)
        return;
    for (i = 0; i < n; i++)                         /* drop the old entry for this host */
        if (strcmp(wp[i].key, key))
            wp[j++] = wp[i];
    n = j;
    if (n >= WINPOS_MAX) {                          /* and the least recently used if it is full */
        memmove(wp, wp + (n - WINPOS_MAX + 1), (size_t)(WINPOS_MAX - 1) * sizeof wp[0]);
        n = WINPOS_MAX - 1;
    }
    snprintf(wp[n].key, sizeof wp[n].key, "%s", key);
    wp[n].x = x;
    wp[n].y = y;
    n++;

    dir = make_filename(INDEX_DIR, NULL);
    errmsg = make_dir_path(dir, 0700);
    sfree(dir);
    if (errmsg) {
        sfree(errmsg);
        return;
    }
    fname = make_filename(INDEX_WINPOS, NULL);
    fp = fopen(fname, "w");
    if (fp) {
        for (i = 0; i < n; i++)
            fprintf(fp, "%s\t%d\t%d\n", wp[i].key, wp[i].x, wp[i].y);
        fclose(fp);
        ro_set_filetype(fname, FILETYPE_TEXT);
    }
    sfree(fname);
}

/*
 * The start-up list of the key agent: a text file naming private keys to add when PuTTY starts, one
 * pathname to a line.  With `create` the file (and its directory) are made if need be, with a note on
 * what goes in it, so that there is something to edit.
 */
char *ro_agentkeys_path(bool create)
{
    char *fname = make_filename(INDEX_AGENTKEYS, NULL);

    if (create) {
        FILE *fp = fopen(fname, "r");
        if (fp) {
            fclose(fp);
        } else {
            char *dir = make_filename(INDEX_DIR, NULL), *errmsg = make_dir_path(dir, 0700);
            sfree(dir);
            sfree(errmsg);
            fp = fopen(fname, "w");
            if (fp) {
                fputs("# Private keys for PuTTY's key agent to pick up when PuTTY starts, one pathname per line.\n"
                      "# A key with a passphrase asks for it then.  Lines that start with # are ignored.\n",
                      fp);
                fclose(fp);
                ro_set_filetype(fname, FILETYPE_TEXT);
            }
        }
    }
    return fname;
}

void cleanup_all(void)
{
}
