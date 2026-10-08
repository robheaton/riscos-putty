/*
 * rodlg.c - declarative dialogue boxes built from Wimp icons.  See rodlg.h.
 * (Adapted from riscos-vncclient, same author.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rodlg.h"

#define MAX_ITEMS 64

struct dlg {
    struct dlg  *next;
    int          win;
    int          w, h;                /* the work area, OS units */
    int          vis_w, vis_h;        /* the part shown: less than that if the screen is too small (scroll bars then) */
    int          vbar, hbar;          /* the room the scroll bars take at the side and below, OS units (0: none) */
    int          n;
    int          icon[MAX_ITEMS];
    int          type[MAX_ITEMS];
    int          id[MAX_ITEMS];
    char        *buf[MAX_ITEMS];
    int          buflen[MAX_ITEMS];
    char        *valid[MAX_ITEMS];
    const char  *help[MAX_ITEMS];
    wbox         box[MAX_ITEMS];      /* the icon's box in the work area, OS units */
    unsigned     rgb[MAX_ITEMS];      /* DI_SWATCH: the colour, as ColourTrans wants it (&BBGGRR00) */
    int          bar[MAX_ITEMS];      /* DI_BAR: how full, 0 to 1000 */
    int          own_redraw;          /* has a swatch, so we draw (not the Wimp) */
    const char  *window_help;
    char         title[64];
    int          open;
    int          default_id, escape_id, close_id;
    dlg_click_fn cb;
    dlg_menu_fn  menu_cb;
    void        *ud;
};

static dlg *dialogs;

/* ------------------------------------------------------------------------ */

static int find_index_by_id(const dlg *d, int id)
{
    int i;
    for (i = 0; i < d->n; i++)
        if (d->id[i] == id) return i;
    return -1;
}

static int find_index_by_icon(const dlg *d, int icon)
{
    int i;
    for (i = 0; i < d->n; i++)
        if (d->icon[i] == icon) return i;
    return -1;
}

static dlg *find_dlg(int win)
{
    dlg *d;
    for (d = dialogs; d; d = d->next)
        if (d->win == win) return d;
    return NULL;
}

static char *dupstr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* ------------------------------------------------------------------------ */

static unsigned item_flags(const dlg_item *it)
{
    const unsigned base = IF_TEXT | IF_VCENTRED | IF_INDIRECTED;   /* (IF_ANTIALIAS would turn the colour bits into a font handle) */
    switch (it->type) {
    case DI_LABEL:
        return base | IF_FG(7) | IF_BG(1);
    case DI_FIELD:
    case DI_PASSWORD:
        return base | IF_BORDER | IF_FILLED | IF_BUTTON(BT_WRITABLE) | IF_FG(7) | IF_BG(0);
    case DI_BUTTON:
        return base | IF_BORDER | IF_HCENTRED | IF_FILLED | IF_BUTTON(BT_CLICK) | IF_FG(7) | IF_BG(1);
    case DI_DEFBUTTON:
        return base | IF_BORDER | IF_HCENTRED | IF_FILLED | IF_BUTTON(BT_CLICK) | IF_FG(7) | IF_BG(1);
    case DI_OPTION:
        return base | IF_SPRITE | IF_BUTTON(BT_RADIO) | IF_FG(7) | IF_BG(1);
    case DI_RADIO:
        return base | IF_SPRITE | IF_BUTTON(BT_RADIO) | IF_ESG(it->esg) | IF_FG(7) | IF_BG(1);
    case DI_BOX:
        return base | IF_BORDER | IF_FG(7) | IF_BG(1);
    case DI_RLABEL:
        return base | IF_RJUSTIFIED | IF_FG(7) | IF_BG(1);
    case DI_DISPLAY:
        return base | IF_BORDER | IF_HCENTRED | IF_FILLED | IF_FG(7) | IF_BG(1);
    case DI_SWATCH:
    case DI_BAR:
        return base | IF_BORDER | IF_FG(7) | IF_BG(1);          /* not filled: the Wimp would paint it over ours */
    case DI_FILE:
        return IF_SPRITE | IF_INDIRECTED | IF_HCENTRED | IF_VCENTRED | IF_BUTTON(BT_CLICK_DRAG) | IF_FG(7) | IF_BG(1);
    case DI_HEADING:
        return base | IF_FILLED | IF_FG(7) | IF_BG(1);
    case DI_INFO:
        return base | IF_BORDER | IF_FILLED | IF_FG(7) | IF_BG(1);
    }
    return base;
}

