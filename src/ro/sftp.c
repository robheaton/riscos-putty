/*
 * sftp.c: the RISC OS parts of PSFTP and PSCP that are not the local file layer (rofile.c):
 * the wait for network activity or a key, and the command line reader.
 *
 * Based on upstream's unix/sftp.c, which has the same job; psftp.c and pscp.c are used
 * as upstream has them apart from the few lines patches/psftp.patch and patches/pscp.patch
 * change (see rosftp.h and docs/DESIGN.md for why).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>

#include "putty.h"
#include "ssh.h"
#include "psftp.h"
#include "ro.h"
#include "ronet.h"
#include "rosftp.h"

/* ----------------------------------------------------------------------
 * Waiting for the network or a key, and reading a command
 */

static bool stdin_tty;
static int pending_key = -1;
static uint32_t test_deadline;         /* ro_time() to give up at, from PuTTY$Timeout (for tests) */

/* OS_Byte 129 with no timeout: the key waiting, or -1. */
static int read_key(void)
{
    int key = 0, status = 255, k = ro_test_key();

    if (k != -2)
        return k;                      /* PuTTY$KeyFile: input from a file (tests) */

    if (_swix(OS_Byte, _INR(0, 2) | _OUTR(1, 2), 129, 0, 0, &key, &status))
        return -1;
    if (status != 0)
        return -1;
    return key & 255;
}

/*
 * Wait for something to happen: network data (or a timer, or a callback) - then return 0
 * - or, if include_stdin and the keyboard is the input, a key, which is left in
 * pending_key and gives 1.  Returns -1 if there is no connection left to wait for.
 */
static int wait_for_activity(bool include_stdin, bool no_fds_ok)
{
    for (;;) {
        unsigned long now, next;
        int timeout_cs, n, state, rwx;
        bool ran;

        if (test_deadline && (int32_t)(ro_time() - test_deadline) > 0) {
            fprintf(stderr, "%s: gave up after PuTTY$Timeout seconds\n", appname);
            cleanup_exit(3);
        }
        if (!no_fds_ok && !toplevel_callback_pending() && !ro_net_busy() &&
            first_fd(&state, &rwx) < 0)
            return -1;

        now = GETTICKCOUNT();
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
        if (include_stdin && timeout_cs > 5)
            timeout_cs = 5;
        if (ro_net_busy() && timeout_cs > 2)
            timeout_cs = 2;

        n = ro_net_poll(timeout_cs);
        ran = run_toplevel_callbacks();
        if (include_stdin) {
            if (pending_key < 0)
                pending_key = read_key();
            if (pending_key >= 0)
                return 1;
        }
        if (n > 0 || ran)
            return 0;
    }
}

int ssh_sftp_loop_iteration(void)
{
    return wait_for_activity(false, false);
}

/* Commands typed at the keyboard are kept, newest last, for the cursor keys to bring back. */
#define HISTORY 32
static char *history[HISTORY];
static int nhistory;

static void remember(const char *line)
{
    if (!*line)
        return;
    if (nhistory > 0 && !strcmp(history[nhistory - 1], line))
        return;
    if (nhistory == HISTORY) {
        sfree(history[0]);
        memmove(history, history + 1, (HISTORY - 1) * sizeof history[0]);
        nhistory--;
    }
    history[nhistory++] = dupstr(line);
}

static void erase_echo(size_t n)
{
    while (n--)
        fputs("\b \b", stdout);
}

