/*
 * rocfgwin.c - the configuration dialogue's window: the category tree, the icons of
 * the panel on show, and the events that turn into PuTTY's handler calls.
 * The control tree and the dlg_* interface are in roconfig.c; see roconfig_i.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "ro.h"
#include "rowimp.h"
#include "rodlg.h"
#include "romenu.h"
#include "rodialog.h"
#include "roapp.h"
#include "roconfig.h"
#include "roconfig_i.h"

static dlgparam *all_dialogs;
static int xeig = 1, yeig = 1;

static int mode_var(int var, int dflt)
{
    int v = dflt;
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, var, &v);
    return v;
}

static void force_redraw_area(dlgparam *dp, int x, int y, int w, int h);

static dlgparam *find_dp(int win)
{
    dlgparam *dp;
    for (dp = all_dialogs; dp; dp = dp->next)
        if (dp->win == win) return dp;
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* icons                                                                    */

static CfgIcon *new_cfgicon(Widget *w)
{
    if (w->nicons == w->iconcap) {
        w->iconcap = w->iconcap ? w->iconcap * 2 : 8;
        w->icons = sresize(w->icons, w->iconcap, CfgIcon);
    }
    memset(&w->icons[w->nicons], 0, sizeof(CfgIcon));
    return &w->icons[w->nicons++];
}

/*
 * Create an icon at (x, y) of size w x h pixels from the window's top left.  With
 * `buf` NULL a buffer of buflen bytes is allocated and filled with `text`;
 * otherwise `buf` (the widget's own text) is used.
 */
static CfgIcon *make_icon(dlgparam *dp, Widget *wd, int role, int arg, unsigned flags,
                          int x, int y, int w, int h, const char *text, char *buf, int buflen,
                          const char *valid)
{
    struct { int win; idef icon; } blk;
    CfgIcon *ci = new_cfgicon(wd);
    const int sx = UI_PX, sy = UI_PX;
    int handle = -1;

    ci->role = role;
    ci->arg = arg;
    if (buf) {
        ci->buf = buf;
        ci->own = false;
    } else {
        ci->buf = (char *)calloc(1, (size_t)buflen);
        ci->own = true;
        if (ci->buf && text) snprintf(ci->buf, (size_t)buflen, "%s", text);
    }
    ci->valid = valid ? dupstr(valid) : dupstr("");

    memset(&blk, 0, sizeof blk);
    blk.win = dp->win;
    blk.icon.box.x0 = x * sx;
    blk.icon.box.x1 = (x + w) * sx;
    blk.icon.box.y1 = -(y * sy);
    blk.icon.box.y0 = -((y + h) * sy);
    blk.icon.flags = flags | IF_INDIRECTED;
    blk.icon.data.ind.buf = ci->buf;
    blk.icon.data.ind.validation = ci->valid;
    blk.icon.data.ind.size = buflen;
    _swix(Wimp_CreateIcon, _INR(0, 1) | _OUT(0), 0, &blk, &handle);
    ci->handle = handle;
    return ci;
}

/* Wimp_DeleteIcon takes a pointer to a block holding the window and icon handles. */
static _kernel_oserror *delete_icon(dlgparam *dp, int icon)
{
    int blk[2];
    blk[0] = dp->win;
    blk[1] = icon;
    return _swix(Wimp_DeleteIcon, _IN(1), blk);
}

static void delete_icons(dlgparam *dp, Widget *wd)
{
    int i;
    for (i = 0; i < wd->nicons; i++) {
        CfgIcon *ci = &wd->icons[i];
        if (ci->handle >= 0) delete_icon(dp, ci->handle);
        if (ci->own) free(ci->buf);
        sfree(ci->valid);
    }
    wd->nicons = 0;
}

static void set_icon(dlgparam *dp, int icon, unsigned eor, unsigned clear)
{
    wimp_set_icon_state(dp->win, icon, eor, clear);
}

static void select_icon(dlgparam *dp, int icon, bool on)
{
    set_icon(dp, icon, on ? IF_SELECTED : 0, IF_SELECTED);
}

static void shade_icon(dlgparam *dp, int icon, bool on)
{
    set_icon(dp, icon, on ? IF_SHADED : 0, IF_SHADED);
}

#define F_LABEL   (IF_TEXT | IF_VCENTRED | IF_FG(7) | IF_BG(1))
#define F_FIELD   (IF_TEXT | IF_VCENTRED | IF_BORDER | IF_FILLED | IF_BUTTON(BT_WRITABLE) | IF_FG(7) | IF_BG(0))
#define F_DISPLAY (IF_TEXT | IF_VCENTRED | IF_BORDER | IF_FILLED | IF_FG(7) | IF_BG(0))
#define F_BUTTON  (IF_TEXT | IF_VCENTRED | IF_BORDER | IF_HCENTRED | IF_FILLED | IF_BUTTON(BT_CLICK) | IF_FG(7) | IF_BG(1))
#define F_OPTION  (IF_TEXT | IF_SPRITE | IF_VCENTRED | IF_BUTTON(BT_RADIO) | IF_FG(7) | IF_BG(1))
#define F_ROW     (IF_TEXT | IF_VCENTRED | IF_FILLED | IF_BUTTON(BT_CLICK) | IF_FG(7) | IF_BG(0))

/* ------------------------------------------------------------------------ */
/* the icons of one control                                                 */


/* The text of row `row` of a list box, column `col`. */
static void row_text(const Widget *w, int index, int col, char *out, size_t cap)
{
    if (index < 0 || index >= w->nent) { out[0] = 0; return; }
    cfg_cell_text(w->ent[index].text, col, out, cap);
}

static int list_ncols(const Widget *w)
{
    return w->ctrl->listbox.ncols > 1 ? w->ctrl->listbox.ncols : 1;
}

/* x offset and width (pixels, in the list) of a column. */
static void list_col_geometry(const Widget *w, int col, int total_w, int *x, int *width)
{
    const int n = list_ncols(w);
    int i, start = 0, pct = 100;
    if (n == 1 || !w->ctrl->listbox.percentages) { *x = 0; *width = total_w; return; }
    for (i = 0; i < col; i++) start += w->ctrl->listbox.percentages[i];
    pct = col == n - 1 ? 100 - start : w->ctrl->listbox.percentages[col];
    *x = total_w * start / 100;
    *width = total_w * pct / 100;
}

static void fill_list_rows(dlgparam *dp, Widget *w)
{
    int i, ncols = list_ncols(w);
    for (i = 0; i < w->nicons; i++) {
        CfgIcon *ci = &w->icons[i];
        if (ci->role == ROLE_ROW) {
            const int row = ci->arg / 16, col = ci->arg % 16;
            const int index = w->top + row;
            row_text(w, index, col, ci->buf, MAX_TEXT);
            if (index >= 0 && index < w->nent && w->ent[index].sel)
                select_icon(dp, ci->handle, true);
            else
                select_icon(dp, ci->handle, false);
            set_icon(dp, ci->handle, 0, 0);
        } else if (ci->role == ROLE_SCROLL_UP) {
            shade_icon(dp, ci->handle, w->top <= 0);
        } else if (ci->role == ROLE_SCROLL_DOWN) {
            shade_icon(dp, ci->handle, w->top + w->rows >= w->nent);
        } else if (ci->role == ROLE_UP || ci->role == ROLE_DOWN) {
            int sel = -1, k;
            for (k = 0; k < w->nent; k++) if (w->ent[k].sel) { sel = k; break; }
            shade_icon(dp, ci->handle, sel < 0 || (ci->role == ROLE_UP ? sel == 0 : sel >= w->nent - 1));
        }
    }
    (void)ncols;
}

