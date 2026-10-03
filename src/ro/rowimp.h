/*
 * rowimp.h - Wimp definitions and thin wrappers used by the application.
 *
 * Adapted from the riscos-vncclient project (same author), whose constants were
 * checked against OSLib's wimp.h.  Only what this program needs is here;
 * everything is plain ints so blocks can be passed straight to _swix().
 */
#ifndef PUTTY_ROWIMP_H
#define PUTTY_ROWIMP_H

#include <stdbool.h>
#include "ro.h"

/* ---- event codes (Wimp_Poll reason) ------------------------------------ */
#define EV_NULL            0
#define EV_REDRAW          1
#define EV_OPEN            2
#define EV_CLOSE           3
#define EV_POINTER_LEAVE   4
#define EV_POINTER_ENTER   5
#define EV_MOUSE_CLICK     6
#define EV_DRAG_END        7
#define EV_KEY             8
#define EV_MENU            9
#define EV_SCROLL          10
#define EV_LOSE_CARET      11
#define EV_GAIN_CARET      12
#define EV_POLLWORD        13
#define EV_MESSAGE         17
#define EV_MESSAGE_RECORDED 18
#define EV_MESSAGE_ACK     19

/* Wimp_Poll mask bits: a set bit MASKS OUT that event */
#define MASK_NULL          0x00000001u

/* ---- message actions --------------------------------------------------- */
#define MSG_QUIT           0x00
#define MSG_DATA_SAVE      0x01
#define MSG_DATA_SAVE_ACK  0x02
#define MSG_DATA_LOAD      0x03
#define MSG_DATA_LOAD_ACK  0x04
#define MSG_DATA_OPEN      0x05
#define MSG_RAM_FETCH      0x06
#define MSG_RAM_TRANSMIT   0x07
#define MSG_PRE_QUIT       0x08
#define MSG_PALETTE_CHANGE 0x09
#define MSG_SHUTDOWN       0x0E
#define MSG_CLAIM_ENTITY   0x0F
#define MSG_DATA_REQUEST   0x10
#define MSG_RELEASE_ENTITY 0x13
#define MSG_MODE_CHANGE    0x400C1
#define MSG_TASK_CLOSEDOWN 0x400C3
#define MSG_HELP_REQUEST   0x502
#define MSG_HELP_REPLY     0x503
#define MSG_MENUS_DELETED  0x400C9
#define MSG_URI_PROCESS    0x4E383

/* Private messages between copies of this program (single-instance check). */
#define MSG_PUTTY_PING     0x4B7C1
#define MSG_PUTTY_PONG     0x4B7C2
#define PUTTY_PING_MAGIC   0x59545550      /* "PUTY" */

/* ---- window flags ------------------------------------------------------ */
#define WF_MOVEABLE        0x00000002u
#define WF_AUTO_REDRAW     0x00000010u
#define WF_PANE            0x00000020u
#define WF_NO_BOUNDS       0x00000040u
#define WF_SCROLL_REPEAT   0x00000100u
#define WF_SCROLL_REQUEST  0x00000200u
#define WF_REAL_COLOURS    0x00000400u
#define WF_BACK_WINDOW     0x00000800u
#define WF_HOT_KEYS        0x00001000u
#define WF_BOUNDED         0x00002000u
#define WF_OPEN            0x00010000u
#define WF_TOP             0x00020000u
#define WF_FULL_SIZE       0x00040000u   /* toggled */
#define WF_BACK_ICON       0x01000000u
#define WF_CLOSE_ICON      0x02000000u
#define WF_TITLE_BAR       0x04000000u
#define WF_TOGGLE_ICON     0x08000000u
#define WF_VSCROLL         0x10000000u
#define WF_SIZE_ICON       0x20000000u
#define WF_HSCROLL         0x40000000u
#define WF_NEW_FORMAT      0x80000000u

/* ---- icon flags -------------------------------------------------------- */
#define IF_TEXT            0x00000001u
#define IF_SPRITE          0x00000002u
#define IF_BORDER          0x00000004u
#define IF_HCENTRED        0x00000008u
#define IF_VCENTRED        0x00000010u
#define IF_FILLED          0x00000020u
#define IF_ANTIALIAS       0x00000040u
#define IF_NEEDS_HELP      0x00000080u
#define IF_INDIRECTED      0x00000100u
#define IF_RJUSTIFIED      0x00000200u
#define IF_ALLOW_ADJUST    0x00000400u
#define IF_HALF_SIZE       0x00000800u
#define IF_SELECTED        0x00200000u
#define IF_SHADED          0x00400000u
#define IF_DELETED         0x00800000u
#define IF_BUTTON(t)       ((unsigned)(t) << 12)
#define IF_ESG(n)          ((unsigned)(n) << 16)
#define IF_FG(c)           ((unsigned)(c) << 24)
#define IF_BG(c)           ((unsigned)(c) << 28)