static const char *item_validation(const dlg_item *it)
{
    switch (it->type) {
    case DI_FIELD:      return "Pptr_write";
    case DI_PASSWORD:   return "Pptr_write;D*";
    case DI_BUTTON:     return "R5,3";
    case DI_DEFBUTTON:  return "R6,3";
    case DI_OPTION:     return "Soptoff,opton";
    case DI_RADIO:      return "Sradiooff,radioon";
    case DI_BOX:        return "R2";
    case DI_DISPLAY:    return "R2";
    case DI_SWATCH:     return "R2";
    case DI_BAR:        return "R2";
    case DI_INFO:       return "R2";
    default:            return NULL;
    }
}

static int mode_var(int var, int dflt)
{
    int v = 0;
    if (_swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, var, &v)) return dflt;
    return v;
}

/* The screen's size in OS units. */
static void screen_os(int *w, int *h)
{
    int v;
    *w = 3200; *h = 2400;
    if (!_swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 11, &v)) *w = (v + 1) << mode_var(4, 1);
    if (!_swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 12, &v)) *h = (v + 1) << mode_var(5, 1);
}

/*
 * A window as big as the dialogue wants, but not bigger than the screen: if the dialogue does not fit, the part shown
 * is cut down and the window gets scroll bars.  The room it needs is the work area, a title bar above it (42 OS
 * units) and, if there is one, a scroll bar below (40) and at the side (40); it may cover the icon bar, as a window
 * can.  Dialogues are about 1000 OS units across and 400 to 800 high, so this matters only in a mode of a thousand
 * OS units or fewer - a 180 dpi mode of a small display - and every dialogue gets it for nothing.
 */
#define TITLE_H  42
#define BAR_W    40

void dlg_fit_to_screen(int w, int h, int *vis_w, int *vis_h, int *vbar, int *hbar)
{
    int sw, sh, aw, ah;
    bool vs = false, hs = false, changed;

    screen_os(&sw, &sh);
    aw = sw;
    ah = sh - TITLE_H;
    do {                                     /* (each bar takes room that may make the other necessary) */
        changed = false;
        if (!vs && h > ah) { vs = true; aw -= BAR_W; changed = true; }
        if (!hs && w > aw) { hs = true; ah -= BAR_W; changed = true; }
    } while (changed);
    *vis_w = w > aw ? aw : w;
    *vis_h = h > ah ? ah : h;
    *vbar = vs ? BAR_W : 0;
    *hbar = hs ? BAR_W : 0;
}

/* Where a window of `vis_w` by `vis_h` (and its scroll bars) goes to be in the middle of the screen, `lift` OS units above
 * the centre, with its title bar and scroll bars on the screen. */
void dlg_centre(int vis_w, int vis_h, int vbar, int hbar, int lift, wbox *out)
{
    int sw, sh;
    screen_os(&sw, &sh);
    out->x0 = (sw - vis_w - vbar) / 2;
    if (out->x0 < 0) out->x0 = 0;
    out->x1 = out->x0 + vis_w;
    out->y1 = (sh + vis_h) / 2 + lift;
    if (out->y1 > sh - TITLE_H) out->y1 = sh - TITLE_H;                  /* (the title bar is above the work area) */
    if (out->y1 - vis_h < hbar) out->y1 = vis_h + hbar;                  /* (and a scroll bar below it) */
    out->y0 = out->y1 - vis_h;
}

dlg *dlg_new(const char *title, int w_px, int h_px, const dlg_item *items, int n,
             dlg_click_fn cb, void *ud)
{
    return dlg_new_style(DLG_NORMAL, title, w_px, h_px, items, n, cb, ud);
}

