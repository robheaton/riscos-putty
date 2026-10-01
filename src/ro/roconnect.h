/*
 * roconnect.h - the Connect dialogue: choose a host and how to log in to it, pick
 * or save a session, and open a terminal window.
 */
#ifndef PUTTY_ROCONNECT_H
#define PUTTY_ROCONNECT_H

#include "rowimp.h"

/* Open the dialogue (or bring it to the front). */
void roconnect_open(void);

/* A Wimp message that may be for the dialogue (a key file dropped on it);
 * returns 1 if it was. */
int roconnect_message(const wmessage *m);

#endif /* PUTTY_ROCONNECT_H */
