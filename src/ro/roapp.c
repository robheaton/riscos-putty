/*
 * roapp.c - the PuTTY Wimp task: start-up, the poll loop, command line, and the
 * platform hooks the shared code expects of a GUI application.
 *
 * Polling.  PuTTY is event driven by sockets, timers and "top level callbacks",
 * not by the Wimp, so each trip round the loop does three things: gives the
 * Wimp a turn (Wimp_PollIdle, which also wakes us every two centiseconds while a
 * session is open so incoming data is noticed promptly), runs whatever PuTTY has
 * pending (sockets, timers, callbacks), then lets each window paint what changed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "putty.h"
#include "ssh.h"
#include "storage.h"
#include "ro.h"
#include "ronet.h"
#include "rowimp.h"
#include "rodlg.h"
#include "rodialog.h"
#include "romenu.h"
#include "roterm.h"
#include "roclip.h"
#include "roapp.h"
#include "rocontext.h"
#include "roconnect.h"
#include "roconfig.h"
#include "roversion.h"

#define APP_NAME    "PuTTY"
#define TASK_MAGIC  0x4B534154
#define IDLE_CS     2                  /* wake this often while a session is open */

static int  task_handle;
static bool quit_requested;
static menu *iconbar_menu;

static const int wanted_messages[] = {
    MSG_PALETTE_CHANGE, MSG_MODE_CHANGE, MSG_CLAIM_ENTITY, MSG_DATA_REQUEST, MSG_DATA_SAVE,
    MSG_DATA_SAVE_ACK, MSG_DATA_LOAD, MSG_DATA_LOAD_ACK, MSG_RAM_FETCH, MSG_RAM_TRANSMIT,
    MSG_RELEASE_ENTITY, MSG_MENUS_DELETED, MSG_HELP_REQUEST, 0
};

/* ------------------------------------------------------------------------ */
/* what the shared code expects of an application                           */

const bool use_event_log = true, new_session = true, saved_sessions = true;
const bool dup_check_launchable = true;

const unsigned cmdline_tooltype =
    TOOLTYPE_HOST_ARG |
    TOOLTYPE_HOST_ARG_CAN_BE_SESSION |
    TOOLTYPE_HOST_ARG_PROTOCOL_PREFIX |
    TOOLTYPE_HOST_ARG_FROM_LAUNCHABLE_LOAD |
    TOOLTYPE_PORT_ARG;

void frontend_net_error_pending(void) {}

/* timing.c tells us when the next timer changes; our loop looks again each time
 * round anyway, so there is nothing to do. */
void timer_change_notify(unsigned long next)
{
}

void old_keyfile_warning(void)
{
    ro_notice(NULL, "PuTTY Key File Warning",
              "You are loading an SSH-2 private key which has an old version of the file format. "
              "This means your key file is not fully tamperproof. Future versions of PuTTY may stop "
              "supporting this private key format, so we recommend you convert your key to the new "
              "format.\nOnce the key is loaded into PuTTYgen, you can perform this conversion "
              "simply by saving it again.");
}

void cmdline_error(const char *fmt, ...)
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

const struct BackendVtable *select_backend(Conf *conf)
{
    const struct BackendVtable *vt = backend_vt_from_proto(conf_get_int(conf, CONF_protocol));
    assert(vt != NULL);
    return vt;
}

FontSpec *platform_default_fontspec(const char *name)
{
    if (!strcmp(name, "Font"))
        return fontspec_new("Corpus.Medium,12");
    return fontspec_new_default();
}

Filename *platform_default_filename(const char *name)
{
    if (!strcmp(name, "LogFileName"))
        return filename_from_str("PuTTYLog");
    return filename_from_str("");
}

char *platform_default_s(const char *name)
{
    if (!strcmp(name, "TermType"))
        return dupstr("xterm");
    return NULL;
}

bool platform_default_b(const char *name, bool def)
{
    return def;
}

