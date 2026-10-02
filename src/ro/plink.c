/*
 * plink.c - Plink, the command-line (stdin/stdout) variant of PuTTY, for
 * RISC OS.
 *
 * Based on upstream's unix/plink.c.  There is no tty line discipline, no
 * signals and no select() on stdin here: the event loop waits for network
 * activity with Socket_Select (see net.c) and polls the keyboard with
 * OS_Byte 129 every few centiseconds.
 *
 * This is mostly a bring-up and test tool for the shared platform layer, but it
 * is a useful program in its own right for running remote commands.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include <unistd.h>

#include "putty.h"
#include "ssh.h"
#include "storage.h"
#include "tree234.h"
#include "ro.h"
#include "ronet.h"
#include "roconsole.h"

#define MAX_STDIN_BACKLOG 4096

/* Connection sharing is not supported here (see stubs.c); each tool says so for itself. */
const bool share_can_be_downstream = false;
const bool share_can_be_upstream = false;

static LogContext *logctx;

void cmdline_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    console_print_error_msg_fmt_v("plink", fmt, ap);
    va_end(ap);
    exit(1);
}

static bool stdin_is_tty;
static uint32_t test_deadline;         /* ro_time() to give up at (for tests) */

static Backend *backend;
static Conf *conf;

/*
 * Default settings that are specific to RISC OS plink.
 */
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

FontSpec *platform_default_fontspec(const char *name)
{
    return fontspec_new_default();
}

Filename *platform_default_filename(const char *name)
{
    if (!strcmp(name, "LogFileName"))
        return filename_from_str("putty.log");
    else
        return filename_from_str("");
}

static void plink_echoedit_update(Seat *seat, bool echo, bool edit)
{
    /* Nothing to do: the keyboard is read raw and the remote end echoes. */
}

static char *plink_get_ttymode(Seat *seat, const char *mode)
{
    return NULL;
}

static bool seen_stdin_eof = false;
static enum { EOF_NO, EOF_PENDING, EOF_SENT } outgoingeof;
static BinarySink *stdout_bs, *stderr_bs;
static bufchain_sink stdout_bcs, stderr_bcs;
static bufchain stdout_data, stderr_data;
static StripCtrlChars *stdout_scc, *stderr_scc;

static size_t output_backlog(void)
{
    return bufchain_size(&stdout_data) + bufchain_size(&stderr_data);
}

static void try_output(bool is_stderr)
{
    bufchain *chain = (is_stderr ? &stderr_data : &stdout_data);
    FILE *fp = is_stderr ? stderr : stdout;

    while (bufchain_size(chain) > 0) {
        ptrlen senddata = bufchain_prefix(chain);
        fwrite(senddata.ptr, 1, senddata.len, fp);
        bufchain_consume(chain, senddata.len);
    }
    fflush(fp);

    backend_unthrottle(backend, output_backlog());

    if (outgoingeof == EOF_PENDING && bufchain_size(&stdout_data) == 0)
        outgoingeof = EOF_SENT;
}

static size_t plink_output(
    Seat *seat, SeatOutputType type, const void *data, size_t len)
{
    bool is_stderr = type != SEAT_OUTPUT_STDOUT;
    assert(is_stderr || outgoingeof == EOF_NO);

    BinarySink *bs = is_stderr ? stderr_bs : stdout_bs;
    put_data(bs, data, len);

    try_output(is_stderr);
    return output_backlog();
}

static bool plink_eof(Seat *seat)
{
    assert(outgoingeof == EOF_NO);
    outgoingeof = EOF_PENDING;
    try_output(false);
    return false;   /* do not respond to incoming EOF with outgoing */
}

static SeatPromptResult plink_get_userpass_input(Seat *seat, prompts_t *p)
{
    /* Plink doesn't support Restart Session, so we can just have a single
     * static cmdline_get_passwd_input_state that's never reset */
    static cmdline_get_passwd_input_state cmdline_state =
        CMDLINE_GET_PASSWD_INPUT_STATE_INIT;

    SeatPromptResult spr;
    spr = cmdline_get_passwd_input(p, &cmdline_state, false);
    if (spr.kind == SPRK_INCOMPLETE)
        spr = console_get_userpass_input(p);
    return spr;
}

