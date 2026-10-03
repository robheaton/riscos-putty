/*
 * rodlg.h - declarative dialogue boxes built from Wimp icons.
 * (Adapted from riscos-vncclient, same author.)
 *
 * A dialogue is a plain (auto-redraw) window containing icons.  Items are
 * described in a table with positions in *pixels* measured from the window's
 * top-left corner (y grows downwards, which is easier to write by hand); they
 * are scaled by the screen mode's eigen factors when the dialogue is built, so
 * one layout works at every pixel density.  The system font is 8 px wide.
 */
#ifndef PUTTY_RODLG_H
#define PUTTY_RODLG_H

#include "rowimp.h"

enum {
    DI_LABEL = 0,       /* static text */
    DI_FIELD,           /* writable text field */
    DI_PASSWORD,        /* writable field that shows bullets */
    DI_BUTTON,          /* action button */
    DI_DEFBUTTON,       /* default action button (thick border; Return activates it) */
    DI_OPTION,          /* check box */
    DI_RADIO,           /* radio button (esg = group number, > 0) */
    DI_BOX,             /* a titled frame / separator (no interaction) */
    DI_RLABEL,          /* static text, right-aligned (the label beside a field) */
    DI_DISPLAY,         /* a read-only field: sunken, text centred (as in Program information) */
    DI_SWATCH,          /* a sunken well filled with a colour, set with dlg_set_swatch(); a dialogue with
                         * one is redrawn by us (dlg_event_redraw), not by the Wimp */
    DI_BAR,             /* a sunken well with a bar in it, set with dlg_set_bar() (redrawn by us too) */
    DI_FILE,            /* a file icon (its sprite name is the text) that can be clicked and dragged: the
                         * callback gets BTN_SELECT for a click and BTN_DRAG_SELECT for a drag */
    DI_HEADING,         /* a box's title: text on a filled background, laid over the top edge of a DI_BOX */
    DI_INFO             /* like DI_DISPLAY, but the text starts at the left (for text that may be too long) */
};

/* What kind of window a dialogue is. */
enum {
    DLG_NORMAL = 0,     /* moveable, with back and close icons: opened with dlg_open() */
    DLG_MENU            /* a title bar and nothing else, for a menu item's submenu: the Wimp
                         * opens and closes it (give dlg_window() to menu_set_window()) */
};

typedef struct {
    int         type;
    int         id;             /* control id (>= 0), reported to the click callback */
    int         x, y, w, h;
    const char *text;           /* label / caption / initial text */
    int         maxlen;         /* DI_FIELD / DI_PASSWORD: buffer size (incl. terminator) */
    int         esg;            /* DI_RADIO: group */
    const char *help;           /* interactive help text for this control (or NULL) */
} dlg_item;

typedef struct dlg dlg;

/* Called when a button, option or radio is clicked.  buttons: BTN_* bits. */
typedef void (*dlg_click_fn)(dlg *d, int id, int buttons, void *ud);
/* Called when Menu is clicked anywhere in the dialogue (x, y: where, in screen OS units). */
typedef void (*dlg_menu_fn)(dlg *d, int x, int y, void *ud);

dlg *dlg_new(const char *title, int w, int h, const dlg_item *items, int n,
             dlg_click_fn cb, void *ud);
dlg *dlg_new_style(int style, const char *title, int w, int h, const dlg_item *items, int n,
                   dlg_click_fn cb, void *ud);
void dlg_free(dlg *d);

/* Open centred on the screen (or just bring to front if already open). */
void dlg_open(dlg *d);
/* Open centred on a point (the pointer, say), kept on the screen. */
void dlg_open_at(dlg *d, int x, int y);
void dlg_set_menu_fn(dlg *d, dlg_menu_fn fn);
void dlg_close(dlg *d);
int  dlg_is_open(const dlg *d);
int  dlg_window(const dlg *d);

void        dlg_set_text(dlg *d, int id, const char *s);
const char *dlg_get_text(dlg *d, int id);
void        dlg_set_selected(dlg *d, int id, int on);
int         dlg_get_selected(dlg *d, int id);
void        dlg_set_shaded(dlg *d, int id, int on);
/* The colour a DI_SWATCH shows (0 to 255 each), redrawn at once if it changed. */
void        dlg_set_swatch(dlg *d, int id, int r, int g, int b);
/* How full a DI_BAR is, 0 to 1000. */
void        dlg_set_bar(dlg *d, int id, int permille);
/* The window's contents as screen coordinates of an item (its box's left edge and top, OS units), for
 * starting a drag from it. */
int         dlg_item_box(dlg *d, int id, int *x0, int *y0, int *x1, int *y1);
/* The dialogue whose window this is (or NULL). */
dlg        *dlg_from_window(int win);
/* Change the title bar's text. */
void        dlg_set_title(dlg *d, const char *title);
/* Put the caret in a writable field. */
void        dlg_focus(dlg *d, int id);
/* Which button Return and Escape (or the close icon) activate. */
void        dlg_set_keys(dlg *d, int default_id, int escape_id);
/* What the close icon does if that is not what Escape does: the callback is told `id`. */
void        dlg_set_close_id(dlg *d, int id);

/* Interactive help: the text for the icon under the pointer in window `win`,
 * or NULL if `win` is not a dialogue (or has nothing to say about `icon`). */
const char *dlg_help_text(int win, int icon);
void        dlg_set_help(dlg *d, const char *text);      /* the whole window's text */

/* Event routing: return 1 if the event was for a dialogue window. */
int dlg_event_click(wevent *e);       /* Mouse_Click */
int dlg_event_key(wevent *e);         /* Key_Pressed */
int dlg_event_open(wevent *e);        /* Open_Window_Request */
int dlg_event_close(wevent *e);       /* Close_Window_Request */
int dlg_event_redraw(wevent *e);      /* Redraw_Window_Request (only dialogues with a swatch get them) */

#endif /* PUTTY_RODLG_H */
