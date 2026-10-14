/*
 * rowindow.c - a PuTTY terminal in a Wimp window.
 *
 * PuTTY's Terminal draws through the TermWin interface: "draw this run of text
 * at cell (x, y) with these attributes".  We do not paint then and there; we
 * record what was drawn in a grid of cells and mark them dirty, and paint from
 * the grid - at the end of each batch of terminal output, and whenever the Wimp
 * asks for a redraw.  That way a redraw is always right, whatever the terminal
 * believes is already on screen.
 *
 * Scrolling.  The terminal does its own scrollback: it tells us (set_scrollbar)
 * how many lines there are and which is at the top, and we are told when the user
 * moves the scroll bar (term_scroll).  The Wimp window's extent is made as tall as
 * the whole scrollback so that its scroll bar has the right proportions, but its
 * contents are never really scrolled: rows are always painted from the top of the
 * visible area, and every change of the scroll offset is followed by a full redraw
 * (the Wimp copies the old pixels when the offset changes, which is wrong for us).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "ssh.h"
#include "terminal.h"
#include "ro.h"
#include "rowimp.h"
#include "rofont.h"
#include "roterm.h"
#include "rodialog.h"
#include "roclip.h"
#include "rocontext.h"
#include "roconfig.h"

#define WIN_MIN_COLS 20
#define WIN_MIN_ROWS 4
#define SCROLL_LINES 3                 /* lines per wheel notch or scroll arrow click */
#define MULTICLICK_CS 40               /* clicks closer together than this count as double/triple */

/* INKEY numbers for OS_Byte 129 (not the numbers key events use, and one more than the scan numbers of
 * OS_Byte 121: see tests/ro/keyscan.c) */
enum { K_LSHIFT = 4, K_LCTRL = 5, K_LALT = 6, K_RSHIFT = 7, K_RCTRL = 8, K_RALT = 9,
       K_UP = 58, K_DOWN = 42, K_HOME = 63, K_PGUP = 64, K_PGDN = 79 };

bool roterm_cmdline_session;
static RoTerm *terms;
static int term_counter;
static int xeig = 1, yeig = 1, screen_w = 3200, screen_h = 2400;   /* OS units */

/* ------------------------------------------------------------------------ */
/* small helpers                                                            */

static int mode_var(int var, int dflt)
{
    int v = dflt;
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, var, &v);
    return v;
}

static void read_screen(void)
{
    xeig = mode_var(4, 1);
    yeig = mode_var(5, 1);
    screen_w = (mode_var(11, 1599) + 1) << xeig;
    screen_h = (mode_var(12, 1199) + 1) << yeig;
}

static int key_down(int n)
{
    int r1 = 0;
    _swix(OS_Byte, _INR(0, 2) | _OUT(1), 129, (-n) & 0xFF, 0xFF, &r1);
    return (r1 & 0xFF) == 0xFF;
}

static bool shift_down(void) { return key_down(K_LSHIFT) || key_down(K_RSHIFT); }
static bool ctrl_down(void)  { return key_down(K_LCTRL) || key_down(K_RCTRL); }
static bool alt_down(void)   { return key_down(K_LALT) || key_down(K_RALT); }
/* Meta, which makes keys send an ESC first: the left Alt key only, since the right one
 * (AltGr) composes characters on many keyboards. */
static bool meta_down(void)  { return key_down(K_LALT); }

static uint32_t rgbword(unsigned r, unsigned g, unsigned b)
{
    return (b << 24) | (g << 16) | (r << 8);
}

static inline RoCell *cell_at(RoTerm *t, int x, int y)
{
    return &t->cells[y * t->cols + x];
}

static void mark_dirty(RoTerm *t, int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= t->cols) x1 = t->cols - 1;
    if (y1 >= t->rows) y1 = t->rows - 1;
    if (x0 > x1 || y0 > y1) return;
    if (t->dx0 > t->dx1) {
        t->dx0 = x0; t->dy0 = y0; t->dx1 = x1; t->dy1 = y1;
        return;
    }
    if (x0 < t->dx0) t->dx0 = x0;
    if (y0 < t->dy0) t->dy0 = y0;
    if (x1 > t->dx1) t->dx1 = x1;
    if (y1 > t->dy1) t->dy1 = y1;
}

static void clear_dirty(RoTerm *t)
{
    t->dx0 = 1; t->dx1 = 0; t->dy0 = 1; t->dy1 = 0;
}

static int win_width(const RoTerm *t)  { return rofont_cx(&t->font, t->cols); }
static int win_height(const RoTerm *t) { return t->rows * t->font.cell_h; }

/* The Wimp will not open a window bigger than its work area, so the work area is
 * made at least as big as the screen: the terminal can then be dragged larger.
 * (Everything outside the character grid is painted in the background colour.) */
static int extent_w(const RoTerm *t)
{
    const int w = win_width(t);
    return w > screen_w ? w : screen_w;
}

/* The scroll bar's thumb is the window's height as a fraction of the work area's, so with a
 * screen-high work area the thumb looks smaller than it should while the scrollback is short.
 * So the work area is only that tall while a resize or a maximise may be starting
 * (wide_extent; see roterm_idle); otherwise it is just the lines there really are. */
static int extent_h(const RoTerm *t)
{
    const int total = t->scrollbar && t->sb_total > t->rows ? t->sb_total : t->rows;
    const int h = total * t->font.cell_h;
    if (t->scrollbar && !t->wide_extent) return h;
    return h > screen_h ? h : screen_h;
}

/* The column whose cell contains offset dx from the left edge (clamped to >= 0). */
static int col_at(const RoFont *f, int dx)
{
    int col;
    if (dx <= 0) return 0;
    col = (int)(((int64_t)dx * 400) / f->cell_w_mp);
    while (rofont_cx(f, col + 1) <= dx) col++;
    while (col > 0 && rofont_cx(f, col) > dx) col--;
    return col;
}

static void default_cell(RoCell *c)
{
    memset(c, 0, sizeof *c);
    c->ch = ' ';
    c->attr = ATTR_DEFAULT;
}

static void reset_grid(RoTerm *t)
{
    int i;
    for (i = 0; i < t->cols * t->rows; i++) default_cell(&t->cells[i]);
    mark_dirty(t, 0, 0, t->cols - 1, t->rows - 1);
}

static bool alloc_grid(RoTerm *t, int cols, int rows)
{
    RoCell *n = (RoCell *)malloc((size_t)cols * (size_t)rows * sizeof(RoCell));
    if (!n) return false;
    free(t->cells);
    t->cells = n;
    t->cols = cols;
    t->rows = rows;
    reset_grid(t);
    return true;
}

/* ------------------------------------------------------------------------ */
/* painting                                                                 */

static uint32_t last_gcol = 0xFFFFFFFFu;

static void set_gcol(uint32_t rgb)
{
    if (rgb == last_gcol) return;
    _swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), rgb, 0, 0);
    last_gcol = rgb;
}

/* Fill [x0,x1) x [y0,y1) in screen OS units. */
static void fill(uint32_t rgb, int x0, int y0, int x1, int y1)
{
    if (x1 <= x0 || y1 <= y0) return;
    set_gcol(rgb);
    _swix(OS_Plot, _INR(0, 2), 4, x0, y0);
    _swix(OS_Plot, _INR(0, 2), 0x65, x1 - 1, y1 - 1);
}

static size_t wc_to_utf8(int wc, char *out)
{
    unsigned c = (unsigned)wc;
    if (c < 0x80)    { out[0] = (char)c; return 1; }
    if (c < 0x800)   { out[0] = (char)(0xC0 | (c >> 6)); out[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12)); out[1] = (char)(0x80 | ((c >> 6) & 63));
        out[2] = (char)(0x80 | (c & 63)); return 3;
    }
    out[0] = (char)(0xF0 | (c >> 18)); out[1] = (char)(0x80 | ((c >> 12) & 63));
    out[2] = (char)(0x80 | ((c >> 6) & 63)); out[3] = (char)(0x80 | (c & 63));
    return 4;
}

typedef struct {
    uint32_t fg, bg;           /* palette words */
    bool     bold_font;
    bool     under, strike;
    bool     curs_passive, curs_active_line;     /* extra cursor decoration */
} Style;

static uint32_t dim23(uint32_t w)
{
    return rgbword(((w >> 8) & 255) * 2 / 3, ((w >> 16) & 255) * 2 / 3, ((w >> 24) & 255) * 2 / 3);
}

/* Work out how a cell is drawn: the same rules as PuTTY's other front ends. */
static void resolve(const RoTerm *t, const RoCell *c, Style *s)
{
    unsigned long attr = c->attr;
    truecolour tc = c->tc;
    int nfg = (int)((attr & ATTR_FGMASK) >> ATTR_FGSHIFT);
    int nbg = (int)((attr & ATTR_BGMASK) >> ATTR_BGSHIFT);

    s->curs_passive = s->curs_active_line = false;
    if (c->curs) {
        if (attr & ATTR_PASCURS) { attr &= ~ATTR_PASCURS; s->curs_passive = true; }
        if ((attr & ATTR_ACTCURS) && t->cursor_type != CURSOR_BLOCK) {
            attr &= ~ATTR_ACTCURS;
            s->curs_active_line = true;
        }
    }
    if (attr & ATTR_REVERSE) {
        int tmp = nfg; nfg = nbg; nbg = tmp;
        { optionalrgb o = tc.fg; tc.fg = tc.bg; tc.bg = o; }
    }
    if ((t->bold_style & BOLD_STYLE_COLOUR) && (attr & ATTR_BOLD)) {
        if (nfg < 16) nfg |= 8; else if (nfg >= 256) nfg |= 1;
    }
    if ((t->bold_style & BOLD_STYLE_COLOUR) && (attr & ATTR_BLINK)) {
        if (nbg < 16) nbg |= 8; else if (nbg >= 256) nbg |= 1;
    }
    if (attr & ATTR_ACTCURS) {
        tc.fg.enabled = tc.bg.enabled = false;
        nfg = 260; nbg = 261;
        attr &= ~ATTR_DIM;
    }
    s->fg = tc.fg.enabled ? rgbword(tc.fg.r, tc.fg.g, tc.fg.b) : t->pal[nfg];
    s->bg = tc.bg.enabled ? rgbword(tc.bg.r, tc.bg.g, tc.bg.b) : t->pal[nbg];
    if (attr & ATTR_DIM) { s->fg = dim23(s->fg); s->bg = dim23(s->bg); }
    s->bold_font = (attr & ATTR_BOLD) && (t->bold_style & BOLD_STYLE_FONT);
    s->under = (attr & ATTR_UNDER) != 0;
    s->strike = (attr & ATTR_STRIKE) != 0;
}

static bool style_equal(const Style *a, const Style *b)
{
    return a->fg == b->fg && a->bg == b->bg && a->bold_font == b->bold_font &&
           a->under == b->under && a->strike == b->strike &&
           a->curs_passive == b->curs_passive && a->curs_active_line == b->curs_active_line;
}

/* The trust sigil: a framed key-like block, drawn by us rather than by anything
 * the server can send, so a server cannot fake it. */
static void draw_sigil(const RoTerm *t, uint32_t fg, uint32_t bg, int x0, int y0, int x1, int y1)
{
    const int px = 1 << xeig, py = 1 << yeig;
    int w = x1 - x0, h = y1 - y0;
    int ix0 = x0 + w / 5, ix1 = x1 - w / 5, iy0 = y0 + h / 6, iy1 = y1 - h / 6;
    fill(fg, ix0, iy0, ix1, iy1);
    fill(bg, ix0 + px, iy0 + py, ix1 - px, iy1 - py);
    fill(fg, ix0 + 2 * px, iy0 + 2 * py, ix1 - 2 * px, iy1 - 2 * py);
    (void)t;
}

