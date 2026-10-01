/*
 * fonttest.c - dev tool: what does the Font Manager say about a font?
 *   fonttest [fontname] [points...]
 * Prints the advance widths of a few strings (to check the font is fixed
 * pitch), the font's bounding box, and the screen's eigen factors.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "swis.h"

#define Font_FindFont 0x40081
#define Font_LoseFont 0x40082
#define Font_ReadInfo 0x40084
#define Font_StringWidth 0x40085
#define Font_SetFont 0x4008A
#define Font_ReadScaleFactor 0x4008F

static int err(const char *what, _kernel_oserror *e)
{
    if (e) { printf("%s: ERROR %08x %s\n", what, (unsigned)e->errnum, e->errmess); return 1; }
    return 0;
}

static int width_mp(const char *s)
{
    int w = 0, h = 0;
    _kernel_oserror *e = _swix(Font_StringWidth, _INR(1, 5) | _OUTR(2, 3), s, 0x7fffffff, 0x7fffffff, -1, (int)strlen(s), &w, &h);
    if (e) { err("StringWidth", e); return -1; }
    return w;
}

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "Corpus.Medium";
    int sizes[8], n = 0, i;
    int xeig, yeig, xlim, ylim, bpp;
    for (i = 2; i < argc && n < 8; i++) sizes[n++] = atoi(argv[i]);
    if (n == 0) { sizes[0] = 10; sizes[1] = 12; sizes[2] = 14; n = 3; }

    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 4, &xeig);
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 5, &yeig);
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 11, &xlim);
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 12, &ylim);
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 9, &bpp);
    printf("screen: %d x %d pixels, xeig %d yeig %d (1 pixel = %d x %d OS units), log2bpp %d\n",
           xlim + 1, ylim + 1, xeig, yeig, 1 << xeig, 1 << yeig, bpp);

    for (i = 0; i < n; i++) {
        int pt = sizes[i], h = 0, xr = 0, yr = 0;
        char full[80];
        int x0, y0, x1, y1, xf, yf;
        sprintf(full, "%s", name);
        if (err("FindFont", _swix(Font_FindFont, _IN(1) | _INR(2, 5) | _OUT(0) | _OUTR(4, 5), full, pt * 16, pt * 16, 0, 0, &h, &xr, &yr)))
            return 1;
        _swix(Font_SetFont, _IN(0), h);
        printf("%s at %d pt: handle %d (resolution %d x %d dpi)\n", name, pt, h, xr, yr);
        printf("  width mp: 'M'=%d 'i'=%d 'W'=%d 'iiiiiiiiii'=%d 'MMMMMMMMMM'=%d ' '=%d\n",
               width_mp("M"), width_mp("i"), width_mp("W"), width_mp("iiiiiiiiii"),
               width_mp("MMMMMMMMMM"), width_mp(" "));
        if (!err("ReadInfo", _swix(Font_ReadInfo, _IN(0) | _OUTR(1, 4), h, &x0, &y0, &x1, &y1)))
            printf("  bbox (ReadInfo): %d %d %d %d\n", x0, y0, x1, y1);
        if (!err("ReadScaleFactor", _swix(Font_ReadScaleFactor, _OUTR(1, 2), &xf, &yf)))
            printf("  scale factor (internal units per OS unit?): %d %d\n", xf, yf);
        _swix(Font_LoseFont, _IN(0), h);
    }
    return 0;
}
