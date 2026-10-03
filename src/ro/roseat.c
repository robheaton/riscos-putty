/*
 * roseat.c - the Seat and LogPolicy of a terminal window: how a session talks to
 * its user.  Modelled on the GTK front end (unix/window.c and unix/dialog.c);
 * the questions PuTTY asks are shown in non-modal boxes (rodialog.c) and
 * answered through PuTTY's callbacks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "ssh.h"
#include "storage.h"
#include "terminal.h"
#include "ro.h"
#include "roterm.h"
#include "rodialog.h"
#include "roseat.h"

#define SEAT(seat) RoTerm *t = container_of(seat, RoTerm, seat)

/* ------------------------------------------------------------------------ */
/* starting and stopping the connection                                     */

extern const struct BackendVtable *select_backend(Conf *conf);   /* roapp.c */

/* Without the CryptRandom module (see noise.c: it is loaded from System:Modules if it is
 * installed) the random numbers behind session keys are only as good as timing jitter:
 * say so in the event log, and once per run in a box. */
static void warn_if_weak_entropy(RoTerm *t)
{
    static bool warned;
    if (!ro_entropy_is_weak())
        return;
    logevent(t->logctx, "Warning: the CryptRandom module is not loaded and could not be found, so random "
             "numbers come only from timing jitter and the seed file (weaker than they should be)");
    if (!warned && !getenv("PuTTY$NoEntropyWarning")) {
        warned = true;
        ro_notice(NULL, "PuTTY",
                  "The CryptRandom module is not loaded and PuTTY could not find it in System:Modules, "
                  "so this computer has no strong source of random numbers for PuTTY and the keys of "
                  "your sessions will be weaker than they should be.\n"
                  "Install the CryptRandom package with !PackMan (it is in the System section) and "
                  "start PuTTY again.\n"
                  "Set the system variable PuTTY$NoEntropyWarning to stop this message.");
    }
}

void roterm_start_backend(RoTerm *t)
{
    const struct BackendVtable *vt;
    char *error, *realhost;

    warn_if_weak_entropy(t);
    t->cmdline_state = cmdline_get_passwd_input_state_new;
    vt = select_backend(t->conf);

    seat_set_trust_status(&t->seat, true);
    error = backend_init(vt, &t->seat, &t->backend, t->logctx, t->conf,
                         conf_get_str(t->conf, CONF_host),
                         conf_get_int(t->conf, CONF_port),
                         &realhost,
                         conf_get_bool(t->conf, CONF_tcp_nodelay),
                         conf_get_bool(t->conf, CONF_tcp_keepalives));
    if (error) {
        seat_connection_fatal(&t->seat, "Unable to open connection to %s:\n%s",
                              conf_dest(t->conf), error);
        sfree(error);
        t->exited = true;
        return;
    }

    term_setup_window_titles(t->term, realhost);
    sfree(realhost);
    term_provide_backend(t->term, t->backend);
    t->ldisc = ldisc_create(t->conf, t->term, t->backend, &t->seat);
    ldisc_echoedit_update(t->ldisc);
    backend_size(t->backend, t->cols, t->rows);
}

void roterm_restart(RoTerm *t)
{
    if (t->backend || !t->term || t->is_log) return;
    logevent(t->logctx, "----- Session restarted -----");
    term_pwron(t->term, false);
    t->exited = false;
    roterm_start_backend(t);
}

void roterm_destroy_connection(RoTerm *t)
{
    t->exited = true;
    if (t->ldisc) {
        ldisc_free(t->ldisc);
        t->ldisc = NULL;
    }
    if (t->backend) {
        backend_free(t->backend);
        t->backend = NULL;
    }
    if (t->term)
        term_provide_backend(t->term, NULL);
}

static void exit_callback(void *vctx)
{
    RoTerm *t = (RoTerm *)vctx;
    int exitcode;

    if (!t->exited && t->backend && (exitcode = backend_exitcode(t->backend)) >= 0) {
        int close_on_exit;
        roterm_destroy_connection(t);
        close_on_exit = conf_get_int(t->conf, CONF_close_on_exit);
        if (close_on_exit == FORCE_ON || (close_on_exit == AUTO && exitcode == 0))
            roterm_free(t);
        else
            roterm_mark_inactive(t);
    }
}

