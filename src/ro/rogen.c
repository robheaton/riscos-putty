/*
 * rogen.c - the PuTTYgen Wimp task: start-up, the icon bar, and the poll loop.  The key
 * generator itself is rokeygen.c.
 *
 * PuTTYgen is a program of its own, as it is on other systems: upstream arranges that the
 * key generation code (which uses arithmetic that does not take constant time) cannot be
 * linked into a program that makes SSH connections.  It needs no network and no timers.
 *
 * Only one copy runs.  A second one, started from PuTTY's menu or by double-clicking the
 * application again, asks the first (with a broadcast message that it answers) to show its
 * window, and quits.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "putty.h"
#include "ro.h"
#include "rowimp.h"
#include "rodlg.h"
#include "rodialog.h"
#include "romenu.h"
#include "roclip.h"
#include "rosave.h"
#include "roinfo.h"
#include "rokeygen.h"
#include "roapp.h"

#define APP_NAME    "PuTTYgen"
#define TASK_MAGIC  0x4B534154
#define IDLE_CS     2                  /* wake this often while the mouse is being watched */
#define YIELD_CS    2                  /* a long operation gives the desktop a turn this often */

/* private messages between copies of this program */
#define MSG_GEN_PING 0x4B7C3
#define MSG_GEN_PONG 0x4B7C4

static int  task_handle;
static bool quit_requested;
static menu *iconbar_menu;
static dlg  *d_info;
static RoMenuHandler menu_handler;

static const int wanted_messages[] = {
    MSG_CLAIM_ENTITY, MSG_DATA_REQUEST, MSG_DATA_SAVE, MSG_DATA_SAVE_ACK, MSG_DATA_LOAD,
    MSG_DATA_LOAD_ACK, MSG_RAM_FETCH, MSG_RAM_TRANSMIT, MSG_RELEASE_ENTITY, MSG_MENUS_DELETED,
    MSG_HELP_REQUEST, MSG_GEN_PING, MSG_GEN_PONG, 0
};

/* ------------------------------------------------------------------------ */
/* what PuTTY's key code expects of an application                          */

void old_keyfile_warning(void)
{
    ro_notice(NULL, "PuTTY Key File Warning",
              "You are loading an SSH-2 private key which has an old version of the file format. "
              "This means your key file is not fully tamperproof. Future versions of PuTTY may stop "
              "supporting this private key format, so we recommend you convert your key to the new "
              "format.\nOnce the key is loaded into PuTTYgen, you can perform this conversion "
              "simply by saving it again.");
}

static void closedown(void)
{
    if (task_handle) {
        _swix(Wimp_CloseDown, _INR(0, 1), task_handle, TASK_MAGIC);
        task_handle = 0;
    }
}

void cleanup_exit(int code)
{
    random_save_seed();
    roclip_shutdown();
    closedown();
    exit(code);
}

void modalfatalbox(const char *fmt, ...)
{
    va_list ap;
    char *msg;
    va_start(ap, fmt);
    msg = dupvprintf(fmt, ap);
    va_end(ap);
    wimp_error(msg);
    sfree(msg);
    cleanup_exit(1);
}

void nonfatal(const char *fmt, ...)
{
    va_list ap;
    char *msg;
    va_start(ap, fmt);
    msg = dupvprintf(fmt, ap);
    va_end(ap);
    wimp_error(msg);
    sfree(msg);
}

/* ------------------------------------------------------------------------ */
/* the icon bar                                                             */

enum { IB_INFO, IB_HELP, IB_KEYGEN, IB_QUIT };

void roapp_menu_opened(RoMenuHandler handler)
{
    menu_handler = handler;
}

static void open_help(void)
{
    const char *dir = getenv("PuTTYgen$Dir");
    if (dir && *dir) {
        char *cmd = dupprintf("Filer_Run %s.!Help", dir);
        _swix(OS_CLI, _IN(0), cmd);
        sfree(cmd);
    } else {
        ro_notice(NULL, APP_NAME, "The help is the !Help file in the !PuTTYgen application directory.");
    }
}

static void quit_answered(void *ctx, int button)
{
    (void)ctx;
    if (button == 0) quit_requested = true;
}

