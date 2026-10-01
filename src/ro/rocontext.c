/*
 * rocontext.c - the menu that opens on a terminal window.
 *
 *   Copy, Copy all, Paste
 *   Special command >   (what the connection offers: Break, signals, rekey ...)
 *   Event log..., Change settings..., Restart session, Reset terminal, Clear scrollback
 *   Duplicate session, New session..., About PuTTY...
 *
 * Wimp menus have no callbacks: the Wimp reports the chosen path of items in a
 * Menu_Selection event, so we remember what each item of the menu on screen means.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "ro.h"
#include "rowimp.h"
#include "romenu.h"
#include "roterm.h"
#include "roapp.h"
#include "rocontext.h"

enum {
    M_COPY, M_COPYALL, M_PASTE, M_SPECIAL, M_EVENTLOG, M_SETTINGS, M_RESTART, M_RESET, M_CLEAR,
    M_DUPLICATE, M_NEW, M_ABOUT
};

/* The Special command menu, and the submenus PuTTY may put in it. */
typedef struct SpecialMenu SpecialMenu;
struct SpecialMenu {
    menu *m;
    int   code[MAX_MENU_ITEMS];
    int   arg[MAX_MENU_ITEMS];
    SpecialMenu *child[MAX_MENU_ITEMS];
};

static menu *cur;                      /* the menu on screen */
static SpecialMenu *specials;
static int cur_win = -1;               /* the window it was opened on */
static int cur_x, cur_y;

static void free_special_menu(SpecialMenu *s)
{
    int i;
    if (!s) return;
    for (i = 0; i < MAX_MENU_ITEMS; i++) free_special_menu(s->child[i]);
    menu_free(s->m);
    free(s);
}

/* Build one level from the list PuTTY gave us, up to the item that closes it. */
static SpecialMenu *build_specials(const SessionSpecial **pp, const char *title)
{
    SpecialMenu *s = (SpecialMenu *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->m = menu_new(title);
    for (;;) {
        const SessionSpecial *sp = (*pp)++;
        int idx;
        if (sp->code == SS_EXITMENU) break;
        if (sp->code == SS_SEP) {
            const int last = s->m->nitems - 1;
            if (last >= 0) menu_set_flags(s->m, last, s->m->flags[last] | MF_DOTTED);
        } else if (sp->code == SS_SUBMENU) {
            SpecialMenu *c = build_specials(pp, sp->name);
            idx = menu_add(s->m, sp->name, 0);
            if (idx >= 0 && c) {
                menu_set_sub(s->m, idx, c->m);
                s->child[idx] = c;
            } else {
                free_special_menu(c);
            }
        } else {
            idx = menu_add(s->m, sp->name, 0);
            if (idx >= 0) {
                s->code[idx] = sp->code;
                s->arg[idx] = sp->arg;
            }
        }
    }
    return s;
}

static void open_current(void)
{
    menu_open(cur, cur_x - 64, cur_y);
}

void rocontext_open(RoTerm *t, int x, int y)
{
    menu *old = cur;
    SpecialMenu *old_specials = specials;
    menu *m = menu_new("PuTTY");
    SpecialMenu *sp = NULL;

    if (!m) return;
    if (t->backend) {
        const SessionSpecial *list = backend_get_specials(t->backend);
        if (list) {
            const SessionSpecial *p = list;
            sp = build_specials(&p, "Special");
        }
    }

    menu_add(m, "Copy", 0);
    menu_add(m, "Copy all", 0);
    menu_add(m, "Paste", MF_DOTTED);
    menu_add(m, "Special command", MF_DOTTED);
    menu_add(m, "Event log...", 0);
    menu_add(m, "Change settings...", 0);
    menu_add(m, "Restart session", 0);
    menu_add(m, "Reset terminal", 0);
    menu_add(m, "Clear scrollback", MF_DOTTED);
    menu_add(m, "Duplicate session", 0);
    menu_add(m, "New session...", MF_DOTTED);
    menu_add(m, "About PuTTY...", 0);

    if (sp && sp->m->nitems) menu_set_sub(m, M_SPECIAL, sp->m);
    else menu_shade(m, M_SPECIAL, 1);
    if (!t->term || t->backend || t->is_log) menu_shade(m, M_RESTART, 1);
    if (!t->term || t->is_log) menu_shade(m, M_SETTINGS, 1);
    if (!t->term) {
        menu_shade(m, M_COPY, 1);
        menu_shade(m, M_COPYALL, 1);
        menu_shade(m, M_PASTE, 1);
        menu_shade(m, M_RESET, 1);
        menu_shade(m, M_CLEAR, 1);
    }
    if (t->is_log) {                                   /* an Event Log window is only for reading */
        menu_shade(m, M_PASTE, 1);
        menu_shade(m, M_EVENTLOG, 1);
        menu_shade(m, M_DUPLICATE, 1);
    }

    cur = m;
    roapp_menu_opened(rocontext_selection);
    specials = sp;
    cur_win = t->win;
    cur_x = x;
    cur_y = y;
    open_current();

    menu_free(old);
    free_special_menu(old_specials);
}

/* Find what a path through the Special command menu means. */
static bool find_special(const int *sel, int *code, int *arg)
{
    SpecialMenu *s = specials;
    int i;
    for (i = 1; s && sel[i] >= 0; i++) {
        if (sel[i] >= MAX_MENU_ITEMS) return false;
        if (s->child[sel[i]]) {
            s = s->child[sel[i]];
            continue;
        }
        *code = s->code[sel[i]];
        *arg = s->arg[sel[i]];
        return true;
    }
    return false;
}

void rocontext_selection(const int *sel)
{
    RoTerm *t = roterm_from_window(cur_win);
    wpointer p;

    if (!cur || !t || sel[0] < 0) return;

    switch (sel[0]) {
      case M_COPY: {
        static const int clips[] = { MENU_CLIPBOARD };
        term_request_copy(t->term, clips, lenof(clips));
        break;
      }
      case M_COPYALL: {
        static const int clips[] = { COPYALL_CLIPBOARDS };
        term_copyall(t->term, clips, lenof(clips));
        break;
      }
      case M_PASTE:
        term_request_paste(t->term, MENU_CLIPBOARD);
        break;
      case M_SPECIAL: {
        int code, arg;
        if (t->backend && find_special(sel, &code, &arg))
            backend_special(t->backend, (SessionSpecialCode)code, arg);
        break;
      }
      case M_EVENTLOG:
        roterm_eventlog_show(t);
        break;
      case M_SETTINGS:
        roterm_change_settings(t);
        break;
      case M_RESTART:
        roterm_restart(t);
        break;
      case M_RESET:
        term_pwron(t->term, true);
        if (t->ldisc) ldisc_echoedit_update(t->ldisc);
        break;
      case M_CLEAR:
        term_clrsb(t->term);
        break;
      case M_DUPLICATE:
        roterm_new(conf_copy(t->conf));
        break;
      case M_NEW:
        roapp_connect_dialog();
        break;
      case M_ABOUT:
        roapp_about();
        break;
    }

    /* Adjust keeps the menu on screen, as for every other menu */
    if (!_swix(Wimp_GetPointerInfo, _IN(1), &p) && (p.buttons & BTN_ADJUST) && cur && t &&
        sel[0] != M_NEW && sel[0] != M_SETTINGS)
        open_current();
}
