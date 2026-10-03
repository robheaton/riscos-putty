/*
 * roconfig_i.h - the configuration dialogue's internals, shared by roconfig.c (the
 * model: PuTTY's control tree, the dlg_* interface, layout) and rocfgwin.c (the
 * Wimp window: icons, events, menus).
 *
 * PuTTY describes its configuration box as a tree of controls (config.c fills in a
 * `struct controlbox`) and expects the front end to draw it, to keep each control's
 * value, and to call the control's handler when the user does something.  We keep
 * the value of every control in a Widget (the model), lay the controls out once
 * (in pixels, from the percentages and columns PuTTY gives), and create the icons
 * for a panel only while that panel is on show.
 */
#ifndef PUTTY_ROCONFIG_I_H
#define PUTTY_ROCONFIG_I_H

#include <stdbool.h>
#include "putty.h"
#include "dialog.h"
#include "rowimp.h"
#include "roconfig.h"

/* ---- layout constants (pixels; the Wimp's text is about 8 px a character) ---- */
#define CFG_W          720          /* the dialogue's work area */
#define CFG_H          580
#define TREE_X         10
#define TREE_Y         10
#define TREE_W         190
#define TREE_H         500
#define PANEL_X        210
#define PANEL_Y        10
#define PANEL_W        500
#define PANEL_H        500
#define HEAD_H         30           /* a panel's heading, above its first box */
#define FOOT_Y         524
#define FOOT_H         50

#define BOX_PAD        10           /* inside a box, at the sides */
#define BOX_TITLE_H    20           /* room for a box's title */
#define BOX_BOTTOM     6
#define BOX_GAP        10           /* between boxes */
#define ROW_GAP        4
#define COL_GAP        8
#define LINE_H         22
#define FIELD_H        26
#define CHECK_H        24
#define BUTTON_H       28
#define LIST_ROW_H     22
#define CHAR_W         8
#define POPUP_W        26           /* the little button beside a drop-down */
#define SCROLL_W       22

#define MAX_TEXT       512

typedef struct Entry {
    char *text;
    int   id;
    bool  sel;
} Entry;

/* What an icon is for, so a click can be traced back to a control. */
enum {
    ROLE_LABEL, ROLE_MAIN, ROLE_RADIO, ROLE_POPUP, ROLE_ROW, ROLE_UP, ROLE_DOWN,
    ROLE_SCROLL_UP, ROLE_SCROLL_DOWN, ROLE_DECOR, ROLE_CHANGE
};

typedef struct CfgIcon {
    int   handle;
    int   role;
    int   arg;                      /* radio button number, list row, ... */
    char *buf;                      /* indirected text (owned) */
    char *valid;                    /* validation string (owned) */
    bool  own;                      /* buf is ours to free (it is not the widget's text) */
} CfgIcon;

typedef struct Panel Panel;
typedef struct Widget Widget;
typedef struct Box Box;

struct Widget {
    dlgcontrol *ctrl;
    Panel *panel;
    int x, y, w, h;                 /* in pixels, relative to the panel's top left */
    int colstart, colspan;
    int lw, lh;                     /* the label's size, 0 if it has none (relative to x, y) */
    int fx, fy, fw, fh;             /* the control proper, relative to x, y */
    int bx, bw;                     /* a button beside it (pop-up, Up and Down, Change): x, width */

    /* the control's value */
    char *label;                    /* current label text (owned) or NULL */
    char *text;                     /* edit box / display text (owned, MAX_TEXT) */
    bool  checked;
    int   radio;                    /* the selected radio button, -1 for none */
    Entry *ent;                     /* list box / drop-down entries */
    int   nent, entcap;
    int   top;                      /* the first row showing in a list box */
    int   cursel;                   /* a drop-down's current entry, -1 for none */
    int   rows;                     /* the rows a list box shows */
    Filename *dropped;              /* a file dropped on a just-button file selector */

    /* the view: only while the panel is shown */
    CfgIcon *icons;
    int   nicons, iconcap;
};

struct Box {
    char *title;                    /* may be NULL */
    int   y0, y1;                   /* pixels, in the panel */
};

struct Panel {
    char *path;                     /* "Session/Logging" */
    char *title;
    int   depth;
    Panel *parent;
    bool  expanded;                 /* its children show in the tree */
    bool  is_footer;                /* the path "" controls (Open, Cancel) */
    Widget **w;
    int   nw, wcap;
    Box  *box;
    int   nbox, boxcap;
    int   height;                   /* of all the boxes, pixels */
    int   width;                    /* pixels */
};

typedef struct TreeRow {
    Panel *panel;
    int icon;
    char *buf;                      /* the icon's text (owned) */
} TreeRow;

typedef struct Decor {              /* an icon that is not a control: a frame, a heading */
    int   handle;
    char *buf;
    char *valid;
} Decor;

struct dlgparam {
    struct controlbox *ctrlbox;
    Conf *conf;                     /* what the controls edit */
    bool  midsession;
    int   protocol, protcfginfo;
    char  title[96];

    Panel **panels;                 /* in tree order, not counting the footer */
    int   npanels;
    Panel *footer;
    Panel *cur;                     /* the panel on show */

    int   win;                      /* the Wimp window */
    bool  open;
    TreeRow *rows;                  /* the tree's icons (rebuilt as it opens and closes) */
    int   nrows, rowcap;
    Decor *decor;                   /* the panel on show's frames and heading */
    int   ndecor, decorcap;
    int   nstatic;                  /* the first decor icons are not the panel's */

    RoConfigDone done;
    void *done_ctx;

    /* sub-dialogues and menus in progress */
    Widget *popup;                  /* the drop-down whose menu is open */
    Widget *colour_for;             /* the control a colour chooser is for */
    int   colour_rgb[3];
    bool  colour_ok;
    Widget *font_for;               /* ... and a font chooser */
    Widget *focus, *last_focus, *prev_focus;
    unsigned last_click_cs;
    int   last_click_row;
    Widget *last_click_widget;
    bool  ending;
    int   end_value;
    dlgparam *next;                 /* the list of open dialogues */
};

/* ---- roconfig.c ---- */
int  cfg_wrap(const char *text, int maxchars, const char **start, int *len, int maxlines);
void cfg_cell_text(const char *text, int col, char *out, size_t cap);
Widget *cfg_find_widget(dlgparam *dp, const dlgcontrol *c);
void cfg_emit(dlgparam *dp, Widget *w, int event);       /* call the control's handler */
void cfg_list_set_sel_only(Widget *w, int index);
void cfg_list_toggle(Widget *w, int index);
void cfg_move_entry(Widget *w, int from, int to);
void cfg_free(dlgparam *dp);
dlgparam *cfg_build(Conf *conf, bool midsession, int protocol, int protcfginfo);

/* ---- rocfgwin.c ---- */
void cfgwin_create(dlgparam *dp);
void cfgwin_show_panel(dlgparam *dp, Panel *p);
void cfgwin_close(dlgparam *dp);
void cfgwin_update_widget(dlgparam *dp, Widget *w);
void cfgwin_update_label(dlgparam *dp, Widget *w);
void cfgwin_set_focus(dlgparam *dp, Widget *w);
bool cfgwin_is_visible(dlgparam *dp, Widget *w);
void cfgwin_start_colour(dlgparam *dp, Widget *w, int r, int g, int b);
bool cfgwin_colour_open(void);
void cfgwin_colour_idle(void);
int  cfgwin_message(const wmessage *m);

#endif /* PUTTY_ROCONFIG_I_H */