static unsigned popup_flags(void)
{
    return F_BUTTON;
}

static void create_widget_icons(dlgparam *dp, Widget *w, int ox, int oy)
{
    dlgcontrol *c = w->ctrl;
    const int x = ox + w->x, y = oy + w->y;
    int i;

    if (w->hidden) return;
    switch (c->type) {
      case CTRL_TEXT: {
        const char *start[24];
        int len[24], n = cfg_wrap(w->label ? w->label : "", w->w / CHAR_W, start, len, 24);
        if (n > 24) n = 24;
        for (i = 0; i < n; i++) {
            char line[256];
            snprintf(line, sizeof line, "%.*s", len[i], start[i]);
            make_icon(dp, w, ROLE_LABEL, i, F_LABEL, x, y + i * LINE_H, w->lw, LINE_H, line, NULL, 256, NULL);
        }
        break;
      }
      case CTRL_EDITBOX: {
        if (w->label)
            make_icon(dp, w, ROLE_LABEL, 0, F_LABEL, x, y, w->lw, w->lh, w->label, NULL, 128, NULL);
        make_icon(dp, w, ROLE_MAIN, 0, F_FIELD, x + w->fx, y + w->fy, w->fw, w->fh, NULL, w->text, MAX_TEXT,
                  c->editbox.password ? "Pptr_write;D*" : "Pptr_write");
        if (c->editbox.has_list)
            make_icon(dp, w, ROLE_POPUP, 0, popup_flags(), x + w->bx, y + w->fy, w->bw, w->fh, "v", NULL, 8, "R5,3");
        break;
      }
      case CTRL_RADIO: {
        const int nc = c->radio.ncolumns < 1 ? 1 : c->radio.ncolumns;
        static int esg = 0;
        const int group = (esg++ % 30) + 1;
        if (w->label)
            make_icon(dp, w, ROLE_LABEL, 0, F_LABEL, x, y, w->lw, w->lh, w->label, NULL, 128, NULL);
        for (i = 0; i < c->radio.nbuttons; i++) {
            const int col = i % nc, row = i / nc;
            const int span = (i == c->radio.nbuttons - 1 && col < nc - 1) ? nc - col : 1;
            const int bx = x + w->fw * col / nc, bw = w->fw * span / nc;
            make_icon(dp, w, ROLE_RADIO, i, F_OPTION | IF_ESG(group), bx, y + w->fy + row * CHECK_H, bw,
                      CHECK_H, c->radio.buttons[i], NULL, 96, "Sradiooff,radioon");
        }
        break;
      }
      case CTRL_CHECKBOX:
        make_icon(dp, w, ROLE_MAIN, 0, F_OPTION, x, y, w->fw, CHECK_H, w->label, NULL, 128, "Soptoff,opton");
        break;
      case CTRL_BUTTON:
        make_icon(dp, w, ROLE_MAIN, 0, F_BUTTON, x, y, w->fw, w->fh, w->label, NULL, 96,
                  c->button.isdefault ? "R6,3" : "R5,3");
        break;
      case CTRL_LISTBOX:
        if (w->rows > 0) {
            const int ncols = list_ncols(w);
            int r, col;
            if (w->label)
                make_icon(dp, w, ROLE_LABEL, 0, F_LABEL, x, y, w->lw, w->lh, w->label, NULL, 128, NULL);
            make_icon(dp, w, ROLE_DECOR, 0, IF_TEXT | IF_BORDER | IF_FILLED | IF_FG(7) | IF_BG(0), x + w->fx,
                      y + w->fy, w->fw, w->fh, "", NULL, 8, "");
            for (r = 0; r < w->rows; r++) {
                for (col = 0; col < ncols; col++) {
                    int cx, cw;
                    const int lw = w->fw - 4 - SCROLL_W;
                    list_col_geometry(w, col, lw, &cx, &cw);
                    make_icon(dp, w, ROLE_ROW, r * 16 + col, F_ROW, x + w->fx + 2 + cx, y + w->fy + 2 + r * LIST_ROW_H,
                              cw, LIST_ROW_H, NULL, NULL, MAX_TEXT, "");
                }
            }
            make_icon(dp, w, ROLE_SCROLL_UP, 0, F_BUTTON, x + w->fx + w->fw - SCROLL_W - 2, y + w->fy + 2, SCROLL_W,
                      SCROLL_W, "^", NULL, 4, "R5,3");
            make_icon(dp, w, ROLE_SCROLL_DOWN, 0, F_BUTTON, x + w->fx + w->fw - SCROLL_W - 2,
                      y + w->fy + w->fh - 2 - SCROLL_W, SCROLL_W, SCROLL_W, "v", NULL, 4, "R5,3");
            if (c->listbox.draglist) {
                make_icon(dp, w, ROLE_UP, 0, F_BUTTON, x + w->bx, y + w->fy, w->bw, BUTTON_H, "Up", NULL, 8, "R5,3");
                make_icon(dp, w, ROLE_DOWN, 0, F_BUTTON, x + w->bx, y + w->fy + BUTTON_H + 6, w->bw, BUTTON_H, "Down", NULL, 8,
                          "R5,3");
            }
            fill_list_rows(dp, w);
        } else {                                       /* a drop-down list */
            if (w->label)
                make_icon(dp, w, ROLE_LABEL, 0, F_LABEL, x, y, w->lw, w->lh, w->label, NULL, 128, NULL);
            make_icon(dp, w, ROLE_MAIN, 0, F_DISPLAY | IF_BUTTON(BT_CLICK), x + w->fx, y + w->fy, w->fw, w->fh,
                      w->cursel >= 0 && w->cursel < w->nent ? w->ent[w->cursel].text : "", NULL, 128, "");
            make_icon(dp, w, ROLE_POPUP, 0, popup_flags(), x + w->bx, y + w->fy, w->bw, w->fh, "v", NULL, 8, "R5,3");
        }
        break;
      case CTRL_FILESELECT:
        if (c->fileselect.just_button) {
            make_icon(dp, w, ROLE_MAIN, 0, F_BUTTON, x, y, w->fw, w->fh, w->label, NULL, 96, "R5,3");
        } else {
            if (w->label)
                make_icon(dp, w, ROLE_LABEL, 0, F_LABEL, x, y, w->lw, w->lh, w->label, NULL, 128, NULL);
            make_icon(dp, w, ROLE_MAIN, 0, F_FIELD, x + w->fx, y + w->fy, w->fw, w->fh, NULL, w->text, MAX_TEXT,
                      "Pptr_write");
        }
        break;
      case CTRL_FONTSELECT:
        if (w->label)
            make_icon(dp, w, ROLE_LABEL, 0, F_LABEL, x, y, w->lw, w->lh, w->label, NULL, 128, NULL);
        make_icon(dp, w, ROLE_MAIN, 0, F_DISPLAY, x + w->fx, y + w->fy, w->fw, w->fh, NULL, w->text, MAX_TEXT, "");
        make_icon(dp, w, ROLE_CHANGE, 0, F_BUTTON, x + w->bx, y + w->fy, w->bw, w->fh, "Change...", NULL, 16, "R5,3");
        break;
    }

    /* the state from the model */
    if (c->type == CTRL_CHECKBOX && w->nicons) select_icon(dp, w->icons[0].handle, w->checked);
    if (c->type == CTRL_RADIO)
        for (i = 0; i < w->nicons; i++)
            if (w->icons[i].role == ROLE_RADIO) select_icon(dp, w->icons[i].handle, w->icons[i].arg == w->radio);
}

