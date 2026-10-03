/*
 * roapp.h - things the application as a whole provides to the rest of the code.
 */
#ifndef PUTTY_ROAPP_H
#define PUTTY_ROAPP_H

#include "putty.h"

/* Open the Connect dialogue (start a new session). */
void roapp_connect_dialog(void);

/* The window of the standard "About this program" box, for use as the submenu of an
 * Info menu item (menu_set_window); 0 if it could not be made. */
int roapp_info_window(void);

/*
 * Long operations (making a key, a file transfer) are plain function calls that cannot return
 * until they are done.  They call roapp_yield() every so often so that the desktop, their own
 * window and PuTTY's sessions carry on; it returns true if the program has been told to quit,
 * and the operation should then give up.  Only one such operation runs at a time:
 * roapp_op_begin() says false if one is already running.
 */
bool roapp_yield(void);
/* ... or, for one that is waiting for the network, wait a moment (not more than 2 cs) for it. */
bool roapp_wait(void);
bool roapp_op_begin(void);
void roapp_op_end(void);

/* Menu_Selection events do not say which menu they came from, so whoever opens a
 * menu says who is to hear about its selection. */
typedef void (*RoMenuHandler)(const int *selection);
void roapp_menu_opened(RoMenuHandler handler);

#endif /* PUTTY_ROAPP_H */