/* ---- box drawing and block elements ------------------------------------
 *
 * The fonts have none of U+2500-U+259F, and curses programs lean on them, so they
 * are drawn with rectangles.  That also makes lines join up from cell to cell
 * whatever the font and its size.  A line character is four arms (left, right,
 * up, down) each with a style: none, light, heavy or double. */

#define BX(l, r, u, d) ((unsigned char)((l) | ((r) << 2) | ((u) << 4) | ((d) << 6)))
static const unsigned char box_tab[0x80] = {
    /* 2500 */ BX(1,1,0,0), BX(2,2,0,0), BX(0,0,1,1), BX(0,0,2,2),
    /* 2504 */ BX(1,1,0,0), BX(2,2,0,0), BX(0,0,1,1), BX(0,0,2,2),      /* dashed: drawn solid */
    /* 2508 */ BX(1,1,0,0), BX(2,2,0,0), BX(0,0,1,1), BX(0,0,2,2),
    /* 250C */ BX(0,1,0,1), BX(0,2,0,1), BX(0,1,0,2), BX(0,2,0,2),
    /* 2510 */ BX(1,0,0,1), BX(2,0,0,1), BX(1,0,0,2), BX(2,0,0,2),
    /* 2514 */ BX(0,1,1,0), BX(0,2,1,0), BX(0,1,2,0), BX(0,2,2,0),
    /* 2518 */ BX(1,0,1,0), BX(2,0,1,0), BX(1,0,2,0), BX(2,0,2,0),
    /* 251C */ BX(0,1,1,1), BX(0,2,1,1), BX(0,1,2,1), BX(0,1,1,2),
               BX(0,1,2,2), BX(0,2,2,1), BX(0,2,1,2), BX(0,2,2,2),
    /* 2524 */ BX(1,0,1,1), BX(2,0,1,1), BX(1,0,2,1), BX(1,0,1,2),
               BX(1,0,2,2), BX(2,0,2,1), BX(2,0,1,2), BX(2,0,2,2),
    /* 252C */ BX(1,1,0,1), BX(2,1,0,1), BX(1,2,0,1), BX(2,2,0,1),
               BX(1,1,0,2), BX(2,1,0,2), BX(1,2,0,2), BX(2,2,0,2),
    /* 2534 */ BX(1,1,1,0), BX(2,1,1,0), BX(1,2,1,0), BX(2,2,1,0),
               BX(1,1,2,0), BX(2,1,2,0), BX(1,2,2,0), BX(2,2,2,0),
    /* 253C */ BX(1,1,1,1), BX(2,1,1,1), BX(1,2,1,1), BX(2,2,1,1),
    /* 2540 */ BX(1,1,2,1), BX(1,1,1,2), BX(1,1,2,2), BX(2,1,2,1),
               BX(1,2,2,1), BX(2,1,1,2), BX(1,2,1,2), BX(2,2,2,1),
    /* 2548 */ BX(2,2,1,2), BX(2,1,2,2), BX(1,2,2,2), BX(2,2,2,2),
    /* 254C */ BX(1,1,0,0), BX(2,2,0,0), BX(0,0,1,1), BX(0,0,2,2),
    /* 2550 */ BX(3,3,0,0), BX(0,0,3,3),
    /* 2552 */ BX(0,3,0,1), BX(0,1,0,3), BX(0,3,0,3),
    /* 2555 */ BX(3,0,0,1), BX(1,0,0,3), BX(3,0,0,3),
    /* 2558 */ BX(0,3,1,0), BX(0,1,3,0), BX(0,3,3,0),
    /* 255B */ BX(3,0,1,0), BX(1,0,3,0), BX(3,0,3,0),
    /* 255E */ BX(0,3,1,1), BX(0,1,3,3), BX(0,3,3,3),
    /* 2561 */ BX(3,0,1,1), BX(1,0,3,3), BX(3,0,3,3),
    /* 2564 */ BX(3,3,0,1), BX(1,1,0,3), BX(3,3,0,3),
    /* 2567 */ BX(3,3,1,0), BX(1,1,3,0), BX(3,3,3,0),
    /* 256A */ BX(3,3,1,1), BX(1,1,3,3), BX(3,3,3,3),
    /* 256D */ BX(0,1,0,1), BX(1,0,0,1), BX(1,0,1,0), BX(0,1,1,0),      /* rounded: drawn square */
    /* 2571 */ 0, 0, 0,                                                  /* diagonals: drawn as lines */
    /* 2574 */ BX(1,0,0,0), BX(0,0,1,0), BX(0,1,0,0), BX(0,0,0,1),
               BX(2,0,0,0), BX(0,0,2,0), BX(0,2,0,0), BX(0,0,0,2),
    /* 257C */ BX(1,2,0,0), BX(0,0,1,2), BX(2,1,0,0), BX(0,0,2,1),
};

/* quadrants for U+2596-U+259F: 1 upper left, 2 upper right, 4 lower left, 8 lower right */
static const unsigned char quad_tab[10] = { 4, 8, 1, 13, 9, 7, 11, 2, 6, 14 };

static bool is_custom_glyph(int ch)
{
    return ch >= 0x2500 && ch <= 0x259F;
}

/* The strokes of one arm across its width, as offsets from the centre pixel:
 * light is one pixel, heavy three, double two pixels with one between. */
static int arm_strokes(int style, int unit, int off[2][2])
{
    switch (style) {
      case 1: off[0][0] = 0;     off[0][1] = unit;     return 1;
      case 2: off[0][0] = -unit; off[0][1] = 2 * unit; return 1;
      case 3: off[0][0] = -unit; off[0][1] = 0;
              off[1][0] = unit;  off[1][1] = 2 * unit; return 2;
    }
    return 0;
}

/* a colour part-way from bg to fg (pct of fg), in palette-word form */
static uint32_t blend(uint32_t fg, uint32_t bg, int pct)
{
    uint32_t out = 0;
    int sh;
    for (sh = 8; sh <= 24; sh += 8) {
        const int a = (int)((fg >> sh) & 255), b = (int)((bg >> sh) & 255);
        out |= (uint32_t)(b + (a - b) * pct / 100) << sh;
    }
    return out;
}

static void line(uint32_t rgb, int x0, int y0, int x1, int y1)
{
    set_gcol(rgb);
    _swix(OS_Plot, _INR(0, 2), 4, x0, y0);
    _swix(OS_Plot, _INR(0, 2), 5, x1, y1);
}

/* Draw glyph ch in the cell [x0,x1) x [y0,y1) (screen OS units, y up). */
static void draw_glyph(int ch, uint32_t fg, uint32_t bg, int x0, int y0, int x1, int y1)
{
    const int px = 1 << xeig, py = 1 << yeig;
    const int w = x1 - x0, h = y1 - y0;

    if (ch < 0x2580) {
        const unsigned code = box_tab[ch - 0x2500];
        const int sL = code & 3, sR = (code >> 2) & 3, sU = (code >> 4) & 3, sD = (code >> 6) & 3;
        const int cx = x0 + (w / 2 / px) * px, cy = y0 + (h / 2 / py) * py;   /* the centre pixel */
        int off[2][2], vA = 0, vB = px, hA = 0, hB = py, k, n;

        if (ch >= 0x2571 && ch <= 0x2573) {
            if (ch != 0x2572) line(fg, x0, y0, x1 - 1, y1 - 1);
            if (ch != 0x2571) line(fg, x0, y1 - 1, x1 - 1, y0);
            return;
        }
        /* how far the strokes across the middle reach, so the arms meet them */
        if (sU || sD) {
            int st[2] = { sU, sD };
            for (k = 0; k < 2; k++) {
                if (!st[k]) continue;
                if (st[k] >= 2) { if (-px < vA) vA = -px; if (2 * px > vB) vB = 2 * px; }
            }
        }
        if (sL || sR) {
            int st[2] = { sL, sR };
            for (k = 0; k < 2; k++) {
                if (!st[k]) continue;
                if (st[k] >= 2) { if (-py < hA) hA = -py; if (2 * py > hB) hB = 2 * py; }
            }
        }
        if (sL) for (n = arm_strokes(sL, py, off), k = 0; k < n; k++)
            fill(fg, x0, cy + off[k][0], cx + vB, cy + off[k][1]);
        if (sR) for (n = arm_strokes(sR, py, off), k = 0; k < n; k++)
            fill(fg, cx + vA, cy + off[k][0], x1, cy + off[k][1]);
        if (sU) for (n = arm_strokes(sU, px, off), k = 0; k < n; k++)
            fill(fg, cx + off[k][0], cy + hA, cx + off[k][1], y1);
        if (sD) for (n = arm_strokes(sD, px, off), k = 0; k < n; k++)
            fill(fg, cx + off[k][0], y0, cx + off[k][1], cy + hB);
        return;
    }

    /* block elements */
    {
        const int mx = x0 + (w / 2 / px) * px, my = y0 + (h / 2 / py) * py;
        if (ch == 0x2580) {                                  /* upper half */
            fill(fg, x0, my, x1, y1);
        } else if (ch <= 0x2587) {                           /* lower 1/8 .. 7/8 */
            fill(fg, x0, y0, x1, y0 + ((h * (ch - 0x2580)) / 8 / py) * py);
        } else if (ch == 0x2588) {                           /* full block */
            fill(fg, x0, y0, x1, y1);
        } else if (ch <= 0x258F) {                           /* left 7/8 .. 1/8 */
            fill(fg, x0, y0, x0 + ((w * (0x2590 - ch)) / 8 / px) * px, y1);
        } else if (ch == 0x2590) {                           /* right half */
            fill(fg, mx, y0, x1, y1);
        } else if (ch <= 0x2593) {                           /* light, medium, dark shade */
            fill(blend(fg, bg, 25 * (ch - 0x2590)), x0, y0, x1, y1);
        } else if (ch == 0x2594) {                           /* upper 1/8 */
            fill(fg, x0, y1 - ((h / 8) / py) * py, x1, y1);
        } else if (ch == 0x2595) {                           /* right 1/8 */
            fill(fg, x1 - ((w / 8) / px) * px, y0, x1, y1);
        } else {                                             /* quadrants */
            const unsigned q = quad_tab[ch - 0x2596];
            if (q & 1) fill(fg, x0, my, mx, y1);
            if (q & 2) fill(fg, mx, my, x1, y1);
            if (q & 4) fill(fg, x0, y0, mx, my);
            if (q & 8) fill(fg, mx, y0, x1, my);
        }
    }
}

/* Paint cells [xa, xb) of row y, which share one style; top/bottom are the row's
 * screen y range, ox the screen x of column 0. */