/* icon button types */
#define BT_NEVER           0
#define BT_ALWAYS          1
#define BT_REPEAT          2
#define BT_CLICK           3
#define BT_RELEASE         4
#define BT_DOUBLE          5
#define BT_CLICK_DRAG      6
#define BT_MENU_ICON       9
#define BT_RADIO           11
#define BT_WRITABLE        15

/* mouse button bits in click events / pointer info */
#define BTN_ADJUST         1
#define BTN_MENU           2
#define BTN_SELECT         4
#define BTN_DRAG_ADJUST    16
#define BTN_DRAG_SELECT    64
/* In a window or icon whose button type tells single clicks from double clicks (the Filer's, type 10),
 * a single click has these values, and BTN_ADJUST / BTN_MENU / BTN_SELECT mean a DOUBLE click. */
#define BTN_CLICK_ADJUST   256
#define BTN_CLICK_MENU     512
#define BTN_CLICK_SELECT   1024

/* ---- structures -------------------------------------------------------- */

typedef struct { int x0, y0, x1, y1; } wbox;

/* Window definition, as passed to Wimp_CreateWindow (followed by icons). */
typedef struct {
    wbox     visible;            /* screen coordinates of the visible area */
    int      scroll_x, scroll_y;
    int      behind;             /* -1 = top, -2 = bottom, else window handle */
    unsigned flags;
    uint8_t  title_fg, title_bg, work_fg, work_bg, scroll_outer, scroll_inner,
             title_hi, reserved;
    wbox     extent;             /* work area, relative to window origin */
    unsigned title_flags;
    unsigned work_flags;         /* button type in bits 12-15 */
    int      sprite_area;        /* 1 = Wimp sprite pool */
    short    min_w, min_h;
    char     title[12];          /* or indirected: ptr, validation, length */
    int      nicons;
} wdef;

typedef struct {
    wbox     box;
    unsigned flags;
    union {
        char text[12];           /* direct text or sprite name */
        struct { char *buf; char *validation; int size; } ind;
    } data;
} idef;

/* Wimp_GetWindowState / Wimp_OpenWindow block */
typedef struct {
    int      handle;
    wbox     visible;
    int      scroll_x, scroll_y;
    int      behind;
    unsigned flags;
} wstate;

/* Wimp_RedrawWindow / Wimp_UpdateWindow / Wimp_GetRectangle block */
typedef struct {
    int  handle;
    wbox visible;                /* update area in / visible area out */
    int  scroll_x, scroll_y;
    wbox clip;                   /* graphics window (out) */
} wdraw;

typedef struct {
    int x, y;
    unsigned buttons;
    int window;
    int icon;
} wpointer;

typedef struct {
    int      window, icon;
    int      x, y;               /* caret offset within the window/icon */
    int      height;             /* + flags in the top bits */
    int      index;
    int      key;
} wkey;

typedef struct {
    int size, sender, my_ref, your_ref, action;
    int data[59];
} wmessage;

typedef union {
    int      words[64];
    wstate   open;
    wpointer mouse;
    wkey     key;
    wmessage msg;
} wevent;

/* ---- wrappers ---------------------------------------------------------- */

/* Report an error to the user in the standard Wimp error box. */
void wimp_error(const char *text);
void wimp_error_os(const _kernel_oserror *e);

/* The pathname in a DataSaveAck / DataLoad message, copied out with a bound and a
 * terminator: messages come from other tasks and are not to be trusted. */
void wimp_msg_path(const wmessage *m, char *out, size_t cap);
/* Answer a DataLoad (Message_DataLoadAck): the file has been dealt with. */
void wimp_send_load_ack(const wmessage *m, const char *path);

/* Answer Message_HelpRequest `m` with this text (nothing is sent if it is NULL). */
void wimp_help_reply(const wmessage *m, const char *text);

/* Is there a sprite of this name in the Wimp's sprite pool? */
bool wimp_sprite_exists(const char *name);

int  wimp_create_icon_iconbar(unsigned flags, const char *sprite, const char *text, int size);
void wimp_set_icon_text(int window, int icon, const char *text);
void wimp_set_icon_state(int window, int icon, unsigned eor, unsigned clear);
void wimp_get_icon_text(int window, int icon, char *out, size_t cap);

#endif /* PUTTY_ROWIMP_H */
