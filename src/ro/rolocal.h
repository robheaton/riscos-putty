/*
 * rolocal.h - the local terminal: a RISC OS task window behind a PuTTY terminal (see rolocal.c).
 */
#ifndef PUTTY_ROLOCAL_H
#define PUTTY_ROLOCAL_H

#include <stdbool.h>

#include "putty.h"
#include "rowimp.h"

/* The value of CONF_protocol for it (PuTTY's own go up to PROT_SUPDUP). */
#define PROT_LOCAL 7

extern const struct BackendVtable local_backend;

/* The messages of the TaskWindow module that the program has to hear (for the list given to Wimp_Initialise). */
#define MSG_TW_OUTPUT  0x808C1
#define MSG_TW_EGO     0x808C2
#define MSG_TW_MORIO   0x808C3

/* Deal with a message from the TaskWindow module: true if it was one. */
bool rolocal_message(const wmessage *m);

/* A message of ours came back unanswered: true if it was one of this module's. */
bool rolocal_bounced(const wmessage *m);

/* The program is quitting: end the task windows and put the system variables back as they were. */
void rolocal_shutdown(void);

#endif /* PUTTY_ROLOCAL_H */
