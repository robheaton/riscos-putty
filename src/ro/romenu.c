/*
 * romenu.c - Wimp menu builder.  (Adapted from riscos-vncclient, same author.)
 *
 * A Wimp menu block is a 28 byte header followed by 24 byte items:
 *   header: title[12], 4 colour bytes, width, height, gap
 *   item:   menu flags, submenu ptr (-1 none), icon flags, icon data[12]
 * Menu flags: b0 ticked, b1 dotted line after, b2 writable, b3 warn before
 * opening submenu, b7 last item.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "romenu.h"

#define ITEM_HEIGHT 44
#define ITEM_GAP    0

menu *menu_new(const char *title)
{
    menu *m = (menu *)calloc(1, sizeof *m);
    if (m) snprintf(m->title, sizeof m->title, "%s", title);
    return m;
}

void menu_free(menu *m)
{
    if (!m) return;
    free(m->block);
    free(m);
}

int menu_add(menu *m, const char *text, unsigned flags)
{
    int i = m->nitems;
    if (i >= MAX_MENU_ITEMS) return -1;
    snprintf(m->text[i], sizeof m->text[i], "%s", text);
    m->flags[i] = flags;
    m->sub[i] = NULL;
    m->win[i] = 0;
    m->nitems++;
    return i;
}

void menu_set_sub(menu *m, int item, menu *sub)    { m->sub[item] = sub; }
void menu_set_window(menu *m, int item, int window) { m->win[item] = window; }
void menu_set_flags(menu *m, int item, unsigned f) { m->flags[item] = f; }
void menu_set_text(menu *m, int item, const char *t) { snprintf(m->text[item], sizeof m->text[item], "%s", t); }

void menu_tick(menu *m, int item, int on)
{
    if (on) m->flags[item] |= MF_TICKED; else m->flags[item] &= ~MF_TICKED;
}

void menu_shade(menu *m, int item, int on)
{
    if (on) m->flags[item] |= MF_SHADED; else m->flags[item] &= ~MF_SHADED;
}

static int text_width(const char *s)
{
    int w = 0;
    _kernel_oserror *e = _swix(Wimp_TextOp, _INR(0, 2) | _OUT(0), 1, s, 0, &w);
    if (e) w = (int)strlen(s) * 16;
    return w;
}

unsigned char *menu_block(menu *m)
{
    int i, width;
    size_t need = 28 + (size_t)m->nitems * 24;
    unsigned char *b;

    if (!m->block || m->blocksize < need) {
        free(m->block);
        m->block = (unsigned char *)calloc(1, need);
        m->blocksize = need;
    }
    b = m->block;
    if (!b) return NULL;
    memset(b, 0, need);

    width = text_width(m->title) + 32;
    for (i = 0; i < m->nitems; i++) {
        int w = text_width(m->text[i]) + 32;   /* room for tick and arrow */
        if (w > width) width = w;
    }
    m->width = width;

    memset(b, 0, 12);
    snprintf((char *)b, 12, "%s", m->title);
    b[12] = 7; b[13] = 2; b[14] = 7; b[15] = 0;
    *(int *)(b + 16) = width;
    *(int *)(b + 20) = ITEM_HEIGHT;
    *(int *)(b + 24) = ITEM_GAP;

    for (i = 0; i < m->nitems; i++) {
        unsigned char *it = b + 28 + (size_t)i * 24;
        unsigned mflags = m->flags[i] & 0x0F;
        unsigned iflags = 0x07000021u;           /* text, filled, fg 7 bg 0 */
        if (i == m->nitems - 1) mflags |= 0x80;
        if (m->flags[i] & MF_SHADED) iflags |= IF_SHADED;
        *(unsigned *)(it + 0) = mflags;
        *(int *)(it + 4) = m->sub[i] ? (int)menu_block(m->sub[i]) : (m->win[i] ? m->win[i] : -1);
        *(unsigned *)(it + 8) = iflags;
        memset(it + 12, 0, 12);
        snprintf((char *)it + 12, 12, "%s", m->text[i]);
        /* text longer than 11 chars: use an indirected icon */
        if (strlen(m->text[i]) > 11) {
            *(unsigned *)(it + 8) = iflags | IF_INDIRECTED;
            *(char **)(it + 12) = m->text[i];
            *(int *)(it + 16) = -1;
            *(int *)(it + 20) = (int)sizeof m->text[i];
        }
    }
    return b;
}

void menu_open(menu *m, int x, int y)
{
    unsigned char *b = menu_block(m);
    if (!b) return;
    _swix(Wimp_CreateMenu, _INR(1, 3), b, x, y);
}
