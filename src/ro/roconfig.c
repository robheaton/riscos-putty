/*
 * roconfig.c - the configuration dialogue's model: PuTTY's control tree turned into
 * panels of widgets, their layout, and the dlg_* functions PuTTY's handlers call to
 * read and write the controls.  See roconfig_i.h.  The window is rocfgwin.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "ssh.h"
#include "storage.h"
#include "ro.h"
#include "rowimp.h"
#include "rodialog.h"
#include "roconfig.h"
#include "roconfig_i.h"

#define MAX_COLS 16

/* The host CA configuration box is not provided here (yet). */
const bool has_ca_config_box = false;

void show_ca_config_box(dlgparam *dlg)
{
    ro_notice(NULL, "PuTTY", "Host certification authorities cannot be configured in this version.");
}

/* ------------------------------------------------------------------------ */
/* text                                                                     */

static int desktop_codepage(void)
{
    static int cp = -1;
    if (cp < 0) {
        int a = 0;
        _swix(OS_Byte, _INR(0, 1) | _OUT(1), 71, 127, &a);
        cp = (a == 111) ? CS_UTF8 : CS_ISO8859_1;
    }
    return cp;
}

static char *convert(const char *s, int from, int to)
{
    wchar_t *w = dup_mb_to_wc(from, s);
    char *r = dup_wc_to_mb(to, w, "?");
    sfree(w);
    return r;
}

static char *desktop_to_utf8(const char *s)
{
    return desktop_codepage() == CS_UTF8 ? dupstr(s) : convert(s, CS_ISO8859_1, CS_UTF8);
}

static char *utf8_to_desktop(const char *s)
{
    return desktop_codepage() == CS_UTF8 ? dupstr(s) : convert(s, CS_UTF8, CS_ISO8859_1);
}

/* Break text into lines of at most maxchars characters at spaces (and at newlines).
 * Fills start[] and len[] for up to maxlines lines (either may be NULL) and
 * returns the number of lines the text needs. */
int cfg_wrap(const char *text, int maxchars, const char **start, int *len, int maxlines)
{
    int n = 0;
    const char *p = text;
    if (maxchars < 4) maxchars = 4;
    do {
        const char *eol = strchr(p, '\n');
        const int avail = eol ? (int)(eol - p) : (int)strlen(p);
        int take = avail;
        if (take > maxchars) {
            take = maxchars;
            while (take > 0 && p[take] != ' ') take--;
            if (take == 0) take = maxchars;            /* a word too long for a line: break it */
        }
        if (n < maxlines) {
            if (start) start[n] = p;
            if (len) len[n] = take;
        }
        n++;
        p += take;
        if (*p == ' ') p++;
        else if (*p == '\n') p++;
    } while (*p);
    return n;
}

