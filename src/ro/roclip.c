/*
 * roclip.c - the RISC OS global clipboard for PuTTY terminals.  See roclip.h for
 * the protocol; the logic follows the clipboard of riscos-vncclient (same author).
 *
 * Message block layout used below (word indices into wmessage):
 *   size, sender, my_ref, your_ref, action, then data[0..]
 *   DataRequest: data[0] window, [1] icon, [2] x, [3] y, [4] flags (4 = clipboard),
 *                [5..] file types, -1 terminated
 *   DataSave:    data[0] window, [1] icon, [2] x, [3] y, [4] size, [5] type, [6..] leaf name
 *   DataSaveAck: same, with a full pathname (size -1: a scrap file the sender may delete)
 *   DataLoad:    same as DataSave/Ack with the pathname of the saved file
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "ro.h"
#include "rowimp.h"
#include "roterm.h"
#include "roclip.h"

#define TEXT_TYPE       0xFFF
#define MAX_TEXT        (256 * 1024)
#define SCRAP           "<Wimp$Scrap>"
#define FLAG_CLIPBOARD  4u             /* DataRequest: wants the clipboard */
#define CLAIM_CLIPBOARD 4u             /* ClaimEntity: the clipboard (bits 0 and 1 are the caret and selection) */
#define PATH_MAX_LEN    200

static int    my_task;
static bool   utf8;                    /* the desktop alphabet is UTF-8 */

/* the text we copied, and whether we still hold the RISC OS clipboard */
static uint8_t *own;
static size_t   own_len;
static bool     own_owner;
static int      owner_task;            /* who claimed it last, 0 if we do not know */

/* offering our text to somebody who pastes: the DataSave we sent, and the text as
 * it will be delivered (in the desktop alphabet) with how much of it has gone */
static int      save_task, save_ref;
static uint8_t *xfer;
static size_t   xfer_len, xfer_off;

/* our own paste: the DataRequest we sent, and the terminal it is for */
static int      ask_ref, ask_win;
/* ... and the DataSaveAck that followed, with where the text is to go */
static int      ack_task, ack_ref, ack_win;

/* ------------------------------------------------------------------------ */
/* text conversion                                                          */

/* The desktop alphabet (Latin-1 or UTF-8) -> wide characters, CRLF and CR -> LF. */
static wchar_t *to_wide(const uint8_t *in, size_t n, size_t *outlen)
{
    wchar_t *out = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    size_t i = 0, o = 0;
    if (!out) return NULL;
    while (i < n) {
        unsigned c = in[i++];
        if (utf8 && c >= 0x80) {
            unsigned cp = c;           /* a stray byte that is not UTF-8: take it as Latin-1 */
            int extra = 0;
            if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; extra = 1; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
            while (extra > 0 && i < n && (in[i] & 0xC0) == 0x80) {
                cp = (cp << 6) | (in[i++] & 0x3Fu);
                extra--;
            }
            c = extra > 0 ? 0xFFFD : cp;
        }
        if (c == '\r') {
            if (i < n && in[i] == '\n') i++;
            c = '\n';
        }
        out[o++] = (wchar_t)c;
    }
    out[o] = 0;
    *outlen = o;
    return out;
}

