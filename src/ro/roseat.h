/*
 * roseat.h - the questions PuTTY asks about a connection (is this host key right? is this weak
 * cipher acceptable?), shown in non-modal boxes, for the seats of both the terminal windows (roseat.c)
 * and the file transfer window (rofiles.c).
 */
#ifndef PUTTY_ROSEAT_H
#define PUTTY_ROSEAT_H

#include "putty.h"

/* `owner` groups the boxes of one connection (see ro_msgbox_drop_owner).  The answer comes later,
 * through `callback`; these return SPR_INCOMPLETE. */
SeatPromptResult roseat_confirm_host_key(
    void *owner, Seat *seat, const char *host, int port, const char *keytype, char *keystr,
    SeatDialogText *text, void (*callback)(void *ctx, SeatPromptResult result), void *ctx);
SeatPromptResult roseat_confirm_yesno(
    void *owner, SeatDialogText *text, void (*callback)(void *ctx, SeatPromptResult result), void *ctx);
const SeatDialogPromptDescriptions *roseat_prompt_descriptions(void);

#endif /* PUTTY_ROSEAT_H */