/* The text of column `col` of a tab-separated list entry. */
void cfg_cell_text(const char *text, int col, char *out, size_t cap)
{
    const char *p = text;
    size_t n;
    while (col > 0 && p) {
        p = strchr(p, '\t');
        if (p) p++;
        col--;
    }
    if (!p) { out[0] = 0; return; }
    n = strcspn(p, "\t");
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

/* ------------------------------------------------------------------------ */
/* building the panels                                                      */

static void add_widget(Panel *p, Widget *w)
{
    if (p->nw == p->wcap) {
        p->wcap = p->wcap ? p->wcap * 2 : 16;
        p->w = sresize(p->w, p->wcap, Widget *);
    }
    p->w[p->nw++] = w;
}

static Box *add_box(Panel *p)
{
    if (p->nbox == p->boxcap) {
        p->boxcap = p->boxcap ? p->boxcap * 2 : 4;
        p->box = sresize(p->box, p->boxcap, Box);
    }
    memset(&p->box[p->nbox], 0, sizeof(Box));
    return &p->box[p->nbox++];
}

static Panel *new_panel(const char *path)
{
    Panel *p = snew(Panel);
    const char *s;
    memset(p, 0, sizeof *p);
    p->path = dupstr(path);
    for (s = path; *s; s++)
        if (*s == '/') p->depth++;
    return p;
}

static Panel *find_panel(dlgparam *dp, const char *path)
{
    int i;
    for (i = 0; i < dp->npanels; i++)
        if (!strcmp(dp->panels[i]->path, path)) return dp->panels[i];
    return NULL;
}

static Widget *new_widget(Panel *p, dlgcontrol *c)
{
    Widget *w = snew(Widget);
    memset(w, 0, sizeof *w);
    w->ctrl = c;
    w->panel = p;
    w->radio = -1;
    w->cursel = -1;
    if (c->label) w->label = dupstr(c->label);
    switch (c->type) {
      case CTRL_EDITBOX:
      case CTRL_FILESELECT:
      case CTRL_FONTSELECT:
        w->text = snewn(MAX_TEXT, char);
        w->text[0] = 0;
        break;
      case CTRL_LISTBOX:
        w->rows = c->listbox.height;
        break;
    }
    return w;
}

/* Size a control (its label, the control proper and any button beside it) in a
 * space ww pixels wide. */
static void measure(Widget *w, int ww)
{
    dlgcontrol *c = w->ctrl;
    int perc, extra;

    w->lw = w->lh = w->fx = w->fy = w->fw = w->fh = w->bx = w->bw = 0;
    switch (c->type) {
      case CTRL_TEXT: {
        int n = cfg_wrap(w->label ? w->label : "", ww / CHAR_W, NULL, NULL, 0);
        w->lw = ww;
        w->lh = n * LINE_H;
        w->h = w->lh;
        break;
      }
      case CTRL_EDITBOX:
        perc = c->editbox.percentwidth;
        extra = c->editbox.has_list ? POPUP_W + 2 : 0;
        goto labelled_field;
      case CTRL_LISTBOX:
        if (c->listbox.height > 0) {
            const int draglist = c->listbox.draglist ? 70 : 0;
            w->lw = ww;
            w->lh = w->label ? LINE_H : 0;
            w->fx = 0;
            w->fy = w->lh;
            w->fw = ww - draglist;
            w->fh = c->listbox.height * LIST_ROW_H + 4;
            if (draglist) { w->bx = ww - 64; w->bw = 64; }
            w->h = w->fy + w->fh;
            break;
        }
        perc = c->listbox.percentwidth;                /* a drop-down list */
        extra = POPUP_W + 2;
      labelled_field:
        if (perc <= 0 || perc > 100) perc = 100;
        if (w->label && perc >= 100) {
            w->lw = ww; w->lh = LINE_H;
            w->fx = 0; w->fy = LINE_H; w->fw = ww - extra; w->fh = FIELD_H;
            w->h = LINE_H + FIELD_H;
        } else if (w->label) {
            w->lw = ww * (100 - perc) / 100 - COL_GAP; w->lh = FIELD_H;
            w->fx = w->lw + COL_GAP; w->fy = 0; w->fw = ww - w->fx - extra; w->fh = FIELD_H;
            w->h = FIELD_H;
        } else {
            w->fw = ww * perc / 100 - extra; w->fh = FIELD_H;
            w->h = FIELD_H;
        }
        if (extra) { w->bx = w->fx + w->fw + 2; w->bw = POPUP_W; }
        break;
      case CTRL_RADIO: {
        const int nc = c->radio.ncolumns < 1 ? 1 : c->radio.ncolumns;
        const int rows = (c->radio.nbuttons + nc - 1) / nc;
        w->lw = ww;
        w->lh = w->label ? LINE_H : 0;
        w->fy = w->lh;
        w->fw = ww;
        w->fh = rows * CHECK_H;
        w->h = w->fy + w->fh;
        break;
      }
      case CTRL_CHECKBOX:
        w->fw = ww; w->fh = CHECK_H;
        w->h = CHECK_H;
        break;
      case CTRL_BUTTON: {
        int bw = (int)strlen(w->label ? w->label : "") * CHAR_W + 32;
        if (bw < 80) bw = 80;
        w->fw = bw > ww ? ww : bw;
        w->fh = BUTTON_H;
        w->h = BUTTON_H;
        break;
      }
      case CTRL_FILESELECT:
        w->lw = ww;
        if (c->fileselect.just_button) {
            int bw = (int)strlen(w->label ? w->label : "") * CHAR_W + 32;
            if (bw < 120) bw = 120;
            w->fw = bw > ww ? ww : bw;
            w->fh = BUTTON_H;
            w->h = BUTTON_H;
        } else {
            w->lh = w->label ? LINE_H : 0;
            w->fy = w->lh;
            w->fw = ww;
            w->fh = FIELD_H;
            w->h = w->fy + FIELD_H;
        }
        break;
      case CTRL_FONTSELECT:
        w->lw = ww;
        w->lh = w->label ? LINE_H : 0;
        w->fy = w->lh;
        w->fw = ww - 104;
        w->fh = FIELD_H;
        w->bx = w->fw + 6; w->bw = 98;
        w->h = w->fy + FIELD_H;
        break;
      default:
        w->h = 0;
        break;
    }
}

/*
 * Lay out one panel's controlsets.  A controlset with a title is a framed box.
 * Inside it PuTTY's columns apply as in the other front ends: each control occupies
 * some columns and goes below the lowest thing already in any of them.
 */
static void layout_panel(dlgparam *dp, Panel *p, int path_matches_footer)
{
    struct controlbox *b = dp->ctrlbox;
    int cur_y = 0;
    size_t i, k;

    for (i = 0; i < b->nctrlsets; i++) {
        struct controlset *cs = b->ctrlsets[i];
        const bool framed = cs->boxtitle && cs->boxtitle[0] && !path_matches_footer;
        const int inner_x = framed ? BOX_PAD : 2;
        const int inner_w = p->width - 2 * inner_x;
        int ncols = 1, ypos[MAX_COLS], pct[MAX_COLS], colx[MAX_COLS + 1], c, y, ybase, maxy;
        Box *box = NULL;

        if (strcmp(cs->pathname, p->path) != 0 || !cs->boxname) continue;
        if (!cs->ncontrols) continue;

        ctrlset_normalise_aligns(cs);
        if (framed) {
            box = add_box(p);
            box->title = dupstr(cs->boxtitle);
            box->y0 = cur_y;
        }
        ybase = cur_y + (framed ? BOX_TITLE_H : 0);
        for (c = 0; c < MAX_COLS; c++) { ypos[c] = ybase; pct[c] = 100; }
        colx[0] = 0; colx[1] = inner_w;

        for (k = 0; k < cs->ncontrols; k++) {
            dlgcontrol *ctl = cs->ctrls[k];
            Widget *w;
            int cstart, cspan, x0, x1;

            if (ctl->type == CTRL_COLUMNS) {
                int total = 0;
                maxy = ybase;
                for (c = 0; c < ncols; c++) if (ypos[c] > maxy) maxy = ypos[c];
                ncols = ctl->columns.ncols;
                if (ncols < 1) ncols = 1;
                if (ncols > MAX_COLS) ncols = MAX_COLS;
                for (c = 0; c < ncols; c++) {
                    pct[c] = ctl->columns.percentages ? ctl->columns.percentages[c] : 100 / ncols;
                    ypos[c] = maxy;
                }
                colx[0] = 0;
                for (c = 0; c < ncols; c++) {
                    total += pct[c];
                    colx[c + 1] = inner_w * total / 100;
                }
                colx[ncols] = inner_w;
                continue;
            }
            if (ctl->type == CTRL_TABDELAY) continue;

            w = new_widget(p, ctl);
            cstart = COLUMN_START(ctl->column);
            cspan = COLUMN_SPAN(ctl->column);
            if (cstart >= ncols) cstart = ncols - 1;
            if (cstart + cspan > ncols) cspan = ncols - cstart;
            x0 = colx[cstart];
            x1 = colx[cstart + cspan];
            w->colstart = cstart;
            w->colspan = cspan;
            w->x = inner_x + x0;
            measure(w, x1 - x0 - (cstart + cspan < ncols ? COL_GAP : 0));
            w->w = x1 - x0 - (cstart + cspan < ncols ? COL_GAP : 0);

            y = ybase;
            for (c = cstart; c < cstart + cspan; c++) if (ypos[c] > y) y = ypos[c];
            if (ctl->align_next_to) {                   /* sit alongside an earlier control */
                int j;
                for (j = 0; j < p->nw; j++)
                    if (p->w[j]->ctrl == ctl->align_next_to) {
                        Widget *o = p->w[j];
                        const int oh = o->h, mh = oh > w->h ? oh : w->h;
                        y = o->y;
                        o->y += (mh - oh) / 2;           /* centre each in the taller height */
                        y += (mh - w->h) / 2;
                        break;
                    }
            }
            w->y = y;
            for (c = cstart; c < cstart + cspan; c++)
                if (ypos[c] < w->y + w->h + ROW_GAP) ypos[c] = w->y + w->h + ROW_GAP;
            add_widget(p, w);
        }

        maxy = ybase;
        for (c = 0; c < ncols; c++) if (ypos[c] > maxy) maxy = ypos[c];
        if (framed) {
            box->y1 = maxy + BOX_BOTTOM - ROW_GAP;
            cur_y = box->y1 + BOX_GAP;
        } else {
            cur_y = maxy + (BOX_GAP / 2);
        }
    }
    p->height = cur_y;
}

/* ------------------------------------------------------------------------ */
/* building the dialogue                                                    */

dlgparam *cfg_build(Conf *conf, bool midsession, int protocol, int protcfginfo)
{
    dlgparam *dp = snew(dlgparam);
    struct controlbox *b;
    size_t i;

    memset(dp, 0, sizeof *dp);
    dp->conf = conf;
    dp->midsession = midsession;
    dp->protocol = protocol;
    dp->protcfginfo = protcfginfo;
    dp->win = -1;
    dp->ctrlbox = b = ctrl_new_box();
    setup_config_box(b, midsession, protocol, protcfginfo);

    dp->footer = new_panel("");
    dp->footer->is_footer = true;
    dp->footer->width = CFG_W - 2 * TREE_X;

    for (i = 0; i < b->nctrlsets; i++) {
        struct controlset *cs = b->ctrlsets[i];
        Panel *p;
        char *slash;
        if (!cs->pathname[0]) continue;                 /* the footer */
        if ((p = find_panel(dp, cs->pathname)) == NULL) {
            p = new_panel(cs->pathname);
            slash = strrchr(cs->pathname, '/');
            if (slash) {
                char *parent = dupprintf("%.*s", (int)(slash - cs->pathname), cs->pathname);
                p->parent = find_panel(dp, parent);
                sfree(parent);
            }
            p->width = PANEL_W;
            dp->panels = sresize(dp->panels, dp->npanels + 1, Panel *);
            dp->panels[dp->npanels++] = p;
        }
        if (!cs->boxname && cs->boxtitle && !p->title)
            p->title = dupstr(cs->boxtitle);
    }
    for (i = 0; i < (size_t)dp->npanels; i++) layout_panel(dp, dp->panels[i], 0);
    layout_panel(dp, dp->footer, 1);
    dp->cur = dp->panels[0];
    return dp;
}

static void free_widget(Widget *w)
{
    int i;
    sfree(w->label);
    sfree(w->text);
    for (i = 0; i < w->nent; i++) sfree(w->ent[i].text);
    sfree(w->ent);
    if (w->dropped) filename_free(w->dropped);
    sfree(w->icons);
    sfree(w);
}

static void free_panel(Panel *p)
{
    int i, j;
    if (!p) return;
    for (i = 0; i < p->nw; i++) free_widget(p->w[i]);
    sfree(p->w);
    for (j = 0; j < p->nbox; j++) sfree(p->box[j].title);
    sfree(p->box);
    sfree(p->path);
    sfree(p->title);
    sfree(p);
}

void cfg_free(dlgparam *dp)
{
    int i;
    if (!dp) return;
    for (i = 0; i < dp->npanels; i++) free_panel(dp->panels[i]);
    sfree(dp->panels);
    free_panel(dp->footer);
    sfree(dp->rows);
    ctrl_free_box(dp->ctrlbox);
    sfree(dp);
}

/* ------------------------------------------------------------------------ */
/* finding widgets                                                          */

Widget *cfg_find_widget(dlgparam *dp, const dlgcontrol *c)
{
    int i, j;
    for (i = 0; i < dp->npanels; i++)
        for (j = 0; j < dp->panels[i]->nw; j++)
            if (dp->panels[i]->w[j]->ctrl == c) return dp->panels[i]->w[j];
    for (j = 0; j < dp->footer->nw; j++)
        if (dp->footer->w[j]->ctrl == c) return dp->footer->w[j];
    return NULL;
}

void cfg_emit(dlgparam *dp, Widget *w, int event)
{
    if (w->ctrl->handler)
        w->ctrl->handler(w->ctrl, dp, dp->conf, event);
}

/* ------------------------------------------------------------------------ */
/* list helpers                                                             */

void cfg_list_set_sel_only(Widget *w, int index)
{
    int i;
    for (i = 0; i < w->nent; i++) w->ent[i].sel = (i == index);
    if (w->ctrl->type == CTRL_LISTBOX && w->rows == 0) w->cursel = index;
}

void cfg_list_toggle(Widget *w, int index)
{
    if (index >= 0 && index < w->nent) w->ent[index].sel = !w->ent[index].sel;
}

/* Move an entry (with its selection) from one place to another. */
void cfg_move_entry(Widget *w, int from, int to)
{
    Entry e;
    int i;
    if (from < 0 || from >= w->nent || to < 0 || to >= w->nent || from == to) return;
    e = w->ent[from];
    if (from < to) for (i = from; i < to; i++) w->ent[i] = w->ent[i + 1];
    else for (i = from; i > to; i--) w->ent[i] = w->ent[i - 1];
    w->ent[to] = e;
}

/* ------------------------------------------------------------------------ */
/* the dlg_* interface PuTTY's handlers use                                 */

void dlg_radiobutton_set(dlgcontrol *ctrl, dlgparam *dp, int which)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    if (!w) return;
    w->radio = which;
    cfgwin_update_widget(dp, w);
}