static void paint_run(RoTerm *t, int ox, int top, int bottom, int y, int xa, int xb, const Style *s)
{
    const RoFont *f = &t->font;
    const int px = 1 << xeig, py = 1 << yeig;
    const int rx0 = ox + rofont_cx(f, xa), rx1 = ox + rofont_cx(f, xb);
    int x;
    bool any_text = false, all_ascii = true;
    RoCell *row = cell_at(t, 0, y);

    fill(s->bg, rx0, bottom, rx1, top);

    for (x = xa; x < xb; x++) {
        int ch = row[x].ch;
        if (ch > 0x20) any_text = true;
        if (ch > 0x7E || ch < 0) all_ascii = false;
    }
    if (any_text) {
        const int base = top - f->baseline;
        rofont_colours(f, s->bold_font, s->bg, s->fg);
        if (all_ascii) {
            char buf[256];
            int n = 0;
            for (x = xa; x < xb && n < (int)sizeof buf; x++) {
                int ch = row[x].ch;
                buf[n++] = (char)(ch < 0x20 ? ' ' : ch);
            }
            rofont_paint(f, s->bold_font, buf, (size_t)n, rx0, base);
        } else {
            /* One character at a time, so a character the font lacks cannot
             * shift everything after it out of its column. */
            for (x = xa; x < xb; x++) {
                int ch = row[x].ch;
                char u[8];
                size_t n;
                if (ch <= 0x20 || row[x].wide_rhs || is_custom_glyph(ch)) continue;
                n = f->utf8 ? wc_to_utf8(ch, u) : 1;
                if (!f->utf8) u[0] = (char)(ch < 0x100 ? ch : '?');
                rofont_paint(f, s->bold_font, u, n, ox + rofont_cx(f, x), base);
            }
        }
    }
    for (x = xa; x < xb; x++)
        if (is_custom_glyph(row[x].ch))
            draw_glyph(row[x].ch, s->fg, s->bg, ox + rofont_cx(f, x), bottom,
                       ox + rofont_cx(f, x + 1), top);

    if (s->under)  fill(s->fg, rx0, top - f->under_y - py, rx1, top - f->under_y);
    if (s->strike) fill(s->fg, rx0, top - f->strike_y - py, rx1, top - f->strike_y);

    for (x = xa; x < xb; x++) {
        if (row[x].sigil) {
            int cx0 = ox + rofont_cx(f, x), cx1 = ox + rofont_cx(f, x + 1);
            draw_sigil(t, s->fg, s->bg, cx0, bottom, cx1, top);
        }
        if (row[x].curs && (s->curs_passive || s->curs_active_line)) {
            const uint32_t cc = t->pal[261];
            int cx0 = ox + rofont_cx(f, x), cx1 = ox + rofont_cx(f, x + 1);
            if (t->cursor_type == CURSOR_BLOCK) {
                fill(cc, cx0, top - py, cx1, top);
                fill(cc, cx0, bottom, cx1, bottom + py);
                fill(cc, cx0, bottom, cx0 + px, top);
                fill(cc, cx1 - px, bottom, cx1, top);
            } else if (t->cursor_type == CURSOR_UNDERLINE) {
                fill(cc, cx0, top - f->under_y - py, cx1, top - f->under_y);
            } else {
                int xl = (row[x].attr & ATTR_RIGHTCURS) ? cx1 - px : cx0;
                fill(cc, xl, bottom, xl + px, top);
            }
        }
    }
}

static void paint_row(RoTerm *t, int ox, int oy, int y, int xa, int xb)
{
    const int top = oy - y * t->font.cell_h, bottom = top - t->font.cell_h;
    int x = xa;
    while (x <= xb) {
        Style s, s2;
        int xe = x + 1;
        resolve(t, cell_at(t, x, y), &s);
        while (xe <= xb) {
            resolve(t, cell_at(t, xe, y), &s2);
            if (!style_equal(&s, &s2)) break;
            xe++;
        }
        paint_run(t, ox, top, bottom, y, x, xe, &s);
        x = xe;
    }
}

/* Paint whatever part of the grid lies inside clip (screen coordinates); (ox, oy)
 * is the screen position of the grid's top left corner. */
static void paint_cells(RoTerm *t, int ox, int oy, const wbox *clip)
{
    const RoFont *f = &t->font;
    const int gw = win_width(t), gh = win_height(t);
    int ya, yb, xa, xb, y;

    last_gcol = 0xFFFFFFFFu;

    /* the part of the clip outside the grid (a window bigger than the terminal) */
    if (clip->x1 > ox + gw)
        fill(t->pal[OSC4_COLOUR_bg], ox + gw > clip->x0 ? ox + gw : clip->x0, clip->y0, clip->x1, clip->y1);
    if (clip->y0 < oy - gh)
        fill(t->pal[OSC4_COLOUR_bg], clip->x0, clip->y0, clip->x1 < ox + gw ? clip->x1 : ox + gw,
             clip->y1 < oy - gh ? clip->y1 : oy - gh);

    ya = (oy - clip->y1) / f->cell_h;
    if (oy - clip->y1 < 0) ya = 0;
    yb = (oy - clip->y0 - 1) / f->cell_h;
    xa = col_at(f, clip->x0 - ox);
    xb = col_at(f, clip->x1 - 1 - ox);
    if (ya < 0) ya = 0;
    if (yb >= t->rows) yb = t->rows - 1;
    if (xb >= t->cols) xb = t->cols - 1;
    for (y = ya; y <= yb; y++) paint_row(t, ox, oy, y, xa, xb);
}

void roterm_redraw(RoTerm *t, wevent *e)
{
    wdraw *d = (wdraw *)e->words;
    int more = 0;
    if (_swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), d, &more)) return;
    while (more) {
        paint_cells(t, d->visible.x0, d->visible.y1, &d->clip);
        if (_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), d, &more)) break;
    }
}

static void force_redraw_all(RoTerm *t)
{
    wbox ext;
    ext.x0 = 0; ext.x1 = extent_w(t); ext.y1 = 0; ext.y0 = -extent_h(t);
    if (t->win >= 0)
        _swix(Wimp_ForceRedraw, _INR(0, 4), t->win, ext.x0, ext.y0, ext.x1, ext.y1);
}

/* Paint the changed cells now, in place. */
static void flush(RoTerm *t)
{
    wstate s;
    wdraw d;
    int more = 0;
    if (t->win < 0 || t->dx0 > t->dx1) return;
    s.handle = t->win;
    if (_swix(Wimp_GetWindowState, _IN(1), &s)) return;
    if (!(s.flags & WF_OPEN)) { clear_dirty(t); return; }
    d.handle = t->win;
    /* work area coordinates of the dirty cells, given the current scroll offset */
    d.visible.x0 = rofont_cx(&t->font, t->dx0);
    d.visible.x1 = rofont_cx(&t->font, t->dx1 + 1);
    d.visible.y1 = s.scroll_y - t->dy0 * t->font.cell_h;
    d.visible.y0 = s.scroll_y - (t->dy1 + 1) * t->font.cell_h;
    clear_dirty(t);
    if (_swix(Wimp_UpdateWindow, _IN(1) | _OUT(0), &d, &more)) return;
    while (more) {
        paint_cells(t, d.visible.x0, d.visible.y1, &d.clip);
        if (_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), &d, &more)) break;
    }
}

/* ------------------------------------------------------------------------ */
/* window geometry                                                          */

static void set_extent(RoTerm *t)
{
    wbox ext;
    ext.x0 = 0;
    ext.x1 = extent_w(t);
    ext.y1 = 0;
    ext.y0 = -extent_h(t);
    _swix(Wimp_SetExtent, _INR(0, 1), t->win, &ext);
}

/* Make the Wimp's scroll bar show what PuTTY last told us. */
static void apply_scrollbar(RoTerm *t)
{
    wstate s;
    int want;
    if (!t->scrollbar || t->win < 0) return;
    set_extent(t);
    s.handle = t->win;
    if (_swix(Wimp_GetWindowState, _IN(1), &s)) return;
    want = -(t->sb_start * t->font.cell_h);
    if (s.scroll_y != want || s.scroll_x != 0) {
        s.scroll_x = 0;
        s.scroll_y = want;
        t->in_open = true;
        _swix(Wimp_OpenWindow, _IN(1), &s);
        t->in_open = false;
        force_redraw_all(t);
    }
    t->scroll_y = want;
}

static void tell_size(RoTerm *t)
{
    if (t->term)
        term_size(t->term, t->rows, t->cols, conf_get_int(t->conf, CONF_savelines));
    if (t->backend)
        backend_size(t->backend, t->cols, t->rows);
}

/* Resize the character grid and everything that depends on it. */
static void resize_cells(RoTerm *t, int cols, int rows)
{
    if (cols == t->cols && rows == t->rows) return;
    if (!alloc_grid(t, cols, rows)) return;
    if (t->conf && !t->is_log) {
        /* The window's size is part of its settings: Change settings shows what it is now,
         * Duplicate session opens one the same size, and saving the session keeps it. */
        conf_set_int(t->conf, CONF_width, cols);
        conf_set_int(t->conf, CONF_height, rows);
    }
    set_extent(t);
    tell_size(t);
    force_redraw_all(t);
}

/*
 * Keep each window's work area the right height for where the pointer is.  The Wimp clamps a
 * resize drag at the work area and sends no request when it does, so the work area has to be
 * screen high BEFORE the drag starts: it is made so while the pointer is at the window's size
 * box or its toggle size icon (with some margin, so that it is ready before the pointer
 * arrives), and stays so while a button is held after that, which is the drag.  Elsewhere it is
 * the text there really is, which is what makes the scroll bar's thumb honest.
 */
void roterm_idle(void)
{
    RoTerm *t;
    wpointer p;
    bool have_pointer = false;

    for (t = terms; t; t = t->next) {
        wstate s;
        bool near = false, want;

        if (!t->scrollbar || t->win < 0) continue;
        if (!have_pointer) {
            if (_swix(Wimp_GetPointerInfo, _IN(1), &p)) return;
            have_pointer = true;
        }
        s.handle = t->win;
        if (!_swix(Wimp_GetWindowState, _IN(1), &s) && (s.flags & WF_OPEN)) {
            near = p.x >= s.visible.x1 - 56 && p.x <= s.visible.x1 + 64 &&
                   ((p.y >= s.visible.y0 - 64 && p.y <= s.visible.y0 + 48) ||      /* the size box */
                    (p.y >= s.visible.y1 && p.y <= s.visible.y1 + 64));            /* the toggle size icon */
        }
        want = near || (t->wide_extent && p.buttons != 0);
        if (want != t->wide_extent) {
            t->wide_extent = want;
            apply_scrollbar(t);
        }
    }
}

void roterm_open(RoTerm *t, wevent *e)
{
    wstate *s = &e->open;
    const RoFont *f = &t->font;
    int cols = col_at(f, s->visible.x1 - s->visible.x0 + 1);
    int rows = (s->visible.y1 - s->visible.y0) / f->cell_h;
    int start_asked = 0;
    bool moved_thumb = false;

    /* the visible size is whole cells; keep the top left where it is */
    if (cols < WIN_MIN_COLS) cols = WIN_MIN_COLS;
    if (rows < WIN_MIN_ROWS) rows = WIN_MIN_ROWS;
    {
        int sw = s->visible.x1 - s->visible.x0, wide = rofont_cx(f, cols);
        /* the size icon rounds to whole cells downwards, but let a drag grow it */
        if (sw > wide + f->cell_w_mp / 400) { cols = col_at(f, sw); wide = rofont_cx(f, cols); }
        s->visible.x1 = s->visible.x0 + wide;
        s->visible.y0 = s->visible.y1 - rows * f->cell_h;
    }
    s->scroll_x = 0;
    if (t->scrollbar) {
        start_asked = (-s->scroll_y + f->cell_h / 2) / f->cell_h;
        if (start_asked < 0) start_asked = 0;
        moved_thumb = (-s->scroll_y != t->sb_start * f->cell_h) && !t->in_open;
        s->scroll_y = -start_asked * f->cell_h;
    } else {
        s->scroll_y = 0;
    }

    if (cols != t->cols || rows != t->rows) {
        t->wide_extent = true;                      /* (the Wimp has let this through, so it is one) */
        set_extent(t);
    }
    t->in_open = true;
    _swix(Wimp_OpenWindow, _IN(1), s);
    t->in_open = false;

    resize_cells(t, cols, rows);
    flush(t);

    if (moved_thumb && t->term && start_asked != t->sb_start) {
        t->scroll_y = s->scroll_y;
        term_scroll(t->term, 1, start_asked);
        flush(t);
    }
}