/* Quitting would lose a key that has not been saved, so ask first. */
static void request_quit(void)
{
    static const char *const buttons[] = { "Quit", "Cancel" };

    if (rokeygen_unsaved())
        ro_msgbox(NULL, APP_NAME, "The key has not been saved.\nQuit PuTTYgen and lose it?", buttons, 2, 1, 1,
                  quit_answered, NULL);
    else
        quit_requested = true;
}

static void iconbar_selection(const int *sel)
{
    switch (sel[0]) {
      case IB_INFO:
        return;
      case IB_HELP:
        open_help();
        break;
      case IB_KEYGEN:
        rokeygen_open();
        break;
      case IB_QUIT:
        request_quit();
        return;
    }
}

static int menu_x, menu_y;

static void open_iconbar_menu(int x, int y)
{
    menu_x = x;
    menu_y = y;
    roapp_menu_opened(iconbar_selection);
    menu_open(iconbar_menu, x, y);
}

static void build_menus(void)
{
    d_info = roinfo_new(APP_NAME, "SSH key generator");
    iconbar_menu = menu_new(APP_NAME);
    menu_add(iconbar_menu, "Info", 0);
    if (d_info) menu_set_window(iconbar_menu, IB_INFO, dlg_window(d_info));
    menu_add(iconbar_menu, "Help...", 0);
    menu_add(iconbar_menu, "Key generator...", 0);
    menu_add(iconbar_menu, "Quit", 0);
}

/* ------------------------------------------------------------------------ */
/* the poll loop                                                            */

static const char *help_for(int win, int icon)
{
    const char *text = dlg_help_text(win, icon);
    if (text) return text;
    if (win == -2)
        return "PuTTYgen. Click to open the key generator, or drop a key file on the icon to load it. "
               "Menu gives the PuTTYgen menu.";
    return NULL;
}

static void dispatch(int event, wevent *e)
{
    switch (event) {
      case EV_REDRAW:
        dlg_event_redraw(e);
        break;
      case EV_OPEN:
        if (!dlg_event_open(e)) _swix(Wimp_OpenWindow, _IN(1), e);
        break;
      case EV_CLOSE:
        dlg_event_close(e);
        break;
      case EV_DRAG_END:
        rosave_drag_end(e);
        break;
      case EV_MOUSE_CLICK:
        if (dlg_event_click(e)) break;
        if (e->mouse.window == -2) {
            if (e->mouse.buttons & BTN_MENU)
                open_iconbar_menu(e->mouse.x - 64, 96 + 44 * iconbar_menu->nitems);
            else
                rokeygen_open();
        }
        break;
      case EV_KEY:
        if (!dlg_event_key(e)) _swix(Wimp_ProcessKey, _IN(0), e->key.key);
        break;
      case EV_MENU:
        if (menu_handler) menu_handler(e->words);
        break;
      case EV_MESSAGE_ACK:
        rosave_bounced(&e->msg);
        break;
      case EV_MESSAGE:
      case EV_MESSAGE_RECORDED:
        if (rosave_message(&e->msg) || rokeygen_message(&e->msg) || roclip_message(&e->msg))
            break;
        switch (e->msg.action) {
          case MSG_QUIT:
            quit_requested = true;
            break;
          case MSG_PRE_QUIT:
            /* The desktop is about to shut down; refuse (by sending the message back as an
             * acknowledgement) and ask, if that would lose a key. */
            if (rokeygen_unsaved()) {
                e->msg.your_ref = e->msg.my_ref;
                _swix(Wimp_SendMessage, _INR(0, 2), 19, &e->msg, e->msg.sender);
                request_quit();
            }
            break;
          case MSG_HELP_REQUEST:
            wimp_help_reply(&e->msg, help_for(e->msg.data[3], e->msg.data[4]));
            break;
          case MSG_GEN_PING: {               /* a second copy was started: show our window, tell it to go */
            int blk[5];
            memset(blk, 0, sizeof blk);
            blk[0] = 20;
            blk[3] = e->msg.my_ref;
            blk[4] = MSG_GEN_PONG;
            _swix(Wimp_SendMessage, _INR(0, 2), 17, blk, e->msg.sender);
            rokeygen_open();
            break;
          }
          default:
            break;
        }
        break;
      default:
        break;
    }
}