int dlg_radiobutton_get(dlgcontrol *ctrl, dlgparam *dp)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    return w ? w->radio : -1;
}

void dlg_checkbox_set(dlgcontrol *ctrl, dlgparam *dp, bool checked)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    if (!w) return;
    w->checked = checked;
    cfgwin_update_widget(dp, w);
}

bool dlg_checkbox_get(dlgcontrol *ctrl, dlgparam *dp)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    return w ? w->checked : false;
}

static void set_text(Widget *w, const char *text)
{
    if (!w->text) w->text = snewn(MAX_TEXT, char);
    snprintf(w->text, MAX_TEXT, "%s", text);
}

void dlg_editbox_set(dlgcontrol *ctrl, dlgparam *dp, char const *text)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    if (!w) return;
    set_text(w, text);
    cfgwin_update_widget(dp, w);
}

void dlg_editbox_set_utf8(dlgcontrol *ctrl, dlgparam *dp, char const *text)
{
    char *s = utf8_to_desktop(text);
    dlg_editbox_set(ctrl, dp, s);
    sfree(s);
}

char *dlg_editbox_get(dlgcontrol *ctrl, dlgparam *dp)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    return dupstr(w && w->text ? w->text : "");
}

char *dlg_editbox_get_utf8(dlgcontrol *ctrl, dlgparam *dp)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    return desktop_to_utf8(w && w->text ? w->text : "");
}

