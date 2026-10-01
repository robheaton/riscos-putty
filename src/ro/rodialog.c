/*
 * rodialog.c - question boxes.  See rodialog.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rodlg.h"
#include "rodialog.h"

#define MSG_COLS     60                /* characters per line (the Wimp font is 8 px wide) */
#define MSG_MAXLINES 24
#define MAX_BUTTONS  4
#define ID_BUTTON    100

typedef struct MsgBox MsgBox;
struct MsgBox {
    MsgBox *next;
    void   *owner;
    dlg    *d;
    RoMsgFn cb;
    void   *ctx;
    int     nbuttons;
};

static MsgBox *boxes;

static void unlink_box(MsgBox *m)
{
    MsgBox **pp;
    for (pp = &boxes; *pp; pp = &(*pp)->next)
        if (*pp == m) { *pp = m->next; break; }
}

static void msg_click(dlg *d, int id, int buttons, void *ud)
{
    MsgBox *m = (MsgBox *)ud;
    RoMsgFn cb = m->cb;
    void *ctx = m->ctx;
    const int idx = id - ID_BUTTON;
    (void)buttons;
    unlink_box(m);
    dlg_free(d);
    free(m);
    if (cb) cb(ctx, idx);
}

/* Break text into lines of at most MSG_COLS characters, at spaces where possible. */
static int wrap(const char *text, char lines[][MSG_COLS + 4])
{
    int n = 0;
    const char *p = text;
    while (*p && n < MSG_MAXLINES) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > MSG_COLS) {
            size_t cut = MSG_COLS;
            while (cut > 0 && p[cut] != ' ') cut--;
            len = cut ? cut : MSG_COLS;
            memcpy(lines[n], p, len);
            lines[n][len] = 0;
            p += len;
            while (*p == ' ') p++;
        } else {
            memcpy(lines[n], p, len);
            lines[n][len] = 0;
            p += len;
            if (*p == '\n') p++;
        }
        n++;
    }
    if (n == 0) { lines[0][0] = 0; n = 1; }
    return n;
}

void ro_msgbox(void *owner, const char *title, const char *text,
               const char *const *buttons, int nbuttons, int default_btn, int cancel_btn,
               RoMsgFn cb, void *ctx)
{
    static char lines[MSG_MAXLINES][MSG_COLS + 4];
    dlg_item items[MSG_MAXLINES + MAX_BUTTONS];
    int bw[MAX_BUTTONS];
    int n, i, w_px, h_px, total = 0, x, by;
    MsgBox *m;
    dlg *d;

    if (nbuttons < 1) nbuttons = 1;
    if (nbuttons > MAX_BUTTONS) nbuttons = MAX_BUTTONS;
    n = wrap(text ? text : "", lines);

    for (i = 0; i < nbuttons; i++) {
        bw[i] = 8 * (int)strlen(buttons[i]) + 28;
        if (bw[i] < 80) bw[i] = 80;
        total += bw[i] + (i ? 10 : 0);
    }
    w_px = 8 * MSG_COLS + 40;
    if (total + 40 > w_px) w_px = total + 40;
    h_px = 14 + 22 * n + 14 + 30 + 14;
    by = 14 + 22 * n + 14;

    for (i = 0; i < n; i++) {
        dlg_item it = { DI_LABEL, 0, 20, 14 + 22 * i, 8 * MSG_COLS, 22, lines[i], 0, 0, NULL };
        items[i] = it;
    }
    x = w_px - 20 - total;
    for (i = 0; i < nbuttons; i++) {
        dlg_item it = { i == default_btn ? DI_DEFBUTTON : DI_BUTTON, ID_BUTTON + i, x, by, bw[i], 30,
                        buttons[i], 0, 0, NULL };
        items[n + i] = it;
        x += bw[i] + 10;
    }

    m = (MsgBox *)calloc(1, sizeof *m);
    if (!m) return;
    m->owner = owner;
    m->cb = cb;
    m->ctx = ctx;
    m->nbuttons = nbuttons;
    d = dlg_new(title, w_px, h_px, items, n + nbuttons, msg_click, m);
    if (!d) { free(m); return; }
    m->d = d;
    dlg_set_keys(d, default_btn >= 0 ? ID_BUTTON + default_btn : -1,
                 cancel_btn >= 0 ? ID_BUTTON + cancel_btn : -1);
    m->next = boxes;
    boxes = m;
    dlg_open(d);
}

void ro_msgbox_drop_owner(void *owner)
{
    MsgBox **pp = &boxes;
    while (*pp) {
        MsgBox *m = *pp;
        if (m->owner == owner) {
            *pp = m->next;
            dlg_free(m->d);
            free(m);
        } else {
            pp = &m->next;
        }
    }
}

void ro_notice(void *owner, const char *title, const char *text)
{
    static const char *const ok[] = { "OK" };
    ro_msgbox(owner, title, text, ok, 1, 0, 0, NULL, NULL);
}
