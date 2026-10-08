/*
 * rofiles.c - the file transfer window.  See rofiles.h.
 *
 * The window is a Wimp window whose rows we paint ourselves (Wimp_PlotIcon, so the sprites and the
 * desktop font look like the Filer's): one row for each entry of the remote directory, and a first row
 * for the parent directory.  The connection and the SFTP operations are roftp.c; this file is the
 * window, the selection, the menu, and the way files move: a file dragged out is saved with the Wimp's
 * save protocol (rosave.c), files dropped in arrive as Message_DataLoad and are copied up.
 *
 * Operations run inside the event that asked for them, and call roapp_wait()/roapp_yield() while they wait,
 * so the desktop goes on; while one runs, others are refused (the SFTP client allows only one).  Files
 * dropped while one runs are queued and copied when it has finished.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#include "putty.h"
#include "storage.h"
#include "psftp.h"
#include "ro.h"
#include "rowimp.h"
#include "rodlg.h"
#include "rodialog.h"
#include "romenu.h"
#include "roapp.h"
#include "rosave.h"
#include "rosftp.h"
#include "roftp.h"
#include "rofiles.h"

/* ---- geometry, in design pixels (UI_PX OS units each, whatever the mode: see rowimp.h) ---- */
#define ROW_H        22
#define X_ICON        6
#define X_NAME       30
#define X_SIZE_R    420          /* right edge of the size column */
#define X_DATE      432
#define X_PERMS     590
#define X_END       700
#define WIN_W       706
#define WIN_H       400

enum {                           /* menu items */
    M_DOWNLOAD, M_UPLOAD, M_NEWDIR, M_RENAME, M_DELETE, M_SELALL, M_SELNONE, M_PARENT, M_HOME, M_GOTO,
    M_REFRESH, M_DOTS, M_SORT, M_DISCONNECT
};
enum { SORT_NAME, SORT_SIZE, SORT_DATE };

/* How the window shows a directory.  These are kept from one window to the next (until the program quits). */
static bool pref_hide_dots;      /* leave out the names that start with a dot */
static int  pref_sort = SORT_NAME;

typedef struct Queued {
    struct Queued *next;
    char *path;                  /* a local file or directory */
    char *target;                /* the remote directory to put it in */
    bool dir;
} Queued;

static struct {
    int         win;             /* the Wimp window, -1 if there is none */
    int         sx, sy;          /* OS units per design pixel (UI_PX) */
    char        title[256];
    Conf       *conf;
    FtConn     *conn;
    char       *cwd;             /* the remote directory shown */
    FtEntry    *all;             /* its entries as the server listed them (they own their names) */
    size_t      nall;
    FtEntry    *ent;             /* the ones shown, directories first: copies of entries of `all`, sharing their names */
    size_t      nent;
    bool       *sel;             /* selected rows */
    bool        has_parent;      /* row 0 is the parent directory */
    size_t      nrow;
    bool        busy;            /* an operation is running */
    bool        closing;         /* the person has closed the window */
    bool        in_drain;
    bool        reported;        /* the loss of the connection has been told to the person */
    long        defer_row;       /* a click on a selected file might start a drag of the whole selection: -1 for none */
    Queued     *queue, **queue_tail;
    char        status[128];     /* the last thing the connection said, while connecting */
    bool        small_sprites;   /* small_xxx and friends exist */
} fw = { .win = -1, .defer_row = -1 };

/* ------------------------------------------------------------------------ */
/* small things                                                             */

static void notice(const char *fmt, ...)
{
    va_list ap;
    char *s;
    va_start(ap, fmt);
    s = dupvprintf(fmt, ap);
    va_end(ap);
    ro_notice(&fw, "File transfer", s);
    sfree(s);
}

static void set_title(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fw.title, sizeof fw.title, fmt, ap);
    va_end(ap);
    if (fw.win >= 0)
        _swix(Wimp_ForceRedraw, _INR(0, 2), fw.win, 0x4B534154, 3);         /* the title bar */
}

static void title_for_dir(void)
{
    const char *user = fw.conf ? conf_get_str_ambi(fw.conf, CONF_username, NULL) : NULL;
    const char *host = fw.conn ? ft_host(fw.conn) : "";
    if (user && *user)
        set_title("%s@%s:%s", user, host, fw.cwd ? fw.cwd : "");
    else
        set_title("%s:%s", host, fw.cwd ? fw.cwd : "");
}

static void fmt_size(char *buf, size_t cap, uint64_t n)
{
    if (n < 1024) snprintf(buf, cap, "%u bytes", (unsigned)n);
    else if (n < 1024 * 1024) snprintf(buf, cap, "%u.%u KB", (unsigned)(n >> 10), (unsigned)((n & 1023) * 10 >> 10));
    else if (n < (uint64_t)1024 * 1024 * 1024)
        snprintf(buf, cap, "%u.%u MB", (unsigned)(n >> 20), (unsigned)(((n >> 10) & 1023) * 10 >> 10));
    else
        snprintf(buf, cap, "%u.%u GB", (unsigned)(n >> 30), (unsigned)(((n >> 20) & 1023) * 10 >> 10));
}

static void fmt_perms(char *buf, unsigned long m)
{
    static const char rwx[] = "rwxrwxrwx";
    int i;
    switch (m & 0170000) {
      case 0040000: buf[0] = 'd'; break;
      case 0120000: buf[0] = 'l'; break;
      case 0100000: buf[0] = '-'; break;
      default:      buf[0] = '?'; break;
    }
    for (i = 0; i < 9; i++)
        buf[1 + i] = (m & (0400u >> i)) ? rwx[i] : '-';
    buf[10] = '\0';
}

static void fmt_date(char *buf, size_t cap, unsigned long t)
{
    time_t tt = (time_t)t;
    struct tm *tm = localtime(&tt);
    if (!tm || strftime(buf, cap, "%d %b %Y %H:%M", tm) == 0)
        buf[0] = '\0';
}

/* The sprite for an entry: "directory" or the file's type. */
static void sprite_for(const FtEntry *e, char *out, size_t cap, bool big)
{
    if (!e) {
        snprintf(out, cap, big ? "directory" : "small_dir");
    } else if (e->dir) {
        snprintf(out, cap, big ? "directory" : "small_dir");
    } else {
        const int t = ro_filetype_for_remote_leaf(e->name);
        char name[16];
        if (t >= 0)
            snprintf(name, sizeof name, "%s%03x", big ? "file_" : "small_", t & 0xFFF);
        if (t < 0 || !wimp_sprite_exists(name))
            snprintf(name, sizeof name, big ? "file_xxx" : "small_xxx");
        snprintf(out, cap, "%s", name);
    }
}

/* ------------------------------------------------------------------------ */
/* rows                                                                     */

static const FtEntry *row_entry(size_t row)
{
    if (fw.has_parent) {
        if (row == 0) return NULL;
        row--;
    }
    return row < fw.nent ? &fw.ent[row] : NULL;
}

static bool row_is_parent(size_t row)
{
    return fw.has_parent && row == 0;
}

static int row_h(void) { return ROW_H * fw.sy; }