/* ------------------------------------------------------------------------ */
/* the swatch in Window > Colours                                           */

/*
 * PuTTY's Window > Colours panel lists the colours and shows the one chosen as three numbers
 * (Red, Green, Blue) beside a Modify button.  We add a sunken well beside the Modify button,
 * level with it and as wide as the number boxes, and paint the colour in it.
 */
static Widget *panel_edit(Panel *p, const char *label)
{
    int i;
    for (i = 0; i < p->nw; i++) {
        dlgcontrol *c = p->w[i]->ctrl;
        if (c->type == CTRL_EDITBOX && c->label && !strcmp(c->label, label)) return p->w[i];
    }
    return NULL;
}

/* The well's box in pixels from the dialogue's top left, if the panel on show has one. */
static bool swatch_geometry(dlgparam *dp, int *x, int *y, int *w, int *h)
{
    Panel *p = dp->cur;
    Widget *red, *modify = NULL;
    int i;

    if (!p || !p->path || strcmp(p->path, "Window/Colours")) return false;
    red = panel_edit(p, "Red");
    for (i = 0; i < p->nw; i++) {
        dlgcontrol *c = p->w[i]->ctrl;
        if (c->type == CTRL_BUTTON && c->label && !strcmp(c->label, "Modify")) modify = p->w[i];
    }
    if (!red || !modify) return false;
    *x = PANEL_X + red->x + red->fx;
    *w = red->fw;
    *y = PANEL_Y + HEAD_H + modify->y + modify->fy;
    *h = modify->fh;
    return true;
}

/* The colour the three number boxes hold; false while nothing is selected (they are empty). */
static bool swatch_colour(dlgparam *dp, int rgb[3])
{
    static const char *const names[3] = { "Red", "Green", "Blue" };
    int i;
    for (i = 0; i < 3; i++) {
        Widget *w = panel_edit(dp->cur, names[i]);
        if (!w || !w->text || !w->text[0]) return false;
        rgb[i] = atoi(w->text);
        if (rgb[i] < 0) rgb[i] = 0;
        if (rgb[i] > 255) rgb[i] = 255;
    }
    return true;
}

static void swatch_changed(dlgparam *dp, Widget *w)
{
    int x, y, ww, hh;
    if (w->panel == dp->cur && w->ctrl->type == CTRL_EDITBOX && swatch_geometry(dp, &x, &y, &ww, &hh))
        force_redraw_area(dp, x, y, ww, hh);
}

/* The Wimp draws the window and its icons for each rectangle that needs it; the colour goes on top. */
int roconfig_event_redraw(wevent *e)
{
    dlgparam *dp = find_dp(e->words[0]);
    wdraw *r = (wdraw *)e->words;
    int more = 0, x, y, w, h, rgb[3];
    bool paint;
    const int sx = UI_PX, sy = UI_PX, ix = 3 << mode_var(4, 1), iy = 3 << mode_var(5, 1);   /* (the inset is three real
                                                                                               * pixels, inside the bevel) */

    if (!dp) return 0;
    if (_swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), r, &more)) return 1;
    paint = swatch_geometry(dp, &x, &y, &w, &h) && swatch_colour(dp, rgb);
    while (more) {
        if (paint) {
            const int ox = r->visible.x0 - r->scroll_x, oy = r->visible.y1 - r->scroll_y;
            const unsigned col = ((unsigned)rgb[2] << 24) | ((unsigned)rgb[1] << 16) | ((unsigned)rgb[0] << 8);
            _swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), col, 0, 0);
            _swix(OS_Plot, _INR(0, 2), 4, ox + x * sx + ix, oy - (y + h) * sy + iy);
            _swix(OS_Plot, _INR(0, 2), 0x65, ox + (x + w) * sx - ix - 1, oy - y * sy - iy - 1);
        }
        if (_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), r, &more)) break;
    }
    return 1;
}

/* Bring the icons of a control into line with its value. */
void cfgwin_update_widget(dlgparam *dp, Widget *w)
{
    dlgcontrol *c = w->ctrl;
    int i;
    if (!w->nicons || dp->win < 0) return;
    switch (c->type) {
      case CTRL_EDITBOX:
      case CTRL_FILESELECT:
      case CTRL_FONTSELECT:
        for (i = 0; i < w->nicons; i++)
            if (w->icons[i].role == ROLE_MAIN) set_icon(dp, w->icons[i].handle, 0, 0);
        swatch_changed(dp, w);
        break;
      case CTRL_CHECKBOX:
        select_icon(dp, w->icons[0].handle, w->checked);
        break;
      case CTRL_RADIO:
        for (i = 0; i < w->nicons; i++)
            if (w->icons[i].role == ROLE_RADIO) select_icon(dp, w->icons[i].handle, w->icons[i].arg == w->radio);
        break;
      case CTRL_LISTBOX:
        if (w->rows > 0) {
            fill_list_rows(dp, w);
        } else {
            for (i = 0; i < w->nicons; i++)
                if (w->icons[i].role == ROLE_MAIN) {
                    snprintf(w->icons[i].buf, 128, "%s",
                             w->cursel >= 0 && w->cursel < w->nent ? w->ent[w->cursel].text : "");
                    set_icon(dp, w->icons[i].handle, 0, 0);
                }
        }
        break;
      default:
        break;
    }
}