static void connection_fatal_callback(void *vctx)
{
    RoTerm *t = (RoTerm *)vctx;
    roterm_destroy_connection(t);
    roterm_mark_inactive(t);
}

/* ------------------------------------------------------------------------ */
/* the Seat                                                                 */

static size_t rs_output(Seat *seat, SeatOutputType type, const void *data, size_t len)
{
    SEAT(seat);
    return term_data(t->term, data, len);
}

static bool rs_eof(Seat *seat)
{
    return true;                       /* do respond to incoming EOF with outgoing */
}

static SeatPromptResult rs_get_userpass_input(Seat *seat, prompts_t *p)
{
    SEAT(seat);
    SeatPromptResult spr;
    spr = cmdline_get_passwd_input(p, &t->cmdline_state, true);
    if (spr.kind == SPRK_INCOMPLETE)
        spr = term_get_userpass_input(t->term, p);
    return spr;
}

static void rs_notify_remote_exit(Seat *seat)
{
    SEAT(seat);
    queue_toplevel_callback(exit_callback, t);
}

static void seat_connection_fatal_(Seat *seat, const char *msg)
{
    SEAT(seat);
    char *title = dupcat(appname, " Fatal Error");
    ro_notice(t, title, msg);
    sfree(title);
    t->exited = true;                  /* suppress normal exit handling */
    queue_toplevel_callback(connection_fatal_callback, t);
}

static void seat_nonfatal_(Seat *seat, const char *msg)
{
    SEAT(seat);
    char *title = dupcat(appname, " Error");
    ro_notice(t, title, msg);
    sfree(title);
}

static void rs_update_specials_menu(Seat *seat)
{
    /* the Special Commands menu is built from backend_get_specials() when it is
     * opened, so there is nothing to keep up to date */
}

static char *rs_get_ttymode(Seat *seat, const char *mode)
{
    SEAT(seat);
    return term_get_ttymode(t->term, mode);
}

static void rs_set_busy_status(Seat *seat, BusyStatus status)
{
    SEAT(seat);
    t->busy = status;
}

const SeatDialogPromptDescriptions *roseat_prompt_descriptions(void)
{
    static const SeatDialogPromptDescriptions descs = {
        .hk_accept_action = "press \"Accept\"",
        .hk_connect_once_action = "press \"Connect Once\"",
        .hk_cancel_action = "press \"Cancel\"",
        .hk_cancel_action_Participle = "Pressing \"Cancel\"",
        .weak_accept_action = "press \"Yes\"",
        .weak_cancel_action = "press \"No\"",
    };
    return &descs;
}

static const SeatDialogPromptDescriptions *rs_prompt_descriptions(Seat *seat)
{
    return roseat_prompt_descriptions();
}

static bool rs_is_utf8(Seat *seat)
{
    SEAT(seat);
    return t->ucsdata.line_codepage == CS_UTF8;
}

static bool rs_get_window_pixel_size(Seat *seat, int *w, int *h)
{
    SEAT(seat);
    wstate s;
    int xe = 1, ye = 1;
    s.handle = t->win;
    if (t->win < 0 || _swix(Wimp_GetWindowState, _IN(1), &s)) return false;
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 4, &xe);
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 5, &ye);
    *w = (s.visible.x1 - s.visible.x0) >> xe;
    *h = (s.visible.y1 - s.visible.y0) >> ye;
    return true;
}

static StripCtrlChars *rs_stripctrl_new(Seat *seat, BinarySink *bs_out, SeatInteractionContext sic)
{
    SEAT(seat);
    return stripctrl_new_term(bs_out, false, 0, t->term);
}

static void rs_set_trust_status(Seat *seat, bool trusted)
{
    SEAT(seat);
    term_set_trust_status(t->term, trusted);
}

static bool rs_can_set_trust_status(Seat *seat)
{
    return true;
}

static bool rs_get_cursor_position(Seat *seat, int *x, int *y)
{
    SEAT(seat);
    if (t->term) {
        term_get_cursor_position(t->term, x, y);
        return true;
    }
    return false;
}

