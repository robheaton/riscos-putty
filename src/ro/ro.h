/*
 * ro.h - the small slice of the RISC OS API the port uses.
 *
 * SWIs are called through _swix() (kernel.h/swis.h from the toolchain), so
 * there is no dependency on OSLib.
 */
#ifndef PUTTY_RO_H
#define PUTTY_RO_H

#include <stddef.h>
#include <stdint.h>
#include "kernel.h"
#include "swis.h"

/* SWIs missing from swis.h (values from OSLib's resolver.h). */
#define Resolver_GetHostByName 0x46000
#define Resolver_GetHost       0x46001

/* Internet module errors are (0x20E00 + errno) on newer OS versions and the
 * bare errno on older ones. */
#define RO_SOCKET_ERR_BASE 0x20E00u

/* BSD errno values as the RISC OS Internet module uses them. */
#define RO_EINTR        4
#define RO_EINVAL       22
#define RO_EAGAIN       11
#define RO_EWOULDBLOCK  35
#define RO_EINPROGRESS  36
#define RO_EALREADY     37
#define RO_EADDRINUSE   48
#define RO_ENETUNREACH  51
#define RO_ECONNABORTED 53
#define RO_ECONNRESET   54
#define RO_ENOBUFS      55
#define RO_EISCONN      56
#define RO_ENOTCONN     57
#define RO_ETIMEDOUT    60
#define RO_ECONNREFUSED 61
#define RO_EHOSTDOWN    64
#define RO_EHOSTUNREACH 65

static inline int ro_errno(const _kernel_oserror *e)
{
    return (int)(e->errnum & ~RO_SOCKET_ERR_BASE);
}

/* Centisecond monotonic clock (wraps every 497 days). */
static inline uint32_t ro_time(void)
{
    int t = 0;
    _swix(OS_ReadMonotonicTime, _OUT(0), &t);
    return (uint32_t)t;
}

/* True if the only entropy is timing jitter and the seed file (see noise.c). */
#include <stdbool.h>
bool ro_entropy_is_weak(void);

/* Filetypes the port writes. */
#define FILETYPE_TEXT 0xFFF
#define FILETYPE_DATA 0xFFD

/* Set the RISC OS filetype of a file (OS_File 18). */
static inline void ro_set_filetype(const char *path, int filetype)
{
    _swix(OS_File, _INR(0, 2), 18, path, filetype);
}

/*
 * A name for a scratch file in the same directory as PATH (malloc'd; free it with
 * sfree).  Upstream's tools write "<name>.tmp" and then rename it over the original,
 * but a dot is a directory separator here, so that would name a file inside <name>.
 */
char *ro_temp_sibling(const char *path);

/*
 * n bytes from the CryptRandom module; false if it is not there or not working
 * (see noise.c), in which case nothing useful has been written to buf.
 */
bool ro_cryptrandom_bytes(void *buf, size_t n);

#endif /* PUTTY_RO_H */