dlg *dlg_new_style(int style, const char *title, int w_px, int h_px, const dlg_item *items, int n,
                   dlg_click_fn cb, void *ud)
{
    dlg *d = (dlg *)calloc(1, sizeof *d);
    wdef wd;
    int handle = -1, i;
    /* Layouts are written in design pixels (the desktop font is about 8 of them to a character), each UI_PX
     * OS units in every mode: see rowimp.h. */
    const int sx = UI_PX, sy = UI_PX;
    const int w = w_px * sx, h = h_px * sy;

    if (!d || n > MAX_ITEMS) { free(d); return NULL; }
    d->w = w;
    d->h = h;
    d->vis_w = w;
    d->vis_h = h;
    if (style != DLG_MENU) dlg_fit_to_screen(w, h, &d->vis_w, &d->vis_h, &d->vbar, &d->hbar);
    d->cb = cb;
    d->ud = ud;
    d->default_id = d->escape_id = d->close_id = -1;
    snprintf(d->title, sizeof d->title, "%s", title);
    for (i = 0; i < n; i++)
        if (items[i].type == DI_SWATCH || items[i].type == DI_BAR) d->own_redraw = 1;

    memset(&wd, 0, sizeof wd);
    wd.visible.x0 = 400; wd.visible.y0 = 400;
    wd.visible.x1 = 400 + d->vis_w; wd.visible.y1 = 400 + d->vis_h;
    wd.behind = -1;
    wd.flags = (style == DLG_MENU)
        ? (WF_AUTO_REDRAW | WF_TITLE_BAR | WF_NEW_FORMAT)
        : (WF_MOVEABLE | WF_AUTO_REDRAW | WF_BACK_ICON | WF_CLOSE_ICON | WF_TITLE_BAR | WF_NEW_FORMAT |
           (d->hbar ? WF_HSCROLL : 0) | (d->vbar ? WF_VSCROLL : 0));
    if (d->own_redraw)
        wd.flags &= ~WF_AUTO_REDRAW;               /* we get the redraw requests, the Wimp still draws the icons */
    wd.title_fg = 7; wd.title_bg = 2; wd.work_fg = 7; wd.work_bg = 1;
    wd.scroll_outer = 3; wd.scroll_inner = 1; wd.title_hi = 12;
    wd.extent.x0 = 0; wd.extent.x1 = w; wd.extent.y0 = -h; wd.extent.y1 = 0;
    wd.title_flags = IF_TEXT | IF_HCENTRED | IF_VCENTRED | IF_INDIRECTED;
    wd.work_flags = BT_NEVER << 12;
    wd.sprite_area = 1;
    wd.min_w = 0; wd.min_h = 0;
    {
        struct { char *buf; char *val; int size; } *ind = (void *)wd.title;
        ind->buf = d->title;
        ind->val = (char *)-1;
        ind->size = (int)sizeof d->title;
    }
    if (_swix(Wimp_CreateWindow, _IN(1) | _OUT(0), &wd, &handle)) { free(d); return NULL; }
    d->win = handle;

    for (i = 0; i < n; i++) {
        const dlg_item *it = &items[i];
        struct { int win; idef icon; } blk;
        int cap = it->maxlen > 0 ? it->maxlen : (int)strlen(it->text ? it->text : "") + 1;
        const char *vs = item_validation(it);
        int ih = -1;

        if (it->type == DI_LABEL || it->type == DI_RLABEL || it->type == DI_DISPLAY ||
            it->type == DI_BUTTON || it->type == DI_DEFBUTTON || it->type == DI_INFO ||
            it->type == DI_HEADING) {
            if (cap < 64) cap = 64;                 /* room for dlg_set_text() */
        }
        d->buf[i] = (char *)calloc(1, (size_t)cap);
        d->buflen[i] = cap;
        d->valid[i] = vs ? dupstr(vs) : NULL;
        if (!d->buf[i]) continue;
        snprintf(d->buf[i], (size_t)cap, "%s", it->text ? it->text : "");
        d->type[i] = it->type;
        d->id[i] = it->id;
        d->help[i] = it->help;
        if (it->type == DI_DEFBUTTON) d->default_id = it->id;

        memset(&blk, 0, sizeof blk);
        blk.win = handle;
        blk.icon.box.x0 = it->x * sx;
        blk.icon.box.x1 = (it->x + it->w) * sx;
        blk.icon.box.y1 = -(it->y * sy);
        blk.icon.box.y0 = -((it->y + it->h) * sy);
        d->box[i] = blk.icon.box;
        blk.icon.flags = item_flags(it);
        blk.icon.data.ind.buf = d->buf[i];
        /* (for a sprite icon the second word is the sprite area: 1 is the Wimp's pool) */
        blk.icon.data.ind.validation = it->type == DI_FILE ? (char *)1 : d->valid[i] ? d->valid[i] : (char *)-1;
        blk.icon.data.ind.size = cap;
        _swix(Wimp_CreateIcon, _INR(0, 1) | _OUT(0), 0, &blk, &ih);
        d->icon[i] = ih;
    }
    d->n = n;
    d->next = dialogs;
    dialogs = d;
    return d;
}