/* Open the window on top of the others, and (for a session) take the keyboard
 * focus with an invisible caret. */
static void open_initially(RoTerm *t, bool take_caret)
{
    wstate s;
    memset(&s, 0, sizeof s);
    s.handle = t->win;
    if (_swix(Wimp_GetWindowState, _IN(1), &s)) return;
    s.behind = -1;
    _swix(Wimp_OpenWindow, _IN(1), &s);
    if (take_caret)
        _swix(Wimp_SetCaretPosition, _INR(0, 5), t->win, -1, 0, 0, 1 << 25, -1);
}

static void bring_to_front(RoTerm *t)
{
    wstate s;
    s.handle = t->win;
    if (t->win < 0 || _swix(Wimp_GetWindowState, _IN(1), &s)) return;
    s.behind = -1;
    _swix(Wimp_OpenWindow, _IN(1), &s);
}

/* The name a window's place is remembered under: its host and port.  False if it has none. */
static bool window_key(const RoTerm *t, char *buf, size_t n)
{
    const char *host;
    if (!t->conf) return false;
    host = conf_get_str(t->conf, CONF_host);
    if (!host || !*host) return false;
    snprintf(buf, n, "%s:%d", host, conf_get_int(t->conf, CONF_port));
    return true;
}

/* Is another terminal window already open with its top left about here? */
static bool spot_taken(int x0, int y1)
{
    const RoTerm *o;
    wstate s;
    for (o = terms; o; o = o->next) {
        if (o->win < 0) continue;
        s.handle = o->win;
        if (_swix(Wimp_GetWindowState, _IN(1), &s) || !(s.flags & WF_OPEN)) continue;
        if (abs(s.visible.x0 - x0) < 24 && abs(s.visible.y1 - y1) < 24) return true;
    }
    return false;
}

/* Keep the title bar reachable on whatever screen this is. */
static void keep_on_screen(int *x0, int *y1)
{
    if (*x0 > screen_w - 160) *x0 = screen_w - 160;
    if (*x0 < 0) *x0 = 0;
    if (*y1 > screen_h - 56) *y1 = screen_h - 56;
    if (*y1 < 160) *y1 = 160;
}

/* Remember where the window is, for the next one for this host. */
static void save_position(const RoTerm *t)
{
    wstate s;
    char key[200];
    if (t->win < 0 || t->is_log || !window_key(t, key, sizeof key)) return;
    s.handle = t->win;
    if (_swix(Wimp_GetWindowState, _IN(1), &s)) return;
    ro_winpos_put(key, s.visible.x0, s.visible.y1);
}

static int create_window(RoTerm *t)
{
    wdef w;
    int handle = -1, k = t->id % 8, x0 = 64 + 56 * k, y1 = screen_h - 120 - 48 * k;
    const int ww = win_width(t), wh = win_height(t);
    char key[200];
    int rx, ry, tries;

    /* Where this host's window was last time, if we know; the next step of the cascade if not.
     * A second window for the same host would land exactly on the first, so it is stepped aside. */
    if (window_key(t, key, sizeof key) && ro_winpos_get(key, &rx, &ry)) {
        x0 = rx; y1 = ry;
        keep_on_screen(&x0, &y1);
        for (tries = 0; tries < 8 && spot_taken(x0, y1); tries++) {
            x0 += 56; y1 -= 48;
            keep_on_screen(&x0, &y1);
        }
    }

    memset(&w, 0, sizeof w);
    w.visible.x0 = x0;
    w.visible.x1 = w.visible.x0 + ww;
    w.visible.y1 = y1;
    w.visible.y0 = w.visible.y1 - wh;
    w.behind = -1;
    w.flags = WF_MOVEABLE | WF_BACK_ICON | WF_CLOSE_ICON | WF_TITLE_BAR | WF_TOGGLE_ICON |
              WF_SIZE_ICON | WF_NEW_FORMAT | WF_SCROLL_REQUEST |
              (t->scrollbar ? WF_VSCROLL : 0);
    w.title_fg = 7; w.title_bg = 2;
    w.work_fg = 7;  w.work_bg = 255;               /* transparent: we paint every pixel */
    w.scroll_outer = 3; w.scroll_inner = 1; w.title_hi = 12;
    w.extent.x0 = 0; w.extent.x1 = extent_w(t); w.extent.y1 = 0; w.extent.y0 = -extent_h(t);
    w.title_flags = IF_TEXT | IF_HCENTRED | IF_VCENTRED | IF_INDIRECTED;
    w.work_flags = BT_CLICK_DRAG << 12;
    w.sprite_area = 1;
    w.min_w = (short)rofont_cx(&t->font, WIN_MIN_COLS);
    w.min_h = (short)(WIN_MIN_ROWS * t->font.cell_h);
    {
        struct { char *buf; char *val; int size; } *ind = (void *)w.title;
        ind->buf = t->title;
        ind->val = (char *)-1;
        ind->size = (int)sizeof t->title;
    }
    w.nicons = 0;
    if (_swix(Wimp_CreateWindow, _IN(1) | _OUT(0), &w, &handle)) return -1;
    t->win = handle;
    return 0;
}

static void set_title_text(RoTerm *t, const char *s)
{
    snprintf(t->title, sizeof t->title, "%s", s);
    if (t->win >= 0)
        _swix(Wimp_ForceRedraw, _INR(0, 2), t->win, 0x4B534154, 3);   /* the title bar */
}

/* ------------------------------------------------------------------------ */
/* the TermWin interface                                                    */

#define TW(tw) RoTerm *t = container_of(tw, RoTerm, termwin)

static bool tw_setup_draw_ctx(TermWin *tw)
{
    return true;
}

static void put_cells(RoTerm *t, int x, int y, const wchar_t *text, int len,
                      unsigned long attr, int lattr, truecolour tc, bool curs)
{
    int i, n = len, step = 1, last;
    if (attr & TATTR_COMBINING) n = 1;                 /* base character only, for now */
    if ((lattr & LATTR_MODE) != LATTR_NORM) { x *= 2; step = 2; }   /* double width lines */
    else if (attr & ATTR_WIDE) step = 2;
    if (y < 0 || y >= t->rows) return;
    attr &= ~(unsigned long)DATTR_MASK;
    for (i = 0; i < n; i++) {
        int cx = x + i * step;
        RoCell *c;
        if (cx < 0 || cx >= t->cols) continue;
        c = cell_at(t, cx, y);
        c->ch = text[i];
        c->attr = attr;
        c->tc = tc;
        c->curs = curs;
        c->sigil = 0;
        c->wide_rhs = 0;
        if (step == 2 && cx + 1 < t->cols) {
            RoCell *r = cell_at(t, cx + 1, y);
            *r = *c;
            r->ch = 0;
            r->wide_rhs = 1;
        }
    }
    last = x + n * step - 1;
    mark_dirty(t, x, y, last, y);
}

static void tw_draw_text(TermWin *tw, int x, int y, wchar_t *text, int len,
                         unsigned long attrs, int line_attrs, truecolour tc)
{
    TW(tw);
    put_cells(t, x, y, text, len, attrs, line_attrs, tc, false);
}

static void tw_draw_cursor(TermWin *tw, int x, int y, wchar_t *text, int len,
                           unsigned long attrs, int line_attrs, truecolour tc)
{
    TW(tw);
    put_cells(t, x, y, text, len, attrs, line_attrs, tc, true);
}

static void tw_draw_trust_sigil(TermWin *tw, int x, int y)
{
    TW(tw);
    if (x >= 0 && x < t->cols && y >= 0 && y < t->rows) {
        cell_at(t, x, y)->sigil = 1;
        mark_dirty(t, x, y, x, y);
    }
}

static int tw_char_width(TermWin *tw, int uc)
{
    return 1;
}

static void tw_free_draw_ctx(TermWin *tw)
{
}

static void tw_set_cursor_pos(TermWin *tw, int x, int y)
{
}

static void tw_set_raw_mouse_mode(TermWin *tw, bool enable)
{
    TW(tw);
    t->raw_mouse = enable;
}

static void tw_set_raw_mouse_mode_pointer(TermWin *tw, bool enable)
{
}

static void tw_set_scrollbar(TermWin *tw, int total, int start, int page)
{
    TW(tw);
    if (!t->scrollbar) return;
    if (total == t->sb_total && start == t->sb_start && page == t->sb_page) return;
    t->sb_total = total;
    t->sb_start = start;
    t->sb_page = page;
    apply_scrollbar(t);
}

static void tw_bell(TermWin *tw, int mode)
{
    if (mode == BELL_DEFAULT)
        _swix(OS_WriteI + 7, 0);
}

static void tw_clip_write(TermWin *tw, int clipboard, wchar_t *text, int *attrs,
                          truecolour *colours, int len, bool must_deselect)
{
    if (clipboard == CLIP_CLIPBOARD)
        roclip_copy(text, (size_t)len);
}

static void tw_clip_request_paste(TermWin *tw, int clipboard)
{
    TW(tw);
    if (clipboard == CLIP_CLIPBOARD)
        roclip_paste(t->win);
}

static void tw_refresh(TermWin *tw)
{
    TW(tw);
    mark_dirty(t, 0, 0, t->cols - 1, t->rows - 1);
}

/* Make the window w columns by h rows, keeping its top left corner where it is. */
static void set_window_cells(RoTerm *t, int w, int h)
{
    wstate s;
    if (w < WIN_MIN_COLS) w = WIN_MIN_COLS;
    if (h < WIN_MIN_ROWS) h = WIN_MIN_ROWS;
    s.handle = t->win;
    if (t->win >= 0 && !t->wide_extent) {         /* room to grow; roterm_idle trims it again */
        t->wide_extent = true;
        set_extent(t);
    }
    if (t->win >= 0 && !_swix(Wimp_GetWindowState, _IN(1), &s)) {
        s.visible.x1 = s.visible.x0 + rofont_cx(&t->font, w);
        s.visible.y0 = s.visible.y1 - h * t->font.cell_h;
        t->in_open = true;
        _swix(Wimp_OpenWindow, _IN(1), &s);
        t->in_open = false;
    }
    resize_cells(t, w, h);
}

static void tw_request_resize(TermWin *tw, int w, int h)
{
    TW(tw);
    set_window_cells(t, w, h);
    term_resize_request_completed(t->term);
}

static void tw_set_title(TermWin *tw, const char *title, int codepage)
{
    TW(tw);
    wchar_t *w = dup_mb_to_wc(codepage, title);
    char *mb = dup_wc_to_mb(DEFAULT_CODEPAGE, w, "?");
    set_title_text(t, mb);
    sfree(w);
    sfree(mb);
}

static void tw_set_icon_title(TermWin *tw, const char *title, int codepage)
{
}

static void tw_set_minimised(TermWin *tw, bool minimised) {}
static void tw_set_maximised(TermWin *tw, bool maximised) {}
static void tw_move(TermWin *tw, int x, int y) {}
static void tw_set_zorder(TermWin *tw, bool top)
{
    TW(tw);
    if (top && t->win >= 0) {
        wstate s;
        s.handle = t->win;
        if (!_swix(Wimp_GetWindowState, _IN(1), &s)) {
            s.behind = -1;
            _swix(Wimp_OpenWindow, _IN(1), &s);
        }
    }
}

