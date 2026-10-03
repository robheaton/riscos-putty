/*
 * rodialog.h - question boxes with a row of buttons, for the things PuTTY has to
 * ask: "do you trust this host key?", "overwrite the log file?" and so on.
 *
 * They are not modal: PuTTY's API lets a question be answered later through a
 * callback, so the desktop and other sessions keep running meanwhile.
 */
#ifndef PUTTY_RODIALOG_H
#define PUTTY_RODIALOG_H

#include <stdbool.h>

typedef void (*RoMsgFn)(void *ctx, int button);

/* Show a message (word-wrapped; \n starts a new line) with the given buttons.
 * When the user answers - or Escapes or closes the box, which counts as cancel_btn -
 * the box goes away and cb(ctx, index_of_button) is called, once.
 *
 * `owner` is any pointer that groups boxes belonging together (a session);
 * ro_msgbox_drop_owner() removes its boxes without calling their callbacks. */
void ro_msgbox(void *owner, const char *title, const char *text,
               const char *const *buttons, int nbuttons, int default_btn, int cancel_btn,
               RoMsgFn cb, void *ctx);

void ro_msgbox_drop_owner(void *owner);

/* A box with just an OK button; nothing happens when it is dismissed. */
void ro_notice(void *owner, const char *title, const char *text);

/*
 * A question with a line to type the answer in (hidden as it is typed if `secret`: a passphrase).
 * When it is answered - OK, or Return - cb(ctx, answer) is called once; if the box is cancelled
 * or closed, cb(ctx, NULL).  The answer is only good during the call: copy it if it is needed.
 * (`owner` and ro_msgbox_drop_owner() are as for ro_msgbox, and drop a prompt silently as well.)
 */
typedef void (*RoPromptFn)(void *ctx, const char *answer);
void ro_prompt(void *owner, const char *title, const char *text, bool secret, const char *initial,
               RoPromptFn cb, void *ctx);

#endif /* PUTTY_RODIALOG_H */
