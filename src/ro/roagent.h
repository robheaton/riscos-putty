/*
 * roagent.h - the key agent: PuTTY's Pageant, running inside the program.
 *
 * Keys added here (from a .ppk file, with the passphrase typed once) are offered to every SSH connection
 * the program makes - sessions and the file transfer window - so the passphrase is not asked for again, and
 * to the other end when agent forwarding is asked for.  The keys are held in the program's memory, as Pageant
 * does, and forgotten when it quits.  The Keys window lists them.
 */
#ifndef PUTTY_ROAGENT_H
#define PUTTY_ROAGENT_H

#include <stdbool.h>
#include "rowimp.h"

/* Start the agent (empty).  Call once, before any connection is made. */
void roagent_init(void);
/* Add the keys named in the start-up list (a text file in Choices, edited from the Keys window); each asks for its
 * passphrase, if it has one, in turn.  Call once, after roagent_init(). */
void roagent_load_startup(void);
/* Forget every key (the program is quitting). */
void roagent_shutdown(void);

/* Show the Keys window (or bring it to the front). */
void roagent_open(void);
/* Add the private key in `path`; asks for its passphrase if it has one. */
void roagent_add_file(const char *path);

/* A Wimp message that may be for the agent (a key file dropped on its window or on the icon bar icon);
 * returns 1 if it was. */
int roagent_message(const wmessage *m);

#endif /* PUTTY_ROAGENT_H */