/* ---- the questions ----------------------------------------------------- */

/* Turn the structured text PuTTY supplies for a question into plain text for a
 * message box: the title, and everything the user needs to read. */
static char *dialog_text(const SeatDialogText *text, const char **title_out)
{
    strbuf *sb = strbuf_new();
    const SeatDialogTextItem *item, *end = text->items + text->nitems;
    *title_out = NULL;
    for (item = text->items; item < end; item++) {
        switch (item->type) {
          case SDT_TITLE:
            *title_out = item->text;
            break;
          case SDT_SCARY_HEADING:
            put_fmt(sb, "%s\n", item->text);
            break;
          case SDT_PARA:
            put_fmt(sb, "%s\n", item->text);
            break;
          case SDT_DISPLAY:
            put_fmt(sb, "    %s\n", item->text);
            break;
          default:
            break;
        }
    }
    return strbuf_to_str(sb);
}

typedef struct {
    Seat *seat;
    char *host, *keytype, *keystr;
    int port;
    void (*callback)(void *ctx, SeatPromptResult result);
    void *ctx;
} HostKeyAsk;

static void hostkey_answered(void *vctx, int button)
{
    HostKeyAsk *a = (HostKeyAsk *)vctx;
    SeatPromptResult r = SPR_USER_ABORT;
    if (button == 0) {                                 /* Accept: remember the key */
        store_host_key(a->seat, a->host, a->port, a->keytype, a->keystr);
        r = SPR_OK;
    } else if (button == 1) {                          /* Connect Once */
        r = SPR_OK;
    }
    a->callback(a->ctx, r);
    sfree(a->host);
    sfree(a->keytype);
    sfree(a->keystr);
    sfree(a);
}

SeatPromptResult roseat_confirm_host_key(
    void *owner, Seat *seat, const char *host, int port, const char *keytype, char *keystr,
    SeatDialogText *text, void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    static const char *const buttons[] = { "Accept", "Connect Once", "Cancel" };
    const char *title;
    char *body = dialog_text(text, &title);
    HostKeyAsk *a = snew(HostKeyAsk);

    a->seat = seat;
    a->host = dupstr(host);
    a->port = port;
    a->keytype = dupstr(keytype);
    a->keystr = dupstr(keystr);
    a->callback = callback;
    a->ctx = ctx;
    ro_msgbox(owner, title ? title : "PuTTY Security Alert", body, buttons, 3, 2, 2, hostkey_answered, a);
    sfree(body);
    return SPR_INCOMPLETE;             /* the answer comes later, by the callback */
}

static SeatPromptResult rs_confirm_ssh_host_key(
    Seat *seat, const char *host, int port, const char *keytype, char *keystr,
    SeatDialogText *text, HelpCtx helpctx,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    SEAT(seat);
    return roseat_confirm_host_key(t, seat, host, port, keytype, keystr, text, callback, ctx);
}

typedef struct {
    void (*callback)(void *ctx, SeatPromptResult result);
    void *ctx;
} YesNoAsk;

static void yesno_answered(void *vctx, int button)
{
    YesNoAsk *a = (YesNoAsk *)vctx;
    a->callback(a->ctx, button == 0 ? SPR_OK : SPR_USER_ABORT);
    sfree(a);
}

SeatPromptResult roseat_confirm_yesno(void *owner, SeatDialogText *text,
                                      void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    static const char *const buttons[] = { "Yes", "No" };
    const char *title;
    char *body = dialog_text(text, &title);
    YesNoAsk *a = snew(YesNoAsk);
    a->callback = callback;
    a->ctx = ctx;
    ro_msgbox(owner, title ? title : "PuTTY Security Alert", body, buttons, 2, 1, 1, yesno_answered, a);
    sfree(body);
    return SPR_INCOMPLETE;
}

static SeatPromptResult rs_confirm_weak_crypto_primitive(
    Seat *seat, SeatDialogText *text, void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    SEAT(seat);
    return roseat_confirm_yesno(t, text, callback, ctx);
}