void dlg_free(dlg *d)
{
    dlg **pp;
    int i;
    if (!d) return;
    for (pp = &dialogs; *pp; pp = &(*pp)->next)
        if (*pp == d) { *pp = d->next; break; }
    if (d->win >= 0) _swix(Wimp_DeleteWindow, _IN(1), &d->win);
    for (i = 0; i < d->n; i++) {
        if (d->type[i] == DI_PASSWORD && d->buf[i]) {          /* a passphrase must not be left in freed memory */
            volatile char *p = d->buf[i];
            int j;
            for (j = 0; j < d->buflen[i]; j++) p[j] = 0;
        }
        free(d->buf[i]);
        free(d->valid[i]);
    }
    free(d);
}

/* ------------------------------------------------------------------------ */

static int first_field(const dlg *d)
{
    int i;
    for (i = 0; i < d->n; i++)
        if (d->type[i] == DI_FIELD || d->type[i] == DI_PASSWORD) return i;
    return -1;
}

static void caret_to(dlg *d, int idx)
{
    if (idx < 0) return;
    _swix(Wimp_SetCaretPosition, _INR(0, 5), d->win, d->icon[idx], -1, -1, -1,
          (int)strlen(d->buf[idx]));
}

void dlg_open(dlg *d)
{
    wstate s;

    if (!d) return;
    memset(&s, 0, sizeof s);
    s.handle = d->win;
    if (d->open && !_swix(Wimp_GetWindowState, _IN(1), &s)) {
        s.behind = -1;                          /* just bring it to the front */
    } else {
        dlg_centre(d->vis_w, d->vis_h, d->vbar, d->hbar, 80, &s.visible);
        s.behind = -1;
    }
    _swix(Wimp_OpenWindow, _IN(1), &s);
    d->open = 1;
    caret_to(d, first_field(d));
}

/* Where a window `w` by `h` (plus `vbar` and `hbar` of scroll bars) should go to be centred on (x, y) and wholly on the screen. */
static void place_at(int w, int h, int vbar, int hbar, int x, int y, wbox *out)
{
    int v, scr_w = 0, scr_h = 0;
    if (!_swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 11, &v)) scr_w = (v + 1) << mode_var(4, 1);
    if (!_swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 12, &v)) scr_h = (v + 1) << mode_var(5, 1);
    out->x0 = x - w / 2;
    out->y1 = y + h / 2;
    if (scr_w > 0 && out->x0 + w + vbar > scr_w) out->x0 = scr_w - w - vbar;
    if (out->x0 < 0) out->x0 = 0;
    if (scr_h > 0 && out->y1 > scr_h - 80) out->y1 = scr_h - 80;     /* (clear of the title bar's top) */
    if (out->y1 - h < 96) out->y1 = 96 + h;                           /* (and of the icon bar) */
    if (scr_h > 0 && out->y1 > scr_h - TITLE_H) out->y1 = scr_h - TITLE_H;   /* (but the title bar must be on the screen) */
    if (out->y1 - h < hbar) out->y1 = h + hbar;                       /* (and a scroll bar under the window) */
    out->x1 = out->x0 + w;
    out->y0 = out->y1 - h;
}

void dlg_open_at(dlg *d, int x, int y)
{
    wstate s;
    if (!d) return;
    if (d->open) { dlg_open(d); return; }
    memset(&s, 0, sizeof s);
    s.handle = d->win;
    place_at(d->vis_w, d->vis_h, d->vbar, d->hbar, x, y, &s.visible);
    s.behind = -1;
    _swix(Wimp_OpenWindow, _IN(1), &s);
    d->open = 1;
    caret_to(d, first_field(d));
}

void dlg_set_menu_fn(dlg *d, dlg_menu_fn fn)
{
    if (d) d->menu_cb = fn;
}

void dlg_close(dlg *d)
{
    if (!d || !d->open) return;
    _swix(Wimp_CloseWindow, _IN(1), &d->win);
    d->open = 0;
}

void dlg_set_help(dlg *d, const char *text) { if (d) d->window_help = text; }

void dlg_set_title(dlg *d, const char *title)
{
    if (!d) return;
    snprintf(d->title, sizeof d->title, "%s", title);
    if (d->open)
        _swix(Wimp_ForceRedraw, _INR(0, 2), d->win, 0x4B534154, 3);         /* the title bar */
}

