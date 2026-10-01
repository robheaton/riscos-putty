/*
 * rofont.c - Font Manager wrapper for the terminal window.  See rofont.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ro.h"
#include "rofont.h"

/* Font_Paint / Font_ScanString flag bits (from OSLib's font.h) */
#define FP_OS_UNITS     0x10u
#define FP_GIVEN_LENGTH 0x80u

static int find_font(const char *name, int pt16, int *handle)
{
    int h = 0;
    _kernel_oserror *e = _swix(Font_FindFont, _IN(1) | _INR(2, 5) | _OUT(0),
                               name, pt16, pt16, 0, 0, &h);
    if (e) return 0;
    *handle = h;
    return 1;
}

/* Open name at pt points with the best encoding available: UTF-8, else Latin-1. */
static int open_encoded(const char *name, int pt, int *handle, bool *utf8)
{
    char full[160];
    snprintf(full, sizeof full, "%s\\EUTF8", name);
    if (find_font(full, pt * 16, handle)) { *utf8 = true; return 1; }
    snprintf(full, sizeof full, "%s\\ELatin1", name);
    if (find_font(full, pt * 16, handle)) { *utf8 = false; return 1; }
    if (find_font(name, pt * 16, handle)) { *utf8 = false; return 1; }
    return 0;
}

static int string_width_mp(int handle, const char *s)
{
    int w = 0, h = 0;
    _swix(Font_SetFont, _IN(0), handle);
    if (_swix(Font_StringWidth, _INR(1, 5) | _OUTR(2, 3), s, 0x7fffffff, 0x7fffffff,
              -1, (int)strlen(s), &w, &h))
        return 0;
    return w;
}

/* "X.Medium" or "X.Medium.Y" -> "X.Bold" or "X.Bold.Y"; 0 if no such pattern. */
static int bold_name(const char *name, char *out, size_t cap)
{
    const char *p = strstr(name, ".Medium");
    size_t head;
    if (!p) p = strstr(name, ".Regular");
    if (!p) return 0;
    head = (size_t)(p - name);
    snprintf(out, cap, "%.*s.Bold%s", (int)head, name, p + strlen(p[1] == 'M' ? ".Medium" : ".Regular"));
    return 1;
}

bool rofont_open(RoFont *f, const char *spec, char *err, size_t errlen)
{
    char name[100], bname[120];
    int pt = 12, asc = 0, desc = 0, x0, y0, x1, y1, h, bh;
    const char *comma = strrchr(spec, ',');
    bool utf8, butf8;

    memset(f, 0, sizeof *f);
    snprintf(f->spec, sizeof f->spec, "%s", spec);
    if (comma) {
        size_t n = (size_t)(comma - spec);
        if (n >= sizeof name) n = sizeof name - 1;
        memcpy(name, spec, n);
        name[n] = 0;
        pt = atoi(comma + 1);
        if (pt < 4) pt = 12;
        if (pt > 72) pt = 72;
    } else {
        snprintf(name, sizeof name, "%s", spec);
    }

    if (!open_encoded(name, pt, &h, &utf8)) {
        if (err) snprintf(err, errlen, "Cannot find the font '%s'", name);
        return false;
    }
    f->handle[0] = f->handle[1] = h;
    f->utf8 = utf8;
    if (bold_name(name, bname, sizeof bname) && open_encoded(bname, pt, &bh, &butf8) && butf8 == utf8) {
        f->handle[1] = bh;
        f->has_bold = true;
    }

    f->cell_w_mp = string_width_mp(h, "M");
    if (f->cell_w_mp <= 0) {
        if (err) snprintf(err, errlen, "The font '%s' has no usable width", name);
        rofont_close(f);
        return false;
    }
    if (!_swix(Font_ReadInfo, _IN(0) | _OUTR(1, 4), h, &x0, &y0, &x1, &y1)) {
        asc = y1;
        desc = -y0;
    }
    if (asc <= 0 || desc < 0) { asc = pt * 2; desc = pt * 3 / 4; }
    f->cell_h = asc + desc;
    if (f->cell_h < pt * 3) f->cell_h = pt * 3;
    f->cell_h = (f->cell_h + 1) & ~1;                     /* a whole number of pixels */
    f->baseline = asc + (f->cell_h - (asc + desc)) / 2;
    f->under_y = f->baseline + 2;
    if (f->under_y > f->cell_h - 2) f->under_y = f->cell_h - 2;
    f->strike_y = f->baseline - asc * 2 / 5;
    return true;
}

void rofont_close(RoFont *f)
{
    if (f->handle[1] && f->handle[1] != f->handle[0])
        _swix(Font_LoseFont, _IN(0), f->handle[1]);
    if (f->handle[0])
        _swix(Font_LoseFont, _IN(0), f->handle[0]);
    f->handle[0] = f->handle[1] = 0;
}

void rofont_colours(const RoFont *f, bool bold, uint32_t bg, uint32_t fg)
{
    _swix(ColourTrans_SetFontColours, _INR(0, 3), f->handle[bold && f->has_bold], bg, fg, 14);
}

void rofont_paint(const RoFont *f, bool bold, const char *s, size_t len, int x, int y)
{
    _swix(Font_Paint, _INR(0, 4) | _IN(7), f->handle[bold && f->has_bold], s,
          FP_OS_UNITS | FP_GIVEN_LENGTH, x, y, (int)len);
}