static void tw_palette_set(TermWin *tw, unsigned start, unsigned ncolours, const rgb *colours)
{
    TW(tw);
    unsigned i;
    for (i = 0; i < ncolours && start + i < OSC4_NCOLOURS; i++) {
        t->palrgb[start + i] = colours[i];
        t->pal[start + i] = rgbword(colours[i].r, colours[i].g, colours[i].b);
    }
    mark_dirty(t, 0, 0, t->cols - 1, t->rows - 1);
}

static void tw_palette_get_overrides(TermWin *tw, Terminal *term)
{
}

static void tw_unthrottle(TermWin *tw, size_t bufsize)
{
    TW(tw);
    if (t->backend)
        backend_unthrottle(t->backend, bufsize);
}

static const TermWinVtable roterm_termwin_vt = {
    .setup_draw_ctx = tw_setup_draw_ctx,
    .draw_text = tw_draw_text,
    .draw_cursor = tw_draw_cursor,
    .draw_trust_sigil = tw_draw_trust_sigil,
    .char_width = tw_char_width,
    .free_draw_ctx = tw_free_draw_ctx,
    .set_cursor_pos = tw_set_cursor_pos,
    .set_raw_mouse_mode = tw_set_raw_mouse_mode,
    .set_raw_mouse_mode_pointer = tw_set_raw_mouse_mode_pointer,
    .set_scrollbar = tw_set_scrollbar,
    .bell = tw_bell,
    .clip_write = tw_clip_write,
    .clip_request_paste = tw_clip_request_paste,
    .refresh = tw_refresh,
    .request_resize = tw_request_resize,
    .set_title = tw_set_title,
    .set_icon_title = tw_set_icon_title,
    .set_minimised = tw_set_minimised,
    .set_maximised = tw_set_maximised,
    .move = tw_move,
    .set_zorder = tw_set_zorder,
    .palette_set = tw_palette_set,
    .palette_get_overrides = tw_palette_get_overrides,
    .unthrottle = tw_unthrottle,
};

/* ------------------------------------------------------------------------ */
/* keyboard                                                                 */

/* Latin-1 / UTF-8 keys: with a UTF-8 alphabet a character above 0x7F arrives as
 * several key events, one byte each. */
static int utf8_pending;
static unsigned utf8_acc;
static int utf8_alphabet = -1;

static void send_raw(RoTerm *t, const char *buf, int n)
{
    if (n <= 0 || !t->term) return;
    term_seen_key_event(t->term);
    term_keyinput(t->term, -1, buf, n);
}

static void send_wide(RoTerm *t, wchar_t wc)
{
    if (!t->term) return;
    term_seen_key_event(t->term);
    term_keyinputw(t->term, &wc, 1);
}

/* Send a key, with an ESC in front of it if Alt (Meta) is held. */
/* Return and Backspace are "dedicated" keys: the line discipline treats them specially
 * (ending a line being edited locally, Telnet's New Line and Erase Character). PuTTY
 * marks them by passing a negative length and a NUL-terminated string. */
static void send_dedicated(RoTerm *t, bool meta, const char *buf, int n)
{
    char out[8];
    int len = 0;
    if (!t->term || n <= 0 || n > 4) return;
    term_seen_key_event(t->term);
    if (meta) out[len++] = '\033';
    memcpy(out + len, buf, (size_t)n);
    len += n;
    out[len] = 0;
    term_keyinput(t->term, -1, out, -2);
}

static void send_meta(RoTerm *t, bool meta, const char *buf, int n)
{
    char out[80];
    if (!meta || n <= 0 || n >= (int)sizeof out - 1) {
        send_raw(t, buf, n);
        return;
    }
    out[0] = '\033';
    memcpy(out + 1, buf, (size_t)n);
    send_raw(t, out, n + 1);
}

/* Shift-Insert (paste) and Ctrl-Insert (copy), or Ctrl-Shift-V and Ctrl-Shift-C, as configured under
 * "Window > Selection": `setting` says which of the two (CONF_ctrlshiftins or CONF_ctrlshiftcv). */
static void clipboard_key(RoTerm *t, int setting, bool paste)
{
    static const int clips[] = { CLIP_CLIPBOARD };
    switch (conf_get_int(t->conf, setting)) {
      case CLIPUI_IMPLICIT:
        if (paste) term_request_paste(t->term, MOUSE_PASTE_CLIPBOARD);
        break;
      case CLIPUI_EXPLICIT:
        if (paste) term_request_paste(t->term, CLIP_CLIPBOARD);
        else term_request_copy(t->term, clips, lenof(clips));
        break;
      default:
        break;
    }
}

void roterm_key(RoTerm *t, wevent *e)
{
    int code = e->key.key;
    bool shift = shift_down(), ctrl = ctrl_down(), alt = meta_down();
    char buf[64];
    int n = -1;
    bool consumed_alt = false;

    if (!t->term || t->is_log) {                        /* nothing to type into */
        _swix(Wimp_ProcessKey, _IN(0), code);
        return;
    }
    noise_ultralight(NOISE_SOURCE_KEY, (unsigned long)code);
    ro_trace("PuTTYkey$Trace", "key %03x shift %d ctrl %d alt %d", code, (int)shift, (int)ctrl, (int)alt);
    if (alt && code >= 0x80 && code <= 0xFF) {
        /* Left Alt is Meta here (see poll_meta), so the characters the keyboard layout
         * puts on Alt-digit and the like are dropped; AltGr, on the right, still composes. */
        utf8_pending = 0;
        return;
    }
    if (utf8_alphabet < 0) {
        int a = 0;
        _swix(OS_Byte, _INR(0, 1) | _OUT(1), 71, 127, &a);
        utf8_alphabet = (a == 111);
    }

    if (code >= 0x180 && code <= 0x1FF) {
        const int base = code & ~0x30;
        if (code & 0x10) shift = true;
        if (code & 0x20) ctrl = true;
        switch (base) {
          case 0x18F: case 0x18E: case 0x18C: case 0x18D: {
            /*
             * Page Up and Page Down arrive as Shift-Up and Shift-Down - and with Shift held, which cancels
             * that, as plain Up and Down - so the keyboard is asked which key is really down.  If neither is
             * (a tap that ended before we looked), a code with the Shift bit and nothing else is taken to be
             * the page key, and a plain one the arrow.
             */
            if (base == 0x18F || base == 0x18E) {
                const bool up = base == 0x18F;
                const bool arrow = key_down(up ? K_UP : K_DOWN);
                const bool page = key_down(up ? K_PGUP : K_PGDN) ||
                                  (!arrow && (code == 0x19F || code == 0x19E));
                if (page && !arrow) {
                    shift = shift_down();
                    ctrl = ctrl_down();
                    if (shift || ctrl) {               /* scroll the terminal itself: a page, or with Ctrl a line */
                        const int lines = shift ? t->rows - 1 : 1;
                        term_scroll(t->term, 0, up ? -lines : lines);
                        flush(t);
                        return;
                    }
                    n = format_small_keypad_key(buf, t->term, up ? SKK_PGUP : SKK_PGDN,
                                                shift, ctrl, alt, &consumed_alt);
                    break;
                }
            }
            n = format_arrow_key(buf, t->term,
                                 base == 0x18F ? 'A' : base == 0x18E ? 'B' : base == 0x18D ? 'C' : 'D',
                                 shift, ctrl, alt, &consumed_alt);
            break;
          }
          case 0x181: case 0x182: case 0x183: case 0x184: case 0x185:
          case 0x186: case 0x187: case 0x188: case 0x189:
            n = format_function_key(buf, t->term, base - 0x180, shift, ctrl, alt, &consumed_alt);
            break;
          case 0x1CA: case 0x1CB: case 0x1CC:
            n = format_function_key(buf, t->term, base - 0x1CA + 10, shift, ctrl, alt, &consumed_alt);
            break;
          case 0x18A:
            if (shift) { memcpy(buf, "\033[Z", 3); n = 3; } else { buf[0] = '\t'; n = 1; }
            break;
          case 0x18B:
            n = format_small_keypad_key(buf, t->term, SKK_END, shift, ctrl, alt, &consumed_alt);
            break;
          case 0x1CD:
            /* Shift-Insert pastes and Ctrl-Insert copies, as in PuTTY elsewhere */
            if (shift != ctrl) {
                clipboard_key(t, CONF_ctrlshiftins, shift);
                return;
            }
            n = format_small_keypad_key(buf, t->term, SKK_INSERT, shift, ctrl, alt, &consumed_alt);
            break;
          default:
            _swix(Wimp_ProcessKey, _IN(0), code);       /* Print, Logo, Menu ...: not ours */
            return;
        }
        send_meta(t, alt && !consumed_alt, buf, n);
        return;
    }

    if (code == 0x0D) {
        if (t->term->cr_lf_return) send_meta(t, alt, "\r\n", 2);
        else send_dedicated(t, alt, "\r", 1);
        return;
    }
    if (code == 0x08) {                                 /* Shift-Backspace sends the other one */
        buf[0] = conf_get_bool(t->conf, CONF_bksp_is_delete) != shift ? 0x7F : 0x08;
        send_dedicated(t, alt, buf, 1);
        return;
    }
    if (code == 0x7F) {
        n = format_small_keypad_key(buf, t->term, SKK_DELETE, shift, ctrl, alt, &consumed_alt);
        send_meta(t, alt && !consumed_alt, buf, n);
        return;
    }
    if (code == 0x1E) {
        /* Home - or Ctrl-^ (vim's alternate file), which has the same code; the Home key tells them apart */
        if (ctrl && !key_down(K_HOME)) {
            buf[0] = 0x1E;
            send_meta(t, alt, buf, 1);
            return;
        }
        n = format_small_keypad_key(buf, t->term, SKK_HOME, shift, ctrl, alt, &consumed_alt);
        send_meta(t, alt && !consumed_alt, buf, n);
        return;
    }
    if ((code == 0x03 || code == 0x16) && shift && ctrl &&
        (conf_get_int(t->conf, CONF_ctrlshiftcv) == CLIPUI_IMPLICIT ||
         conf_get_int(t->conf, CONF_ctrlshiftcv) == CLIPUI_EXPLICIT)) {
        clipboard_key(t, CONF_ctrlshiftcv, code == 0x16);   /* Ctrl-Shift-C copies, Ctrl-Shift-V pastes */
        return;
    }
    if (code >= 1 && code < 0x20) {                     /* Escape and the Ctrl- characters */
        buf[0] = (char)code;
        send_meta(t, alt, buf, 1);
        return;
    }
    if (code >= 0x20 && code <= 0x7E) {
        buf[0] = (code == 0x20 && ctrl) ? 0 : (char)code;       /* Ctrl-Space is NUL, as emacs expects */
        send_meta(t, alt, buf, 1);
        return;
    }
    if (code >= 0x80 && code <= 0xFF) {
        if (utf8_alphabet) {
            if (code >= 0xC0) {
                utf8_pending = code >= 0xF0 ? 3 : code >= 0xE0 ? 2 : 1;
                utf8_acc = (unsigned)(code & (0x3F >> utf8_pending));
            } else if (utf8_pending > 0) {
                utf8_acc = (utf8_acc << 6) | (unsigned)(code & 0x3F);
                if (--utf8_pending == 0) send_wide(t, (wchar_t)utf8_acc);
            }
        } else {
            send_wide(t, (wchar_t)code);
        }
        return;
    }
    _swix(Wimp_ProcessKey, _IN(0), code);
}

/* ------------------------------------------------------------------------ */
/* mouse                                                                    */

static Mouse_Button translate_button(Mouse_Button b)
{
    if (b == MBT_LEFT) return MBT_SELECT;
    if (b == MBT_MIDDLE) return MBT_PASTE;
    if (b == MBT_RIGHT) return MBT_EXTEND;
    return MBT_NOTHING;
}

