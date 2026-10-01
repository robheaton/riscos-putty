/*
 * rocontext.h - the menu that opens on a terminal window (Copy, Paste, Special
 * command, Event log, ...).
 */
#ifndef PUTTY_ROCONTEXT_H
#define PUTTY_ROCONTEXT_H

#include <stdbool.h>
#include "roterm.h"

/* Open the menu for terminal t with the pointer at (x, y). */
void rocontext_open(RoTerm *t, int x, int y);

/* A Menu_Selection event while the context menu is the open one (see
 * roapp_menu_opened()): carry out the choice.  `sel` is the event's path of item
 * numbers, -1 terminated. */
void rocontext_selection(const int *sel);

#endif /* PUTTY_ROCONTEXT_H */
