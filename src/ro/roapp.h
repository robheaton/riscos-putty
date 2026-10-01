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

/* Menu_Selection events do not say which menu they came from, so whoever opens a
 * menu says who is to hear about its selection. */
typedef void (*RoMenuHandler)(const int *selection);
void roapp_menu_opened(RoMenuHandler handler);

#endif /* PUTTY_ROAPP_H */
