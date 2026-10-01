/*
 * roterm.h - one PuTTY terminal session in a Wimp window.
 *
 * A RoTerm owns everything a session needs: the Wimp window, PuTTY's Terminal
 * and the Backend/Ldisc that connect it to the network, and a grid of the cells
 * PuTTY has drawn.  PuTTY calls our TermWin methods to say what to draw; we keep
 * the answers in the grid and paint from it, so a redraw (the window uncovered,
 * the scroll bar moved) never depends on what PuTTY thinks is already on screen.
 */
#ifndef PUTTY_ROTERM_H
#define PUTTY_ROTERM_H

#include <stdbool.h>

#include "putty.h"
#include "rowimp.h"
#include "rofont.h"

/* What the terminal last drew into one cell. */
typedef struct RoCell {
    wchar_t       ch;          /* 0: nothing (blank, or the right half of a wide character) */
    unsigned long attr;
    truecolour    tc;
    unsigned char curs;        /* drawn by draw_cursor (attr carries the cursor flags) */
    unsigned char sigil;       /* the trust sigil is drawn over this cell */
    unsigned char wide_rhs;    /* right-hand half of a double-width character */
} RoCell;

typedef struct RoTerm RoTerm;
struct RoTerm {
    RoTerm *next;
    int id;
    int win;                   /* Wimp window handle, -1 if none */

    Conf *conf;
    Terminal *term;            /* NULL in the painting demo */
    Backend *backend;
    Ldisc *ldisc;
    LogContext *logctx;
    struct unicode_data ucsdata;
    cmdline_get_passwd_input_state cmdline_state;
    TermWin termwin;
    Seat seat;
    LogPolicy logpolicy;

    RoFont font;
    int bold_style;            /* BOLD_STYLE_* */
    int cursor_type;           /* CURSOR_* */
    bool scrollbar;            /* the window has a scroll bar for the scrollback */

    int cols, rows;
    RoCell *cells;             /* rows * cols */
    uint32_t pal[OSC4_NCOLOURS];       /* palette words 0xBBGGRR00 */
    rgb      palrgb[OSC4_NCOLOURS];

    /* cells changed since they were last painted (inclusive; empty if x0 > x1) */
    int dx0, dy0, dx1, dy1;

    char title[256];           /* indirected window title */
    bool exited;               /* the session has ended (the window stays) */
    bool close_ask;            /* "Close this session?" is on screen */
    bool focus;

    int  sb_total, sb_start, sb_page;   /* the scroll bar, as PuTTY last set it */
    int  scroll_y;                      /* the window's scroll offset to match */
    bool in_open;                       /* inside our own Wimp_OpenWindow: ignore its echo */

    /* The event log.  A session keeps its lines; its Event Log window (shown on
     * request) is itself a terminal with no connection, flagged is_log. */
    bool    is_log;
    RoTerm *evwin;             /* this session's Event Log window, if shown */
    RoTerm *evparent;          /* for an Event Log window, the session it belongs to */
    char  **evlines;
    int     nev;

    bool raw_mouse;            /* PuTTY wants mouse events passed on (xterm mouse tracking) */
    int  mouse_held;           /* Mouse_Button held down, MBT_NOTHING if none */
    int  mouse_x, mouse_y;     /* the cell it was last reported at */
    uint32_t click_time;       /* ro_time() of the last click, and what it was, for double/triple */
    int  click_count, click_x, click_y, click_button;
    int  wheel_accum;
    BusyStatus busy;
};

/* ---- window management (rowindow.c) ------------------------------------- */

/* Create a session window for conf (which the new session takes over) and start
 * the connection.  Returns NULL if the window cannot be made. */
RoTerm *roterm_new(Conf *conf);
/* A window that only paints a test pattern: no terminal, no network. */
RoTerm *roterm_new_demo(Conf *conf);
void    roterm_free(RoTerm *t);

RoTerm *roterm_from_window(int win);
RoTerm *roterm_first(void);

/* Wimp events for a session window */
void roterm_redraw(RoTerm *t, wevent *e);
void roterm_open(RoTerm *t, wevent *e);
void roterm_close_request(RoTerm *t);
void roterm_key(RoTerm *t, wevent *e);
void roterm_mouse_click(RoTerm *t, wevent *e);
void roterm_scroll_request(RoTerm *t, wevent *e);
void roterm_gain_caret(RoTerm *t);
void roterm_lose_caret(RoTerm *t);

/* Called every trip round the main loop: paints changed cells, tracks a drag. */
void roterm_poll(RoTerm *t);
/* Redraw every window (palette or mode change). */
void roterm_redraw_all(void);

/* True once the session has ended and the window can go if the user closes it. */
bool roterm_is_exited(const RoTerm *t);

/* The connection has gone: say so in the window's title. */
void roterm_mark_inactive(RoTerm *t);
/* Does the session still have a connection to lose? */
bool roterm_is_live(const RoTerm *t);

/* The event log: record a line, and show the Event Log window. */
void roterm_log_event(RoTerm *t, const char *line);
void roterm_eventlog_show(RoTerm *t);
/* Start the connection again after the session has ended. */
void roterm_restart(RoTerm *t);
/* Open the configuration dialogue for a running session; Apply puts the new settings to use. */
void roterm_change_settings(RoTerm *t);

/* ---- the seat and log policy (roseat.c) --------------------------------- */

extern const SeatVtable roterm_seat_vt;
extern const LogPolicyVtable roterm_logpolicy_vt;

/* Start the connection for a session whose window and terminal exist. */
void roterm_start_backend(RoTerm *t);
/* Tear the connection down (but not the window). */
void roterm_destroy_connection(RoTerm *t);

#endif /* PUTTY_ROTERM_H */