void cfgwin_update_label(dlgparam *dp, Widget *w)
{
    int i;
    if (!w->nicons || dp->win < 0) return;
    if (w->ctrl->type == CTRL_TEXT) {
        const char *start[24];
        int len[24], n = cfg_wrap(w->label ? w->label : "", w->w / CHAR_W, start, len, 24);
        for (i = 0; i < w->nicons; i++) {
            CfgIcon *ci = &w->icons[i];
            if (ci->role != ROLE_LABEL) continue;
            if (ci->arg < n) snprintf(ci->buf, 256, "%.*s", len[ci->arg], start[ci->arg]);
            else ci->buf[0] = 0;
            set_icon(dp, ci->handle, 0, 0);
        }
        return;
    }
    for (i = 0; i < w->nicons; i++) {
        CfgIcon *ci = &w->icons[i];
        if (ci->role == ROLE_LABEL || (w->ctrl->type == CTRL_CHECKBOX && ci->role == ROLE_MAIN) ||
            (w->ctrl->type == CTRL_BUTTON && ci->role == ROLE_MAIN)) {
            snprintf(ci->buf, 96, "%s", w->label ? w->label : "");
            set_icon(dp, ci->handle, 0, 0);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* the panel on show                                                        */

/* ------------------------------------------------------------------------ */
/* frames and headings                                                      */


static void add_decor(dlgparam *dp, unsigned flags, int x, int y, int w, int h, const char *text, const char *valid)
{
    struct { int win; idef icon; } blk;
    const int sx = UI_PX, sy = UI_PX;
    Decor d;
    int handle = -1;

    d.buf = (char *)calloc(1, 160);
    snprintf(d.buf, 160, "%s", text ? text : "");
    d.valid = dupstr(valid ? valid : "");
    memset(&blk, 0, sizeof blk);
    blk.win = dp->win;
    blk.icon.box.x0 = x * sx;
    blk.icon.box.x1 = (x + w) * sx;
    blk.icon.box.y1 = -(y * sy);
    blk.icon.box.y0 = -((y + h) * sy);
    blk.icon.flags = flags | IF_INDIRECTED;
    blk.icon.data.ind.buf = d.buf;
    blk.icon.data.ind.validation = d.valid;
    blk.icon.data.ind.size = 160;
    _swix(Wimp_CreateIcon, _INR(0, 1) | _OUT(0), 0, &blk, &handle);
    d.handle = handle;
    if (dp->ndecor == dp->decorcap) {
        dp->decorcap = dp->decorcap ? dp->decorcap * 2 : 16;
        dp->decor = sresize(dp->decor, dp->decorcap, Decor);
    }
    dp->decor[dp->ndecor++] = d;
}

/* Remove the decoration icons from index `from` on (the first nstatic stay for the
 * life of the window). */
static void clear_decor(dlgparam *dp, bool wimp_too, int from)
{
    int i;
    for (i = from; i < dp->ndecor; i++) {
        if (wimp_too && dp->decor[i].handle >= 0) delete_icon(dp, dp->decor[i].handle);
        free(dp->decor[i].buf);
        sfree(dp->decor[i].valid);
    }
    dp->ndecor = from;
}

/* ------------------------------------------------------------------------ */
/* the category tree                                                        */

static const char *last_component(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static bool panel_has_children(dlgparam *dp, Panel *p)
{
    int i;
    for (i = 0; i < dp->npanels; i++)
        if (dp->panels[i]->parent == p) return true;
    return false;
}

static bool panel_visible_in_tree(Panel *p)
{
    Panel *q;
    for (q = p->parent; q; q = q->parent)
        if (!q->expanded) return false;
    return true;
}

static void clear_tree(dlgparam *dp, bool wimp_too)
{
    int i;
    for (i = 0; i < dp->nrows; i++) {
        if (wimp_too) delete_icon(dp, dp->rows[i].icon);
        free(dp->rows[i].buf);
    }
    dp->nrows = 0;
}

static void rebuild_tree(dlgparam *dp)
{
    int i, y = TREE_Y + 4;
    const int sx = UI_PX, sy = UI_PX;

    clear_tree(dp, true);
    for (i = 0; i < dp->npanels; i++) {
        Panel *p = dp->panels[i];
        struct { int win; idef icon; } blk;
        char *buf;
        int handle = -1;

        if (!panel_visible_in_tree(p)) continue;
        if (y + LINE_H > TREE_Y + TREE_H) break;
        buf = (char *)calloc(1, 64);
        snprintf(buf, 64, "%s%s", panel_has_children(dp, p) ? (p->expanded ? "- " : "+ ") : "  ",
                 last_component(p->path));
        memset(&blk, 0, sizeof blk);
        blk.win = dp->win;
        blk.icon.box.x0 = (TREE_X + 4 + p->depth * 14) * sx;
        blk.icon.box.x1 = (TREE_X + TREE_W - 4) * sx;
        blk.icon.box.y1 = -(y * sy);
        blk.icon.box.y0 = -((y + LINE_H) * sy);
        blk.icon.flags = F_ROW | IF_INDIRECTED | (p == dp->cur ? IF_SELECTED : 0);
        blk.icon.data.ind.buf = buf;
        blk.icon.data.ind.validation = "";
        blk.icon.data.ind.size = 64;
        _swix(Wimp_CreateIcon, _INR(0, 1) | _OUT(0), 0, &blk, &handle);
        dp->rows = sresize(dp->rows, dp->nrows + 1, TreeRow);
        dp->rows[dp->nrows].panel = p;
        dp->rows[dp->nrows].icon = handle;
        dp->rows[dp->nrows].buf = buf;
        dp->nrows++;
        y += LINE_H;
    }
}

/* ------------------------------------------------------------------------ */
/* showing a panel                                                          */

static bool is_writable(const Widget *w, const CfgIcon *ci)
{
    return ci->role == ROLE_MAIN &&
           (w->ctrl->type == CTRL_EDITBOX ||
            (w->ctrl->type == CTRL_FILESELECT && !w->ctrl->fileselect.just_button));
}

/*
 * The Wimp edits a writable icon itself and does not tell us what was typed.  So
 * that PuTTY's handlers see it, each text field of the panel on show (and of the
 * footer) is handed to its handler before anything that reads the settings: a click,
 * a change of panel, Return.
 */
static void sync_fields(dlgparam *dp)
{
    Panel *ps[2];
    int p, i, k;

    ps[0] = dp->cur;
    ps[1] = dp->footer;
    for (p = 0; p < 2; p++) {
        if (!ps[p]) continue;
        for (i = 0; i < ps[p]->nw; i++) {
            Widget *w = ps[p]->w[i];
            for (k = 0; k < w->nicons; k++)
                if (is_writable(w, &w->icons[k])) {
                    cfg_emit(dp, w, EVENT_VALCHANGE);
                    break;
                }
        }
    }
}

static void widget_origin(dlgparam *dp, const Widget *w, int *ox, int *oy)
{
    if (w->panel == dp->footer) { *ox = TREE_X; *oy = FOOT_Y; }
    else { *ox = PANEL_X; *oy = PANEL_Y + HEAD_H; }
}

static void caret_to_icon(dlgparam *dp, int icon, const char *text)
{
    _swix(Wimp_SetCaretPosition, _INR(0, 5), dp->win, icon, -1, -1, -1, (int)strlen(text));
}

void cfgwin_set_focus(dlgparam *dp, Widget *w)
{
    int i;
    if (!w->nicons || dp->win < 0) return;
    for (i = 0; i < w->nicons; i++)
        if (is_writable(w, &w->icons[i])) {
            caret_to_icon(dp, w->icons[i].handle, w->text ? w->text : "");
            return;
        }
}

/* Ask the Wimp to repaint a rectangle of the window (pixels, top left origin). */
static void force_redraw_area(dlgparam *dp, int x, int y, int w, int h)
{
    const int sx = UI_PX, sy = UI_PX;
    _swix(Wimp_ForceRedraw, _INR(0, 4), dp->win, x * sx, -((y + h) * sy), (x + w) * sx, -(y * sy));
}

void cfgwin_show_panel(dlgparam *dp, Panel *p)
{
    int i, j;
    const int oy = PANEL_Y + HEAD_H;
    Panel *q;

    if (dp->win < 0) { dp->cur = p; return; }
    if (dp->cur)
        for (i = 0; i < dp->cur->nw; i++) delete_icons(dp, dp->cur->w[i]);
    clear_decor(dp, true, dp->nstatic);
    dp->cur = p;
    for (q = p->parent; q; q = q->parent) q->expanded = true;
    rebuild_tree(dp);

    if (p->title)
        add_decor(dp, IF_TEXT | IF_VCENTRED | IF_FG(7) | IF_BG(1), PANEL_X, PANEL_Y, PANEL_W, LINE_H + 4,
                  p->title, "");
    for (j = 0; j < p->nbox; j++) {
        Box *b = &p->box[j];
        add_decor(dp, IF_TEXT | IF_BORDER | IF_FG(7) | IF_BG(1), PANEL_X, oy + b->y0 + 9, p->width,
                  b->y1 - b->y0 - 9, "", "R2");
        add_decor(dp, IF_TEXT | IF_VCENTRED | IF_FILLED | IF_FG(7) | IF_BG(1), PANEL_X + BOX_PAD - 4, oy + b->y0,
                  (int)strlen(b->title) * CHAR_W + 12, 18, b->title, "");
    }
    for (i = 0; i < p->nw; i++) create_widget_icons(dp, p->w[i], PANEL_X, oy);
    {
        int sx, sy, sw, sh;
        if (swatch_geometry(dp, &sx, &sy, &sw, &sh))        /* the sunken well the colour is painted in */
            add_decor(dp, IF_TEXT | IF_BORDER | IF_FG(7) | IF_BG(1), sx, sy, sw, sh, "", "R2");
    }

    /* Wimp_DeleteIcon does not repaint what the icon covered, so redraw the tree and
     * the panel (they are drawn once, with the icons as they now are). */
    force_redraw_area(dp, TREE_X, TREE_Y, TREE_W, TREE_H);
    force_redraw_area(dp, PANEL_X, PANEL_Y, PANEL_W, PANEL_H);

    for (i = 0; i < p->nw; i++) {                      /* the caret goes to the first text field */
        for (j = 0; j < p->w[i]->nicons; j++)
            if (is_writable(p->w[i], &p->w[i]->icons[j])) {
                cfgwin_set_focus(dp, p->w[i]);
                return;
            }
    }
}

bool cfgwin_is_visible(dlgparam *dp, Widget *w)
{
    return dp->win >= 0 && (w->panel == dp->cur || w->panel == dp->footer);
}

/* ------------------------------------------------------------------------ */
/* the window                                                               */

void cfgwin_create(dlgparam *dp)
{
    wdef wd;
    int handle = -1, i, vis_w, vis_h, vbar, hbar;
    wstate s;
    xeig = mode_var(4, 1);
    yeig = mode_var(5, 1);
    /* The dialogue is CFG_W by CFG_H design pixels whatever the mode, so it may not fit a small screen: then it has scroll bars. */
    dlg_fit_to_screen(CFG_W * UI_PX, CFG_H * UI_PX, &vis_w, &vis_h, &vbar, &hbar);

    memset(&wd, 0, sizeof wd);
    wd.visible.x0 = 200; wd.visible.y0 = 200;
    wd.visible.x1 = 200 + vis_w; wd.visible.y1 = 200 + vis_h;
    wd.behind = -1;
    /* (not auto-redraw: roconfig_event_redraw lets the Wimp draw the icons and then paints the
     * colour swatch in Window > Colours over them) */
    wd.flags = WF_MOVEABLE | WF_BACK_ICON | WF_CLOSE_ICON | WF_TITLE_BAR | WF_NEW_FORMAT |
               (vbar ? WF_VSCROLL : 0) | (hbar ? WF_HSCROLL : 0);
    wd.title_fg = 7; wd.title_bg = 2; wd.work_fg = 7; wd.work_bg = 1;
    wd.scroll_outer = 3; wd.scroll_inner = 1; wd.title_hi = 12;
    wd.extent.x0 = 0; wd.extent.x1 = CFG_W * UI_PX; wd.extent.y0 = -(CFG_H * UI_PX); wd.extent.y1 = 0;
    wd.title_flags = IF_TEXT | IF_HCENTRED | IF_VCENTRED | IF_INDIRECTED;
    wd.work_flags = BT_NEVER << 12;
    wd.sprite_area = 1;
    {
        struct { char *buf; char *val; int size; } *ind = (void *)wd.title;
        ind->buf = dp->title;
        ind->val = (char *)"";
        ind->size = (int)sizeof dp->title;
    }
    if (_swix(Wimp_CreateWindow, _IN(1) | _OUT(0), &wd, &handle)) return;
    dp->win = handle;
    dp->next = all_dialogs;
    all_dialogs = dp;

    /* a sunken white frame for the category tree, then the Open and Cancel buttons */
    add_decor(dp, IF_TEXT | IF_BORDER | IF_FILLED | IF_FG(7) | IF_BG(0), TREE_X, TREE_Y, TREE_W, TREE_H, "", "R2");
    for (i = 0; i < dp->footer->nw; i++) create_widget_icons(dp, dp->footer->w[i], TREE_X, FOOT_Y);
    dp->nstatic = dp->ndecor;

    memset(&s, 0, sizeof s);
    s.handle = dp->win;
    if (!_swix(Wimp_GetWindowState, _IN(1), &s)) {
        dlg_centre(vis_w, vis_h, vbar, hbar, 40, &s.visible);
        s.scroll_x = 0;
        s.scroll_y = 0;
        s.behind = -1;
        _swix(Wimp_OpenWindow, _IN(1), &s);
    }
    dp->open = true;
}

void cfgwin_close(dlgparam *dp)
{
    dlgparam **pp;
    int i, j;

    if (dp->win < 0) return;
    for (i = 0; i < dp->npanels; i++)
        for (j = 0; j < dp->panels[i]->nw; j++) {
            Widget *w = dp->panels[i]->w[j];
            int k;
            for (k = 0; k < w->nicons; k++) {
                if (w->icons[k].own) free(w->icons[k].buf);
                sfree(w->icons[k].valid);
            }
            w->nicons = 0;
        }
    for (j = 0; j < dp->footer->nw; j++) {
        Widget *w = dp->footer->w[j];
        int k;
        for (k = 0; k < w->nicons; k++) {
            if (w->icons[k].own) free(w->icons[k].buf);
            sfree(w->icons[k].valid);
        }
        w->nicons = 0;
    }
    clear_tree(dp, false);
    clear_decor(dp, false, 0);
    sfree(dp->decor);
    dp->decor = NULL;
    _swix(Wimp_DeleteWindow, _IN(1), &dp->win);
    dp->win = -1;
    dp->open = false;
    for (pp = &all_dialogs; *pp; pp = &(*pp)->next)
        if (*pp == dp) { *pp = dp->next; break; }
}

/* ------------------------------------------------------------------------ */
/* finding what was clicked                                                 */

static Widget *find_icon(dlgparam *dp, int icon, CfgIcon **out)
{
    Panel *ps[2];
    int p, i, k;
    ps[0] = dp->cur;
    ps[1] = dp->footer;
    for (p = 0; p < 2; p++) {
        if (!ps[p]) continue;
        for (i = 0; i < ps[p]->nw; i++) {
            Widget *w = ps[p]->w[i];
            for (k = 0; k < w->nicons; k++)
                if (w->icons[k].handle == icon) {
                    if (out) *out = &w->icons[k];
                    return w;
                }
        }
    }
    return NULL;
}

static bool icon_selected(dlgparam *dp, int icon)
{
    struct { int win, icon; idef def; } st;
    st.win = dp->win;
    st.icon = icon;
    if (_swix(Wimp_GetIconState, _IN(1), &st)) return false;
    return (st.def.flags & IF_SELECTED) != 0;
}

static void note_focus(dlgparam *dp, Widget *w)
{
    if (dp->focus != w) {
        dp->prev_focus = dp->focus;
        dp->focus = w;
    }
}

/* ------------------------------------------------------------------------ */
/* pop-up menus (drop-down lists and combo boxes)                           */

static dlgparam *menu_dp;
static Widget *menu_w;
static menu *menu_cur;
static int menu_group_size;

static void popup_selection(const int *sel)
{
    dlgparam *dp = menu_dp;
    Widget *w = menu_w;
    int index;
    dlgparam *d;

    for (d = all_dialogs; d && d != dp; d = d->next) ;
    if (!d || !w || sel[0] < 0) return;
    index = menu_group_size ? sel[0] * menu_group_size + (sel[1] >= 0 ? sel[1] : 0) : sel[0];
    if (index < 0 || index >= w->nent) return;
    note_focus(dp, w);
    if (w->ctrl->type == CTRL_EDITBOX) {               /* a combo box: the choice becomes the text */
        char first[MAX_TEXT];
        cfg_cell_text(w->ent[index].text, 0, first, sizeof first);
        snprintf(w->text, MAX_TEXT, "%s", first);
        cfg_list_set_sel_only(w, index);
        cfgwin_update_widget(dp, w);
        cfg_emit(dp, w, EVENT_VALCHANGE);
    } else {                                            /* a drop-down: the choice is the selection */
        cfg_list_set_sel_only(w, index);
        w->cursel = index;
        cfgwin_update_widget(dp, w);
        cfg_emit(dp, w, EVENT_SELCHANGE);
    }
}

static void open_popup(dlgparam *dp, Widget *w)
{
    wstate s;
    int ox, oy, i, x, y;
    menu *m;
    const int group = 30;
    char first[40];

    if (!w->nent) return;
    s.handle = dp->win;
    if (_swix(Wimp_GetWindowState, _IN(1), &s)) return;
    widget_origin(dp, w, &ox, &oy);
    x = s.visible.x0 - s.scroll_x + (ox + w->x + w->fx) * UI_PX;
    y = s.visible.y1 - s.scroll_y - (oy + w->y + w->fy + w->fh) * UI_PX;

    menu_free(menu_cur);
    m = menu_cur = menu_new(w->label && strlen(w->label) < 12 ? w->label : "Choose");
    if (!m) return;
    menu_group_size = 0;
    if (w->nent <= 40) {
        for (i = 0; i < w->nent; i++) {
            cfg_cell_text(w->ent[i].text, 0, first, sizeof first);
            menu_add(m, first, (w->ctrl->type == CTRL_LISTBOX && i == w->cursel) ? MF_TICKED : 0);
        }
    } else {                                            /* a long list goes in submenus */
        int g;
        menu_group_size = group;
        for (g = 0; g * group < w->nent && g < MAX_MENU_ITEMS; g++) {
            char title[16];
            menu *sub;
            snprintf(title, sizeof title, "%d-%d", g * group + 1, (g + 1) * group < w->nent ? (g + 1) * group : w->nent);
            sub = menu_new(title);
            for (i = g * group; i < (g + 1) * group && i < w->nent; i++) {
                cfg_cell_text(w->ent[i].text, 0, first, sizeof first);
                menu_add(sub, first, 0);
            }
            menu_add(m, title, 0);
            menu_set_sub(m, g, sub);
        }
    }
    menu_dp = dp;
    menu_w = w;
    roapp_menu_opened(popup_selection);
    menu_open(m, x - 8, y);
}

/* ------------------------------------------------------------------------ */
/* the colour chooser and the font chooser                                  */

enum { K_RED = 1, K_GREEN, K_BLUE, K_CANCEL, K_OK, K_SWATCH };
static dlg *d_colour;
static dlgparam *colour_dp;
static int swatch_rgb[3] = { -1, -1, -1 };      /* what the swatch shows now */

/* The three fields as numbers, 0 to 255 (an empty or wrong field counts as 0). */
static void colour_fields(int v[3])
{
    const int ids[3] = { K_RED, K_GREEN, K_BLUE };
    int i;
    for (i = 0; i < 3; i++) {
        v[i] = atoi(dlg_get_text(d_colour, ids[i]));
        if (v[i] < 0) v[i] = 0;
        if (v[i] > 255) v[i] = 255;
    }
}

/* Show the colour the fields describe.  The Wimp edits a field without telling us, so this
 * is called when the chooser opens and now and then while it is open (roconfig_idle). */
static void colour_update_swatch(void)
{
    int v[3];
    if (!d_colour) return;
    colour_fields(v);
    if (v[0] == swatch_rgb[0] && v[1] == swatch_rgb[1] && v[2] == swatch_rgb[2]) return;
    swatch_rgb[0] = v[0]; swatch_rgb[1] = v[1]; swatch_rgb[2] = v[2];
    dlg_set_swatch(d_colour, K_SWATCH, v[0], v[1], v[2]);
}

bool cfgwin_colour_open(void)
{
    return d_colour && dlg_is_open(d_colour);
}

void cfgwin_colour_idle(void)
{
    if (cfgwin_colour_open()) colour_update_swatch();
}

static void colour_click(dlg *d, int id, int buttons, void *ud)
{
    dlgparam *dp = colour_dp, *it;
    Widget *w;
    (void)ud; (void)buttons;
    for (it = all_dialogs; it && it != dp; it = it->next) ;
    if (id == K_OK && it && it->colour_for) {
        int v[3], i;
        colour_fields(v);
        for (i = 0; i < 3; i++)
            it->colour_rgb[i] = v[i];
        it->colour_ok = true;
        w = it->colour_for;
        it->colour_for = NULL;
        dlg_close(d);
        cfg_emit(it, w, EVENT_CALLBACK);
        return;
    }
    if (it) { it->colour_ok = false; it->colour_for = NULL; }
    dlg_close(d);
}

void cfgwin_start_colour(dlgparam *dp, Widget *w, int r, int g, int b)
{
    char buf[8];
    if (!d_colour) {
        static const dlg_item items[] = {
            { DI_RLABEL, 0, 8, 14, 60, 22, "Red:", 0, 0, NULL },
            { DI_FIELD, K_RED, 72, 12, 56, 26, "", 6, 0, "The red part of the colour, 0 to 255." },
            { DI_RLABEL, 0, 8, 46, 60, 22, "Green:", 0, 0, NULL },
            { DI_FIELD, K_GREEN, 72, 44, 56, 26, "", 6, 0, "The green part of the colour, 0 to 255." },
            { DI_RLABEL, 0, 8, 78, 60, 22, "Blue:", 0, 0, NULL },
            { DI_FIELD, K_BLUE, 72, 76, 56, 26, "", 6, 0, "The blue part of the colour, 0 to 255." },
            { DI_SWATCH, K_SWATCH, 142, 12, 64, 90, "", 0, 0, "The colour these numbers make." },
            { DI_BUTTON, K_CANCEL, 78, 116, 66, 28, "Cancel", 0, 0, NULL },
            { DI_DEFBUTTON, K_OK, 152, 116, 56, 28, "OK", 0, 0, NULL },
        };
        d_colour = dlg_new("Colour", 216, 156, items, (int)(sizeof items / sizeof items[0]), colour_click, NULL);
        if (d_colour) dlg_set_keys(d_colour, K_OK, K_CANCEL);
    }
    if (!d_colour) return;
    colour_dp = dp;
    dp->colour_for = w;
    dp->colour_ok = false;
    snprintf(buf, sizeof buf, "%d", r); dlg_set_text(d_colour, K_RED, buf);
    snprintf(buf, sizeof buf, "%d", g); dlg_set_text(d_colour, K_GREEN, buf);
    snprintf(buf, sizeof buf, "%d", b); dlg_set_text(d_colour, K_BLUE, buf);
    swatch_rgb[0] = swatch_rgb[1] = swatch_rgb[2] = -1;        /* force the first update */
    dlg_open(d_colour);
    colour_update_swatch();
    dlg_focus(d_colour, K_RED);
}

/* ---- fonts ---- */

enum { FN_NAME = 1, FN_LIST, FN_SIZE, FN_CANCEL, FN_OK };
static dlg *d_font;
static dlgparam *font_dp;
static char **font_names;
static int nfont_names;

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The fonts the Font Manager knows, as Family.Weight identifiers. */
static void load_fonts(void)
{
    int ctr = 0;
    if (font_names) return;
    for (;;) {
        char id[128];
        int next = -1;
        id[0] = 0;
        if (_swix(Font_ListFonts, _INR(1, 3) | _OUT(2), id, (1 << 16) | ctr, (int)sizeof id, &next)) break;
        if (next == -1) break;
        if (id[0] && strchr(id, '.') && strchr(id, '.') == strrchr(id, '.')) {
            font_names = sresize(font_names, nfont_names + 1, char *);
            font_names[nfont_names++] = dupstr(id);
        }
        ctr = next;
        if (nfont_names >= 600) break;
    }
    if (nfont_names) qsort(font_names, (size_t)nfont_names, sizeof *font_names, name_cmp);
}

static menu *font_menu;

static void font_menu_selection(const int *sel)
{
    int index = sel[0] * 30 + (sel[1] >= 0 ? sel[1] : 0);
    if (nfont_names <= 40) index = sel[0];
    if (d_font && index >= 0 && index < nfont_names) dlg_set_text(d_font, FN_NAME, font_names[index]);
}

static void open_font_list(void)
{
    wpointer p;
    int i;
    load_fonts();
    if (!nfont_names || _swix(Wimp_GetPointerInfo, _IN(1), &p)) return;
    menu_free(font_menu);
    font_menu = menu_new("Fonts");
    if (!font_menu) return;
    if (nfont_names <= 40) {
        for (i = 0; i < nfont_names; i++) menu_add(font_menu, font_names[i], 0);
    } else {
        int g;
        for (g = 0; g * 30 < nfont_names && g < MAX_MENU_ITEMS; g++) {
            char title[16];
            menu *sub;
            snprintf(title, sizeof title, "%.10s...", font_names[g * 30]);
            sub = menu_new("Fonts");
            for (i = g * 30; i < (g + 1) * 30 && i < nfont_names; i++) menu_add(sub, font_names[i], 0);
            menu_add(font_menu, title, 0);
            menu_set_sub(font_menu, g, sub);
        }
    }
    roapp_menu_opened(font_menu_selection);
    menu_open(font_menu, p.x - 64, p.y);
}

static void font_click(dlg *d, int id, int buttons, void *ud)
{
    dlgparam *dp = font_dp, *it;
    (void)ud; (void)buttons;
    for (it = all_dialogs; it && it != dp; it = it->next) ;
    switch (id) {
      case FN_LIST:
        open_font_list();
        return;
      case FN_OK:
        if (it && it->font_for) {
            Widget *w = it->font_for;
            const char *name = dlg_get_text(d, FN_NAME), *size = dlg_get_text(d, FN_SIZE);
            snprintf(w->text, MAX_TEXT, "%s,%s", name, atoi(size) > 0 ? size : "12");
            it->font_for = NULL;
            dlg_close(d);
            cfgwin_update_widget(it, w);
            cfg_emit(it, w, EVENT_VALCHANGE);
            return;
        }
        break;
      default:
        break;
    }
    if (it) it->font_for = NULL;
    dlg_close(d);
}

static void change_font(dlgparam *dp, Widget *w)
{
    char name[96], *comma;
    if (!d_font) {
        static const dlg_item items[] = {
            { DI_RLABEL, 0, 6, 14, 60, 22, "Font:", 0, 0, NULL },
            { DI_FIELD, FN_NAME, 70, 12, 200, 26, "", 64, 0, "The font's name, such as Corpus.Medium." },
            { DI_BUTTON, FN_LIST, 276, 12, 70, 26, "List...", 0, 0, "Choose from the fonts the computer has." },
            { DI_RLABEL, 0, 6, 48, 60, 22, "Size:", 0, 0, NULL },
            { DI_FIELD, FN_SIZE, 70, 46, 56, 26, "", 6, 0, "The size in points." },
            { DI_BUTTON, FN_CANCEL, 200, 88, 70, 28, "Cancel", 0, 0, NULL },
            { DI_DEFBUTTON, FN_OK, 276, 88, 70, 28, "OK", 0, 0, NULL },
        };
        d_font = dlg_new("Font", 354, 128, items, (int)(sizeof items / sizeof items[0]), font_click, NULL);
        if (d_font) dlg_set_keys(d_font, FN_OK, FN_CANCEL);
    }
    if (!d_font) return;
    font_dp = dp;
    dp->font_for = w;
    snprintf(name, sizeof name, "%s", w->text);
    comma = strrchr(name, ',');
    if (comma) { *comma = 0; dlg_set_text(d_font, FN_SIZE, comma + 1); }
    else dlg_set_text(d_font, FN_SIZE, "12");
    dlg_set_text(d_font, FN_NAME, name);
    dlg_open(d_font);
    dlg_focus(d_font, FN_NAME);
}

/* ------------------------------------------------------------------------ */
/* clicks                                                                   */

/* The tree has room for about a screenful of rows, so keep open only the branch that
 * leads to the panel on show. */
static void collapse_others(dlgparam *dp, const Panel *keep)
{
    int i;
    for (i = 0; i < dp->npanels; i++) {
        const Panel *q;
        bool on_path = false;
        for (q = keep; q; q = q->parent)
            if (q == dp->panels[i]) on_path = true;
        if (!on_path) dp->panels[i]->expanded = false;
    }
}

static void select_panel(dlgparam *dp, Panel *p, unsigned buttons)
{
    if (p == dp->cur && panel_has_children(dp, p)) {
        p->expanded = !p->expanded;                     /* a second click folds the branch */
        rebuild_tree(dp);
        return;
    }
    collapse_others(dp, p);
    if (panel_has_children(dp, p)) p->expanded = true;
    cfgwin_show_panel(dp, p);
}

static void scroll_list(dlgparam *dp, Widget *w, int delta)
{
    int top = w->top + delta;
    if (top > w->nent - w->rows) top = w->nent - w->rows;
    if (top < 0) top = 0;
    w->top = top;
    cfgwin_update_widget(dp, w);
}

static void list_row_click(dlgparam *dp, Widget *w, int index, unsigned buttons)
{
    const unsigned now = ro_time();
    bool dbl;
    if (index < 0 || index >= w->nent) return;
    dbl = dp->last_click_widget == w && dp->last_click_row == index && (now - dp->last_click_cs) < 40;
    dp->last_click_widget = w;
    dp->last_click_row = index;
    dp->last_click_cs = now;
    note_focus(dp, w);
    if ((buttons & BTN_ADJUST) && w->ctrl->listbox.multisel)
        cfg_list_toggle(w, index);
    else
        cfg_list_set_sel_only(w, index);
    cfgwin_update_widget(dp, w);
    cfg_emit(dp, w, EVENT_SELCHANGE);
    if (dbl) cfg_emit(dp, w, EVENT_ACTION);
}

static void move_selected(dlgparam *dp, Widget *w, int dir)
{
    int sel = -1, i;
    for (i = 0; i < w->nent; i++) if (w->ent[i].sel) { sel = i; break; }
    if (sel < 0 || sel + dir < 0 || sel + dir >= w->nent) return;
    cfg_move_entry(w, sel, sel + dir);
    if (sel + dir < w->top) w->top = sel + dir;
    else if (sel + dir >= w->top + w->rows) w->top = sel + dir - w->rows + 1;
    cfgwin_update_widget(dp, w);
    cfg_emit(dp, w, EVENT_VALCHANGE);
}

static void widget_click(dlgparam *dp, Widget *w, CfgIcon *ci, unsigned buttons)
{
    dlgcontrol *c = w->ctrl;
    note_focus(dp, w);
    switch (ci->role) {
      case ROLE_MAIN:
        if (c->type == CTRL_CHECKBOX) {
            w->checked = icon_selected(dp, ci->handle);
            cfg_emit(dp, w, EVENT_VALCHANGE);
        } else if (c->type == CTRL_BUTTON) {
            cfg_emit(dp, w, EVENT_ACTION);
        } else if (c->type == CTRL_LISTBOX && w->rows == 0) {
            open_popup(dp, w);
        } else if (c->type == CTRL_FILESELECT && c->fileselect.just_button) {
            ro_notice(NULL, "PuTTY", "Drag a file onto this button.");
        }
        break;
      case ROLE_RADIO:
        w->radio = ci->arg;
        cfgwin_update_widget(dp, w);
        cfg_emit(dp, w, EVENT_VALCHANGE);
        break;
      case ROLE_POPUP:
        open_popup(dp, w);
        break;
      case ROLE_ROW:
        list_row_click(dp, w, w->top + ci->arg / 16, buttons);
        break;
      case ROLE_SCROLL_UP:
        scroll_list(dp, w, -(w->rows > 1 ? w->rows - 1 : 1));
        break;
      case ROLE_SCROLL_DOWN:
        scroll_list(dp, w, w->rows > 1 ? w->rows - 1 : 1);
        break;
      case ROLE_UP:
        move_selected(dp, w, -1);
        break;
      case ROLE_DOWN:
        move_selected(dp, w, 1);
        break;
      case ROLE_CHANGE:
        change_font(dp, w);
        break;
      default:
        break;
    }
}

int roconfig_event_click(wevent *e)
{
    dlgparam *dp = find_dp(e->mouse.window);
    CfgIcon *ci = NULL;
    Widget *w;
    int i;

    if (!dp) return 0;
    if (dp->ending) return 1;
    sync_fields(dp);
    for (i = 0; i < dp->nrows; i++)
        if (dp->rows[i].icon == e->mouse.icon) {
            select_panel(dp, dp->rows[i].panel, e->mouse.buttons);
            return 1;
        }
    w = find_icon(dp, e->mouse.icon, &ci);
    if (w && ci) widget_click(dp, w, ci, e->mouse.buttons);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* keys                                                                     */

static Widget *button_for(dlgparam *dp, bool cancel)
{
    int i;
    for (i = 0; i < dp->footer->nw; i++) {
        Widget *w = dp->footer->w[i];
        if (w->ctrl->type == CTRL_BUTTON &&
            (cancel ? w->ctrl->button.iscancel : w->ctrl->button.isdefault))
            return w;
    }
    return NULL;
}

/* Move the caret to the next (dir 1) or previous (-1) text field of the panel. */
static void next_field(dlgparam *dp, int icon, int dir)
{
    Panel *p = dp->cur;
    int i, k, n = 0, cur = -1;
    struct { Widget *w; int icon; } f[64];

    if (!p) return;
    for (i = 0; i < p->nw && n < 64; i++)
        for (k = 0; k < p->w[i]->nicons && n < 64; k++)
            if (is_writable(p->w[i], &p->w[i]->icons[k])) {
                f[n].w = p->w[i];
                f[n].icon = p->w[i]->icons[k].handle;
                if (f[n].icon == icon) cur = n;
                n++;
            }
    if (!n) return;
    cur = (cur + dir + n) % n;
    caret_to_icon(dp, f[cur].icon, f[cur].w->text ? f[cur].w->text : "");
    note_focus(dp, f[cur].w);
}

int roconfig_event_key(wevent *e)
{
    dlgparam *dp = find_dp(e->key.window);
    CfgIcon *ci = NULL;
    Widget *w;
    int code;

    if (!dp) return 0;
    if (dp->ending) return 1;
    code = e->key.key;
    if (code == 0x0D) {
        Widget *b = button_for(dp, false);
        sync_fields(dp);
        if (b) cfg_emit(dp, b, EVENT_ACTION);
        return 1;
    }
    if (code == 0x1B) {
        Widget *b = button_for(dp, true);
        if (b) cfg_emit(dp, b, EVENT_ACTION);
        else dlg_end(dp, 0);
        return 1;
    }
    if (code == 0x18A || code == 0x18E) { sync_fields(dp); next_field(dp, e->key.icon, 1); return 1; }
    if (code == 0x19A || code == 0x18F) { sync_fields(dp); next_field(dp, e->key.icon, -1); return 1; }

    w = find_icon(dp, e->key.icon, &ci);
    _swix(Wimp_ProcessKey, _IN(0), code);
    if (w && ci && is_writable(w, ci)) {
        note_focus(dp, w);
        cfg_emit(dp, w, EVENT_VALCHANGE);
    }
    return 1;
}

bool roconfig_raise(void *ctx)
{
    dlgparam *dp;
    for (dp = all_dialogs; dp; dp = dp->next) {
        wstate s;
        if (dp->done_ctx != ctx || dp->ending) continue;
        s.handle = dp->win;
        if (!_swix(Wimp_GetWindowState, _IN(1), &s)) {
            s.behind = -1;
            _swix(Wimp_OpenWindow, _IN(1), &s);
        }
        return true;
    }
    return false;
}

void roconfig_end_for(void *ctx)
{
    dlgparam *dp;
    for (dp = all_dialogs; dp; dp = dp->next)
        if (dp->done_ctx == ctx) dlg_end(dp, 0);
}

int roconfig_event_open(wevent *e)
{
    dlgparam *dp = find_dp(e->words[0]);
    if (!dp) return 0;
    _swix(Wimp_OpenWindow, _IN(1), e);
    return 1;
}

int roconfig_event_close(wevent *e)
{
    dlgparam *dp = find_dp(e->words[0]);
    Widget *b;
    if (!dp) return 0;
    if (!dp->ending) {
        b = button_for(dp, true);
        if (b) cfg_emit(dp, b, EVENT_ACTION);
        else dlg_end(dp, 0);
    }
    return 1;
}

int roconfig_event_scroll(wevent *e)
{
    dlgparam *dp = find_dp(e->words[0]);
    wpointer p;
    CfgIcon *ci = NULL;
    Widget *w;
    if (!dp) return 0;
    if (_swix(Wimp_GetPointerInfo, _IN(1), &p)) return 1;
    w = find_icon(dp, p.icon, &ci);
    if (w && w->ctrl->type == CTRL_LISTBOX && w->rows > 0)
        scroll_list(dp, w, e->words[9] > 0 ? -3 : 3);               /* (+36 is the y direction: see roterm_scroll_request) */
    return 1;
}

/* ------------------------------------------------------------------------ */
/* files dropped on a file field                                            */

int cfgwin_message(const wmessage *m)
{
    dlgparam *dp;
    Widget *w = NULL;
    CfgIcon *ci = NULL;
    char path[200];
    int i;

    if (m->action != MSG_DATA_LOAD) return 0;
    dp = find_dp(m->data[0]);
    if (!dp) return 0;
    if (!dp->ending) {
        w = find_icon(dp, m->data[1], &ci);
        if (!w || w->ctrl->type != CTRL_FILESELECT) {   /* not on a field: use the panel's first */
            w = NULL;
            if (dp->cur)
                for (i = 0; i < dp->cur->nw; i++)
                    if (dp->cur->w[i]->ctrl->type == CTRL_FILESELECT && dp->cur->w[i]->nicons) {
                        w = dp->cur->w[i];
                        break;
                    }
        }
        wimp_msg_path(m, path, sizeof path);
        if (w && path[0]) {
            if (w->ctrl->fileselect.just_button) {
                if (w->dropped) filename_free(w->dropped);
                w->dropped = filename_from_str(path);
                cfg_emit(dp, w, EVENT_ACTION);
            } else {
                snprintf(w->text, MAX_TEXT, "%s", path);
                cfgwin_update_widget(dp, w);
                cfg_emit(dp, w, EVENT_VALCHANGE);
            }
            wimp_send_load_ack(m, path);
        }
    }
    return 1;
}

int roconfig_message(const wmessage *m)
{
    return cfgwin_message(m);
}