void dlg_editbox_select_range(dlgcontrol *ctrl, dlgparam *dp, size_t start, size_t len)
{
    /* the caret goes to the end when the field is focused; nothing to do */
}

void dlg_listbox_clear(dlgcontrol *ctrl, dlgparam *dp)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    int i;
    if (!w) return;
    for (i = 0; i < w->nent; i++) sfree(w->ent[i].text);
    w->nent = 0;
    w->top = 0;
    w->cursel = -1;
    cfgwin_update_widget(dp, w);
}

void dlg_listbox_del(dlgcontrol *ctrl, dlgparam *dp, int index)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    if (!w || index < 0 || index >= w->nent) return;
    sfree(w->ent[index].text);
    memmove(&w->ent[index], &w->ent[index + 1], (size_t)(w->nent - index - 1) * sizeof(Entry));
    w->nent--;
    if (w->cursel >= w->nent) w->cursel = w->nent - 1;
    cfgwin_update_widget(dp, w);
}

void dlg_listbox_addwithid(dlgcontrol *ctrl, dlgparam *dp, char const *text, int id)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    if (!w) return;
    if (w->nent == w->entcap) {
        w->entcap = w->entcap ? w->entcap * 2 : 16;
        w->ent = sresize(w->ent, w->entcap, Entry);
    }
    w->ent[w->nent].text = dupstr(text);
    w->ent[w->nent].id = id;
    w->ent[w->nent].sel = false;
    w->nent++;
    cfgwin_update_widget(dp, w);
}

