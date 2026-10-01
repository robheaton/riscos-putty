/*
 * compat.c: standard C library functions that PuTTY uses but the Shared C
 * Library (as wrapped by GCCSDK's -mlibscl) does not provide.
 *
 * The multibyte functions treat the "system character set" as ISO 8859-1,
 * which is what DEFAULT_CODEPAGE means on this platform (see platform.h).
 */

#include <errno.h>
#include <stddef.h>
#include <wchar.h>

size_t wcslen(const wchar_t *s)
{
    const wchar_t *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps)
{
    if (!s)
        return 0;                      /* reset the state: we have none */
    if (n == 0)
        return (size_t)-2;
    if (pwc)
        *pwc = (unsigned char)*s;
    return *s ? 1 : 0;
}

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps)
{
    if (!s)
        return 1;
    if (wc < 0 || wc > 255) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    *s = (char)wc;
    return 1;
}