static bool plink_seat_interactive(Seat *seat)
{
    return (!*conf_get_str_ambi(conf, CONF_remote_cmd, NULL) &&
            !*conf_get_str_ambi(conf, CONF_remote_cmd2, NULL) &&
            !*conf_get_str(conf, CONF_ssh_nc_host));
}

static const SeatVtable plink_seat_vt = {
    .output = plink_output,
    .eof = plink_eof,
    .sent = nullseat_sent,
    .banner = nullseat_banner_to_stderr,
    .get_userpass_input = plink_get_userpass_input,
    .notify_session_started = nullseat_notify_session_started,
    .notify_remote_exit = nullseat_notify_remote_exit,
    .notify_remote_disconnect = nullseat_notify_remote_disconnect,
    .connection_fatal = console_connection_fatal,
    .nonfatal = console_nonfatal,
    .update_specials_menu = nullseat_update_specials_menu,
    .get_ttymode = plink_get_ttymode,
    .set_busy_status = nullseat_set_busy_status,
    .confirm_ssh_host_key = console_confirm_ssh_host_key,
    .confirm_weak_crypto_primitive = console_confirm_weak_crypto_primitive,
    .confirm_weak_cached_hostkey = console_confirm_weak_cached_hostkey,
    .prompt_descriptions = console_prompt_descriptions,
    .is_utf8 = nullseat_is_never_utf8,
    .echoedit_update = plink_echoedit_update,
    .get_display = nullseat_get_display,
    .get_windowid = nullseat_get_windowid,
    .get_window_pixel_size = nullseat_get_window_pixel_size,
    .stripctrl_new = console_stripctrl_new,
    .set_trust_status = console_set_trust_status,
    .can_set_trust_status = console_can_set_trust_status,
    .has_mixed_input_stream = console_has_mixed_input_stream,
    .verbose = cmdline_seat_verbose,
    .interactive = plink_seat_interactive,
    .get_cursor_position = nullseat_get_cursor_position,
};
static Seat plink_seat[1] = {{ &plink_seat_vt }};

/*
 * Keyboard input.  OS_Byte 129 with a zero timeout is INKEY(0): R2 comes back
 * as 0 if a key was waiting (R1 holds it), 255 if not, and 27 for Escape.
 * Special keys arrive as codes 128-255; we translate the cursor keys, and
 * Return becomes CR as a terminal would send it.
 */
static void send_tty_key(int c)
{
    const char *seq = NULL;
    char one;

    switch (c) {
      case 13: seq = "\r"; break;
      case 8:  seq = "\177"; break;
      case 136: seq = "\033[D"; break;     /* Left */
      case 137: seq = "\033[C"; break;     /* Right */
      case 138: seq = "\033[B"; break;     /* Down */
      case 139: seq = "\033[A"; break;     /* Up */
      default:
        if (c >= 128)
            return;                    /* other special keys: ignore for now */
        one = (char)c;
        seq = &one;
        backend_send(backend, seq, 1);
        return;
    }
    backend_send(backend, seq, strlen(seq));
}

static void poll_stdin(void)
{
    if (seen_stdin_eof || !backend_connected(backend) ||
        !backend_sendok(backend))
        return;

    if (stdin_is_tty) {
        int i;
        for (i = 0; i < 64 && backend_sendbuffer(backend) < MAX_STDIN_BACKLOG;
             i++) {
            int key = 0, status = 255;
            if (_swix(OS_Byte, _INR(0, 2) | _OUTR(1, 2), 129, 0, 0,
                      &key, &status))
                break;
            if (status != 0)
                break;
            key &= 255;
            noise_ultralight(NOISE_SOURCE_KEY, key);
            send_tty_key(key);
        }
    } else if (backend_sendbuffer(backend) < MAX_STDIN_BACKLOG) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof buf, stdin);
        if (n > 0) {
            backend_send(backend, buf, n);
        } else {
            backend_special(backend, SS_EOF, 0);
            seen_stdin_eof = true;
        }
    }
}

