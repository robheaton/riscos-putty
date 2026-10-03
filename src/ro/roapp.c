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
const bool share_can_be_downstream = false;     /* connection sharing: see stubs.c */
const bool share_can_be_upstream = false;
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

/*
 * The icon bar menu follows the usual RISC OS pattern: Info first (its submenu is the
 * "About this program" window), then Help, the application's own entries, and Quit.
 */
enum { IB_INFO, IB_HELP, IB_NEW, IB_SESSIONS, IB_QUIT };

static dlg   *d_info;
static menu  *sessions_menu;           /* the saved sessions, as the Sessions submenu shows them */
static char **session_names;
static int    nsessions;
static int    menu_x, menu_y;          /* where the icon bar menu was opened (for Adjust) */

/*
 * The standard "About this program" box, laid out as the Toolbox's ProgInfo and the
 * ROM applications show it (Edit, Paint): a title bar and nothing else, a right-aligned
 * label and a sunken read-only field for each of name, purpose, author, port,
 * licence and version.  It is the submenu of the Info item, so the Wimp opens and
 * closes it.  Sizes are in pixels, as measured from !Edit's: 320 wide, rows 30
 * apart, fields 26 high, labels 76 wide.
 */
enum { I_NAME = 10, I_PURPOSE, I_AUTHOR, I_PORT, I_LICENCE, I_VERSION };

static void build_info(void)
{
    static const dlg_item items[] = {
        { DI_RLABEL,  0,         0,   4, 76, 26, "Name",    0, 0, NULL },
        { DI_DISPLAY, I_NAME,    77,  4, 238, 26, "",       0, 0, NULL },
        { DI_RLABEL,  0,         0,  34, 76, 26, "Purpose", 0, 0, NULL },
        { DI_DISPLAY, I_PURPOSE, 77, 34, 238, 26, "",       0, 0, NULL },
        { DI_RLABEL,  0,         0,  64, 76, 26, "Author",  0, 0, NULL },
        { DI_DISPLAY, I_AUTHOR,  77, 64, 238, 26, "",       0, 0, NULL },
        { DI_RLABEL,  0,         0,  94, 76, 26, "Port",    0, 0, NULL },
        { DI_DISPLAY, I_PORT,    77, 94, 238, 26, "",       0, 0, NULL },
        { DI_RLABEL,  0,         0, 124, 76, 26, "Licence", 0, 0, NULL },
        { DI_DISPLAY, I_LICENCE, 77, 124, 238, 26, "",      0, 0, NULL },
        { DI_RLABEL,  0,         0, 154, 76, 26, "Version", 0, 0, NULL },
        { DI_DISPLAY, I_VERSION, 77, 154, 238, 26, "",      0, 0, NULL },
    };
    char text[64];

    d_info = dlg_new_style(DLG_MENU, "About this program", 320, 184, items,
                           (int)(sizeof items / sizeof items[0]), NULL, NULL);
    if (!d_info) return;
    dlg_set_text(d_info, I_NAME, "PuTTY");
    dlg_set_text(d_info, I_PURPOSE, "SSH and Telnet client");
    dlg_set_text(d_info, I_AUTHOR, "Simon Tatham and others");
    dlg_set_text(d_info, I_PORT, "Rob Heaton and Claude, 2026");
    dlg_set_text(d_info, I_LICENCE, "MIT licence");
    snprintf(text, sizeof text, "%s (%s), PuTTY %.*s", RO_PORT_VERSION, RO_PORT_DATE,
             (int)strspn(ver + (strncmp(ver, "Release ", 8) ? 0 : 8), "0123456789."),
             ver + (strncmp(ver, "Release ", 8) ? 0 : 8));
    dlg_set_text(d_info, I_VERSION, text);
    dlg_set_help(d_info, "Information about this program: its name, purpose, author, licence and version.");
}

int roapp_info_window(void)
{
    return d_info ? dlg_window(d_info) : 0;
}

static void build_menus(void)
{
    build_info();
    iconbar_menu = menu_new(APP_NAME);
    menu_add(iconbar_menu, "Info", 0);
    if (d_info) menu_set_window(iconbar_menu, IB_INFO, dlg_window(d_info));
    menu_add(iconbar_menu, "Help...", 0);
    menu_add(iconbar_menu, "New session...", 0);
    menu_add(iconbar_menu, "Sessions", 0);
    menu_add(iconbar_menu, "Quit", 0);
}

void roapp_connect_dialog(void)
{
    roconnect_open();
}

/* Open the help file (!Help in the application directory) in whatever shows text. */
static void open_help(void)
{
    const char *dir = getenv("PuTTY$Dir");           /* set by !Boot and !Run */
    if (dir && *dir) {
        char *cmd = dupprintf("Filer_Run %s.!Help", dir);   /* the real path, so the editor's title shows it */
        _swix(OS_CLI, _IN(0), cmd);
        sfree(cmd);
    } else {
        ro_notice(NULL, "PuTTY", "The help is the !Help file in the !PuTTY application directory.");
    }
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

static void iconbar_selection(const int *sel);

/*
 * Open the icon bar menu.  The Sessions submenu lists the sessions saved when the menu
 * is opened, so a session saved a moment ago is there.  The previous list is freed
 * only after the new menu is up, because the Wimp may still be showing it.
 */
static void open_iconbar_menu(int x, int y)
{
    menu *old_menu = sessions_menu;
    char **old_names = session_names;
    const int old_n = nsessions;
    int i;

    session_names = roconnect_session_names(&nsessions);
    if (nsessions > 0) {
        sessions_menu = menu_new("Sessions");
        for (i = 0; i < nsessions && i < MAX_MENU_ITEMS; i++)
            menu_add(sessions_menu, session_names[i], 0);
    } else {
        sessions_menu = NULL;
    }
    menu_set_sub(iconbar_menu, IB_SESSIONS, sessions_menu);
    menu_shade(iconbar_menu, IB_SESSIONS, sessions_menu == NULL);

    menu_x = x;
    menu_y = y;
    roapp_menu_opened(iconbar_selection);
    menu_open(iconbar_menu, x, y);

    menu_free(old_menu);
    roconnect_free_names(old_names, old_n);
}

static void iconbar_selection(const int *sel)
{
    wpointer p;

    switch (sel[0]) {
      case IB_INFO:
        return;                        /* its window is the submenu; nothing to do */
      case IB_HELP:
        open_help();
        break;
      case IB_NEW:
        roapp_connect_dialog();
        break;
      case IB_SESSIONS:
        if (sel[1] >= 0 && sel[1] < nsessions)
            roconnect_start_session(session_names[sel[1]]);
        break;
      case IB_QUIT:
        request_quit();
        return;
    }
    /* Adjust keeps the menu on screen, as for every other menu */
    if (!_swix(Wimp_GetPointerInfo, _IN(1), &p) && (p.buttons & BTN_ADJUST))
        open_iconbar_menu(menu_x, menu_y);
}

static void iconbar_click(wevent *e)
{
    if (e->mouse.buttons & BTN_MENU) {
        open_iconbar_menu(e->mouse.x - 64, 96 + 44 * iconbar_menu->nitems);
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
        if (dlg_event_redraw(e)) break;
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
        const bool lively = roterm_first() || roconfig_idle_wanted();    /* wants null events */
        unsigned mask = lively ? 0u : MASK_NULL;
        if (lively) {
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
        if (roconfig_idle_wanted()) roconfig_idle();
        roterm_idle();
    }

    while (roterm_first()) roterm_free(roterm_first());
    cleanup_exit(0);
    return 0;
}