static char *read_line_from_keyboard(bool no_fds_ok)
{
    size_t len = 0, cap = 256;
    char *line = snewn(cap, char);
    int hist = nhistory;

    line[0] = '\0';
    for (;;) {
        int r = wait_for_activity(true, no_fds_ok), c;

        if (r < 0) {
            printf("connection died\n");
            sfree(line);
            return NULL;
        }
        if (r == 0)
            continue;
        c = pending_key;
        pending_key = -1;
        noise_ultralight(NOISE_SOURCE_KEY, (unsigned long)c);

        if (c == 13 || c == 10) {
            fputs("\n", stdout);
            fflush(stdout);
            remember(line);
            line[len++] = '\n';
            line[len] = '\0';
            return line;
        } else if (c == 4 && len == 0) {           /* Ctrl-D at the start of a line: end of input */
            sfree(line);
            return NULL;
        } else if (c == 8 || c == 127) {
            if (len > 0) {
                line[--len] = '\0';
                erase_echo(1);
            }
        } else if (c == 21) {                      /* Ctrl-U */
            erase_echo(len);
            len = 0;
            line[0] = '\0';
        } else if (c == 139 || c == 138) {         /* up, down: the history */
            if (c == 139 && hist > 0)
                hist--;
            else if (c == 138 && hist < nhistory)
                hist++;
            else
                continue;
            erase_echo(len);
            len = 0;
            if (hist < nhistory) {
                size_t n = strlen(history[hist]);
                while (len + n + 2 > cap)
                    cap *= 2, line = sresize(line, cap, char);
                memcpy(line, history[hist], n + 1);
                len = n;
                fputs(line, stdout);
            } else {
                line[0] = '\0';
            }
        } else if (c >= 32 && c != 127 && c < 256 && !(c >= 128 && c < 160)) {
            if (len + 3 > cap)
                cap *= 2, line = sresize(line, cap, char);
            line[len++] = (char)c;
            line[len] = '\0';
            fputc(c, stdout);
        }
        fflush(stdout);
    }
}

static char *read_line_from_file(void)
{
    size_t len = 0, cap = 256;
    char *line = snewn(cap, char);
    int c;

    while ((c = fgetc(stdin)) != EOF) {
        if (len + 2 > cap)
            cap *= 2, line = sresize(line, cap, char);
        line[len++] = (char)c;
        if (c == '\n')
            break;
    }
    if (len == 0) {
        sfree(line);
        return NULL;
    }
    line[len] = '\0';
    return line;
}

/*
 * Read a command.  Like upstream's the result ends in a newline, so that an empty line is
 * not mistaken for the end of the input; NULL means end of input or a dead connection.
 */
char *ssh_sftp_get_cmdline(const char *prompt, bool no_fds_ok)
{
    fputs(prompt, stdout);
    fflush(stdout);
    if (stdin_tty)
        return read_line_from_keyboard(no_fds_ok);
    return read_line_from_file();
}

/* ----------------------------------------------------------------------
 * Odds and ends upstream's Unix file has
 */

char *x_get_default(const char *key)
{
    return NULL;
}

char *platform_default_s(const char *name)
{
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

SeatPromptResult filexfer_get_userpass_input(Seat *seat, prompts_t *p)
{
    /* The file transfer tools don't support Restart Session, so we can just have a
     * single static cmdline_get_passwd_input_state that's never reset */
    static cmdline_get_passwd_input_state cmdline_state =
        CMDLINE_GET_PASSWD_INPUT_STATE_INIT;
    SeatPromptResult spr;

    spr = cmdline_get_passwd_input(p, &cmdline_state, false);
    if (spr.kind == SPRK_INCOMPLETE)
        spr = console_get_userpass_input(p);
    return spr;
}

void frontend_net_error_pending(void)
{
}

void platform_psftp_pre_conn_setup(LogPolicy *lp)
{
    if (ro_entropy_is_weak() && !getenv("PuTTY$NoEntropyWarning"))
        fprintf(stderr,
                "%s: warning: the CryptRandom module is not loaded and could not be found in\n"
                "%s: System:Modules, so this system has no strong source of random numbers.\n"
                "%s: Session keys will be weaker than they should be. Install the CryptRandom\n"
                "%s: package with !PackMan (System section).\n",
                appname, appname, appname, appname);
}

const bool buildinfo_gtk_relevant = false;

int main(int argc, char **argv)
{
    const char *t = getenv("PuTTY$Timeout");

    enable_dit();
    stdin_tty = isatty(0);
    if (t && atoi(t) > 0)
        test_deadline = ro_time() + (uint32_t)atoi(t) * 100;
    uxsel_init();
    CmdlineArgList *arglist = cmdline_arg_list_from_argv(argc, argv);
    return psftp_main(arglist);
}