const char *dlg_help_text(int win, int icon)
{
    const dlg *d = find_dlg(win);
    int i;
    if (!d) return NULL;
    i = find_index_by_icon(d, icon);
    if (i >= 0 && d->help[i]) return d->help[i];
    return d->window_help;
}

int dlg_is_open(const dlg *d) { return d && d->open; }
int dlg_window(const dlg *d)  { return d ? d->win : -1; }

/* ------------------------------------------------------------------------ */

void dlg_set_swatch(dlg *d, int id, int r, int g, int b)
{
    int i = find_index_by_id(d, id);
    unsigned rgb;
    if (i < 0 || d->type[i] != DI_SWATCH) return;
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    rgb = ((unsigned)b << 24) | ((unsigned)g << 16) | ((unsigned)r << 8);
    if (rgb == d->rgb[i]) return;
    d->rgb[i] = rgb;
    if (d->open)
        _swix(Wimp_ForceRedraw, _INR(0, 4), d->win, d->box[i].x0, d->box[i].y0, d->box[i].x1, d->box[i].y1);
}

void dlg_set_bar(dlg *d, int id, int permille)
{
    int i = find_index_by_id(d, id);
    if (i < 0 || d->type[i] != DI_BAR) return;
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;
    if (permille == d->bar[i]) return;
    d->bar[i] = permille;
    if (d->open)
        _swix(Wimp_ForceRedraw, _INR(0, 4), d->win, d->box[i].x0, d->box[i].y0, d->box[i].x1, d->box[i].y1);
}

int dlg_item_box(dlg *d, int id, int *x0, int *y0, int *x1, int *y1)
{
    wstate s;
    int i = find_index_by_id(d, id);
    if (i < 0) return 0;
    s.handle = d->win;
    if (_swix(Wimp_GetWindowState, _IN(1), &s)) return 0;
    *x0 = s.visible.x0 - s.scroll_x + d->box[i].x0;
    *x1 = s.visible.x0 - s.scroll_x + d->box[i].x1;
    *y1 = s.visible.y1 - s.scroll_y + d->box[i].y1;
    *y0 = s.visible.y1 - s.scroll_y + d->box[i].y0;
    return 1;
}

dlg *dlg_from_window(int win)
{
    return find_dlg(win);
}

/* The Wimp has drawn the window and its icons for each rectangle that needs it; put the colours in. */
int dlg_event_redraw(wevent *e)
{
    dlg *d = find_dlg(e->words[0]);
    wdraw *r = (wdraw *)e->words;
    const int sx = 1 << mode_var(4, 1), sy = 1 << mode_var(5, 1), inset_x = 3 * sx, inset_y = 3 * sy;
    int more = 0, i;

    if (!d || !d->own_redraw) return 0;
    if (_swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), r, &more)) return 1;
    while (more) {
        const int ox = r->visible.x0 - r->scroll_x, oy = r->visible.y1 - r->scroll_y;
        for (i = 0; i < d->n; i++) {
            int left = ox + d->box[i].x0 + inset_x, right = ox + d->box[i].x1 - inset_x;
            unsigned colour;
            if (d->type[i] == DI_SWATCH) {
                colour = d->rgb[i];
            } else if (d->type[i] == DI_BAR) {
                colour = 0xCC663300u;                          /* a blue, &BBGGRR00 */
                right = left + (int)(((long long)(right - left) * d->bar[i]) / 1000);
            } else {
                continue;
            }
            if (right <= left) continue;
            _swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), colour, 0, 0);
            _swix(OS_Plot, _INR(0, 2), 4, left, oy + d->box[i].y0 + inset_y);
            _swix(OS_Plot, _INR(0, 2), 0x65, right - 1, oy + d->box[i].y1 - inset_y - 1);
        }
        if (_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), r, &more)) break;
    }
    return 1;
}

void dlg_set_text(dlg *d, int id, const char *s)
{
    int i = find_index_by_id(d, id);
    if (i < 0) return;
    snprintf(d->buf[i], (size_t)d->buflen[i], "%s", s);
    /* redraw the icon with its new text */
    wimp_set_icon_state(d->win, d->icon[i], 0, 0);
}

const char *dlg_get_text(dlg *d, int id)
{
    int i = find_index_by_id(d, id);
    return i < 0 ? "" : d->buf[i];
}

