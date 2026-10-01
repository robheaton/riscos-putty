/*
 * roconnect.h - the Connect dialogue: choose a host and how to log in to it, pick
 * or save a session, and open a terminal window.
 */
#ifndef PUTTY_ROCONNECT_H
#define PUTTY_ROCONNECT_H

#include "rowimp.h"

/* Open the dialogue (or bring it to the front). */
void roconnect_open(void);

/* The saved sessions' names, sorted, without "Default Settings" (for the icon bar
 * menu).  Free the list with roconnect_free_names(). */
char **roconnect_session_names(int *count);
void   roconnect_free_names(char **names, int count);

/* Start the saved session `name`: a terminal window at once or, if it does not say
 * where to connect, the Connect box with the session loaded. */
void roconnect_start_session(const char *name);

/* A Wimp message that may be for the dialogue (a key file dropped on it);
 * returns 1 if it was. */
int roconnect_message(const wmessage *m);

#endif /* PUTTY_ROCONNECT_H */