/* Directories first, whatever the order; then by name (ignoring case), or biggest or newest first. */
static int cmp_name(const void *a, const void *b)
{
    const FtEntry *x = (const FtEntry *)a, *y = (const FtEntry *)b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static int cmp_size(const void *a, const void *b)
{
    const FtEntry *x = (const FtEntry *)a, *y = (const FtEntry *)b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    if (!x->dir && x->size != y->size) return x->size > y->size ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static int cmp_date(const void *a, const void *b)
{
    const FtEntry *x = (const FtEntry *)a, *y = (const FtEntry *)b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    if (x->mtime != y->mtime) return x->mtime > y->mtime ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void clear_listing(void)
{
    ft_free_entries(fw.all, fw.nall);
    fw.all = NULL;
    fw.nall = 0;
    sfree(fw.ent);
    fw.ent = NULL;
    fw.nent = 0;
    sfree(fw.sel);
    fw.sel = NULL;
    fw.nrow = 0;
}

/* Work out which entries to show, and in what order; nothing is selected afterwards. */
static void rebuild_view(void)
{
    size_t i, n = 0;

    sfree(fw.ent);
    fw.ent = snewn(fw.nall ? fw.nall : 1, FtEntry);
    for (i = 0; i < fw.nall; i++)
        if (!(pref_hide_dots && fw.all[i].name[0] == '.'))
            fw.ent[n++] = fw.all[i];
    qsort(fw.ent, n, sizeof *fw.ent, pref_sort == SORT_SIZE ? cmp_size : pref_sort == SORT_DATE ? cmp_date : cmp_name);
    fw.nent = n;
    fw.nrow = n + (fw.has_parent ? 1 : 0);
    sfree(fw.sel);
    fw.sel = snewn(fw.nrow ? fw.nrow : 1, bool);
    memset(fw.sel, 0, (fw.nrow ? fw.nrow : 1) * sizeof(bool));
}

static int visible_height(void)
{
    wstate s;
    s.handle = fw.win;
    if (_swix(Wimp_GetWindowState, _IN(1), &s)) return WIN_H * fw.sy;
    return s.visible.y1 - s.visible.y0;
}

/* The work area is as tall as the rows (or the window, if that is taller). */
static void set_extent(void)
{
    wbox ext;
    int h = (int)fw.nrow * row_h();
    int vh = visible_height();
    if (h < vh) h = vh;
    ext.x0 = 0; ext.x1 = X_END * fw.sx; ext.y1 = 0; ext.y0 = -h;
    _swix(Wimp_SetExtent, _INR(0, 1), fw.win, &ext);
}

static void redraw_row(size_t row)
{
    const int h = row_h();
    if (fw.win < 0) return;
    _swix(Wimp_ForceRedraw, _INR(0, 4), fw.win, 0, -(int)(row + 1) * h, X_END * fw.sx, -(int)row * h);
}

static void redraw_all(void)
{
    if (fw.win >= 0)
        _swix(Wimp_ForceRedraw, _INR(0, 4), fw.win, 0, -0x1000000, 0x1000000, 0);
}

static size_t count_selected(void)
{
    size_t i, n = 0;
    for (i = 0; i < fw.nrow; i++)
        if (fw.sel && fw.sel[i] && !row_is_parent(i)) n++;
    return n;
}

static void select_only(size_t row, bool on)
{
    size_t i;
    for (i = 0; i < fw.nrow; i++) {
        const bool want = (i == row) && on;
        if (fw.sel[i] != want) {
            fw.sel[i] = want;
            redraw_row(i);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* drawing                                                                  */

static void plot_text(int x0, int y0, int x1, int y1, const char *text, bool right, bool selected)
{
    idef ic;
    char buf[256];

    snprintf(buf, sizeof buf, "%s", text);
    memset(&ic, 0, sizeof ic);
    ic.box.x0 = x0; ic.box.y0 = y0; ic.box.x1 = x1; ic.box.y1 = y1;
    ic.flags = IF_TEXT | IF_INDIRECTED | IF_VCENTRED | IF_FG(7) | IF_BG(1) |
               (right ? IF_RJUSTIFIED : 0) | (selected ? (IF_SELECTED | IF_FILLED) : 0);
    ic.data.ind.buf = buf;
    ic.data.ind.validation = (char *)-1;
    ic.data.ind.size = (int)sizeof buf;
    _swix(Wimp_PlotIcon, _IN(1), &ic);
}

static void plot_sprite(int x0, int y0, int x1, int y1, const char *name, bool selected)
{
    idef ic;
    memset(&ic, 0, sizeof ic);
    ic.box.x0 = x0; ic.box.y0 = y0; ic.box.x1 = x1; ic.box.y1 = y1;
    ic.flags = IF_SPRITE | IF_HCENTRED | IF_VCENTRED | IF_FG(7) | IF_BG(1) | (selected ? IF_SELECTED : 0);
    snprintf(ic.data.text, sizeof ic.data.text, "%s", name);
    _swix(Wimp_PlotIcon, _IN(1), &ic);
}

/*
 * Wimp_PlotIcon takes its coordinates from the origin of the work area of the window being redrawn,
 * not from the screen's, so a row is placed by its distance below the top of the work area.
 */
static void draw_row(size_t row)
{
    const int h = row_h();
    const int top = -(int)row * h, bot = top - h;
    const FtEntry *e = row_entry(row);
    const bool sel = fw.sel && fw.sel[row];
    char sprite[16], text[64];

    sprite_for(e, sprite, sizeof sprite, !fw.small_sprites);
    plot_sprite(X_ICON * fw.sx, bot, (X_ICON + 20) * fw.sx, top, sprite, sel);
    plot_text(X_NAME * fw.sx, bot, (X_SIZE_R - 90) * fw.sx, top,
              row_is_parent(row) ? "Parent directory" : e ? e->name : "", false, sel);
    if (!e) return;
    if (!e->dir && e->has_size) {
        fmt_size(text, sizeof text, e->size);
        plot_text((X_SIZE_R - 90) * fw.sx, bot, X_SIZE_R * fw.sx, top, text, true, false);
    }
    if (e->has_mtime) {
        fmt_date(text, sizeof text, e->mtime);
        plot_text(X_DATE * fw.sx, bot, (X_PERMS - 4) * fw.sx, top, text, false, false);
    }
    if (e->has_perms) {
        fmt_perms(text, e->perms);
        plot_text(X_PERMS * fw.sx, bot, X_END * fw.sx, top, text, false, false);
    }
}

int rofiles_event_redraw(wevent *e)
{
    wdraw *r = (wdraw *)e->words;
    int more = 0;

    if (fw.win < 0 || e->words[0] != fw.win) return 0;
    if (_swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), r, &more)) return 1;
    while (more) {
        const int oy = r->visible.y1 - r->scroll_y, h = row_h();
        long first = (oy - r->clip.y1) / h, last = (oy - r->clip.y0) / h, i;
        if (first < 0) first = 0;
        for (i = first; i <= last && i < (long)fw.nrow; i++)
            draw_row((size_t)i);
        if (_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), r, &more)) break;
    }
    return 1;
}

/* ------------------------------------------------------------------------ */
/* the progress box                                                         */

enum { P_NAME = 1, P_INFO, P_BAR, P_CANCEL };

static struct {
    dlg      *d;
    bool      cancel;
    uint32_t  last;
} pd;

static void pd_click(dlg *d, int id, int buttons, void *ud)
{
    (void)d; (void)buttons; (void)ud;
    if (id == P_CANCEL)
        pd.cancel = true;
}

static void pd_open(const char *title, const char *what)
{
    static const dlg_item items[] = {
        { DI_LABEL,  P_NAME,    16, 12, 368, 22, "", 96, 0, NULL },
        { DI_LABEL,  P_INFO,    16, 38, 368, 22, "", 96, 0, NULL },
        { DI_BAR,    P_BAR,     16, 66, 368, 18, "", 0, 0, "How much of the file has been copied." },
        { DI_BUTTON, P_CANCEL, 288, 98, 96, 30, "Cancel", 0, 0, "Stop copying." },
    };
    if (pd.d) return;
    pd.d = dlg_new(title, 400, 142, items, 4, pd_click, NULL);
    pd.cancel = false;
    pd.last = 0;
    if (!pd.d) return;
    dlg_set_keys(pd.d, -1, P_CANCEL);
    dlg_set_text(pd.d, P_NAME, what);
    dlg_open(pd.d);
}

static void pd_close(void)
{
    if (pd.d) {
        dlg_free(pd.d);
        pd.d = NULL;
    }
}

/* FtProgress callback; false to cancel */
static bool pd_update(void *ctx, uint64_t done, uint64_t total)
{
    const uint32_t now = ro_time();
    (void)ctx;
    if (pd.d && (int32_t)(now - pd.last) >= 4) {
        char a[32], b[32], text[96];
        pd.last = now;
        fmt_size(a, sizeof a, done);
        if (total > 0) {
            fmt_size(b, sizeof b, total);
            snprintf(text, sizeof text, "%s of %s", a, b);
            dlg_set_bar(pd.d, P_BAR, (int)(done * 1000 / total));
        } else {
            snprintf(text, sizeof text, "%s", a);
        }
        dlg_set_text(pd.d, P_INFO, text);
    }
    return !pd.cancel && !fw.closing;
}

static const FtProgress progress = { pd_update, NULL };

/* ------------------------------------------------------------------------ */
/* asking before a file is replaced                                         */

enum { OW_REPLACE, OW_SKIP, OW_CANCEL };

static struct {
    bool all;                    /* "Replace all" has been chosen for the copying in hand */
    int  answer;                 /* the button chosen; -1 while the question is up */
} ow;

static void ow_answered(void *ctx, int button)
{
    (void)ctx;
    ow.answer = button;
}

/* "3.4 KB, 2 Oct 2026 10:00" - what is known of a file, for the question */
static void describe(char *buf, size_t cap, const FtEntry *e)
{
    char size[32] = "", date[48] = "";

    if (e && e->has_size) fmt_size(size, sizeof size, e->size);
    if (e && e->has_mtime) fmt_date(date, sizeof date, e->mtime);
    snprintf(buf, cap, "%s%s%s", size, size[0] && date[0] ? ", " : "", date);
}

/*
 * The file `leaf` exists already, on the other computer (`remote`) or here: may it be replaced?  `old` is
 * what is there and `incoming` what would take its place.  When more files are to come (`several`) the person
 * may also say "Replace all" (no more questions this time) or "Skip" this one.  This is asked inside an
 * operation, which keeps any other out meanwhile; the desktop goes on while the box is up.
 */
static int ask_replace(const char *leaf, bool remote, const FtEntry *old, const FtEntry *incoming, bool several)
{
    static const char *const one[] = { "Replace", "Cancel" };
    static const char *const many[] = { "Replace", "Replace all", "Skip", "Cancel" };
    char a[96], b[96], *msg;
    const int last = several ? 3 : 1;
    int answer;

    if (ow.all) return OW_REPLACE;
    describe(a, sizeof a, old);
    describe(b, sizeof b, incoming);
    msg = dupprintf("The file %s already exists%s.\n%s%s%s%s%s%s\nReplace it?", leaf,
                    remote ? " on the other computer" : "",
                    a[0] ? "\nThe old one: " : "", a, a[0] ? "\n" : "",
                    b[0] ? "The new one: " : "", b, b[0] ? "\n" : "");
    ow.answer = -1;
    ro_msgbox(&fw, "File exists", msg, several ? many : one, last + 1, last, last, ow_answered, NULL);
    sfree(msg);
    while (ow.answer < 0 && !fw.closing && roapp_wait())
        ;
    if (ow.answer < 0) {                                   /* the window or the program is going */
        ro_msgbox_drop_owner(&fw);
        return OW_CANCEL;
    }
    answer = ow.answer;
    if (answer == last) return OW_CANCEL;
    if (several && answer == 2) return OW_SKIP;
    if (several && answer == 1) ow.all = true;
    return OW_REPLACE;
}

/* May the local file `path` be written?  (It is a question if there is a file there.) */
static int confirm_local(const char *path, const FtEntry *incoming, bool several)
{
    unsigned long size = 0, mtime = 0;
    FtEntry old;

    if (ro_local_stat(path, NULL, &size, &mtime) != 1) return OW_REPLACE;     /* (a directory fails later) */
    memset(&old, 0, sizeof old);
    old.has_size = old.has_mtime = true;
    old.size = size;
    old.mtime = mtime;
    return ask_replace(ro_local_leaf(path), false, &old, incoming, several);
}

/* ------------------------------------------------------------------------ */
/* operations                                                               */

static void finish_close(void);
static void drain_queue(void);

static bool op_begin(void)
{
    if (fw.win < 0 || fw.closing) return false;
    if (fw.busy || !roapp_op_begin()) {
        ro_notice(&fw, "File transfer", "Please wait: the computer is still busy with the last request.");
        return false;
    }
    if (fw.conn && !ft_alive(fw.conn)) {
        roapp_op_end();
        notice("The connection has been lost (%s)\nClose this window and connect again.", ft_why_dead(fw.conn));
        return false;
    }
    fw.busy = true;
    if (!fw.in_drain) ow.all = false;                      /* (a run of dropped files is one lot of copying) */
    return true;
}

static void op_end(void)
{
    fw.busy = false;
    roapp_op_end();
    if (fw.closing) {
        finish_close();
        return;
    }
    if (fw.queue && !fw.in_drain)
        drain_queue();
}

/* Make the window show the directory `path`'s listing. */
static bool install_listing(const char *path, FtEntry *list, size_t n)
{
    char *where = dupstr(path);                 /* (path may be fw.cwd, which is about to be replaced) */
    clear_listing();
    fw.all = list;
    fw.nall = n;
    sfree(fw.cwd);
    fw.cwd = where;
    fw.has_parent = strcmp(path, "/") != 0;
    rebuild_view();
    return true;
}

/* Read the directory `path` and show it. */
static bool load_dir(const char *path)
{
    FtEntry *list;
    size_t n;
    char *err = NULL;
    bool ok;
    wstate s;

    if (!op_begin()) return false;
    set_title("%s - reading %s...", ft_host(fw.conn), path);
    ok = ft_list(fw.conn, path, &list, &n, &err);
    if (!ok) {
        title_for_dir();
        op_end();
        if (fw.win >= 0 && !fw.closing)
            notice("%s", err);
        sfree(err);
        return false;
    }
    install_listing(path, list, n);
    title_for_dir();
    set_extent();
    s.handle = fw.win;
    if (!_swix(Wimp_GetWindowState, _IN(1), &s)) {
        s.scroll_y = 0;                                  /* back to the top */
        s.behind = -1;
        _swix(Wimp_OpenWindow, _IN(1), &s);
    }
    redraw_all();
    op_end();
    return true;
}

/* The hiding of dot files, or the order, has changed: show the directory again, at the top. */
static void view_changed(void)
{
    wstate st;

    if (fw.win < 0 || !fw.all) return;
    rebuild_view();
    set_extent();
    st.handle = fw.win;
    if (!_swix(Wimp_GetWindowState, _IN(1), &st)) {
        st.scroll_y = 0;
        st.behind = -1;
        _swix(Wimp_OpenWindow, _IN(1), &st);
    }
    redraw_all();
}

static void go_to(const char *where)
{
    char *err = NULL, *canon;

    if (!op_begin()) return;
    canon = ft_realpath(fw.conn, fw.cwd, where, &err);
    op_end();
    if (!canon) {
        if (fw.win >= 0 && !fw.closing) notice("%s", err);
        sfree(err);
        return;
    }
    load_dir(canon);
    sfree(canon);
}

static void go_into(const FtEntry *e)
{
    char *p = ft_join(fw.cwd, e->name);
    char *err = NULL, *canon;

    if (!op_begin()) { sfree(p); return; }
    canon = ft_realpath(fw.conn, NULL, p, &err);              /* (a link to a directory goes where it points) */
    op_end();
    sfree(p);
    if (!canon) {
        if (fw.win >= 0 && !fw.closing) notice("%s", err);
        sfree(err);
        return;
    }
    load_dir(canon);
    sfree(canon);
}

static void go_parent(void)
{
    if (fw.has_parent)
        go_to("..");
}

/* ---- downloads ---- */

typedef struct DlJob {
    struct DlJob *next;          /* the next file of a drag of several */
    char *remote;
    char *leaf;                  /* the name offered to whoever the file is dropped on */
    int type;                    /* the filetype to give the file, -1 to let the contents decide */
    int offer_type;              /* ... and the one offered with the name (never -1) */
} DlJob;

/* (The save box and the drag to a directory display have asked already, if there is a file at `local`.) */
static bool download_one(const char *remote, const char *local, int type, const char *shown)
{
    char *err = NULL;
    bool ok;

    if (!op_begin()) return false;
    pd_open("Copying a file", shown);
    ok = ft_download(fw.conn, remote, local, type, &progress, &err);
    pd_close();
    op_end();
    if (!ok && fw.win >= 0) {
        if (err && strcmp(err, "Cancelled.")) notice("%s", err);
        sfree(err);
    }
    return ok;
}

static bool dl_save(void *ctx, const char *path)
{
    DlJob *j = (DlJob *)ctx;
    return download_one(j->remote, path, j->type, ro_local_leaf(path));
}

static void free_dl_jobs(DlJob *j)
{
    while (j) {
        DlJob *n = j->next;
        sfree(j->remote);
        sfree(j->leaf);
        sfree(j);
        j = n;
    }
}

/* One file has been saved (or the drag came to nothing): go on to the next of a drag of several, if any. */
static void dl_done(void *ctx, bool saved)
{
    DlJob *j = (DlJob *)ctx, *next = j->next;

    j->next = NULL;
    free_dl_jobs(j);
    if (!next)
        return;
    if (!saved || !rosave_drop_another(next->leaf, next->offer_type, dl_save, dl_done, next))
        free_dl_jobs(next);
}

/* A job to fetch a remote file, with the leaf and type to offer for it. */
static DlJob *new_dl_job(const FtEntry *e)
{
    DlJob *j = snew(DlJob);
    const int t = ro_filetype_for_remote_leaf(e->name);

    memset(j, 0, sizeof *j);
    j->remote = ft_join(fw.cwd, e->name);
    j->type = t;
    j->leaf = ro_remote_leaf_to_save_leaf(e->name);
    j->offer_type = t >= 0 ? t : FILETYPE_DATA;
    return j;
}

static void download_with_box(const FtEntry *e)
{
    DlJob *j;

    if (!e || e->dir) return;
    j = new_dl_job(e);
    rosave_open("Save file", j->leaf, j->offer_type, dl_save, dl_done, j);
}

/* ---- trees ---- */

/* Copy the remote directory `remote` into the local directory `local` (made if need be).  Symbolic links to
 * directories inside it are left alone: a link back up the tree would never end. */
static bool download_tree(const char *remote, const char *local, int *count)
{
    FtEntry *list;
    size_t n, i;
    char *err = NULL;
    bool ok = true;

    if (file_type(local) != FILE_TYPE_DIRECTORY && !create_directory(local)) {
        notice("%s: unable to make the directory", local);
        return false;
    }
    if (!ft_list(fw.conn, remote, &list, &n, &err)) {
        notice("%s", err);
        sfree(err);
        return false;
    }
    for (i = 0; i < n && ok && !pd.cancel && !fw.closing; i++) {
        char *r = ft_join(remote, list[i].name);
        char *l = dir_file_cat(local, list[i].name);
        char shown[96];
        if (list[i].dir && list[i].link) {
            ok = true;                                 /* skipped */
        } else if (list[i].dir) {
            ok = download_tree(r, l, count);
        } else {
            const int again = confirm_local(l, &list[i], true);
            snprintf(shown, sizeof shown, "%.80s", list[i].name);
            if (pd.d) dlg_set_text(pd.d, P_NAME, shown);
            if (again == OW_CANCEL) {
                pd.cancel = true;
                ok = false;
            } else if (again == OW_REPLACE) {
                err = NULL;
                ok = ft_download(fw.conn, r, l, -1, &progress, &err);
                if (!ok) {
                    if (err && strcmp(err, "Cancelled.")) notice("%s", err);
                    sfree(err);
                }
                (*count)++;
            }
        }
        sfree(r);
        sfree(l);
    }
    ft_free_entries(list, n);
    return ok;
}

static void download_selection_to(const char *dir)
{
    size_t row;
    int count = 0;

    if (!op_begin()) return;
    pd_open("Copying files", "");
    for (row = 0; row < fw.nrow && !pd.cancel && !fw.closing; row++) {
        const FtEntry *e;
        char *r, *l, *err = NULL;
        if (!fw.sel[row] || (e = row_entry(row)) == NULL) continue;
        r = ft_join(fw.cwd, e->name);
        l = dir_file_cat(dir, e->name);
        if (e->dir) {                                  /* (a link to a directory, chosen by name, is followed) */
            if (!download_tree(r, l, &count)) { sfree(r); sfree(l); break; }
        } else {
            const int again = confirm_local(l, e, true);
            if (pd.d) dlg_set_text(pd.d, P_NAME, e->name);
            if (again == OW_CANCEL) {
                pd.cancel = true;
                sfree(r); sfree(l);
                break;
            }
            if (again == OW_REPLACE) {
                if (!ft_download(fw.conn, r, l, -1, &progress, &err)) {
                    if (err && strcmp(err, "Cancelled.")) notice("%s", err);
                    sfree(err);
                    sfree(r); sfree(l);
                    break;
                }
                count++;
            }
        }
        sfree(r);
        sfree(l);
    }
    pd_close();
    op_end();
}

static void dir_answered(void *ctx, const char *answer)
{
    (void)ctx;
    if (answer && *answer && fw.win >= 0)
        download_selection_to(answer);
}

static void do_download(void)
{
    size_t row, n = count_selected();
    const FtEntry *only = NULL;

    for (row = 0; row < fw.nrow; row++)
        if (fw.sel[row] && !row_is_parent(row))
            only = row_entry(row);
    if (n == 0) return;
    if (n == 1 && only && !only->dir) {
        download_with_box(only);
        return;
    }
    {
        char *msg = dupprintf("Type the pathname of the directory to copy the %u selected item%s into.",
                              (unsigned)n, n == 1 ? "" : "s");
        ro_prompt(&fw, "Copy to a directory", msg, false, "", dir_answered, NULL);
        sfree(msg);
    }
}

/* ---- uploads ---- */

static void enqueue_upload(const char *path, bool dir, const char *target)
{
    Queued *q = snew(Queued);
    q->next = NULL;
    q->path = dupstr(path);
    q->target = dupstr(target);
    q->dir = dir;
    if (!fw.queue_tail) fw.queue_tail = &fw.queue;
    *fw.queue_tail = q;
    fw.queue_tail = &q->next;
}

static char *remote_leaf_for_dir(const char *local)
{
    return ro_local_leaf_to_remote_leaf(ro_local_leaf(local), -1);
}

/* Copy a local file or directory up into the remote directory `target`.  False if it should stop. */
static bool upload_item(const char *local, bool dir, const char *target, int *count)
{
    char *err = NULL;

    if (dir) {
        char *leaf = remote_leaf_for_dir(local);
        char *rdir = ft_join(target, leaf);
        DirHandle *dh;
        const char *derr;
        char *name, **names = NULL;
        size_t nnames = 0, cap = 0, i;
        bool ok = true;
        FtEntry st;

        sfree(leaf);
        if (!ft_stat(fw.conn, rdir, &st) || !st.dir) {
            if (!ft_mkdir(fw.conn, rdir, &err)) {
                notice("%s", err);
                sfree(err);
                sfree(rdir);
                return false;
            }
        }
        dh = open_directory(local, &derr);
        if (!dh) {
            notice("%s: %s", local, derr);
            sfree(rdir);
            return false;
        }
        while ((name = read_filename(dh)) != NULL) {
            sgrowarray(names, cap, nnames);
            names[nnames++] = name;
        }
        close_directory(dh);
        for (i = 0; i < nnames && ok && !pd.cancel && !fw.closing; i++) {
            char *l = dir_file_cat(local, names[i]);
            bool sub = file_type(l) == FILE_TYPE_DIRECTORY;
            ok = upload_item(l, sub, rdir, count);
            sfree(l);
        }
        for (i = 0; i < nnames; i++) sfree(names[i]);
        sfree(names);
        sfree(rdir);
        return ok;
    } else {
        char *leaf = dupstr(ro_local_path_to_remote_leaf(local));
        char *rpath = ft_join(target, leaf);
        bool ok;
        FtEntry old;

        if (pd.d) dlg_set_text(pd.d, P_NAME, ro_local_leaf(local));
        if (ft_stat(fw.conn, rpath, &old) && !old.dir) {          /* there is a file there: replace it? */
            unsigned long size = 0, mtime = 0;
            FtEntry incoming;
            int again;

            memset(&incoming, 0, sizeof incoming);
            if (ro_local_stat(local, NULL, &size, &mtime) == 1) {
                incoming.has_size = incoming.has_mtime = true;
                incoming.size = size;
                incoming.mtime = mtime;
            }
            again = ask_replace(leaf, true, &old, &incoming, true);
            if (again != OW_REPLACE) {
                sfree(leaf);
                sfree(rpath);
                if (again == OW_CANCEL) pd.cancel = true;
                return again == OW_SKIP;
            }
        }
        ok = ft_upload(fw.conn, local, rpath, &progress, &err);
        sfree(leaf);
        sfree(rpath);
        if (!ok) {
            if (err && strcmp(err, "Cancelled.")) notice("%s", err);
            sfree(err);
            return false;
        }
        (*count)++;
        return true;
    }
}

static void drain_queue(void)
{
    int count = 0;

    fw.in_drain = true;
    ow.all = false;
    while (fw.queue && !fw.closing && fw.win >= 0) {
        Queued *q = fw.queue;
        bool ok;

        fw.queue = q->next;
        if (!fw.queue) fw.queue_tail = &fw.queue;
        if (!op_begin()) {
            sfree(q->path); sfree(q->target); sfree(q);
            break;
        }
        pd_open("Copying files", "");
        ok = upload_item(q->path, q->dir, q->target, &count);
        pd_close();
        fw.busy = false;                               /* (op_end without its drain) */
        roapp_op_end();
        sfree(q->path); sfree(q->target); sfree(q);
        if (!ok || pd.cancel) {
            while (fw.queue) {                         /* give up on the rest */
                Queued *n = fw.queue->next;
                sfree(fw.queue->path); sfree(fw.queue->target); sfree(fw.queue);
                fw.queue = n;
            }
            fw.queue_tail = &fw.queue;
        }
    }
    fw.in_drain = false;
    if (fw.closing) {
        finish_close();
        return;
    }
    if (count > 0 && fw.win >= 0)
        load_dir(fw.cwd);
}

static void upload_answered(void *ctx, const char *answer)
{
    int type;
    (void)ctx;
    if (!answer || !*answer || fw.win < 0) return;
    type = ro_local_stat(answer, NULL, NULL, NULL);
    if (type == 0) {
        notice("%s does not exist.", answer);
        return;
    }
    enqueue_upload(answer, type == 2, fw.cwd);
    if (!fw.busy) drain_queue();
}

/* ---- the rest: new directory, rename, delete ---- */

static void newdir_answered(void *ctx, const char *answer)
{
    char *p, *err = NULL;
    bool ok;
    (void)ctx;
    if (!answer || !*answer || fw.win < 0) return;
    if (strchr(answer, '/')) { notice("A name cannot contain a '/'."); return; }
    p = ft_join(fw.cwd, answer);
    if (!op_begin()) { sfree(p); return; }
    ok = ft_mkdir(fw.conn, p, &err);
    op_end();
    sfree(p);
    if (!ok) {
        if (fw.win >= 0) notice("%s", err);
        sfree(err);
    } else if (fw.win >= 0) {
        load_dir(fw.cwd);
    }
}

static char *rename_from;

static void rename_answered(void *ctx, const char *answer)
{
    char *from = rename_from, *to, *err = NULL;
    bool ok;
    (void)ctx;
    rename_from = NULL;
    if (!from) return;
    if (!answer || !*answer || fw.win < 0 || !strcmp(answer, from)) { sfree(from); return; }
    if (strchr(answer, '/')) { notice("A name cannot contain a '/'."); sfree(from); return; }
    {
        char *a = ft_join(fw.cwd, from);
        to = ft_join(fw.cwd, answer);
        if (!op_begin()) { sfree(a); sfree(to); sfree(from); return; }
        ok = ft_rename(fw.conn, a, to, &err);
        op_end();
        sfree(a);
        sfree(to);
    }
    sfree(from);
    if (!ok) {
        if (fw.win >= 0) notice("%s", err);
        sfree(err);
    } else if (fw.win >= 0) {
        load_dir(fw.cwd);
    }
}

static void do_rename(void)
{
    size_t row;
    const FtEntry *only = NULL;
    for (row = 0; row < fw.nrow; row++)
        if (fw.sel[row] && !row_is_parent(row))
            only = row_entry(row);
    if (!only || count_selected() != 1) return;
    sfree(rename_from);
    rename_from = dupstr(only->name);
    ro_prompt(&fw, "Rename", "Type the new name.", false, only->name, rename_answered, NULL);
}

/* Remove a remote file or directory and everything in it.  A link is only unlinked. */
static bool delete_tree(const char *path, bool dir, bool link, char **err)
{
    if (dir && !link) {
        FtEntry *list;
        size_t n, i;
        bool ok = true;
        if (!ft_list(fw.conn, path, &list, &n, err))
            return false;
        for (i = 0; i < n && ok; i++) {
            char *p = ft_join(path, list[i].name);
            ok = delete_tree(p, list[i].dir, list[i].link, err);
            sfree(p);
            if (roapp_yield()) ok = false;
        }
        ft_free_entries(list, n);
        return ok && ft_remove(fw.conn, path, true, err);
    }
    return ft_remove(fw.conn, path, false, err);
}

static void delete_confirmed(void *ctx, int button)
{
    size_t row;
    char *err = NULL;
    (void)ctx;
    if (button != 0 || fw.win < 0 || !op_begin()) return;
    for (row = 0; row < fw.nrow; row++) {
        const FtEntry *e;
        char *p;
        bool ok;
        if (!fw.sel[row] || (e = row_entry(row)) == NULL) continue;
        p = ft_join(fw.cwd, e->name);
        ok = delete_tree(p, e->dir, e->link, &err);
        sfree(p);
        if (!ok) {
            if (fw.win >= 0 && !fw.closing) notice("%s", err);
            sfree(err);
            break;
        }
    }
    op_end();
    if (fw.win >= 0) load_dir(fw.cwd);
}

static void do_delete(void)
{
    static const char *const buttons[] = { "Delete", "Cancel" };
    size_t n = count_selected();
    char *msg;
    if (n == 0) return;
    msg = dupprintf("Delete the %u selected item%s from the other computer?\n"
                    "(Directories are deleted with everything in them.)", (unsigned)n, n == 1 ? "" : "s");
    ro_msgbox(&fw, "Delete", msg, buttons, 2, 1, 1, delete_confirmed, NULL);
    sfree(msg);
}

/* ------------------------------------------------------------------------ */
/* the menu                                                                 */

static void goto_answered(void *ctx, const char *answer)
{
    (void)ctx;
    if (answer && *answer && fw.win >= 0)
        go_to(answer);
}

static void menu_selected(const int *sel)
{
    size_t i;

    if (fw.win < 0) return;
    switch (sel[0]) {
      case M_DOWNLOAD: do_download(); break;
      case M_UPLOAD:
        ro_prompt(&fw, "Copy to the other computer",
                  "Type the pathname of the file or directory to copy to this directory.\n"
                  "(Or drag files from a directory display onto this window.)", false, "", upload_answered, NULL);
        break;
      case M_NEWDIR:
        ro_prompt(&fw, "New directory", "Type the name of the new directory.", false, "", newdir_answered, NULL);
        break;
      case M_RENAME: do_rename(); break;
      case M_DELETE: do_delete(); break;
      case M_SELALL:
        for (i = 0; i < fw.nrow; i++)
            if (!row_is_parent(i) && !fw.sel[i]) { fw.sel[i] = true; redraw_row(i); }
        break;
      case M_SELNONE:
        for (i = 0; i < fw.nrow; i++)
            if (fw.sel[i]) { fw.sel[i] = false; redraw_row(i); }
        break;
      case M_PARENT: go_parent(); break;
      case M_HOME: if (fw.conn) go_to(ft_home(fw.conn)); break;
      case M_GOTO:
        ro_prompt(&fw, "Go to", "Type the pathname of the directory to go to.", false, fw.cwd ? fw.cwd : "/",
                  goto_answered, NULL);
        break;
      case M_REFRESH: if (fw.cwd) load_dir(fw.cwd); break;
      case M_DOTS:
        pref_hide_dots = !pref_hide_dots;
        view_changed();
        break;
      case M_SORT:
        if (sel[1] >= SORT_NAME && sel[1] <= SORT_DATE && sel[1] != pref_sort) {
            pref_sort = sel[1];
            view_changed();
        }
        break;
      case M_DISCONNECT: rofiles_shutdown(); break;
    }
}

/* The menu is made once and never freed: the Wimp may still be showing it (Adjust keeps a menu open)
 * when the window it belongs to goes. */
static menu *files_menu, *sort_menu;

static void open_menu(int x, int y)
{
    const size_t nsel = count_selected();
    size_t row;
    bool one_file = false;

    if (!files_menu) {
        files_menu = menu_new("Files");                       /* (a menu title is at most 12 characters) */
        if (!files_menu) return;
        menu_add(files_menu, "Save file...", 0);
        menu_add(files_menu, "Copy to here...", 0);
        menu_add(files_menu, "New directory...", 0);
        menu_add(files_menu, "Rename...", 0);
        menu_add(files_menu, "Delete", MF_DOTTED);
        menu_add(files_menu, "Select all", 0);
        menu_add(files_menu, "Clear selection", MF_DOTTED);
        menu_add(files_menu, "Parent directory", 0);
        menu_add(files_menu, "Home directory", 0);
        menu_add(files_menu, "Go to...", 0);
        menu_add(files_menu, "Refresh", MF_DOTTED);
        menu_add(files_menu, "Hide dot files", 0);
        menu_add(files_menu, "Sort by", MF_DOTTED);
        menu_add(files_menu, "Disconnect", 0);
        sort_menu = menu_new("Sort by");
        if (sort_menu) {
            menu_add(sort_menu, "Name", 0);
            menu_add(sort_menu, "Size", 0);
            menu_add(sort_menu, "Date", 0);
            menu_set_sub(files_menu, M_SORT, sort_menu);
        }
    }
    for (row = 0; row < fw.nrow; row++)
        if (fw.sel[row] && !row_is_parent(row) && row_entry(row) && !row_entry(row)->dir && nsel == 1)
            one_file = true;
    menu_shade(files_menu, M_DOWNLOAD, nsel == 0);
    menu_set_text(files_menu, M_DOWNLOAD, nsel > 1 || (nsel == 1 && !one_file) ? "Copy to..." : "Save file...");
    menu_shade(files_menu, M_RENAME, nsel != 1);
    menu_shade(files_menu, M_DELETE, nsel == 0);
    menu_shade(files_menu, M_SELNONE, nsel == 0);
    menu_shade(files_menu, M_PARENT, !fw.has_parent);
    menu_shade(files_menu, M_NEWDIR, !fw.conn);
    menu_set_text(files_menu, M_DOTS, pref_hide_dots ? "Show dot files" : "Hide dot files");
    if (sort_menu) {
        menu_tick(sort_menu, SORT_NAME, pref_sort == SORT_NAME);
        menu_tick(sort_menu, SORT_SIZE, pref_sort == SORT_SIZE);
        menu_tick(sort_menu, SORT_DATE, pref_sort == SORT_DATE);
    }
    roapp_menu_opened(menu_selected);
    menu_open(files_menu, x - 64, y);
}

/* ------------------------------------------------------------------------ */
/* events                                                                   */

/* Which row is at screen y? -1 for none. */
static long row_at(int y)
{
    wstate s;
    long row;
    s.handle = fw.win;
    if (_swix(Wimp_GetWindowState, _IN(1), &s)) return -1;
    row = (s.visible.y1 - s.scroll_y) - y;
    if (row < 0) return -1;
    row /= row_h();
    return row < (long)fw.nrow ? row : -1;
}

/* Drag the file in `row` out of the window.  If it is one of several selected, they all go, one after
 * the other, to wherever it is dropped (directories cannot be dragged: the menu's Copy to... does those). */
static void start_drag(size_t row)
{
    const FtEntry *e = row_entry(row);
    wstate s;
    int box[4];
    char sprite[16];
    DlJob *j, **tail;
    size_t i;

    if (!e || e->dir) return;
    s.handle = fw.win;
    if (_swix(Wimp_GetWindowState, _IN(1), &s)) return;
    box[0] = s.visible.x0 - s.scroll_x + X_ICON * fw.sx;
    box[2] = box[0] + 20 * fw.sx;
    box[3] = s.visible.y1 - s.scroll_y - (int)row * row_h();
    box[1] = box[3] - row_h();
    sprite_for(e, sprite, sizeof sprite, true);
    j = new_dl_job(e);
    tail = &j->next;
    for (i = 0; i < fw.nrow; i++) {
        const FtEntry *o = row_entry(i);
        if (i != row && fw.sel[i] && o && !o->dir) {
            *tail = new_dl_job(o);
            tail = &(*tail)->next;
        }
    }
    if (!rosave_drag(sprite, box, fw.win, j->leaf, j->offer_type, dl_save, dl_done, j))
        free_dl_jobs(j);
}

int rofiles_event_click(wevent *e)
{
    wpointer *p = &e->mouse;
    long row;
    unsigned b = p->buttons;

    if (fw.win < 0 || p->window != fw.win) return 0;
    row = row_at(p->y);
    if (b & (BTN_CLICK_MENU | BTN_MENU)) {
        open_menu(p->x, p->y);
        return 1;
    }
    if (b & BTN_SELECT) {                             /* a double click: open it */
        fw.defer_row = -1;
        if (row >= 0) {
            const FtEntry *e = row_entry((size_t)row);
            if (row_is_parent((size_t)row)) go_parent();
            else if (e && e->dir) go_into(e);
            else download_with_box(e);
        }
        return 1;
    }
    if (b & BTN_DRAG_SELECT) {                        /* a file dragged out of the window */
        fw.defer_row = -1;                            /* (the selection stays whole) */
        if (row >= 0 && !row_is_parent((size_t)row)) {
            if (!fw.sel[row]) select_only((size_t)row, true);
            start_drag((size_t)row);
        }
        return 1;
    }
    if (b & BTN_CLICK_SELECT) {                       /* a click selects that file alone, or none */
        if (row >= 0 && !row_is_parent((size_t)row)) {
            if (fw.sel[row] && count_selected() > 1)
                fw.defer_row = row;                   /* maybe a drag of all of them: wait to see (rofiles_idle) */
            else
                select_only((size_t)row, true);
        } else {
            select_only((size_t)-1, false);
        }
        return 1;
    }
    if (b & BTN_CLICK_ADJUST) {                       /* Adjust adds a file to the selection or takes it out */
        if (row >= 0 && !row_is_parent((size_t)row)) {
            fw.sel[row] = !fw.sel[row];
            redraw_row((size_t)row);
        }
        return 1;
    }
    return 1;
}

int rofiles_event_open(wevent *e)
{
    wstate *s = &e->open;
    wbox ext;
    int need;

    if (fw.win < 0 || e->words[0] != fw.win) return 0;
    /* a window taller than its rows needs a work area as tall as itself */
    need = (int)fw.nrow * row_h();
    if (need < s->visible.y1 - s->visible.y0) need = s->visible.y1 - s->visible.y0;
    ext.x0 = 0; ext.x1 = X_END * fw.sx; ext.y1 = 0; ext.y0 = -need;
    _swix(Wimp_SetExtent, _INR(0, 1), fw.win, &ext);
    _swix(Wimp_OpenWindow, _IN(1), e);
    return 1;
}

int rofiles_event_scroll(wevent *e)
{
    wstate s;
    const int xdir = e->words[8], dir = e->words[9];            /* (+32 and +36: see roterm_scroll_request) */
    int visible_h, visible_w;

    if (fw.win < 0 || e->words[0] != fw.win) return 0;
    memcpy(&s, e->words, sizeof s);
    visible_h = s.visible.y1 - s.visible.y0;
    visible_w = s.visible.x1 - s.visible.x0;
    if (dir == 1) s.scroll_y += 3 * row_h();
    else if (dir == -1) s.scroll_y -= 3 * row_h();
    else if (dir == 2) s.scroll_y += visible_h - row_h();
    else if (dir == -2) s.scroll_y -= visible_h - row_h();
    if (xdir == 1) s.scroll_x += 40 * fw.sx;                /* (the columns: a window narrower than they are, on a small screen) */
    else if (xdir == -1) s.scroll_x -= 40 * fw.sx;
    else if (xdir == 2) s.scroll_x += visible_w - 40 * fw.sx;
    else if (xdir == -2) s.scroll_x -= visible_w - 40 * fw.sx;
    _swix(Wimp_OpenWindow, _IN(1), &s);
    return 1;
}

int rofiles_event_key(wevent *e)
{
    if (fw.win < 0 || e->key.window != fw.win) return 0;
    _swix(Wimp_ProcessKey, _IN(0), e->key.key);
    return 1;
}

int rofiles_event_close(wevent *e)
{
    if (fw.win < 0 || e->words[0] != fw.win) return 0;
    rofiles_shutdown();
    return 1;
}

/* ---- files dropped on the window ---- */

int rofiles_message(const wmessage *m)
{
    char path[256], *target;
    int type;
    long row;

    if (m->action != MSG_DATA_LOAD || fw.win < 0 || m->data[0] != fw.win) return 0;
    wimp_msg_path(m, path, sizeof path);
    if (!path[0]) return 1;
    wimp_send_load_ack(m, path);                            /* the Filer waits for this before the next file */
    if (!fw.conn || fw.closing) return 1;
    type = ro_local_stat(path, NULL, NULL, NULL);
    if (type == 0) return 1;

    target = dupstr(fw.cwd);
    row = row_at(m->data[3]);                               /* dropped on a directory: copy into it */
    if (row >= 0 && !row_is_parent((size_t)row) && row_entry((size_t)row) && row_entry((size_t)row)->dir) {
        sfree(target);
        target = ft_join(fw.cwd, row_entry((size_t)row)->name);
    } else if (row >= 0 && row_is_parent((size_t)row)) {
        char *err = NULL;
        char *parent = fw.conn ? ft_realpath(fw.conn, fw.cwd, "..", &err) : NULL;
        sfree(err);
        if (parent) { sfree(target); target = parent; }
    }
    enqueue_upload(path, type == 2, target);
    sfree(target);
    if (!fw.busy && !fw.in_drain) drain_queue();
    return 1;
}

const char *rofiles_help(int win, int icon)
{
    if (fw.win < 0 || win != fw.win) return NULL;
    return "The files on another computer. Double-click a directory to go into it. "
           "Drag a file to a directory display to copy it there, and drag files from a directory "
           "display onto this window to copy them up. Menu has more: new directory, rename, delete.";
}

/* ------------------------------------------------------------------------ */
/* the window                                                               */

static int mode_var(int var, int dflt)
{
    int v = 0;
    if (_swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, var, &v)) return dflt;
    return v;
}

static int create_window(void)
{
    wdef w;
    int handle = -1, sw, sh, x0, y1, vis_w, vis_h;

    fw.sx = fw.sy = UI_PX;
    sw = (mode_var(11, 1599) + 1) << mode_var(4, 1);
    sh = (mode_var(12, 1199) + 1) << mode_var(5, 1);
    /* WIN_W by WIN_H design pixels, or what the screen has room for (the scroll bars at the side and below, the title
     * bar above, a margin); the columns can be scrolled to when the window is narrower than they are. */
    vis_w = WIN_W * fw.sx;
    vis_h = WIN_H * fw.sy;
    if (vis_w > sw - 80) vis_w = sw - 80;
    if (vis_h > sh - 182) vis_h = sh - 182;
    x0 = (sw - vis_w - 40) / 2;
    y1 = sh - 160;
    if (y1 - vis_h < 40) y1 = vis_h + 40;                     /* (clear of the bottom: the scroll bar is under the window) */

    memset(&w, 0, sizeof w);
    w.visible.x0 = x0;
    w.visible.x1 = x0 + vis_w;
    w.visible.y1 = y1;
    w.visible.y0 = y1 - vis_h;
    w.behind = -1;
    w.flags = WF_MOVEABLE | WF_BACK_ICON | WF_CLOSE_ICON | WF_TITLE_BAR | WF_TOGGLE_ICON | WF_VSCROLL | WF_HSCROLL |
              WF_SIZE_ICON | WF_NEW_FORMAT | WF_SCROLL_REQUEST;
    w.title_fg = 7; w.title_bg = 2;
    w.work_fg = 7;  w.work_bg = 1;
    w.scroll_outer = 3; w.scroll_inner = 1; w.title_hi = 12;
    w.extent.x0 = 0; w.extent.x1 = X_END * fw.sx; w.extent.y1 = 0; w.extent.y0 = -WIN_H * fw.sy;
    w.title_flags = IF_TEXT | IF_HCENTRED | IF_VCENTRED | IF_INDIRECTED;
    w.work_flags = 10 << 12;                                  /* click/drag/double-click, like a Filer window */
    w.sprite_area = 1;
    w.min_w = (short)(300 * fw.sx);
    w.min_h = (short)(120 * fw.sy);
    {
        struct { char *buf; char *val; int size; } *ind = (void *)w.title;
        ind->buf = fw.title;
        ind->val = (char *)-1;
        ind->size = (int)sizeof fw.title;
    }
    if (_swix(Wimp_CreateWindow, _IN(1) | _OUT(0), &w, &handle)) return -1;
    return handle;
}

static void status_cb(void *ctx, const char *text)
{
    (void)ctx;
    snprintf(fw.status, sizeof fw.status, "%s", text);
    if (fw.win >= 0 && fw.conn == NULL)
        set_title("%s - %s", fw.conf ? conf_get_str(fw.conf, CONF_host) : "", fw.status);
}

void rofiles_open(Conf *conf)
{
    char *err = NULL;
    wstate s;

    if (fw.win >= 0) {
        ro_notice(NULL, "File transfer", "A file transfer window is open already.\n"
                  "Close it before opening another.");
        conf_free(conf);
        return;
    }
    memset(&fw, 0, sizeof fw);
    fw.defer_row = -1;
    fw.win = create_window();
    if (fw.win < 0) {
        fw.win = -1;
        conf_free(conf);
        return;
    }
    fw.small_sprites = wimp_sprite_exists("small_dir") && wimp_sprite_exists("small_xxx");
    fw.queue_tail = &fw.queue;
    fw.conf = conf;
    set_title("%s - connecting...", conf_get_str(conf, CONF_host));
    s.handle = fw.win;
    if (!_swix(Wimp_GetWindowState, _IN(1), &s)) {
        s.behind = -1;
        _swix(Wimp_OpenWindow, _IN(1), &s);
    }

    if (!roapp_op_begin()) {
        notice("Please wait: the computer is still busy with the last request.");
        finish_close();
        return;
    }
    fw.busy = true;
    fw.conn = ft_connect(conf, status_cb, NULL, &err);
    fw.busy = false;
    roapp_op_end();
    if (!fw.conn || fw.closing) {
        if (!fw.closing && err)
            ro_notice(NULL, "File transfer", err);
        sfree(err);
        finish_close();
        return;
    }
    if (!load_dir(ft_home(fw.conn)) && fw.win >= 0 && fw.cwd == NULL) {
        /* the home directory cannot be read: show the root instead */
        load_dir("/");
    }
}

bool rofiles_live(void)
{
    return fw.win >= 0 && fw.conn && ft_alive(fw.conn);
}

bool rofiles_raise(void)
{
    wstate s;
    if (fw.win < 0) return false;
    s.handle = fw.win;
    if (!_swix(Wimp_GetWindowState, _IN(1), &s)) {
        s.behind = -1;
        _swix(Wimp_OpenWindow, _IN(1), &s);
    }
    return true;
}

/* The server may hang up, or the network fail, while nothing is being asked of it. */
/* The main loop must keep waking (to run the network, and to notice a loss) while there is a connection that
 * has not been reported dead. */
bool rofiles_idle_wanted(void)
{
    return fw.win >= 0 && fw.conn && !fw.reported && !fw.closing;
}

void rofiles_idle(void)
{
    if (fw.defer_row >= 0) {                          /* the button came up with no drag: it was a plain click */
        wpointer p;
        if (_swix(Wimp_GetPointerInfo, _IN(1), &p) || !(p.buttons & BTN_SELECT)) {
            const long row = fw.defer_row;
            fw.defer_row = -1;
            if (fw.win >= 0 && fw.sel && row < (long)fw.nrow)
                select_only((size_t)row, true);
        }
    }
    if (fw.win < 0 || !fw.conn || fw.busy || fw.closing || fw.reported) return;
    if (ft_alive(fw.conn)) return;
    fw.reported = true;
    set_title("%s - disconnected", ft_host(fw.conn));
    notice("The connection to %s has been lost.\n%s\nClose this window and connect again.",
           ft_host(fw.conn), ft_why_dead(fw.conn));
}

/* The window is to go: stop whatever is running, and go when it has stopped. */
void rofiles_shutdown(void)
{
    if (fw.win < 0) return;
    fw.closing = true;
    ft_abort(fw.conn);
    pd.cancel = true;
    if (!fw.busy)
        finish_close();
}

static void finish_close(void)
{
    Queued *q;
    if (fw.win < 0) return;
    ro_msgbox_drop_owner(&fw);
    pd_close();
    if (fw.conn) {
        FtConn *c = fw.conn;
        fw.conn = NULL;
        ft_close(c);
    }
    clear_listing();
    while ((q = fw.queue) != NULL) {
        fw.queue = q->next;
        sfree(q->path); sfree(q->target); sfree(q);
    }
    sfree(fw.cwd);
    if (fw.conf) conf_free(fw.conf);
    _swix(Wimp_DeleteWindow, _IN(1), &fw.win);
    memset(&fw, 0, sizeof fw);
    fw.win = -1;
    fw.defer_row = -1;
}
