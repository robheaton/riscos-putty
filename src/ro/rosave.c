/*
 * rosave.c - dragging a file out to save it, and the "save as" box.  See rosave.h.
 *
 * The data transfer protocol, from our side as the sender (PRM 3-260):
 *
 *   drop on a window:   we send  Message_DataSave  to the window and icon under the pointer, naming
 *                       the file (a leaf only) and its type;
 *   receiver answers:   Message_DataSaveAck  with the full pathname to write to.  Its size word is
 *                       -1 if that is a scrap file for us to hand over, otherwise a real destination
 *                       (the Filer's directory displays answer this way);
 *   we save the file    and, for a scrap file, send  Message_DataLoad  so the receiver reads it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ro.h"
#include "rowimp.h"
#include "rodlg.h"
#include "rodialog.h"
#include "rosave.h"

enum { SV_ICON = 1, SV_NAME, SV_SAVE, SV_CANCEL };

#define W_PX      272
#define H_PX      168
#define EST_SIZE  4096                  /* the "estimated size" in a DataSave: the receiver only checks for room */

/* ------------------------------------------------------------------------ */
/* dragging a file out                                                      */

static struct {
    bool       dragging, drag_sprite;
    int        ref;                     /* my_ref of the DataSave we await an answer to, or 0 */
    wpointer   target;                  /* where the file was dropped */
    bool       have_target;
    int        source_win;
    char       leaf[200];
    int        filetype;
    RoSaveFn   save;
    RoSaveDone done;
    void      *ctx;
} xf;

static int name_words(const char *name)
{
    return (int)((strlen(name) + 4) & ~(size_t)3);      /* including the terminator, word aligned */
}

static int object_type(const char *path, int *length)
{
    int type = 0, len = 0;
    if (_swix(OS_File, _INR(0, 1) | _OUT(0) | _OUT(4), 17, path, &type, &len)) return 0;
    if (length) *length = len;
    return type;
}

/* Where the file just saved was dropped, while its done callback runs (see rosave_drop_another). */
static wpointer chain_target;
static bool     chain_valid;

/* The drag, and anything it started, is over. */
static void drag_finished(bool saved)
{
    RoSaveDone done = xf.done;
    void *ctx = xf.ctx;

    if (saved && xf.have_target) {
        chain_target = xf.target;
        chain_valid = true;
    }
    memset(&xf, 0, sizeof xf);
    if (done) done(ctx, saved);
    chain_valid = false;
}

static void screen_size(int *w, int *h)
{
    int v;
    *w = 2048; *h = 2048;
    if (!_swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 11, &v)) {
        int e = 1;
        _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 4, &e);
        *w = (v + 1) << e;
    }
    if (!_swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 12, &v)) {
        int e = 1;
        _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 5, &e);
        *h = (v + 1) << e;
    }
}

bool rosave_drag(const char *sprite, const int *from, int source_win, const char *leaf, int filetype,
                 RoSaveFn save, RoSaveDone done, void *ctx)
{
    int box[4];

    if (xf.dragging || xf.ref) return false;
    memcpy(box, from, sizeof box);
    /* DragASprite shows the file's own icon being carried; if it is not there (before RISC OS 3.5),
     * an outline box does. */
    xf.drag_sprite = !_swix(DragASprite_Start, _INR(0, 4), 0, 1, sprite, box, 0);
    if (!xf.drag_sprite) {
        struct { int win, type; wbox box, parent; } db;
        int sw, sh;
        screen_size(&sw, &sh);
        db.win = source_win;
        db.type = 5;
        db.box.x0 = box[0]; db.box.y0 = box[1]; db.box.x1 = box[2]; db.box.y1 = box[3];
        db.parent.x0 = 0; db.parent.y0 = 0; db.parent.x1 = sw; db.parent.y1 = sh;
        if (_swix(Wimp_DragBox, _IN(1), &db)) return false;
    }
    xf.dragging = true;
    xf.source_win = source_win;
    snprintf(xf.leaf, sizeof xf.leaf, "%s", leaf && *leaf ? leaf : "Data");
    xf.filetype = filetype;
    xf.save = save;
    xf.done = done;
    xf.ctx = ctx;
    return true;
}

