/*
 * romenu.h - Wimp menu builder.  (Adapted from riscos-vncclient, same author.)
 */
#ifndef PUTTY_ROMENU_H
#define PUTTY_ROMENU_H

#include "rowimp.h"

#define MAX_MENU_ITEMS 64

typedef struct menu {
    int   nitems;
    int   width;
    char  title[16];
    char  text[MAX_MENU_ITEMS][40];
    unsigned flags[MAX_MENU_ITEMS];       /* MF_* */
    struct menu *sub[MAX_MENU_ITEMS];
    int   win[MAX_MENU_ITEMS];            /* or a window to open as the item's submenu (0 = none) */
    unsigned char *block;                 /* built Wimp menu block */
    size_t blocksize;
} menu;

#define MF_TICKED    0x01u
#define MF_DOTTED    0x02u                /* dotted separator after the item */
#define MF_SHADED    0x100u               /* (icon flag, applied at build time) */

menu *menu_new(const char *title);
void  menu_free(menu *m);
int   menu_add(menu *m, const char *text, unsigned flags);       /* returns index */
void  menu_set_sub(menu *m, int item, menu *sub);
/* Make a window (a DLG_MENU dialogue, say) the item's submenu, as the standard
 * "Info" entry does: the Wimp opens it beside the item and closes it with the menu. */
void  menu_set_window(menu *m, int item, int window);
void  menu_set_flags(menu *m, int item, unsigned flags);
void  menu_set_text(menu *m, int item, const char *text);
void  menu_tick(menu *m, int item, int on);
void  menu_shade(menu *m, int item, int on);
/* Build (if needed) and open as the Wimp's current menu. */
void  menu_open(menu *m, int x, int y);
/* Ensure the block is built and return a pointer usable as a submenu link. */
unsigned char *menu_block(menu *m);

#endif /* PUTTY_ROMENU_H */