static SeatPromptResult rs_confirm_weak_cached_hostkey(
    Seat *seat, SeatDialogText *text, void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    SEAT(seat);
    return roseat_confirm_yesno(t, text, callback, ctx);
}

const SeatVtable roterm_seat_vt = {
    .output = rs_output,
    .eof = rs_eof,
    .sent = nullseat_sent,
    .banner = nullseat_banner_to_stderr,
    .get_userpass_input = rs_get_userpass_input,
    .notify_session_started = nullseat_notify_session_started,
    .notify_remote_exit = rs_notify_remote_exit,
    .notify_remote_disconnect = nullseat_notify_remote_disconnect,
    .connection_fatal = seat_connection_fatal_,
    .nonfatal = seat_nonfatal_,
    .update_specials_menu = rs_update_specials_menu,
    .get_ttymode = rs_get_ttymode,
    .set_busy_status = rs_set_busy_status,
    .confirm_ssh_host_key = rs_confirm_ssh_host_key,
    .confirm_weak_crypto_primitive = rs_confirm_weak_crypto_primitive,
    .confirm_weak_cached_hostkey = rs_confirm_weak_cached_hostkey,
    .prompt_descriptions = rs_prompt_descriptions,
    .is_utf8 = rs_is_utf8,
    .echoedit_update = nullseat_echoedit_update,
    .get_display = nullseat_get_display,
    .get_windowid = nullseat_get_windowid,
    .get_window_pixel_size = rs_get_window_pixel_size,
    .stripctrl_new = rs_stripctrl_new,
    .set_trust_status = rs_set_trust_status,
    .can_set_trust_status = rs_can_set_trust_status,
    .has_mixed_input_stream = nullseat_has_mixed_input_stream_yes,
    .verbose = nullseat_verbose_yes,
    .interactive = nullseat_interactive_yes,
    .get_cursor_position = rs_get_cursor_position,
};

/* ------------------------------------------------------------------------ */
/* the LogPolicy                                                            */

static void lp_eventlog_(LogPolicy *lp, const char *string)
{
    RoTerm *t = container_of(lp, RoTerm, logpolicy);
    roterm_log_event(t, string);
    /* also kept in a system variable, which is handy when debugging */
    _swix(OS_SetVarVal, _INR(0, 4), "PuTTY$LastEvent", string, (int)strlen(string), 0, 4);
}

typedef struct {
    void (*callback)(void *ctx, int result);
    void *ctx;
} AppendAsk;

static void append_answered(void *vctx, int button)
{
    AppendAsk *a = (AppendAsk *)vctx;
    /* buttons: Overwrite, Append, Cancel -> 2, 1, 0 */
    a->callback(a->ctx, button == 0 ? 2 : button == 1 ? 1 : 0);
    sfree(a);
}

static int lp_askappend_(LogPolicy *lp, Filename *filename, void (*callback)(void *ctx, int result), void *ctx)
{
    RoTerm *t = container_of(lp, RoTerm, logpolicy);
    static const char *const buttons[] = { "Overwrite", "Append", "Don't log" };
    char *msg = dupprintf("The session log file \"%s\" already exists.\n"
                          "You can overwrite it with a new session log, append your session "
                          "log to the end of it, or disable session logging for this session.",
                          filename->path);
    AppendAsk *a = snew(AppendAsk);
    a->callback = callback;
    a->ctx = ctx;
    ro_msgbox(t, "PuTTY Log to File", msg, buttons, 3, 1, 2, append_answered, a);
    sfree(msg);
    return -1;                         /* answer to follow by the callback */
}

static void lp_logging_error_(LogPolicy *lp, const char *event)
{
    RoTerm *t = container_of(lp, RoTerm, logpolicy);
    /* Send 'can't open log file' errors to the terminal window. */
    seat_stderr_pl(&t->seat, ptrlen_from_asciz(event));
    seat_stderr_pl(&t->seat, PTRLEN_LITERAL("\r\n"));
}

const LogPolicyVtable roterm_logpolicy_vt = {
    .eventlog = lp_eventlog_,
    .askappend = lp_askappend_,
    .logging_error = lp_logging_error_,
    .verbose = null_lp_verbose_yes,
};
