/*
 * roconfig.h - PuTTY's configuration dialogue (the tree of panels: Session,
 * Terminal, Window, Connection, SSH ...), drawn in the Wimp.
 *
 * PuTTY's own code (config.c) describes the controls and what they do; this is the
 * RISC OS side of that: it implements the dlg_* interface PuTTY expects and shows
 * the controls in a window.  It is used both to set up a new session ("Settings...")
 * and to change a running one ("Change settings...").
 */
#ifndef PUTTY_ROCONFIG_H
#define PUTTY_ROCONFIG_H

#include <stdbool.h>
#include "putty.h"
#include "rowimp.h"

/* Called when the dialogue ends.  ok is true if the user chose Open (or Apply); the
 * dialogue edited `conf` in place and passes it back, and the callee owns it from
 * then on.  If ok is false the dialogue has already freed conf. */
typedef void (*RoConfigDone)(void *ctx, bool ok, Conf *conf);

/* Show the dialogue for conf (which it takes over).  midsession is for changing the
 * settings of a session that is already running (protocol and protcfginfo say which
 * kind); title is for the window. */
void roconfig_open(Conf *conf, bool midsession, int protocol, int protcfginfo,
                   const char *title, RoConfigDone done, void *ctx);

/* Dialogues are told apart by the ctx they were opened with.  Bring the one for ctx to
 * the front (false if there is none), or end it as if Cancel had been pressed. */
bool roconfig_raise(void *ctx);
void roconfig_end_for(void *ctx);

/* Wimp messages that may be for a dialogue (a file dropped on a file field);
 * returns 1 if it was one. */
int roconfig_message(const wmessage *m);

/* The colour chooser shows a swatch of the colour being typed, and the Wimp says nothing as digits
 * are typed in a field: while it is open the main loop wakes now and then and calls roconfig_idle. */
bool roconfig_idle_wanted(void);
void roconfig_idle(void);

/* Window events: each returns 1 if the window is one of ours. */
int roconfig_event_click(wevent *e);
int roconfig_event_key(wevent *e);
int roconfig_event_open(wevent *e);
int roconfig_event_close(wevent *e);
int roconfig_event_scroll(wevent *e);
int roconfig_event_redraw(wevent *e);       /* Redraw_Window_Request: paints the colour swatch */

#endif /* PUTTY_ROCONFIG_H */