/*
 * One trip round the poll loop; returns the event the Wimp reported.  `nowait` is for a long
 * operation giving the desktop a moment: the Wimp must come back at once, with a null event if it
 * has nothing else.
 */
static int poll_once(bool nowait)
{
    int block[64];
    int event = 0;
    const uint32_t now = ro_time();
    const bool lively = nowait || rokeygen_idle_wanted();
    unsigned mask = lively ? 0u : MASK_NULL;
    uint32_t wake = nowait ? now : lively ? now + IDLE_CS : now + 6000;
    _kernel_oserror *e;

    e = _swix(Wimp_PollIdle, _INR(0, 3) | _OUT(0), mask, block, (int)wake, 0, &event);
    if (e) {
        wimp_error_os(e);
        return EV_NULL;
    }
    dispatch(event, (wevent *)block);
    if (rokeygen_idle_wanted()) rokeygen_idle();
    return event;
}

/* Long operations (making a key) call this every so often so the desktop carries on. */
static bool op_running;
static int  yield_depth;

bool roapp_op_begin(void)
{
    if (op_running) return false;
    op_running = true;
    return true;
}

void roapp_op_end(void)
{
    op_running = false;
}

bool roapp_yield(void)
{
    static uint32_t last;
    const uint32_t now = ro_time();

    if (yield_depth == 0 && (int32_t)(now - last) >= YIELD_CS) {
        int n;
        yield_depth++;
        /* The Wimp returns one event per poll, redraw requests ahead of mouse clicks, and our own
         * progress bar asks for a redraw before every yield: so poll until it has nothing more to
         * say (a null event), or the mouse would never get a turn. */
        for (n = 0; n < 16; n++)
            if (poll_once(true) == EV_NULL) break;
        yield_depth--;
        last = ro_time();
    }
    return quit_requested;
}

/*
 * Is another copy running?  Broadcast a ping and see whether a PONG comes back within a third of
 * a second.  (A recorded broadcast that nobody answers is never bounced back by this Wimp, so
 * there is no telling "nobody is there" except by waiting.)
 */
static bool another_copy_running(void)
{
    int blk[5], event, block[64];
    const uint32_t until = ro_time() + 30;

    memset(blk, 0, sizeof blk);
    blk[0] = 20;
    blk[4] = MSG_GEN_PING;
    if (_swix(Wimp_SendMessage, _INR(0, 2), 17, blk, 0)) return false;
    while ((int32_t)(until - ro_time()) > 0) {
        event = 0;
        /* (null events must be allowed: they are what end the wait at `until`) */
        if (_swix(Wimp_PollIdle, _INR(0, 3) | _OUT(0), 0, block, (int)until, 0, &event)) return false;
        if ((event == EV_MESSAGE || event == EV_MESSAGE_RECORDED) &&
            ((wevent *)block)->msg.action == MSG_GEN_PONG)
            return true;
        if ((event == EV_MESSAGE || event == EV_MESSAGE_RECORDED) &&
            ((wevent *)block)->msg.action == MSG_QUIT) {
            quit_requested = true;
            return false;
        }
    }
    return false;
}

int main(int argc, char **argv)
{
    _kernel_oserror *e;

    e = _swix(Wimp_Initialise, _INR(0, 3) | _OUT(1), 310, TASK_MAGIC, APP_NAME, wanted_messages, &task_handle);
    if (e) {
        fprintf(stderr, "Cannot start: %s\n", e->errmess);
        return 1;
    }
    if (another_copy_running()) {
        closedown();
        return 0;
    }

    roclip_init(task_handle);
    build_menus();
    if (wimp_sprite_exists("!puttygen"))
        wimp_create_icon_iconbar(IF_SPRITE | IF_HCENTRED | IF_VCENTRED | IF_BUTTON(BT_CLICK),
                                 "!puttygen", "", 68);
    else
        wimp_create_icon_iconbar(IF_TEXT | IF_HCENTRED | IF_BORDER | IF_FILLED |
                                 IF_BUTTON(BT_CLICK) | IF_FG(7) | IF_BG(1), "", APP_NAME, 100);

    if (argc > 1) rokeygen_load(argv[1]);       /* a key file named on the command line */
    else rokeygen_open();

    while (!quit_requested)
        poll_once(false);
    cleanup_exit(0);
    return 0;
}
