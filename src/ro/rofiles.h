/*
 * rofiles.h - the file transfer window: a directory of another computer, copied to and from over SFTP.
 *
 * It looks like a Filer directory display of the remote directory (full information: name, size, date,
 * permissions).  Double-click a directory to go into it, drag a file to a Filer window to copy it
 * there, drop files from the Filer on the window to copy them up, and Menu has the rest (new directory,
 * rename, delete, go to...).  Only one such window is open at a time.
 */
#ifndef PUTTY_ROFILES_H
#define PUTTY_ROFILES_H

#include <stdbool.h>
#include "putty.h"
#include "rowimp.h"

/* Open the window and connect to the host of `conf` (which is taken over). */
void rofiles_open(Conf *conf);

/* Is there a connection that quitting would close? */
bool rofiles_live(void);
/* Is the window open?  If so, bring it to the front. */
bool rofiles_raise(void);
/* While there is a connection to keep an eye on, the main loop wakes now and then and calls rofiles_idle,
 * which notices a connection that has broken while the window sat idle. */
bool rofiles_idle_wanted(void);
void rofiles_idle(void);
/* Close the window and its connection at once (the program is quitting). */
void rofiles_shutdown(void);

/* Window events: each returns 1 if the window is the file transfer window. */
int  rofiles_event_redraw(wevent *e);
int  rofiles_event_open(wevent *e);
int  rofiles_event_close(wevent *e);
int  rofiles_event_click(wevent *e);
int  rofiles_event_scroll(wevent *e);
int  rofiles_event_key(wevent *e);

/* A Wimp message that may be for the window (files dropped on it); returns 1 if it was. */
int  rofiles_message(const wmessage *m);

/* The interactive help text for the window under the pointer, or NULL. */
const char *rofiles_help(int win, int icon);

#endif /* PUTTY_ROFILES_H */
