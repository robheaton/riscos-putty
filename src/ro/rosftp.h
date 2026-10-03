/*
 * rosftp.h - the few RISC OS name-handling functions that psftp.c and pscp.c call
 * directly (from the call sites patches/psftp.patch and patches/pscp.patch change).
 * The rest of the interface is upstream's psftp.h, implemented in sftp.c.
 *
 * PuTTY's file transfer code passes file names about as plain strings, with '/' between
 * the parts of a path and '.' as the dot before an extension.  On RISC OS the separator
 * is '.', the extension dot is written '/', and a file's type is not part of its name.
 * So there are two kinds of name:
 *
 *  - a *local path*, as the user typed it and the file system understands it:
 *    ADFS::HardDisc4.$.Docs.notes/txt, or just Docs.notes/txt
 *  - a *leaf* (a single name, no directory), in the form it has on the remote Unix
 *    system: notes.txt, or notes.txt,fff when it should carry its RISC OS filetype
 *    (the ",xxx" convention of NFS, Samba and the FTP clients).
 *
 * Names the code gets from read_filename() and from the server are leaves; dir_file_cat()
 * turns a leaf into a local path.
 */
#ifndef PUTTY_ROSFTP_H
#define PUTTY_ROSFTP_H

#include <stdbool.h>

/*
 * The local name to give a file whose remote name is LEAF (the default when you say
 * "get notes.txt" without a name).  The result is in a small ring of buffers and is good
 * until a few more calls have been made; do not free it.
 */
char *ro_remote_leaf_to_local(const char *leaf);

/*
 * The remote leaf name for the local file PATH (the default when you say "put" or "pscp"
 * without a destination name), with ",xxx" added if it has a filetype other than Text or
 * Data.  Same lifetime as above.
 */
char *ro_local_path_to_remote_leaf(const char *path);

/* The last part of a local path, for showing to the user.  Points into PATH. */
const char *ro_local_leaf(const char *path);

/*
 * pscp: is SPEC (say "Docs.file" or "ADFS::4.$.file" or "Choices:PuTTY.x") a local file
 * and not host:path?  A colon after a filing system name or a path variable is local.
 */
bool ro_looks_local(const char *spec);

/*
 * For the Wimp file transfer window (rofile.c):
 *
 *  - the RISC OS filetype a remote leaf implies (its ",xxx" suffix, else a well-known extension), -1 if none;
 *  - the RISC OS leaf to save a remote file under (converted, no ",xxx"); free it with sfree;
 *  - the remote leaf for a local leaf and filetype (-1: none);  free it with sfree;
 *  - what a local object is: 0 none, 1 file, 2 directory; with its filetype, size and mtime if wanted.
 */
int   ro_filetype_for_remote_leaf(const char *leaf);
char *ro_remote_leaf_to_save_leaf(const char *leaf);
char *ro_local_leaf_to_remote_leaf(const char *local_leaf, int filetype);
int   ro_local_stat(const char *path, int *filetype, unsigned long *size, unsigned long *mtime);

#endif /* PUTTY_ROSFTP_H */