int platform_default_i(const char *name, int def)
{
    return def;
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
    sk_cleanup();
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
/* the iconbar                                                              */

static RoMenuHandler menu_handler;

void roapp_menu_opened(RoMenuHandler handler)
{
    menu_handler = handler;
}

enum { IB_NEW, IB_ABOUT, IB_QUIT };

static void build_menus(void)
{
    iconbar_menu = menu_new(APP_NAME);
    menu_add(iconbar_menu, "New session...", 0);
    menu_add(iconbar_menu, "About PuTTY...", MF_DOTTED);
    menu_add(iconbar_menu, "Quit", 0);
}

void roapp_connect_dialog(void)
{
    roconnect_open();
}

void roapp_about(void)
{
    char *text = dupprintf("PuTTY for RISC OS %s\nbased on PuTTY %s\n\n"
                           "PuTTY is copyright 1997-2024 Simon Tatham and contributors.\n"
                           "The RISC OS port is copyright 2026 Rob Heaton.\n"
                           "Both are distributed under the MIT licence.",
                           RO_PORT_VERSION, ver);
    ro_notice(NULL, "About PuTTY", text);
    sfree(text);
}

static int live_sessions(void)
{
    RoTerm *t;
    int n = 0;
    for (t = roterm_first(); t; t = t->next)
        if (roterm_is_live(t)) n++;
    return n;
}

static void quit_answered(void *ctx, int button)
{
    if (button == 0) quit_requested = true;
}

/* Quitting closes every session, so ask first if any connection would be lost. */
static void request_quit(void)
{
    static const char *const buttons[] = { "Quit", "Cancel" };
    const int n = live_sessions();
    char *msg;

    if (n == 0) {
        quit_requested = true;
        return;
    }
    msg = dupprintf("%d session%s still connected.\nQuit PuTTY and close %s?", n,
                    n == 1 ? " is" : "s are", n == 1 ? "it" : "them");
    ro_msgbox(NULL, "PuTTY Exit Confirmation", msg, buttons, 2, 1, 1, quit_answered, NULL);
    sfree(msg);
}

static void iconbar_selection(const int *sel)
{
    switch (sel[0]) {
      case IB_NEW:
        roapp_connect_dialog();
        break;
      case IB_ABOUT:
        roapp_about();
        break;
      case IB_QUIT:
        request_quit();
        break;
    }
}

static void iconbar_click(wevent *e)
{
    if (e->mouse.buttons & BTN_MENU) {
        roapp_menu_opened(iconbar_selection);
        menu_open(iconbar_menu, e->mouse.x - 64, 96 + 44 * iconbar_menu->nitems);
        return;
    }
    roapp_connect_dialog();
}

/* ------------------------------------------------------------------------ */
/* interactive help                                                         */

static const char *help_for(int win, int icon)
{
    const char *text = dlg_help_text(win, icon);
    if (text) return text;
    if (win == -2)
        return "PuTTY. Click to open the Connect box and start a session. "
               "Menu gives the PuTTY menu.";
    if (roterm_from_window(win))
        return "A PuTTY terminal: type to send to the computer you are logged in to. "
               "Select-drag selects text and copies it; Shift-Insert pastes it. "
               "Menu gives Copy, Paste, Special command, Event log, Change settings and more.";
    return NULL;
}

/* Message_HelpRequest: answer with the text for the window and icon under the pointer. */
static void reply_help(const wmessage *m)
{
    const char *text = help_for(m->data[3], m->data[4]);
    int blk[64];
    size_t n;
    if (!text) return;
    memset(blk, 0, sizeof blk);
    n = strlen(text);
    if (n > 235) n = 235;
    memcpy((char *)&blk[5], text, n);
    blk[0] = 20 + (int)((n + 4) & ~(size_t)3);
    blk[3] = m->my_ref;
    blk[4] = MSG_HELP_REPLY;
    _swix(Wimp_SendMessage, _INR(0, 2), 17, blk, m->sender);
}

/* ------------------------------------------------------------------------ */
/* the poll loop                                                            */

static void dispatch(int event, wevent *e)
{
    RoTerm *t;

    switch (event) {
      case EV_REDRAW:
        if ((t = roterm_from_window(e->words[0])) != NULL) roterm_redraw(t, e);
        break;
      case EV_OPEN:
        if (dlg_event_open(e) || roconfig_event_open(e)) break;
        if ((t = roterm_from_window(e->words[0])) != NULL) roterm_open(t, e);
        else _swix(Wimp_OpenWindow, _IN(1), e);
        break;
      case EV_CLOSE:
        if (dlg_event_close(e) || roconfig_event_close(e)) break;
        if ((t = roterm_from_window(e->words[0])) != NULL) roterm_close_request(t);
        break;
      case EV_MOUSE_CLICK:
        if (dlg_event_click(e) || roconfig_event_click(e)) break;
        if (e->mouse.window == -2) { iconbar_click(e); break; }
        if ((t = roterm_from_window(e->mouse.window)) != NULL) roterm_mouse_click(t, e);
        break;
      case EV_KEY:
        if (dlg_event_key(e) || roconfig_event_key(e)) break;
        if ((t = roterm_from_window(e->key.window)) != NULL) roterm_key(t, e);
        else _swix(Wimp_ProcessKey, _IN(0), e->key.key);
        break;
      case EV_SCROLL:
        if (roconfig_event_scroll(e)) break;
        if ((t = roterm_from_window(e->words[0])) != NULL) roterm_scroll_request(t, e);
        break;
      case EV_GAIN_CARET:
        if ((t = roterm_from_window(e->words[0])) != NULL) roterm_gain_caret(t);
        break;
      case EV_LOSE_CARET:
        if ((t = roterm_from_window(e->words[0])) != NULL) roterm_lose_caret(t);
        break;
      case EV_MENU:
        if (menu_handler) menu_handler(e->words);
        break;
      case EV_MESSAGE:
      case EV_MESSAGE_RECORDED:
        if (roconnect_message(&e->msg) || roconfig_message(&e->msg) || roclip_message(&e->msg)) break;
        switch (e->msg.action) {
          case MSG_QUIT:
            quit_requested = true;
            break;
          case MSG_PRE_QUIT:
            /* The desktop is about to shut down.  Live connections would be lost, so
             * refuse (by sending the message back as an acknowledgement) and ask. */
            if (live_sessions()) {
                e->msg.your_ref = e->msg.my_ref;
                _swix(Wimp_SendMessage, _INR(0, 2), 19, &e->msg, e->msg.sender);
                request_quit();
            }
            break;
          case MSG_HELP_REQUEST:
            reply_help(&e->msg);
            break;
          case MSG_SHUTDOWN:
            break;
          case MSG_PALETTE_CHANGE:
          case MSG_MODE_CHANGE:
            roterm_redraw_all();
            break;
          default:
            break;
        }
        break;
      default:
        break;
    }
}

/* Run what PuTTY has pending.  Returns the earliest time (centiseconds, ro_time
 * scale) it wants to be called again, or 0 for "no particular time". */
static uint32_t run_putty(void)
{
    unsigned long now, next;
    uint32_t wake = 0;
    RoTerm *t;

    ro_net_poll(0);
    now = GETTICKCOUNT();
    if (run_timers(now, &next)) {
        long ticks = (long)(next - now);
        wake = ro_time() + (ticks <= 0 ? 0 : (uint32_t)(ticks / 10) + 1);
    }
    run_toplevel_callbacks();
    for (t = roterm_first(); t; t = t->next) roterm_poll(t);
    if (toplevel_callback_pending()) wake = ro_time();
    return wake;
}

/* ------------------------------------------------------------------------ */
/* the command line                                                         */

static void usage(void)
{
    ro_notice(NULL, "PuTTY",
              "Usage: putty [options] [user@]host\n"
              "  -ssh -telnet -rlogin -raw   protocol\n"
              "  -P port   -l user   -i keyfile   -pw password\n"
              "  -load session   -v\n"
              "  --demo    paint a test pattern (no network)");
}

static bool start_from_command_line(int argc, char **argv)
{
    Conf *conf = conf_new();
    CmdlineArgList *arglist;
    size_t pos = 0;
    bool errors = false, demo = false;

    do_defaults(NULL, conf);
    settings_set_default_protocol(conf_get_int(conf, CONF_protocol));
    settings_set_default_port(conf_get_int(conf, CONF_port));

    arglist = cmdline_arg_list_from_argv(argc, argv);
    while (arglist->args[pos]) {
        CmdlineArg *arg = arglist->args[pos++];
        CmdlineArg *nextarg = arglist->args[pos];
        const char *p = cmdline_arg_to_str(arg);
        int ret = cmdline_process_param(arg, nextarg, 1, conf);
        if (ret == -2) {
            errors = true;
        } else if (ret == 2) {
            pos++;
        } else if (ret == 1) {
            continue;
        } else if (!strcmp(p, "--demo")) {
            demo = true;
        } else if (!strcmp(p, "--help")) {
            usage();
            cmdline_arg_list_free(arglist);
            conf_free(conf);
            return false;
        } else {
            errors = true;
        }
    }
    if (demo) {
        cmdline_arg_list_free(arglist);
        return roterm_new_demo(conf) != NULL;
    }
    if (errors || !cmdline_host_ok(conf)) {
        cmdline_arg_list_free(arglist);
        conf_free(conf);
        if (argc > 1) usage();
        return false;
    }
    cmdline_run_saved(conf);
    cmdline_arg_list_free(arglist);
    return roterm_new(conf) != NULL;
}

int main(int argc, char **argv)
{
    static int event_block[64];
    _kernel_oserror *e;

    e = _swix(Wimp_Initialise, _INR(0, 3) | _OUT(1), 310, TASK_MAGIC, APP_NAME,
              wanted_messages, &task_handle);
    if (e) {
        fprintf(stderr, "Cannot start: %s\n", e->errmess);
        return 1;
    }

    settings_set_default_protocol(be_default_protocol);
    settings_set_default_port(22);
    roclip_init(task_handle);
    sk_init();
    uxsel_init();
    build_menus();
    if (wimp_sprite_exists("!putty"))                   /* run from !PuTTY, whose !Boot or !Run loaded it */
        wimp_create_icon_iconbar(IF_SPRITE | IF_HCENTRED | IF_VCENTRED | IF_BUTTON(BT_CLICK),
                                 "!putty", "", 68);
    else
        wimp_create_icon_iconbar(IF_TEXT | IF_HCENTRED | IF_BORDER | IF_FILLED |
                                 IF_BUTTON(BT_CLICK) | IF_FG(7) | IF_BG(1), "", APP_NAME, 100);

    start_from_command_line(argc, argv);
    if (!roterm_first()) roapp_connect_dialog();       /* nothing to connect to yet: ask */

    while (!quit_requested) {
        int event = 0;
        uint32_t wake = run_putty(), now = ro_time();
        unsigned mask = roterm_first() ? 0u : MASK_NULL;
        if (roterm_first()) {
            uint32_t idle = now + IDLE_CS;
            if (!wake || (int32_t)(idle - wake) < 0) wake = idle;
        } else {
            wake = now + 6000;
        }
        e = _swix(Wimp_PollIdle, _INR(0, 3) | _OUT(0), mask, event_block, (int)wake, 0, &event);
        if (e) {
            wimp_error_os(e);
            continue;
        }
        dispatch(event, (wevent *)event_block);
    }

    while (roterm_first()) roterm_free(roterm_first());
    cleanup_exit(0);
    return 0;
}
