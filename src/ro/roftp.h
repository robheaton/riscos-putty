/*
 * roftp.h - an SFTP connection, for the file transfer window.
 *
 * Everything here is synchronous in the way PSFTP is: a call returns when the server has answered.  While
 * it waits it keeps the desktop going (roapp_wait), so windows redraw, buttons work and PuTTY's other
 * sessions carry on; the questions of the connection - the host key, a password - are boxes that the
 * wait lets the person answer.  Only one operation may run at a time (roapp_op_begin), and only one
 * connection is supported: upstream's SFTP client keeps its table of requests in a global.
 */
#ifndef PUTTY_ROFTP_H
#define PUTTY_ROFTP_H

#include <stdbool.h>
#include <stdint.h>
#include "putty.h"

typedef struct FtConn FtConn;

/* One entry of a directory, as the server lists it. */
typedef struct FtEntry {
    char *name;                    /* the leaf, as the server sent it */
    bool dir;                      /* a directory (or a symbolic link to one) */
    bool link;                     /* a symbolic link */
    bool has_size, has_mtime, has_perms;
    uint64_t size;
    unsigned long mtime;           /* seconds since 1970 */
    unsigned long perms;           /* POSIX mode bits */
} FtEntry;

/* Progress of a transfer: called now and then with the bytes done of the total (0 if not known);
 * return false to cancel. */
typedef struct FtProgress {
    bool (*update)(void *ctx, uint64_t done, uint64_t total);
    void *ctx;
} FtProgress;

/* What the connection is doing, for the window to show: the last event of the log. */
typedef void (*FtStatusFn)(void *ctx, const char *text);

/*
 * Connect to the host in `conf` (which is copied) and start SFTP.  Returns NULL, with the reason in *err
 * (free it with sfree), if that fails or the connection is abandoned with ft_abort().  The boxes the
 * connection puts up (the host key, passwords) go with it when it is closed.
 */
FtConn *ft_connect(Conf *conf, FtStatusFn status, void *ctx, char **err);

/* Say goodbye to the server and free the connection. */
void    ft_close(FtConn *c);

/* Make whatever the connection is waiting for give up (it can be called from inside the wait). */
void    ft_abort(FtConn *c);

bool    ft_alive(const FtConn *c);
/* Why the connection broke, if it did (valid until ft_close). */
const char *ft_why_dead(const FtConn *c);
/* The server's name for "."; valid until ft_close. */
const char *ft_home(const FtConn *c);
/* The name and port the connection was made to, for titles; valid until ft_close. */
const char *ft_host(const FtConn *c);

/* A canonical absolute path for `path` (relative to `base`, which may be NULL if path is absolute);
 * malloc'd, or NULL with *err. */
char   *ft_realpath(FtConn *c, const char *base, const char *path, char **err);

/* The entries of a directory (not "." and ".."); free them with ft_free_entries. */
bool    ft_list(FtConn *c, const char *dir, FtEntry **out, size_t *n, char **err);
void    ft_free_entries(FtEntry *e, size_t n);

/* What a path is: false if it does not exist. */
bool    ft_stat(FtConn *c, const char *path, FtEntry *out);

bool    ft_mkdir(FtConn *c, const char *path, char **err);
bool    ft_rename(FtConn *c, const char *from, const char *to, char **err);
bool    ft_remove(FtConn *c, const char *path, bool is_dir, char **err);

/*
 * Copy a file down: `local` is the RISC OS path to make; `local_type` is the filetype to give it, or -1 to let
 * the contents decide.  A file that is only partly copied is removed again.
 */
bool    ft_download(FtConn *c, const char *remote, const char *local, int local_type, const FtProgress *pr,
                    char **err);
/* ... or up: `local` is a RISC OS path, `remote` the path to make. */
bool    ft_upload(FtConn *c, const char *local, const char *remote, const FtProgress *pr, char **err);

/* Join a directory and a leaf into a path. */
char   *ft_join(const char *dir, const char *leaf);

#endif /* PUTTY_ROFTP_H */