/*
 * Short description of parameters.
 */
static void usage(void)
{
    printf("Plink: command-line connection utility (RISC OS)\n");
    printf("%s\n", ver);
    printf("Usage: plink [options] [user@]host [command]\n");
    printf("       (\"host\" can also be a PuTTY saved session name)\n");
    printf("Options:\n");
    printf("  -V        print version information and exit\n");
    printf("  -pgpfp    print PGP key fingerprints and exit\n");
    printf("  -v        show verbose messages\n");
    printf("  -load sessname  Load settings from saved session\n");
    printf("  -ssh -telnet -rlogin -raw\n");
    printf("            force use of a particular protocol\n");
    printf("  -P port   connect to specified port\n");
    printf("  -l user   connect with specified username\n");
    printf("  -batch    disable all interactive prompts\n");
    printf("The following options only apply to SSH connections:\n");
    printf("  -pw passw login with specified password\n");
    printf("  -pwfile file   login with password read from specified file\n");
    printf("  -L [listen-IP:]listen-port:host:port\n");
    printf("            Forward local port to remote address\n");
    printf("  -R [listen-IP:]listen-port:host:port\n");
    printf("            Forward remote port to local address\n");
    printf("  -t -T     enable / disable pty allocation\n");
    printf("  -1 -2     force use of particular SSH protocol version\n");
    printf("  -C        enable compression\n");
    printf("  -i key    private key file for user authentication\n");
    printf("  -hostkey keyid\n");
    printf("            manually specify a host key (may be repeated)\n");
    printf("  -m file   read remote command(s) from file\n");
    printf("  -s        remote command is an SSH subsystem (SSH-2 only)\n");
    printf("  -N        don't start a shell/command (SSH-2 only)\n");
    printf("  -sshlog file\n");
    printf("  -sshrawlog file\n");
    printf("            log protocol details to a file\n");
}

static void version(void)
{
    char *buildinfo_text = buildinfo("\n");
    printf("plink: %s\n%s\n", ver, buildinfo_text);
    sfree(buildinfo_text);
    exit(0);
}

void frontend_net_error_pending(void) {}

const unsigned cmdline_tooltype =
    TOOLTYPE_HOST_ARG |
    TOOLTYPE_HOST_ARG_CAN_BE_SESSION |
    TOOLTYPE_HOST_ARG_PROTOCOL_PREFIX |
    TOOLTYPE_HOST_ARG_FROM_LAUNCHABLE_LOAD;

