/*
 * rofont.h - the terminal's font: a fixed-pitch RISC OS outline font, its
 * bold sibling, and the cell geometry derived from them.
 *
 * All sizes are in OS units except cell_w_mp (millipoints: 400 per OS unit),
 * which is kept exact because a font's advance is rarely a whole number of OS
 * units; rofont_cx() turns a column number into an x offset without letting
 * rounding errors accumulate along a row.
 */
#ifndef PUTTY_ROFONT_H
#define PUTTY_ROFONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct RoFont {
    int  handle[2];            /* Font Manager handles: [0] normal, [1] bold (may equal [0]) */
    bool has_bold;             /* [1] really is a different, bold face */
    bool utf8;                 /* strings are painted as UTF-8 (else Latin-1) */
    int  cell_w_mp;            /* advance of one cell, millipoints */
    int  cell_h;               /* height of one cell */
    int  baseline;             /* from the top of a cell down to the baseline */
    int  under_y;              /* ... down to the underline */
    int  strike_y;             /* ... down to the strike-through */
    char spec[100];
} RoFont;

/* "Font.Name,size" (size in points, default 12).  False, with a message in err
 * (if given), if the font cannot be found. */
bool rofont_open(RoFont *f, const char *spec, char *err, size_t errlen);
void rofont_close(RoFont *f);

/* x offset of a column's left edge from the left edge of column 0 */
static inline int rofont_cx(const RoFont *f, int col)
{
    return (int)(((int64_t)col * f->cell_w_mp) / 400);
}

/* Set the colours the next paint uses (palette words 0xBBGGRR00). */
void rofont_colours(const RoFont *f, bool bold, uint32_t bg, uint32_t fg);

/* Paint a string with its baseline origin at screen position (x, y).  The
 * string is UTF-8 if f->utf8, else Latin-1; len is in bytes. */
void rofont_paint(const RoFont *f, bool bold, const char *s, size_t len, int x, int y);

#endif /* PUTTY_ROFONT_H */