/* The cell under screen position (mx, my); may be outside the grid (dragging). */
static void cell_under(RoTerm *t, int mx, int my, int *cx, int *cy)
{
    wstate s;
    int dx = 0, dy = 0;
    s.handle = t->win;
    if (!_swix(Wimp_GetWindowState, _IN(1), &s)) {
        dx = mx - s.visible.x0;
        dy = s.visible.y1 - my;
    }
    *cx = dx < 0 ? -1 : col_at(&t->font, dx);
    *cy = dy < 0 ? -1 : dy / t->font.cell_h;
}

void roterm_gain_caret(RoTerm *t)
{
    t->focus = true;
    if (t->term) { term_set_focus(t->term, true); term_update(t->term); flush(t); }
}

void roterm_lose_caret(RoTerm *t)
{
    t->focus = false;
    if (t->term) { term_set_focus(t->term, false); term_update(t->term); flush(t); }
}

void roterm_mouse_click(RoTerm *t, wevent *e)
{
    const unsigned b = e->mouse.buttons;
    int cx, cy;
    Mouse_Button raw;
    uint32_t now = ro_time();

    /* a click gives the window the keyboard focus */
    _swix(Wimp_SetCaretPosition, _INR(0, 5), t->win, -1, 0, 0, 1 << 25, -1);

    if (b & BTN_MENU) {
        rocontext_open(t, e->mouse.x, e->mouse.y);
        return;
    }
    if (!t->term) return;
    raw = (b & BTN_SELECT) ? MBT_LEFT : (b & BTN_ADJUST) ? MBT_RIGHT : MBT_NOTHING;
    if (raw == MBT_NOTHING) return;

    noise_ultralight(NOISE_SOURCE_MOUSEBUTTON, b);
    cell_under(t, e->mouse.x, e->mouse.y, &cx, &cy);

    if (t->click_count > 0 && (int32_t)(now - t->click_time) < MULTICLICK_CS &&
        cx == t->click_x && cy == t->click_y && t->click_button == (int)raw)
        t->click_count = t->click_count % 3 + 1;
    else
        t->click_count = 1;
    t->click_time = now;
    t->click_x = cx;
    t->click_y = cy;
    t->click_button = (int)raw;

    {
        const bool shift = shift_down(), ctrl = ctrl_down(), alt = alt_down();
        const bool rawmode = t->raw_mouse && !(shift && conf_get_bool(t->conf, CONF_mouse_override));
        term_mouse(t->term, raw, translate_button(raw), MA_CLICK, cx, cy, shift, ctrl, alt);
        if (!rawmode && t->click_count == 2)
            term_mouse(t->term, raw, translate_button(raw), MA_2CLK, cx, cy, shift, ctrl, alt);
        else if (!rawmode && t->click_count == 3)
            term_mouse(t->term, raw, translate_button(raw), MA_3CLK, cx, cy, shift, ctrl, alt);
    }
    t->mouse_held = raw;
    t->mouse_x = cx;
    t->mouse_y = cy;
    flush(t);
}

static void mouse_track(RoTerm *t)
{
    wpointer p;
    int cx, cy;
    if (t->mouse_held == MBT_NOTHING || !t->term) return;
    if (_swix(Wimp_GetPointerInfo, _IN(1), &p)) return;
    cell_under(t, p.x, p.y, &cx, &cy);
    {
        const bool shift = shift_down(), ctrl = ctrl_down(), alt = alt_down();
        const Mouse_Button held = (Mouse_Button)t->mouse_held;
        const bool down = (held == MBT_LEFT && (p.buttons & BTN_SELECT)) ||
                          (held == MBT_RIGHT && (p.buttons & BTN_ADJUST));
        if (!down) {
            term_mouse(t->term, held, translate_button(held), MA_RELEASE, cx, cy, shift, ctrl, alt);
            t->mouse_held = MBT_NOTHING;
            flush(t);
            return;
        }
        if (cx != t->mouse_x || cy != t->mouse_y) {
            term_mouse(t->term, held, translate_button(held), MA_DRAG, cx, cy, shift, ctrl, alt);
            t->mouse_x = cx;
            t->mouse_y = cy;
            flush(t);
        }
    }
}

void roterm_scroll_request(RoTerm *t, wevent *e)
{
    /* The block is the Open_Window_Request's - window, visible area x0 y0 x1 y1, scroll x y, the window to go behind -
     * followed by the directions: x at +32 and y at +36 (words 8 and 9; measured by tracing the events: we used to read
     * word 10, which is whatever the last event left there, so the arrows and the page clicks of the scroll bar did
     * nothing).  For y: -2 page down, -1 step down, +1 step up, +2 page up. */
    const int dir = e->words[9];
    int lines = 0;
    if (!t->term) return;
    if (dir == 1) lines = -SCROLL_LINES;
    else if (dir == -1) lines = SCROLL_LINES;
    else if (dir == 2) lines = -(t->rows - 1);
    else if (dir == -2) lines = t->rows - 1;
    if (!lines) return;

    if (t->raw_mouse && !shift_down() && (dir == 1 || dir == -1)) {
        wpointer p;
        int cx = 0, cy = 0;
        if (!_swix(Wimp_GetPointerInfo, _IN(1), &p)) cell_under(t, p.x, p.y, &cx, &cy);
        term_mouse(t->term, dir == 1 ? MBT_WHEEL_UP : MBT_WHEEL_DOWN,
                   dir == 1 ? MBT_WHEEL_UP : MBT_WHEEL_DOWN, MA_CLICK, cx, cy,
                   false, ctrl_down(), alt_down());
    } else {
        term_scroll(t->term, 0, lines);
    }
    flush(t);
}

/* ------------------------------------------------------------------------ */
/* life cycle                                                               */

static void link_term(RoTerm *t)
{
    t->next = terms;
    terms = t;
}

static void unlink_term(RoTerm *t)
{
    RoTerm **pp;
    for (pp = &terms; *pp; pp = &(*pp)->next)
        if (*pp == t) { *pp = t->next; break; }
}

RoTerm *roterm_from_window(int win)
{
    RoTerm *t;
    for (t = terms; t; t = t->next)
        if (t->win == win) return t;
    return NULL;
}

RoTerm *roterm_first(void)
{
    return terms;
}

bool roterm_is_exited(const RoTerm *t)
{
    return t->exited;
}

/*
 * Meta (the left Alt key).  The Wimp does not deliver Alt-letter at all, so while
 * Alt is held we scan the keyboard ourselves and send ESC and the character when
 * a letter, digit or punctuation key goes down.  INKEY numbers are those of the BBC
 * keyboard table the OS keeps for every layout, so the characters are those of a
 * UK/US layout.
 */
static const struct { signed char key; char lower, upper; } meta_keys[] = {
    { 17, 'q', 'Q' }, { 34, 'w', 'W' }, { 35, 'e', 'E' }, { 52, 'r', 'R' }, { 36, 't', 'T' },
    { 69, 'y', 'Y' }, { 54, 'u', 'U' }, { 38, 'i', 'I' }, { 55, 'o', 'O' }, { 56, 'p', 'P' },
    { 66, 'a', 'A' }, { 82, 's', 'S' }, { 51, 'd', 'D' }, { 68, 'f', 'F' }, { 84, 'g', 'G' },
    { 85, 'h', 'H' }, { 70, 'j', 'J' }, { 71, 'k', 'K' }, { 87, 'l', 'L' },
    { 98, 'z', 'Z' }, { 67, 'x', 'X' }, { 83, 'c', 'C' }, { 100, 'v', 'V' }, { 101, 'b', 'B' },
    { 86, 'n', 'N' }, { 102, 'm', 'M' },
    { 49, '1', '!' }, { 50, '2', '@' }, { 18, '3', '#' }, { 19, '4', '$' }, { 20, '5', '%' },
    { 53, '6', '^' }, { 37, '7', '&' }, { 22, '8', '*' }, { 39, '9', '(' }, { 40, '0', ')' },
    { 24, '-', '_' }, { 25, '=', '+' }, { 57, '[', '{' }, { 89, ']', '}' }, { 88, ';', ':' },
    { 73, '\'', '"' }, { 103, ',', '<' }, { 104, '.', '>' }, { 105, '/', '?' }, { 121, '\\', '|' },
    { 99, ' ', ' ' },
};
#define N_META_KEYS ((int)(sizeof meta_keys / sizeof meta_keys[0]))

/*
 * ... and the same for the keys that send sequences rather than characters: with Alt held the Wimp is
 * told of none of the cursor, editing and function keys, nor of Backspace, Return and Escape.  (Tab and Space
 * do arrive, and are dealt with as usual.)
 */
enum { MK_ARROW, MK_SMALL, MK_FUNC, MK_BACKSPACE, MK_RETURN, MK_ESCAPE };
static const struct { signed char key; unsigned char kind, arg; } meta_special[] = {
    { 58, MK_ARROW, 'A' }, { 42, MK_ARROW, 'B' }, { 122, MK_ARROW, 'C' }, { 26, MK_ARROW, 'D' },
    { 63, MK_SMALL, SKK_HOME }, { 106, MK_SMALL, SKK_END }, { 62, MK_SMALL, SKK_INSERT },
    { 90, MK_SMALL, SKK_DELETE }, { 64, MK_SMALL, SKK_PGUP }, { 79, MK_SMALL, SKK_PGDN },
    { 114, MK_FUNC, 1 }, { 115, MK_FUNC, 2 }, { 116, MK_FUNC, 3 }, { 21, MK_FUNC, 4 }, { 117, MK_FUNC, 5 },
    { 118, MK_FUNC, 6 }, { 23, MK_FUNC, 7 }, { 119, MK_FUNC, 8 }, { 120, MK_FUNC, 9 }, { 31, MK_FUNC, 10 },
    { 29, MK_FUNC, 11 }, { 30, MK_FUNC, 12 },
    { 48, MK_BACKSPACE, 0 }, { 74, MK_RETURN, 0 }, { 113, MK_ESCAPE, 0 },
};
#define N_META_SPECIAL ((int)(sizeof meta_special / sizeof meta_special[0]))

static void send_meta_special(RoTerm *t, int kind, int arg, bool shift, bool ctrl)
{
    char buf[64];
    bool consumed_alt = false;
    int n = -1;

    switch (kind) {
      case MK_ARROW:
        n = format_arrow_key(buf, t->term, arg, shift, ctrl, true, &consumed_alt);
        break;
      case MK_SMALL:
        n = format_small_keypad_key(buf, t->term, arg, shift, ctrl, true, &consumed_alt);
        break;
      case MK_FUNC:
        n = format_function_key(buf, t->term, arg, shift, ctrl, true, &consumed_alt);
        break;
      case MK_BACKSPACE:
        buf[0] = conf_get_bool(t->conf, CONF_bksp_is_delete) != shift ? 0x7F : 0x08;
        send_dedicated(t, true, buf, 1);
        return;
      case MK_RETURN:
        if (t->term->cr_lf_return) send_meta(t, true, "\r\n", 2);
        else send_dedicated(t, true, "\r", 1);
        return;
      default:
        buf[0] = '\033';
        send_meta(t, true, buf, 1);
        return;
    }
    send_meta(t, !consumed_alt, buf, n);
}

