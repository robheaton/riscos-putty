/*
 * ronet.h - what the event loops (console and Wimp) need from net.c.
 */
#ifndef PUTTY_RONET_H
#define PUTTY_RONET_H

#include <stdbool.h>

/*
 * Wait up to timeout_cs centiseconds for activity on any socket PuTTY has
 * registered with uxsel, then dispatch it (the plugs' receive/sent/closing
 * callbacks run from here).  Also steps name lookups and connection setup
 * along.  A timeout of 0 just polls.  Returns the number of sockets that had
 * something to report.
 */
int ro_net_poll(int timeout_cs);

/* True while a name lookup or connection attempt is in progress, i.e. the
 * caller should poll again soon rather than sleep for a long time. */
bool ro_net_busy(void);

#endif /* PUTTY_RONET_H */