void dlg_listbox_add(dlgcontrol *ctrl, dlgparam *dp, char const *text)
{
    dlg_listbox_addwithid(ctrl, dp, text, 0);
}

int dlg_listbox_getid(dlgcontrol *ctrl, dlgparam *dp, int index)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    return (w && index >= 0 && index < w->nent) ? w->ent[index].id : 0;
}

int dlg_listbox_index(dlgcontrol *ctrl, dlgparam *dp)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    int i, found = -1, count = 0;
    if (!w) return -1;
    if (ctrl->type == CTRL_LISTBOX && w->rows == 0)
        return w->cursel;                              /* a drop-down */
    for (i = 0; i < w->nent; i++)
        if (w->ent[i].sel) { found = i; count++; }
    return count == 1 ? found : -1;
}

bool dlg_listbox_issel(dlgcontrol *ctrl, dlgparam *dp, int index)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    if (!w || index < 0 || index >= w->nent) return false;
    if (ctrl->type == CTRL_LISTBOX && w->rows == 0) return w->cursel == index;
    return w->ent[index].sel;
}

void dlg_listbox_select(dlgcontrol *ctrl, dlgparam *dp, int index)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    if (!w) return;
    if (index >= w->nent) index = w->nent - 1;
    cfg_list_set_sel_only(w, index);
    if (index >= 0 && w->rows > 0) {                   /* scroll it into view */
        if (index < w->top) w->top = index;
        else if (index >= w->top + w->rows) w->top = index - w->rows + 1;
    }
    cfgwin_update_widget(dp, w);
}