/* Wide characters -> the desktop alphabet.  `out` needs room for 4 * n bytes. */
static size_t from_wide(const wchar_t *in, size_t n, uint8_t *out)
{
    size_t i, o = 0;
    for (i = 0; i < n; i++) {
        unsigned c = (unsigned)in[i];
        if (utf8 && c >= 0x80) {
            if (c < 0x800) {
                out[o++] = (uint8_t)(0xC0 | (c >> 6));
                out[o++] = (uint8_t)(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                out[o++] = (uint8_t)(0xE0 | (c >> 12));
                out[o++] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
                out[o++] = (uint8_t)(0x80 | (c & 0x3F));
            } else {
                out[o++] = (uint8_t)(0xF0 | (c >> 18));
                out[o++] = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
                out[o++] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
                out[o++] = (uint8_t)(0x80 | (c & 0x3F));
            }
        } else {
            out[o++] = (uint8_t)(c > 0xFF ? '?' : c);
        }
    }
    return o;
}

/* ------------------------------------------------------------------------ */
/* messages                                                                 */

/* Send a message; the Wimp fills in the sender and my_ref words of blk.  Returns
 * my_ref, or 0 if the message could not be sent. */
static int send_message(int reason, int *blk, int task)
{
    _kernel_oserror *e = _swix(Wimp_SendMessage, _INR(0, 2), reason, blk, task);
    return e ? 0 : blk[2];
}

static int name_words(const char *name)
{
    return (int)((strlen(name) + 4) & ~(size_t)3);      /* including the terminator, word aligned */
}

static void claim_clipboard(void)
{
    int blk[6];
    memset(blk, 0, sizeof blk);
    blk[0] = 24;
    blk[4] = MSG_CLAIM_ENTITY;
    blk[5] = (int)CLAIM_CLIPBOARD;
    send_message(17, blk, 0);
    own_owner = true;
}

static void release_clipboard(void)
{
    int blk[6];
    if (!own_owner) return;
    memset(blk, 0, sizeof blk);
    blk[0] = 24;
    blk[4] = MSG_RELEASE_ENTITY;
    blk[5] = (int)CLAIM_CLIPBOARD;
    send_message(17, blk, 0);
    own_owner = false;
}

/* Ask whoever owns the clipboard for its text, to be pasted into window `win`. */
static void request_clipboard(int win)
{
    int blk[12];
    int tries;
    for (tries = 0; tries < 2; tries++) {
        memset(blk, 0, sizeof blk);
        blk[0] = 48;
        blk[4] = MSG_DATA_REQUEST;
        blk[5] = win;
        blk[6] = -1;
        blk[9] = (int)FLAG_CLIPBOARD;
        blk[10] = TEXT_TYPE;
        blk[11] = -1;
        ask_win = win;
        ask_ref = send_message(18, blk, owner_task);
        if (ask_ref || !owner_task) break;
        owner_task = 0;                 /* the owner has gone: try everyone */
    }
}

/* ---- we are the owner: somebody pastes ---- */

static int wants_text(const int *types)
{
    int i;
    for (i = 0; i < 16 && types[i] != -1; i++)
        if (types[i] == TEXT_TYPE) return 1;
    return 0;
}

static void answer_request(const wmessage *m)
{
    int blk[16];
    const char *leaf = "Clipboard";
    if (!own || !own_owner) return;
    if (!(((unsigned)m->data[4]) & FLAG_CLIPBOARD) || !wants_text(&m->data[5])) return;
    free(xfer);
    xfer = (uint8_t *)malloc(own_len + 1);
    if (!xfer) return;
    memcpy(xfer, own, own_len);
    xfer_len = own_len;
    xfer_off = 0;
    memset(blk, 0, sizeof blk);
    blk[0] = 44 + name_words(leaf);
    blk[3] = m->my_ref;
    blk[4] = MSG_DATA_SAVE;
    blk[5] = m->data[0];
    blk[6] = m->data[1];
    blk[7] = m->data[2];
    blk[8] = m->data[3];
    blk[9] = (int)xfer_len;
    blk[10] = TEXT_TYPE;
    strcpy((char *)&blk[11], leaf);
    save_task = m->sender;
    save_ref = send_message(18, blk, m->sender);
}

/* The requester wants the text in a file (DataSaveAck): save it and say so. */
static void save_to(const wmessage *m)
{
    char path[PATH_MAX_LEN + 1];
    FILE *f;
    int blk[64];

    wimp_msg_path(m, path, sizeof path);
    if (!xfer || !path[0]) return;
    f = fopen(path, "wb");
    if (!f) return;
    fwrite(xfer, 1, xfer_len, f);
    fclose(f);
    ro_set_filetype(path, TEXT_TYPE);

    memset(blk, 0, sizeof blk);
    blk[0] = 44 + name_words(path);
    blk[3] = m->my_ref;
    blk[4] = MSG_DATA_LOAD;
    blk[5] = m->data[0];
    blk[6] = m->data[1];
    blk[7] = m->data[2];
    blk[8] = m->data[3];
    blk[9] = (int)xfer_len;
    blk[10] = TEXT_TYPE;
    strcpy((char *)&blk[11], path);
    send_message(18, blk, m->sender);
    save_task = 0;
}

/* ... or in memory (RAMFetch names a buffer in its address space): copy as much
 * as fits with Wimp_TransferBlock and report how much (RAMTransmit).  A full
 * buffer means it will ask again for the rest. */
static void ram_fetch(const wmessage *m)
{
    const size_t room = (size_t)(unsigned)m->data[1];
    size_t n = xfer_len - xfer_off;
    int blk[8];

    if (!xfer) return;
    if (n > room) n = room;
    if (n > 0 &&
        _swix(Wimp_TransferBlock, _INR(0, 4), my_task, xfer + xfer_off, m->sender, m->data[0], (int)n))
        n = 0;
    xfer_off += n;
    memset(blk, 0, sizeof blk);
    blk[0] = 28;
    blk[3] = m->my_ref;
    blk[4] = MSG_RAM_TRANSMIT;
    blk[5] = m->data[0];
    blk[6] = (int)n;
    send_message(17, blk, m->sender);
}

/* ---- we are the requester: text arrives from a RISC OS application ---- */

static void paste_into(int win, const uint8_t *raw, size_t n)
{
    RoTerm *t = roterm_from_window(win);
    wchar_t *w;
    size_t wn;
    if (!t || !t->term) return;
    w = to_wide(raw, n, &wn);
    if (!w) return;
    term_do_paste(t->term, w, wn);
    free(w);
}

static void load_file(const char *path, int delete_after, int win)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    size_t n;
    if (!f) return;
    buf = (uint8_t *)malloc(MAX_TEXT + 1);
    if (!buf) { fclose(f); return; }
    n = fread(buf, 1, MAX_TEXT, f);
    fclose(f);
    if (delete_after) remove(path);
    if (n > 0) paste_into(win, buf, n);
    free(buf);
}