static void poll_meta(RoTerm *t)
{
    static bool was[N_META_KEYS], was_special[N_META_SPECIAL];
    int i;

    if (!meta_down() || !t->focus || !t->term || t->is_log) {
        memset(was, 0, sizeof was);     /* start again with the next press of Alt */
        memset(was_special, 0, sizeof was_special);
        return;
    }
    {
        const bool shift = shift_down(), ctrl = ctrl_down();
        for (i = 0; i < N_META_KEYS; i++) {
            const bool down = key_down(meta_keys[i].key);
            if (down && !was[i]) {
                char c = shift ? meta_keys[i].upper : meta_keys[i].lower;
                if (ctrl && c >= '@' && c <= '~') c &= 0x1F;
                else if (ctrl && c == ' ') c = 0;
                send_meta(t, true, &c, 1);
            }
            was[i] = down;
        }
        for (i = 0; i < N_META_SPECIAL; i++) {
            const bool down = key_down(meta_special[i].key);
            if (down && !was_special[i])
                send_meta_special(t, meta_special[i].kind, meta_special[i].arg, shift, ctrl);
            was_special[i] = down;
        }
    }
}

void roterm_poll(RoTerm *t)
{
    poll_meta(t);
    mouse_track(t);
    flush(t);
}

void roterm_redraw_all(void)
{
    RoTerm *t;
    read_screen();
    for (t = terms; t; t = t->next) force_redraw_all(t);
}

/* The Terminal tells us (palette_set) only the colours that differ from all black,
 * so that is what every entry starts as. */
static void default_palette(RoTerm *t)
{
    memset(t->palrgb, 0, sizeof t->palrgb);
    memset(t->pal, 0, sizeof t->pal);
}

static RoTerm *new_common(Conf *conf)
{
    RoTerm *t = (RoTerm *)calloc(1, sizeof *t);
    char err[160];
    FontSpec *fs;

    if (!t) return NULL;
    t->win = -1;
    t->id = term_counter++;
    t->conf = conf;
    t->cmdline_pw = roterm_cmdline_session;
    t->mouse_held = MBT_NOTHING;
    t->termwin.vt = &roterm_termwin_vt;
    t->seat.vt = &roterm_seat_vt;
    t->logpolicy.vt = &roterm_logpolicy_vt;
    clear_dirty(t);
    read_screen();
    default_palette(t);

    fs = conf_get_fontspec(conf, CONF_font);
    if (!rofont_open(&t->font, fs && fs->name[0] ? fs->name : "Corpus.Medium,12", err, sizeof err) &&
        !rofont_open(&t->font, "Corpus.Medium,12", err, sizeof err)) {
        wimp_error(err);
        free(t);
        return NULL;
    }
    t->bold_style = conf_get_int(conf, CONF_bold_style);
    t->cursor_type = conf_get_int(conf, CONF_cursor_type);
    t->scrollbar = conf_get_bool(conf, CONF_scrollbar);
    if (!alloc_grid(t, conf_get_int(conf, CONF_width), conf_get_int(conf, CONF_height))) {
        rofont_close(&t->font);
        free(t);
        return NULL;
    }
    snprintf(t->title, sizeof t->title, "%s", appname);
    if (create_window(t) != 0) {
        free(t->cells);
        rofont_close(&t->font);
        free(t);
        return NULL;
    }
    link_term(t);
    return t;
}

/* Which clipboards a mouse selection is copied to, and the one a mouse paste
 * reads (the same arrangement as unix/window.c). */
static void setup_clipboards(RoTerm *t)
{
    Terminal *term = t->term;
    Conf *conf = t->conf;

    assert(term->mouse_select_clipboards[0] == CLIP_LOCAL);
    term->n_mouse_select_clipboards = 1;
    term->mouse_select_clipboards[term->n_mouse_select_clipboards++] = MOUSE_SELECT_CLIPBOARD;
    if (conf_get_bool(conf, CONF_mouseautocopy))
        term->mouse_select_clipboards[term->n_mouse_select_clipboards++] = CLIP_CLIPBOARD;

    switch (conf_get_int(conf, CONF_mousepaste)) {
      case CLIPUI_IMPLICIT:
        term->mouse_paste_clipboard = MOUSE_PASTE_CLIPBOARD;
        break;
      case CLIPUI_EXPLICIT:
        term->mouse_paste_clipboard = CLIP_CLIPBOARD;
        break;
      default:
        term->mouse_paste_clipboard = CLIP_NULL;
        break;
    }
}

RoTerm *roterm_new(Conf *conf)
{
    RoTerm *t;

    prepare_session(conf);
    t = new_common(conf);
    if (!t) return NULL;

    init_ucs(&t->ucsdata, conf_get_str(conf, CONF_line_codepage),
             conf_get_bool(conf, CONF_utf8_override), CS_UTF8, conf_get_int(conf, CONF_vtmode));
    t->term = term_init(conf, &t->ucsdata, &t->termwin);
    setup_clipboards(t);
    t->logctx = log_init(&t->logpolicy, conf);
    term_provide_logctx(t->term, t->logctx);
    open_initially(t, true);
    term_size(t->term, t->rows, t->cols, conf_get_int(conf, CONF_savelines));
    flush(t);
    roterm_start_backend(t);
    flush(t);
    return t;
}

/* ------------------------------------------------------------------------ */
/* changing the settings of a running session                               */

/* The dialogue is told which session it is for by a number, not a pointer: the
 * session may be closed while the dialogue is open. */
static void *settings_ctx(const RoTerm *t)
{
    return (void *)(intptr_t)(t->id + 1);
}

static RoTerm *term_from_ctx(void *ctx)
{
    RoTerm *t;
    for (t = terms; t; t = t->next)
        if (settings_ctx(t) == ctx) return t;
    return NULL;
}

/* A window with a scroll bar and one without are different Wimp windows (the bar is
 * a creation flag), so a change to "Display scrollbar" makes a new one, in the same
 * place and in front. */
static void recreate_window(RoTerm *t)
{
    wstate s;
    int old = t->win;

    s.handle = old;
    if (_swix(Wimp_GetWindowState, _IN(1), &s)) return;
    if (create_window(t) != 0) { t->win = old; return; }
    _swix(Wimp_DeleteWindow, _IN(1), &old);
    s.handle = t->win;
    s.scroll_x = s.scroll_y = 0;
    s.behind = -1;
    t->in_open = true;
    _swix(Wimp_OpenWindow, _IN(1), &s);
    t->in_open = false;
    _swix(Wimp_SetCaretPosition, _INR(0, 5), t->win, -1, 0, 0, 1 << 25, -1);
}

/* Make a session follow its new settings, as unix/window.c does after its
 * reconfiguration dialogue.  newconf is the session's from now on. */
static void apply_settings(RoTerm *t, Conf *newconf)
{
    Conf *oldconf = t->conf;
    const char *ofont = conf_get_fontspec(oldconf, CONF_font)->name;
    const char *nfont = conf_get_fontspec(newconf, CONF_font)->name;
    const int ocols = conf_get_int(oldconf, CONF_width), orows = conf_get_int(oldconf, CONF_height);
    const int ncols = conf_get_int(newconf, CONF_width), nrows = conf_get_int(newconf, CONF_height);
    const int osave = conf_get_int(oldconf, CONF_savelines);
    const bool charset_changed =
        strcmp(conf_get_str(oldconf, CONF_line_codepage), conf_get_str(newconf, CONF_line_codepage)) ||
        conf_get_bool(oldconf, CONF_utf8_override) != conf_get_bool(newconf, CONF_utf8_override) ||
        conf_get_int(oldconf, CONF_vtmode) != conf_get_int(newconf, CONF_vtmode);
    bool need_size = false;

    t->conf = newconf;
    if (t->logctx) log_reconfig(t->logctx, newconf);
    if (t->ldisc) {                                    /* flushes the edit buffer if local editing went off */
        ldisc_configure(t->ldisc, newconf);
        ldisc_echoedit_update(t->ldisc);
    }
    term_reconfig(t->term, newconf);
    setup_clipboards(t);
    if (t->backend) backend_reconfig(t->backend, newconf);

    t->bold_style = conf_get_int(newconf, CONF_bold_style);
    t->cursor_type = conf_get_int(newconf, CONF_cursor_type);

    if (charset_changed)
        init_ucs(&t->ucsdata, conf_get_str(newconf, CONF_line_codepage),
                 conf_get_bool(newconf, CONF_utf8_override), CS_UTF8, conf_get_int(newconf, CONF_vtmode));

    if (strcmp(ofont, nfont)) {
        RoFont f;
        char err[160];
        if (rofont_open(&f, nfont[0] ? nfont : "Corpus.Medium,12", err, sizeof err)) {
            rofont_close(&t->font);
            t->font = f;
            need_size = true;
        } else {
            char *msg = dupprintf("Could not change fonts in terminal window: %s", err);
            ro_notice(NULL, "PuTTY", msg);
            sfree(msg);
        }
    }

    if (conf_get_bool(newconf, CONF_scrollbar) != t->scrollbar) {
        t->scrollbar = conf_get_bool(newconf, CONF_scrollbar);
        recreate_window(t);
        need_size = true;
    }

    /* A new font keeps the window's size in characters; Columns and Rows apply if they were edited. */
    if (ncols != ocols || nrows != orows || need_size) {
        const bool edited = ncols != ocols || nrows != orows;
        set_window_cells(t, edited ? ncols : t->cols, edited ? nrows : t->rows);
        set_extent(t);
        apply_scrollbar(t);
    } else if (osave != conf_get_int(newconf, CONF_savelines)) {
        term_size(t->term, t->rows, t->cols, conf_get_int(newconf, CONF_savelines));
    }

    term_invalidate(t->term);
    mark_dirty(t, 0, 0, t->cols - 1, t->rows - 1);
    force_redraw_all(t);
    conf_free(oldconf);
}

/*
 * The Font Manager works out a font's size in pixels from the screen mode when the font is found, and does not
 * redo it when the mode changes: a font found in a 90 dpi mode is drawn half the size it should be in a 180 dpi
 * one (the letters shrink inside their cells), and twice the size going the other way (they overlap).  So after
 * a mode change every window's font is found again - after the old one is lost, or Font_FindFont hands back
 * the same handle - and a window whose cells came out a different size follows them.
 */
void roterm_mode_changed(void)
{
    RoTerm *t;

    read_screen();
    for (t = terms; t; t = t->next) {
        const RoFont old = t->font;
        char spec[sizeof t->font.spec], err[160];
        bool resized;

        snprintf(spec, sizeof spec, "%s", old.spec);
        rofont_close(&t->font);
        if (!rofont_open(&t->font, spec, err, sizeof err) &&
            !rofont_open(&t->font, "Corpus.Medium,12", err, sizeof err)) {
            t->font = old;                             /* (no font at all is not going to happen: it was found a moment ago) */
            t->font.handle[0] = t->font.handle[1] = 0;
        }
        resized = t->font.cell_w_mp != old.cell_w_mp || t->font.cell_h != old.cell_h;
        if (resized && t->win >= 0) {
            set_window_cells(t, t->cols, t->rows);
            set_extent(t);
            apply_scrollbar(t);
        }
        mark_dirty(t, 0, 0, t->cols - 1, t->rows - 1);
        force_redraw_all(t);
    }
}

static void settings_done(void *ctx, bool ok, Conf *newconf)
{
    RoTerm *t = term_from_ctx(ctx);
    if (!t) {
        if (ok) conf_free(newconf);
        return;
    }
    if (ok) apply_settings(t, newconf);
    _swix(Wimp_SetCaretPosition, _INR(0, 5), t->win, -1, 0, 0, 1 << 25, -1);   /* typing goes to the session again */
}

void roterm_change_settings(RoTerm *t)
{
    Conf *c;
    char *title;

    if (!t->term || t->is_log) return;
    if (roconfig_raise(settings_ctx(t))) return;       /* already open */
    c = conf_copy(t->conf);
    term_pre_reconfig(t->term, c);
    title = dupcat(appname, " Reconfiguration");
    roconfig_open(c, true, conf_get_int(c, CONF_protocol), t->backend ? backend_cfg_info(t->backend) : 0,
                  title, settings_done, settings_ctx(t));
    sfree(title);
}