void dlg_text_set(dlgcontrol *ctrl, dlgparam *dp, char const *text)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    if (!w) return;
    sfree(w->label);
    w->label = dupstr(text);
    cfgwin_update_label(dp, w);
}

void dlg_filesel_set(dlgcontrol *ctrl, dlgparam *dp, Filename *fn)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    if (!w) return;
    set_text(w, fn ? filename_to_str(fn) : "");
    cfgwin_update_widget(dp, w);
}

Filename *dlg_filesel_get(dlgcontrol *ctrl, dlgparam *dp)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    if (!w) return filename_from_str("");
    if (ctrl->fileselect.just_button)
        return w->dropped ? filename_copy(w->dropped) : filename_from_str("");
    return filename_from_str(w->text ? w->text : "");
}

void dlg_fontsel_set(dlgcontrol *ctrl, dlgparam *dp, FontSpec *fn)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    if (!w) return;
    set_text(w, fn && fn->name ? fn->name : "");
    cfgwin_update_widget(dp, w);
}

FontSpec *dlg_fontsel_get(dlgcontrol *ctrl, dlgparam *dp)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    return fontspec_new(w && w->text ? w->text : "");
}

void dlg_update_start(dlgcontrol *ctrl, dlgparam *dp)
{
}

void dlg_update_done(dlgcontrol *ctrl, dlgparam *dp)
{
}