static void send_data_save(const wpointer *p)
{
    int blk[64];

    memset(blk, 0, sizeof blk);
    blk[0] = 44 + name_words(xf.leaf);
    blk[4] = MSG_DATA_SAVE;
    blk[5] = p->window;
    blk[6] = p->icon;
    blk[7] = p->x;
    blk[8] = p->y;
    blk[9] = EST_SIZE;
    blk[10] = xf.filetype;
    strcpy((char *)&blk[11], xf.leaf);
    if (!_swix(Wimp_SendMessage, _INR(0, 3), 18, blk, p->window, p->icon)) {
        xf.ref = blk[2];
        xf.target = *p;
        xf.have_target = true;
    }
}

bool rosave_drop_another(const char *leaf, int filetype, RoSaveFn save, RoSaveDone done, void *ctx)
{
    wpointer where;

    if (!chain_valid || xf.ref || xf.dragging) return false;
    where = chain_target;
    chain_valid = false;                                    /* (one more at a time: this call is the next link) */
    snprintf(xf.leaf, sizeof xf.leaf, "%s", leaf && *leaf ? leaf : "Data");
    xf.filetype = filetype;
    xf.save = save;
    xf.done = done;
    xf.ctx = ctx;
    send_data_save(&where);
    return xf.ref != 0;
}

void rosave_drag_end(wevent *e)
{
    wpointer p;
    (void)e;
    if (!xf.dragging) return;
    xf.dragging = false;
    if (xf.drag_sprite) _swix(DragASprite_Stop, 0);
    if (_swix(Wimp_GetPointerInfo, _IN(1), &p) || p.window == xf.source_win || p.window == -1) {
        drag_finished(false);                              /* dropped on ourselves, or on nothing */
        return;
    }
    send_data_save(&p);
    if (!xf.ref) drag_finished(false);
}

/* The receiver has told us where to put the file. */
static void saved_where_told(const wmessage *m)
{
    char path[256];
    int blk[64], length = 0;

    wimp_msg_path(m, path, sizeof path);
    ro_trace("PuTTYsave$Trace", "DataSaveAck from task %d: size word %d, type %03x, path %s", m->sender, m->data[4],
             m->data[5], path);
    if (!path[0] || !xf.save(xf.ctx, path)) {                /* it said what went wrong */
        drag_finished(false);
        return;
    }
    if (m->data[4] == -1) {                                /* a scrap file: the receiver must be told to load it */
        object_type(path, &length);
        memset(blk, 0, sizeof blk);
        blk[0] = 44 + name_words(path);
        blk[3] = m->my_ref;
        blk[4] = MSG_DATA_LOAD;
        memcpy(&blk[5], &m->data[0], 4 * sizeof(int));
        blk[9] = length;
        blk[10] = xf.filetype;
        strcpy((char *)&blk[11], path);
        _swix(Wimp_SendMessage, _INR(0, 2), 18, blk, m->sender);
    }
    drag_finished(true);
}

int rosave_message(const wmessage *m)
{
    if (!xf.ref) return 0;
    if (m->action == MSG_DATA_SAVE_ACK && m->your_ref == xf.ref) {
        xf.ref = 0;
        saved_where_told(m);
        return 1;
    }
    return 0;
}

void rosave_bounced(const wmessage *m)
{
    /* nothing there understood the drop */
    if (xf.ref && m->action == MSG_DATA_SAVE && m->my_ref == xf.ref) {
        xf.ref = 0;
        drag_finished(false);
    }
}

/* ------------------------------------------------------------------------ */
/* the save box                                                             */

static struct {
    dlg        *d;
    char        sprite[16];
    int         filetype;
    RoSaveFn    save;
    RoSaveDone  done;
    void       *ctx;
} sv;

static unsigned generation;             /* counts boxes opened, so a late answer to a question about an old one is spotted */

/* The box has done its job (or been given up on). */
static void finish(bool saved)
{
    RoSaveDone done = sv.done;
    void *ctx = sv.ctx;
    if (!sv.d) return;
    dlg_free(sv.d);
    memset(&sv, 0, sizeof sv);
    if (done) done(ctx, saved);
}

typedef struct { unsigned generation; char path[256]; } Overwrite;

static void save_to_path(const char *path)
{
    if (sv.save(sv.ctx, path))
        finish(true);
}

static void overwrite_answered(void *ctx, int button)
{
    Overwrite *o = (Overwrite *)ctx;
    if (button == 0 && sv.d && o->generation == generation)
        save_to_path(o->path);
    free(o);
}