void dlg_set_selected(dlg *d, int id, int on)
{
    int i = find_index_by_id(d, id);
    if (i < 0) return;
    wimp_set_icon_state(d->win, d->icon[i], on ? IF_SELECTED : 0, IF_SELECTED);
}

int dlg_get_selected(dlg *d, int id)
{
    struct { int win, icon; idef def; } st;
    int i = find_index_by_id(d, id);
    if (i < 0) return 0;
    st.win = d->win;
    st.icon = d->icon[i];
    if (_swix(Wimp_GetIconState, _IN(1), &st)) return 0;
    return (st.def.flags & IF_SELECTED) != 0;
}

void dlg_set_shaded(dlg *d, int id, int on)
{
    int i = find_index_by_id(d, id);
    if (i < 0) return;
    wimp_set_icon_state(d->win, d->icon[i], on ? IF_SHADED : 0, IF_SHADED);
}

/* The Wimp does not draw or notice an icon whose "deleted" flag is set, so this hides an item and brings it back. */
void dlg_set_hidden(dlg *d, int id, int on)
{
    int i = find_index_by_id(d, id);
    if (i < 0) return;
    wimp_set_icon_state(d->win, d->icon[i], on ? IF_DELETED : 0, IF_DELETED);
    _swix(Wimp_ForceRedraw, _INR(0, 4), d->win, d->box[i].x0, d->box[i].y0, d->box[i].x1, d->box[i].y1);
}

void dlg_focus(dlg *d, int id)
{
    caret_to(d, find_index_by_id(d, id));
}

void dlg_set_keys(dlg *d, int default_id, int escape_id)
{
    d->default_id = default_id;
    d->escape_id = d->close_id = escape_id;
}

void dlg_set_close_id(dlg *d, int id)
{
    d->close_id = id;
}

/* ------------------------------------------------------------------------ */
/* events                                                                   */

int dlg_event_click(wevent *e)
{
    wpointer *p = &e->mouse;
    dlg *d = find_dlg(p->window);
    int i;
    if (!d) return 0;
    if ((p->buttons & BTN_MENU) && d->menu_cb) {
        d->menu_cb(d, p->x, p->y, d->ud);
        return 1;
    }
    i = find_index_by_icon(d, p->icon);
    if (i < 0) return 1;
    switch (d->type[i]) {
    case DI_BUTTON: case DI_DEFBUTTON: case DI_OPTION: case DI_RADIO: case DI_FILE:
        if (d->cb) d->cb(d, d->id[i], (int)p->buttons, d->ud);
        break;
    default:
        break;
    }
    return 1;
}

int dlg_event_key(wevent *e)
{
    wkey *k = &e->key;
    dlg *d = find_dlg(k->window);
    int i, dir = 0;
    if (!d) return 0;

    if (k->key == 0x0D) {                       /* Return */
        if (d->default_id >= 0 && d->cb) { d->cb(d, d->default_id, BTN_SELECT, d->ud); return 1; }
    } else if (k->key == 0x1B) {                /* Escape */
        if (d->escape_id >= 0 && d->cb) { d->cb(d, d->escape_id, BTN_SELECT, d->ud); return 1; }
    } else if (k->key == 0x18A || k->key == 0x18E) {
        dir = 1;                                /* Tab / Down: next field */
    } else if (k->key == 0x19A || k->key == 0x18F) {
        dir = -1;                               /* Shift-Tab / Up: previous field */
    }
    if (dir) {
        int cur = find_index_by_icon(d, k->icon), j;
        for (j = 1; j <= d->n; j++) {
            int idx = ((cur < 0 ? 0 : cur) + dir * j + d->n * 2) % d->n;
            if (d->type[idx] == DI_FIELD || d->type[idx] == DI_PASSWORD) {
                caret_to(d, idx);
                return 1;
            }
        }
        return 1;
    }
    (void)i;
    _swix(Wimp_ProcessKey, _IN(0), k->key);
    return 1;
}

int dlg_event_open(wevent *e)
{
    dlg *d = find_dlg(e->words[0]);
    if (!d) return 0;
    _swix(Wimp_OpenWindow, _IN(1), e);
    return 1;
}

int dlg_event_close(wevent *e)
{
    dlg *d = find_dlg(e->words[0]);
    if (!d) return 0;
    if (d->close_id >= 0 && d->cb) d->cb(d, d->close_id, BTN_SELECT, d->ud);
    else dlg_close(d);
    return 1;
}