int main(int argc, char **argv)
{
    int exitcode;
    bool errors;
    enum TriState sanitise_stdout = AUTO, sanitise_stderr = AUTO;
    bool use_subsystem = false;
    const struct BackendVtable *backvt;

    enable_dit();

    /*
     * Initialise port and protocol to sensible defaults.  (These will be
     * overridden by more or less anything.)
     */
    settings_set_default_protocol(PROT_SSH);
    settings_set_default_port(22);

    bufchain_init(&stdout_data);
    bufchain_init(&stderr_data);
    bufchain_sink_init(&stdout_bcs, &stdout_data);
    bufchain_sink_init(&stderr_bcs, &stderr_data);
    stdout_bs = BinarySink_UPCAST(&stdout_bcs);
    stderr_bs = BinarySink_UPCAST(&stderr_bcs);
    outgoingeof = EOF_NO;

    stderr_tty_init();
    stdin_is_tty = isatty(0);

    /*
     * Process the command line.
     */
    conf = conf_new();
    do_defaults(NULL, conf);
    settings_set_default_protocol(conf_get_int(conf, CONF_protocol));
    settings_set_default_port(conf_get_int(conf, CONF_port));
    errors = false;
    CmdlineArgList *arglist = cmdline_arg_list_from_argv(argc, argv);
    size_t arglistpos = 0;
    while (arglist->args[arglistpos]) {
        CmdlineArg *arg = arglist->args[arglistpos++];
        CmdlineArg *nextarg = arglist->args[arglistpos];
        const char *p = cmdline_arg_to_str(arg);
        int ret = cmdline_process_param(arg, nextarg, 1, conf);
        if (ret == -2) {
            fprintf(stderr,
                    "plink: option \"%s\" requires an argument\n", p);
            errors = true;
        } else if (ret == 2) {
            arglistpos++;
        } else if (ret == 1) {
            continue;
        } else if (!strcmp(p, "-s")) {
            /* Save status to write to conf later. */
            use_subsystem = true;
        } else if (!strcmp(p, "-V") || !strcmp(p, "--version")) {
            version();
        } else if (!strcmp(p, "--help")) {
            usage();
            exit(0);
        } else if (!strcmp(p, "-pgpfp")) {
            pgp_fingerprints();
            exit(0);
        } else if (!strcmp(p, "-sanitise-stdout") ||
                   !strcmp(p, "-sanitize-stdout")) {
            sanitise_stdout = FORCE_ON;
        } else if (!strcmp(p, "-no-sanitise-stdout") ||
                   !strcmp(p, "-no-sanitize-stdout")) {
            sanitise_stdout = FORCE_OFF;
        } else if (!strcmp(p, "-sanitise-stderr") ||
                   !strcmp(p, "-sanitize-stderr")) {
            sanitise_stderr = FORCE_ON;
        } else if (!strcmp(p, "-no-sanitise-stderr") ||
                   !strcmp(p, "-no-sanitize-stderr")) {
            sanitise_stderr = FORCE_OFF;
        } else if (!strcmp(p, "-no-antispoof")) {
            console_antispoof_prompt = false;
        } else if (*p != '-') {
            strbuf *cmdbuf = strbuf_new();

            while (arg) {
                if (cmdbuf->len > 0)
                    put_byte(cmdbuf, ' '); /* add space separator */
                put_dataz(cmdbuf, cmdline_arg_to_str(arg));
                arg = arglist->args[arglistpos++];
            }

            conf_set_str(conf, CONF_remote_cmd, cmdbuf->s);
            conf_set_str(conf, CONF_remote_cmd2, "");
            conf_set_bool(conf, CONF_nopty, true);  /* command => no tty */

            strbuf_free(cmdbuf);
            break;                     /* done with cmdline */
        } else {
            fprintf(stderr, "plink: unknown option \"%s\"\n", p);
            errors = true;
        }
    }

    if (errors)
        return 1;

    if (!cmdline_host_ok(conf)) {
        fprintf(stderr, "plink: no valid host name provided\n"
                "try \"plink --help\" for help\n");
        cmdline_arg_list_free(arglist);
        return 1;
    }

    prepare_session(conf);

    /* Perform command-line overrides on session configuration. */
    cmdline_run_saved(conf);

    cmdline_arg_list_free(arglist);

    /*
     * If we have no better ideas for the remote username, use the local one,
     * as 'ssh' does.  (On RISC OS that is only available if the user set it.)
     */
    if (conf_get_str_ambi(conf, CONF_username, NULL)[0] == '\0') {
        char *user = get_username();
        if (user) {
            conf_set_str(conf, CONF_username, user);
            sfree(user);
        }
    }

    /* Apply subsystem status. */
    if (use_subsystem)
        conf_set_bool(conf, CONF_ssh_subsys, true);

    /*
     * Select protocol.  This is farmed out into a table in a separate file to
     * enable an ssh-free variant.
     */
    backvt = backend_vt_from_proto(conf_get_int(conf, CONF_protocol));
    if (!backvt) {
        fprintf(stderr,
                "Internal fault: Unsupported protocol found\n");
        return 1;
    }

    if (backvt->flags & BACKEND_NEEDS_TERMINAL) {
        fprintf(stderr,
                "Plink doesn't support %s, which needs terminal emulation\n",
                backvt->displayname_lc);
        return 1;
    }

    /*
     * Remote output goes straight to the RISC OS screen, where the VDU drivers
     * would act on control codes - including ones that change the screen mode
     * or redefine characters.  So unless told otherwise, strip control
     * characters from anything shown on a terminal.
     */
    if (sanitise_stdout == FORCE_ON ||
        (sanitise_stdout == AUTO && isatty(1))) {
        stdout_scc = stripctrl_new(stdout_bs, true, L'\0');
        stdout_bs = BinarySink_UPCAST(stdout_scc);
    }
    if (sanitise_stderr == FORCE_ON ||
        (sanitise_stderr == AUTO && isatty(2))) {
        stderr_scc = stripctrl_new(stderr_bs, true, L'\0');
        stderr_bs = BinarySink_UPCAST(stderr_scc);
    }

    sk_init();
    uxsel_init();

    if (ro_entropy_is_weak() && !getenv("PuTTY$NoEntropyWarning"))
        fprintf(stderr,
                "plink: warning: the CryptRandom module is not loaded and could not\n"
                "plink: be found in System:Modules, so this system has no strong\n"
                "plink: source of random numbers. Session keys will be weaker than\n"
                "plink: they should be. Install the CryptRandom package with\n"
                "plink: !PackMan (System section).\n");

    /*
     * Plink doesn't provide any way to add forwardings after the connection is
     * set up, so if there are none now, we can safely set the "simple" flag.
     */
    if (conf_get_int(conf, CONF_protocol) == PROT_SSH &&
        !conf_get_bool(conf, CONF_x11_forward) &&
        !conf_get_bool(conf, CONF_agentfwd) &&
        !conf_get_str_nthstrkey(conf, CONF_portfwd, 0))
        conf_set_bool(conf, CONF_ssh_simple, true);

    /*
     * Start up the connection.
     */
    logctx = log_init(console_cli_logpolicy, conf);
    {
        char *error, *realhost;
        /* nodelay is only useful if stdin is a terminal device */
        bool nodelay = conf_get_bool(conf, CONF_tcp_nodelay) && stdin_is_tty;

        error = backend_init(backvt, plink_seat, &backend, logctx, conf,
                             conf_get_str(conf, CONF_host),
                             conf_get_int(conf, CONF_port),
                             &realhost, nodelay,
                             conf_get_bool(conf, CONF_tcp_keepalives));
        if (error) {
            fprintf(stderr, "Unable to open connection:\n%s\n", error);
            sfree(error);
            return 1;
        }
        ldisc_create(conf, NULL, backend, plink_seat);
        sfree(realhost);
    }

    seat_echoedit_update(plink_seat, 1, 1);

    /*
     * The main loop: wait for network activity (or the next timer, or a short
     * while if the keyboard needs polling), then let PuTTY's callbacks run.
     */
    {
        const char *t = getenv("PuTTY$Timeout");
        if (t && atoi(t) > 0)
            test_deadline = ro_time() + (uint32_t)atoi(t) * 100;
    }

    for (;;) {
        unsigned long now = GETTICKCOUNT(), next;
        int timeout_cs;

        if (test_deadline && (int32_t)(ro_time() - test_deadline) > 0) {
            fprintf(stderr, "plink: gave up after PuTTY$Timeout seconds\n");
            cleanup_exit(3);
        }

        if (toplevel_callback_pending()) {
            timeout_cs = 0;
        } else if (run_timers(now, &next)) {
            long ticks = (long)(next - now);
            timeout_cs = ticks <= 0 ? 0 : (int)(ticks / 10) + 1;
        } else {
            timeout_cs = 100;
        }
        if (timeout_cs > 100)
            timeout_cs = 100;
        if (stdin_is_tty && !seen_stdin_eof && timeout_cs > 5)
            timeout_cs = 5;
        if (ro_net_busy() && timeout_cs > 2)
            timeout_cs = 2;

        ro_net_poll(timeout_cs);
        poll_stdin();
        run_toplevel_callbacks();

        if (!backend_connected(backend) &&
            bufchain_size(&stdout_data) == 0 &&
            bufchain_size(&stderr_data) == 0)
            break;
    }

    exitcode = backend_exitcode(backend);
    if (exitcode < 0) {
        fprintf(stderr, "Remote process exit code unavailable\n");
        exitcode = 1;                  /* this is an error condition */
    }
    cleanup_exit(exitcode);
    return exitcode;                   /* shouldn't happen, but placates gcc */
}
