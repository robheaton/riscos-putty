/*
 * rosave.h - saving a file by dragging it, with the Wimp's data transfer protocol, and the "save as"
 * box built on it.
 *
 * Dragging.  A program offers a file that does not exist yet by letting the person drag its icon
 * (rosave_drag).  When it is dropped on a window - a directory display of the Filer, a window of an
 * application - the protocol runs: we send Message_DataSave naming the file (a leaf only) and its
 * type; the receiver answers Message_DataSaveAck with the full pathname to write to; the callback
 * writes the data there; and if the receiver asked for a scrap file (not the Filer, which keeps what is
 * saved where it said) it is told to load it, with Message_DataLoad.
 *
 * The save box shows a file icon, the name to save under, and Cancel and Save buttons.  The icon is
 * dragged as above; or a full pathname is typed in the box and Save clicked.  Only one save box is open
 * at a time; opening another closes the first.
 */
#ifndef PUTTY_ROSAVE_H
#define PUTTY_ROSAVE_H

#include <stdbool.h>
#include "rowimp.h"

/* Write the data to `path`.  Return true if it was saved; if not, say why. */
typedef bool (*RoSaveFn)(void *ctx, const char *path);
/* The save, or the attempt, is over (saved or not); ctx can be freed now.  May be NULL. */
typedef void (*RoSaveDone)(void *ctx, bool saved);

/*
 * Start dragging `sprite` (a Wimp sprite name) from the screen box `from` (x0, y0, x1, y1), offering a
 * file called `leaf` of type `filetype`.  `source_win` is the window the drag starts in; dropping it
 * there, or on nothing, offers nothing.  When the drag ends done(ctx, saved) is called once, after the
 * file has been saved, or at once if nothing is to be saved.  Returns false if no drag could start.
 */
bool rosave_drag(const char *sprite, const int *from, int source_win, const char *leaf, int filetype,
                 RoSaveFn save, RoSaveDone done, void *ctx);

/*
 * Dragging several files: from the `done` callback of a drag whose file was saved (saved == true), offer another
 * file to the same place.  Returns false if that is not possible (call done yourself, with false, then).
 */
bool rosave_drop_another(const char *leaf, int filetype, RoSaveFn save, RoSaveDone done, void *ctx);

/* `leaf` is the name to offer, `filetype` the RISC OS filetype of what is saved. */
void rosave_open(const char *title, const char *leaf, int filetype, RoSaveFn save, RoSaveDone done,
                 void *ctx);

/* Close the box if it is open, as if Cancel had been clicked. */
void rosave_close(void);

/* Events for the main loop: the end of a drag started here, a reply to a message of ours (returns 1 if
 * the message was one), and a message of ours that nobody answered. */
void rosave_drag_end(wevent *e);
int  rosave_message(const wmessage *m);
void rosave_bounced(const wmessage *m);

#endif /* PUTTY_ROSAVE_H */