void dlg_set_focus(dlgcontrol *ctrl, dlgparam *dp)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    if (w) cfgwin_set_focus(dp, w);
}

void dlg_label_change(dlgcontrol *ctrl, dlgparam *dp, char const *text)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    if (!w) return;
    sfree(w->label);
    w->label = dupstr(text);
    cfgwin_update_label(dp, w);
}

dlgcontrol *dlg_last_focused(dlgcontrol *ctrl, dlgparam *dp)
{
    if (dp->focus && dp->focus->ctrl != ctrl) return dp->focus->ctrl;
    if (dp->prev_focus && dp->prev_focus->ctrl != ctrl) return dp->prev_focus->ctrl;
    return NULL;
}

bool dlg_is_visible(dlgcontrol *ctrl, dlgparam *dp)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    return w && cfgwin_is_visible(dp, w);
}

void dlg_beep(dlgparam *dp)
{
    _swix(OS_WriteI + 7, 0);
}

void dlg_error_msg(dlgparam *dp, const char *msg)
{
    ro_notice(NULL, "PuTTY Error", msg);
}

/* The dialogue ends from a callback, not at once: the handler that called dlg_end is
 * still on the stack and the dialogue is about to be freed. */
static void finish(void *ctx)
{
    dlgparam *dp = (dlgparam *)ctx;
    const int value = dp->end_value;
    cfgwin_close(dp);
    if (dp->done) {
        RoConfigDone done = dp->done;
        void *done_ctx = dp->done_ctx;
        Conf *conf = dp->conf;
        dp->conf = NULL;
        dp->done = NULL;
        if (value) done(done_ctx, true, conf);
        else {
            done(done_ctx, false, NULL);
            conf_free(conf);
        }
    }
    cfg_free(dp);
}

void dlg_end(dlgparam *dp, int value)
{
    if (dp->ending) return;
    dp->ending = true;
    dp->end_value = value;
    queue_toplevel_callback(finish, dp);
}

void dlg_coloursel_start(dlgcontrol *ctrl, dlgparam *dp, int r, int g, int b)
{
    Widget *w = cfg_find_widget(dp, ctrl);
    if (w) cfgwin_start_colour(dp, w, r, g, b);
}

bool dlg_coloursel_results(dlgcontrol *ctrl, dlgparam *dp, int *r, int *g, int *b)
{
    if (!dp->colour_ok) return false;
    *r = dp->colour_rgb[0];
    *g = dp->colour_rgb[1];
    *b = dp->colour_rgb[2];
    return true;
}

void dlg_refresh(dlgcontrol *ctrl, dlgparam *dp)
{
    if (ctrl) {
        Widget *w = cfg_find_widget(dp, ctrl);
        if (w) cfg_emit(dp, w, EVENT_REFRESH);
    } else {
        int i, j;
        for (i = 0; i < dp->npanels; i++)
            for (j = 0; j < dp->panels[i]->nw; j++)
                cfg_emit(dp, dp->panels[i]->w[j], EVENT_REFRESH);
        for (j = 0; j < dp->footer->nw; j++)
            cfg_emit(dp, dp->footer->w[j], EVENT_REFRESH);
    }
}

/* ------------------------------------------------------------------------ */

void roconfig_open(Conf *conf, bool midsession, int protocol, int protcfginfo,
                   const char *title, RoConfigDone done, void *ctx)
{
    dlgparam *dp = cfg_build(conf, midsession, protocol, protcfginfo);
    snprintf(dp->title, sizeof dp->title, "%s", title ? title : "PuTTY Configuration");
    dp->done = done;
    dp->done_ctx = ctx;
    cfgwin_create(dp);
    dlg_refresh(NULL, dp);
    cfgwin_show_panel(dp, dp->cur);
}