/* An application offers text (DataSave): in answer to our request, or saved onto
 * a terminal window.  Ask for it in the scrap file. */
static void accept_save(const wmessage *m, int win)
{
    int blk[64];
    const int type = m->data[5];
    if (type != TEXT_TYPE || !roterm_from_window(win)) return;
    memset(blk, 0, sizeof blk);
    blk[0] = 44 + name_words(SCRAP);
    blk[3] = m->my_ref;
    blk[4] = MSG_DATA_SAVE_ACK;
    blk[5] = m->data[0];
    blk[6] = m->data[1];
    blk[7] = m->data[2];
    blk[8] = m->data[3];
    blk[9] = -1;                                    /* a scrap file: the sender may delete it */
    blk[10] = type;
    strcpy((char *)&blk[11], SCRAP);
    ack_task = m->sender;
    ack_win = win;
    ack_ref = send_message(17, blk, m->sender);
}

/* The text has been saved (DataLoad), or a file was dropped on a terminal window. */
static void accept_load(const wmessage *m, bool ours)
{
    char path[PATH_MAX_LEN + 1];
    const int win = ours ? ack_win : m->data[0];

    wimp_msg_path(m, path, sizeof path);
    if (m->data[5] != TEXT_TYPE || !path[0] || !roterm_from_window(win)) return;
    load_file(path, ours && !strcmp(path, SCRAP), win);
    ack_task = ack_ref = ack_win = 0;
    wimp_send_load_ack(m, path);
}

/* ------------------------------------------------------------------------ */

void roclip_init(int task_handle)
{
    int alphabet = 0;
    my_task = task_handle;
    _swix(OS_Byte, _INR(0, 1) | _OUT(1), 71, 127, &alphabet);
    utf8 = (alphabet == 111);
}

void roclip_shutdown(void)
{
    release_clipboard();
    free(own);
    free(xfer);
    own = xfer = NULL;
    own_len = 0;
}

void roclip_copy(const wchar_t *text, size_t len)
{
    uint8_t *buf;
    if (len == 0) return;
    buf = (uint8_t *)malloc(len * 4 + 1);
    if (!buf) return;
    free(own);
    own = buf;
    own_len = from_wide(text, len, buf);
    save_task = 0;                      /* a paste of the old text in progress is void */
    claim_clipboard();
}

void roclip_paste(int win)
{
    if (win < 0) return;
    if (own_owner && own) {
        paste_into(win, own, own_len);  /* our own text: no need to ask */
        return;
    }
    request_clipboard(win);
}

int roclip_message(const wmessage *m)
{
    switch (m->action) {
    case MSG_CLAIM_ENTITY:
        if (m->sender == my_task) return 1;
        if ((unsigned)m->data[0] & CLAIM_CLIPBOARD) {
            own_owner = false;          /* somebody else copied something */
            owner_task = m->sender;
        }
        return 1;

    case MSG_RELEASE_ENTITY:
        if (m->sender != my_task && ((unsigned)m->data[0] & CLAIM_CLIPBOARD) && m->sender == owner_task)
            owner_task = 0;
        return 1;

    case MSG_DATA_REQUEST:
        if (m->sender != my_task) answer_request(m);
        return 1;

    case MSG_DATA_SAVE: {
        /* an answer to our request, or a text file being saved onto a terminal window */
        const bool ours = ask_ref && m->your_ref == ask_ref;
        if (ours) {
            ask_ref = 0;
            accept_save(m, ask_win);
        } else if (roterm_from_window(m->data[0])) {
            accept_save(m, m->data[0]);
        }
        return 1;
    }

    case MSG_DATA_SAVE_ACK:
        if (save_task && m->sender == save_task && (!save_ref || m->your_ref == save_ref)) save_to(m);
        return 1;

    case MSG_DATA_LOAD: {
        const bool ours = ack_task == m->sender && ack_ref && m->your_ref == ack_ref;
        if (ours || roterm_from_window(m->data[0])) accept_load(m, ours);
        return 1;
    }

    case MSG_RAM_FETCH:
        if (save_task && m->sender == save_task) ram_fetch(m);
        return 1;

    case MSG_RAM_TRANSMIT:
    case MSG_DATA_LOAD_ACK:
        return 1;
    }
    return 0;
}
