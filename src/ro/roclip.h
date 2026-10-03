/*
 * roclip.h - the RISC OS global clipboard, for PuTTY terminals.
 *
 * RISC OS has a "global clipboard" protocol carried by Wimp messages:
 *  - an application that copies something broadcasts Message_ClaimEntity;
 *  - one that wants to paste sends Message_DataRequest to the owner, which
 *    answers with Message_DataSave; the requester replies DataSaveAck naming a
 *    scrap file, the owner saves the text there and sends DataLoad, and the
 *    requester loads it and answers DataLoadAck.
 *
 * A terminal takes both roles: Copy claims the clipboard and later hands the text
 * to whoever pastes; Paste asks the owner for its text and delivers it to PuTTY
 * (term_do_paste).  A text file dropped on a terminal window is pasted as well.
 * Only plain text is transferred.
 */
#ifndef PUTTY_ROCLIP_H
#define PUTTY_ROCLIP_H

#include <stddef.h>
#include <wchar.h>
#include "rowimp.h"

void roclip_init(int task_handle);
void roclip_shutdown(void);

/* Where pasted text is delivered.  is_target says whether a window takes pasted text; paste
 * gives it some (wide characters, LF line ends).  Without this - a program that only copies -
 * nothing can be pasted. */
typedef struct {
    bool (*is_target)(int win);
    void (*paste)(int win, const wchar_t *text, size_t len);
} RoClipPaste;
void roclip_set_paste(const RoClipPaste *p);

/* Handle a Wimp message; returns 1 if it was one of ours. */
int  roclip_message(const wmessage *m);

/* Copy: text is wide characters with LF line ends (as PuTTY supplies it). */
void roclip_copy(const wchar_t *text, size_t len);

/* Paste into the terminal shown in window `win`: the text arrives later (or at
 * once, if we own the clipboard) through term_do_paste. */
void roclip_paste(int win);

#endif /* PUTTY_ROCLIP_H */