static void save_typed(void)
{
    const char *name = dlg_get_text(sv.d, SV_NAME);
    int type;

    if (!strpbrk(name, ".:")) {
        ro_notice(NULL, "Save", "To save the file, drag its icon to a directory display, or type the whole "
                  "pathname of the file and click Save.");
        return;
    }
    type = object_type(name, NULL);
    if (type == 2) {
        ro_notice(NULL, "Save", "That is the name of a directory, not a file.");
        return;
    }
    if (type != 0) {
        static const char *const buttons[] = { "Overwrite", "Cancel" };
        Overwrite *o = (Overwrite *)calloc(1, sizeof *o);
        char *msg;
        if (!o) return;
        o->generation = generation;
        snprintf(o->path, sizeof o->path, "%s", name);
        msg = (char *)malloc(strlen(name) + 40);
        if (!msg) { free(o); return; }
        sprintf(msg, "The file\n%s\nalready exists.", name);
        ro_msgbox(NULL, "Save", msg, buttons, 2, 1, 1, overwrite_answered, o);
        free(msg);
        return;
    }
    save_to_path(name);
}

/* The icon has been dragged and dropped (or not): a file that was saved there is the box's job done. */
static void box_drag_done(void *ctx, bool saved)
{
    if (saved && sv.d && (unsigned)(size_t)ctx == generation)
        finish(true);
}

/* The receiver says where; the box's own callback writes the data. */
static bool box_drag_save(void *ctx, const char *path)
{
    (void)ctx;
    return sv.d && sv.save(sv.ctx, path);
}

static void start_drag(void)
{
    int box[4];
    const char *name, *leaf;

    if (!sv.d || !dlg_item_box(sv.d, SV_ICON, &box[0], &box[1], &box[2], &box[3]))
        return;
    name = dlg_get_text(sv.d, SV_NAME);
    leaf = strrchr(name, '.');                              /* only the leaf goes in the message */
    leaf = leaf ? leaf + 1 : name;
    if (strrchr(leaf, ':')) leaf = strrchr(leaf, ':') + 1;
    rosave_drag(sv.sprite, box, dlg_window(sv.d), leaf, sv.filetype, box_drag_save, box_drag_done,
                (void *)(size_t)generation);
}

static void save_click(dlg *d, int id, int buttons, void *ud)
{
    (void)d; (void)ud;
    switch (id) {
      case SV_ICON:
        if (buttons & (BTN_DRAG_SELECT | BTN_DRAG_ADJUST)) start_drag();
        break;
      case SV_SAVE:
        save_typed();
        break;
      case SV_CANCEL:
        finish(false);
        break;
    }
}

void rosave_close(void)
{
    finish(false);
}

void rosave_open(const char *title, const char *leaf, int filetype, RoSaveFn save, RoSaveDone done,
                 void *ctx)
{
    wpointer p;
    char sprite[16];
    dlg_item items[4];
    dlg *d;

    finish(false);                                          /* only one box at a time */

    snprintf(sprite, sizeof sprite, "file_%03x", filetype & 0xFFF);
    if (!wimp_sprite_exists(sprite)) snprintf(sprite, sizeof sprite, "file_xxx");

    memset(items, 0, sizeof items);
    items[0] = (dlg_item){ DI_FILE, SV_ICON, 100, 12, 72, 56, sprite, 16, 0,
                           "Drag this icon to a directory display to save the file there." };
    items[1] = (dlg_item){ DI_FIELD, SV_NAME, 16, 76, 240, 26, leaf, 200, 0,
                           "The name to save the file under. Drag the icon to a directory display, or "
                           "type the whole pathname here and click Save." };
    items[2] = (dlg_item){ DI_BUTTON, SV_CANCEL, 16, 122, 100, 30, "Cancel", 0, 0, "Close this box without saving." };
    items[3] = (dlg_item){ DI_DEFBUTTON, SV_SAVE, 156, 122, 100, 30, "Save", 0, 0,
                           "Save the file under the pathname typed above." };

    d = dlg_new(title, W_PX, H_PX, items, 4, save_click, NULL);
    if (!d) return;
    memset(&sv, 0, sizeof sv);
    sv.d = d;
    snprintf(sv.sprite, sizeof sv.sprite, "%s", sprite);
    sv.filetype = filetype;
    sv.save = save;
    sv.done = done;
    sv.ctx = ctx;
    generation++;
    dlg_set_keys(d, SV_SAVE, SV_CANCEL);
    dlg_set_help(d, "Save: drag the file icon to a directory display, or type a whole pathname and click Save.");

    if (!_swix(Wimp_GetPointerInfo, _IN(1), &p)) dlg_open_at(d, p.x, p.y);
    else dlg_open(d);
    dlg_focus(d, SV_NAME);
}
