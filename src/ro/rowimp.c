/*
 * rowimp.c - small Wimp helpers (adapted from riscos-vncclient).
 */
#include <stdio.h>
#include <string.h>
#include "rowimp.h"

void wimp_error(const char *text)
{
    struct { int errnum; char msg[248]; } e;
    e.errnum = 0;
    snprintf(e.msg, sizeof e.msg, "%s", text);
    /* R1 bit 0: OK button.  R2: application name for the title. */
    _swix(Wimp_ReportError, _INR(0, 2), &e, 1, "PuTTY");
}

void wimp_error_os(const _kernel_oserror *e)
{
    if (e) wimp_error(e->errmess);
}

bool wimp_sprite_exists(const char *name)
{
    int w = 0, h = 0;
    return !_swix(Wimp_SpriteOp, _INR(0, 2) | _OUTR(3, 4), 40, 0, name, &w, &h);
}

void wimp_msg_path(const wmessage *m, char *out, size_t cap)
{
    const char *src = (const char *)&m->data[6];
    const size_t avail = sizeof m->data - 6 * sizeof m->data[0];
    size_t n = 0;
    while (n < avail && n + 1 < cap && src[n] >= ' ') n++;     /* stop at the terminator or a control code */
    memcpy(out, src, n);
    out[n] = 0;
}

void wimp_send_load_ack(const wmessage *m, const char *path)
{
    int blk[64];
    memset(blk, 0, sizeof blk);
    blk[0] = 44 + (int)((strlen(path) + 4) & ~(size_t)3);      /* with the terminator, word aligned */
    blk[3] = m->my_ref;
    blk[4] = MSG_DATA_LOAD_ACK;
    memcpy(&blk[5], &m->data[0], 4 * sizeof(int));
    blk[9] = m->data[4];
    blk[10] = m->data[5];
    strcpy((char *)&blk[11], path);
    _swix(Wimp_SendMessage, _INR(0, 2), 17, blk, m->sender);
}

int wimp_create_icon_iconbar(unsigned flags, const char *sprite, const char *text, int size)
{
    /* Wimp_CreateIcon block: window handle, then the icon definition.  For an
     * iconbar icon the handle is -1 (right hand side). */
    struct { int win; idef icon; } blk;
    static char textbuf[24];
    int handle = -1;

    memset(&blk, 0, sizeof blk);
    blk.win = -1;
    blk.icon.box.x0 = 0;
    blk.icon.box.y0 = 0;
    blk.icon.box.x1 = size;
    blk.icon.box.y1 = size;
    if (flags & IF_SPRITE) {
        /* a plain (not indirected) sprite icon: the 12 byte data field is the
         * sprite's name, looked up in the Wimp's sprite pool */
        blk.icon.flags = flags & ~IF_INDIRECTED;
        snprintf(blk.icon.data.text, sizeof blk.icon.data.text, "%s", sprite ? sprite : "");
    } else {
        snprintf(textbuf, sizeof textbuf, "%s", text ? text : "");
        blk.icon.flags = flags | IF_INDIRECTED;
        blk.icon.data.ind.buf = textbuf;
        /* Not -1: for an iconbar icon the Wimp reads the validation string even
         * when it is only text, and faults on that pointer. */
        blk.icon.data.ind.validation = (char *)"";
        blk.icon.data.ind.size = (int)sizeof textbuf;
    }
    _swix(Wimp_CreateIcon, _INR(0, 1) | _OUT(0), 0, &blk, &handle);
    return handle;
}

/* Wimp_SetIconState takes a block: window, icon, EOR word, clear word;
 * new flags = (old & ~clear) ^ eor. */
void wimp_set_icon_state(int window, int icon, unsigned eor, unsigned clear)
{
    unsigned blk[4];
    blk[0] = (unsigned)window;
    blk[1] = (unsigned)icon;
    blk[2] = eor;
    blk[3] = clear;
    _swix(Wimp_SetIconState, _IN(1), blk);
}

void wimp_set_icon_text(int window, int icon, const char *text)
{
    /* Indirected text icons: find the buffer via Wimp_GetIconState. */
    struct { int win, icon; idef def; } st;
    st.win = window;
    st.icon = icon;
    if (_swix(Wimp_GetIconState, _IN(1), &st)) return;
    if (st.def.flags & IF_INDIRECTED) {
        snprintf(st.def.data.ind.buf, (size_t)st.def.data.ind.size, "%s", text);
        wimp_set_icon_state(window, icon, 0, 0);                    /* redraw */
    }
}

void wimp_get_icon_text(int window, int icon, char *out, size_t cap)
{
    struct { int win, icon; idef def; } st;
    st.win = window;
    st.icon = icon;
    out[0] = 0;
    if (_swix(Wimp_GetIconState, _IN(1), &st)) return;
    if (st.def.flags & IF_INDIRECTED)
        snprintf(out, cap, "%s", st.def.data.ind.buf);
    else
        snprintf(out, cap, "%.11s", st.def.data.text);
}