/* ------------------------------------------------------------------------ */
/* the event log                                                            */

/* A session keeps the events PuTTY reports.  Its Event Log window is another
 * terminal, with no connection, that they are written to as text: it gets
 * scrolling, selection and copying for free. */
#define EVENT_LINES 1000

/* Write a line to an Event Log window.  Event text can include things the server
 * sent, so control characters are removed first. */
static void log_feed(RoTerm *lt, const char *line)
{
    StripCtrlChars *scc;
    char *clean;
    if (!lt->term) return;
    scc = stripctrl_new_term(NULL, false, '?', lt->term);
    clean = stripctrl_string(scc, line);
    stripctrl_free(scc);
    term_data(lt->term, clean, strlen(clean));
    term_data(lt->term, "\r\n", 2);
    sfree(clean);
}

static RoTerm *new_log_window(RoTerm *parent)
{
    Conf *conf = conf_new();
    RoTerm *t;

    do_defaults(NULL, conf);
    conf_set_int(conf, CONF_width, 80);
    conf_set_int(conf, CONF_height, 12);
    conf_set_int(conf, CONF_savelines, EVENT_LINES);
    conf_set_bool(conf, CONF_scrollbar, true);
    conf_set_str(conf, CONF_line_codepage, "UTF-8");
    conf_set_fontspec(conf, CONF_font, conf_get_fontspec(parent->conf, CONF_font));

    t = new_common(conf);
    if (!t) {
        conf_free(conf);
        return NULL;
    }
    t->is_log = true;
    t->evparent = parent;
    init_ucs(&t->ucsdata, conf_get_str(conf, CONF_line_codepage), true, CS_UTF8,
             conf_get_int(conf, CONF_vtmode));
    t->term = term_init(conf, &t->ucsdata, &t->termwin);
    setup_clipboards(t);
    term_set_trust_status(t->term, false);             /* no trust sigils: it is all ours */
    term_size(t->term, t->rows, t->cols, EVENT_LINES);
    term_data(t->term, "\033[?25l", 6);                /* no cursor */
    set_title_text(t, "PuTTY Event Log");
    open_initially(t, false);
    return t;
}

void roterm_log_event(RoTerm *t, const char *line)
{
    if (!t->evlines) t->evlines = snewn(EVENT_LINES, char *);
    if (t->nev == EVENT_LINES) {                       /* forget the oldest */
        sfree(t->evlines[0]);
        memmove(t->evlines, t->evlines + 1, (EVENT_LINES - 1) * sizeof *t->evlines);
        t->nev--;
    }
    t->evlines[t->nev++] = dupstr(line);
    if (t->evwin) log_feed(t->evwin, line);
}

void roterm_eventlog_show(RoTerm *t)
{
    int i;
    if (t->is_log) return;
    if (!t->evwin) {
        RoTerm *lt = new_log_window(t);
        if (!lt) return;
        t->evwin = lt;
        for (i = 0; i < t->nev; i++) log_feed(lt, t->evlines[i]);
        flush(lt);
    }
    bring_to_front(t->evwin);
}

void roterm_free(RoTerm *t)
{
    int i;
    unlink_term(t);
    ro_msgbox_drop_owner(t);
    roconfig_end_for(settings_ctx(t));
    if (t->evwin) {                                    /* the Event Log goes with its session */
        RoTerm *lt = t->evwin;
        t->evwin = NULL;
        lt->evparent = NULL;
        roterm_free(lt);
    }
    if (t->is_log && t->evparent) t->evparent->evwin = NULL;
    roterm_destroy_connection(t);
    if (t->term) { term_free(t->term); t->term = NULL; }
    if (t->logctx) { log_free(t->logctx); t->logctx = NULL; }
    save_position(t);
    if (t->win >= 0) _swix(Wimp_DeleteWindow, _IN(1), &t->win);
    if (t->conf) conf_free(t->conf);
    delete_callbacks_for_context(t);
    rofont_close(&t->font);
    for (i = 0; i < t->nev; i++) sfree(t->evlines[i]);
    sfree(t->evlines);
    free(t->cells);
    free(t);
}

bool roterm_is_live(const RoTerm *t)
{
    return !t->is_log && t->term && t->backend && !t->exited;
}

void roterm_mark_inactive(RoTerm *t)
{
    char buf[sizeof t->title];
    const char tag[] = " (inactive)";
    if (t->is_log || t->win < 0) return;
    if (strlen(t->title) >= sizeof tag - 1 &&
        !strcmp(t->title + strlen(t->title) - (sizeof tag - 1), tag))
        return;                                         /* already says so */
    snprintf(buf, sizeof buf, "%.*s%s", (int)(sizeof buf - sizeof tag), t->title, tag);
    set_title_text(t, buf);
}

static void close_answered(void *vctx, int button)
{
    RoTerm *t = (RoTerm *)vctx;
    if (button == 0)
        roterm_free(t);
    else
        t->close_ask = false;
}

void roterm_close_request(RoTerm *t)
{
    static const char *const buttons[] = { "Close Session", "Cancel" };

    /* closing the window of a connection that is still up loses it: ask first */
    if (!roterm_is_live(t) || !conf_get_bool(t->conf, CONF_warn_on_close)) {
        roterm_free(t);
        return;
    }
    if (t->close_ask) return;
    t->close_ask = true;
    ro_msgbox(t, "PuTTY Exit Confirmation", "Are you sure you want to close this session?",
              buttons, 2, 1, 1, close_answered, t);
}

/* ------------------------------------------------------------------------ */
/* painting demo: no terminal, no network                                   */

static void demo_text(RoTerm *t, int x, int y, const char *s, unsigned long attr)
{
    wchar_t w[200];
    int n = 0;
    truecolour tc;
    memset(&tc, 0, sizeof tc);
    while (s[n] && n < 199) { w[n] = (unsigned char)s[n]; n++; }
    put_cells(t, x, y, w, n, attr, LATTR_NORM, tc, false);
}

RoTerm *roterm_new_demo(Conf *conf)
{
    RoTerm *t = new_common(conf);
    int i, y;
    static const struct { const char *n; int r, g, b; } ansi[8] = {
        { "black", 0, 0, 0 }, { "red", 187, 0, 0 }, { "green", 0, 187, 0 }, { "yellow", 187, 187, 0 },
        { "blue", 0, 0, 187 }, { "magenta", 187, 0, 187 }, { "cyan", 0, 187, 187 }, { "white", 187, 187, 187 }
    };
    if (!t) return NULL;
    for (i = 0; i < 8; i++) {
        unsigned r = ansi[i].r, g = ansi[i].g, b = ansi[i].b;
        t->pal[i] = rgbword(r, g, b);
        t->pal[i + 8] = rgbword(r ? 255 : (g || b ? 85 : 85), g ? 255 : 85, b ? 255 : 85);
    }
    t->pal[OSC4_COLOUR_fg] = rgbword(187, 187, 187);
    t->pal[OSC4_COLOUR_fg_bold] = rgbword(255, 255, 255);
    t->pal[OSC4_COLOUR_bg] = rgbword(0, 0, 0);
    t->pal[OSC4_COLOUR_cursor_fg] = rgbword(0, 0, 0);
    t->pal[OSC4_COLOUR_cursor_bg] = rgbword(0, 255, 0);
    open_initially(t, true);

    demo_text(t, 0, 0, "PuTTY for RISC OS - terminal painting demo", ATTR_DEFAULT | ATTR_BOLD);
    demo_text(t, 0, 1, "0123456789012345678901234567890123456789012345678901234567890123456789012345678901", ATTR_DEFAULT);
    for (i = 0; i < 8; i++) {
        unsigned long a = ATTR_DEFAULT & ~(ATTR_FGMASK | ATTR_BGMASK);
        char buf[40];
        snprintf(buf, sizeof buf, " %-8s ", ansi[i].n);
        demo_text(t, i * 10, 3, buf, a | ((unsigned long)i << ATTR_FGSHIFT) | ((unsigned long)OSC4_COLOUR_bg << ATTR_BGSHIFT));
        demo_text(t, i * 10, 4, buf, a | ((unsigned long)(i + 8) << ATTR_FGSHIFT) | ((unsigned long)OSC4_COLOUR_bg << ATTR_BGSHIFT));
        demo_text(t, i * 10, 5, buf, a | ((unsigned long)OSC4_COLOUR_fg << ATTR_FGSHIFT) | ((unsigned long)i << ATTR_BGSHIFT));
    }
    demo_text(t, 0, 7, "underline", ATTR_DEFAULT | ATTR_UNDER);
    demo_text(t, 12, 7, "strike", ATTR_DEFAULT | ATTR_STRIKE);
    demo_text(t, 20, 7, "reverse", ATTR_DEFAULT | ATTR_REVERSE);
    demo_text(t, 30, 7, "dim", ATTR_DEFAULT | ATTR_DIM);
    demo_text(t, 36, 7, "bold", ATTR_DEFAULT | ATTR_BOLD);
    demo_text(t, 0, 9, "Accents: \xE1\xE9\xED\xF3\xFA \xF1 \xFC \xE7 \xA3 \xA9 \xB0 \xB1 \xD7 \xF7", ATTR_DEFAULT);
    {
        static const wchar_t uni[] = { 'U','n','i','c','o','d','e',':',' ', 0x20AC, ' ', 0x2018, 0x2019, ' ',
                                       0x201C, 0x201D, ' ', 0x2013, ' ', 0x2014, ' ', 0x2026, ' ', 0x2022, ' ',
                                       0x2192, ' ', 0x2264, 0x2265, ' ', 0x03B1, 0x03B2, 0 };
        truecolour tc;
        memset(&tc, 0, sizeof tc);
        put_cells(t, 0, 10, uni, 31, ATTR_DEFAULT, LATTR_NORM, tc, false);
        {
            static const wchar_t box[] = { 0x250C, 0x2500, 0x252C, 0x2500, 0x2510, ' ', 0x2588, 0x2593, 0x2592, 0x2591, ' ', 0x2502, 0x2514, 0x2518, 0 };
            put_cells(t, 0, 11, box, 14, ATTR_DEFAULT, LATTR_NORM, tc, false);
        }
        {
            static const wchar_t tcm[] = { 'T','r','u','e','c','o','l','o','u','r',0 };
            tc.fg.enabled = true; tc.fg.r = 255; tc.fg.g = 128; tc.fg.b = 0;
            tc.bg.enabled = true; tc.bg.r = 0; tc.bg.g = 64; tc.bg.b = 128;
            put_cells(t, 0, 12, tcm, 10, ATTR_DEFAULT, LATTR_NORM, tc, false);
        }
    }
    demo_text(t, 0, 14, "cursor (block, active):", ATTR_DEFAULT);
    {
        wchar_t ch[1] = { ' ' };
        truecolour tc;
        memset(&tc, 0, sizeof tc);
        put_cells(t, 24, 14, ch, 1, ATTR_DEFAULT | ATTR_ACTCURS, LATTR_NORM, tc, true);
        demo_text(t, 0, 15, "cursor (block, passive):", ATTR_DEFAULT);
        put_cells(t, 25, 15, ch, 1, ATTR_DEFAULT | ATTR_PASCURS, LATTR_NORM, tc, true);
    }
    for (y = 17; y < t->rows; y++) {
        char buf[80];
        snprintf(buf, sizeof buf, "row %d", y);
        demo_text(t, 0, y, buf, ATTR_DEFAULT);
    }
    flush(t);
    return t;
}
