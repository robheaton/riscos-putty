/*
 * console.c: various interactive-prompt routines shared between the RISC OS
 * console PuTTY tools.
 *
 * Based on upstream's unix/console.c.  The prompting logic is unchanged; what
 * differs is the plumbing.  Unix puts the terminal into the right mode with
 * termios; here the keyboard is read a key at a time with OS_ReadC and echoing
 * is done by hand, which is how one gets a no-echo password prompt on RISC OS.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>

#include "putty.h"
#include "storage.h"
#include "ssh.h"
#include "console.h"
#include "ro.h"
#include "roconsole.h"

void stderr_tty_init(void)
{
}

void cleanup_exit(int code)
{
    /* Clean up. */
    sk_cleanup();
    random_save_seed();
    exit(code);
}

void console_print_error_msg(const char *prefix, const char *msg)
{
    fputs(prefix, stderr);
    fputs(": ", stderr);
    fputs(msg, stderr);
    fputc('\n', stderr);
    fflush(stderr);
}

/*
 * One character from the keyboard: OS_ReadC, which waits for ever - or, if the system
 * variable PuTTY$Timeout is set (a number of seconds, for tests), a key that does not
 * come within that time counts as Escape, so that a test script cannot hang on a prompt
 * nobody will answer.  Returns -1 for Escape (or an error).
 */
static int read_char(void)
{
    const char *t = getenv("PuTTY$Timeout");
    int limit = t ? atoi(t) : 0;
    int k = ro_test_key();
    uint32_t start;

    if (k != -2)
        return k;                      /* PuTTY$KeyFile: input from a file (tests) */
    if (limit <= 0)
        return _kernel_osrdch();
    start = ro_time();
    for (;;) {
        int key = 0, status = 255;

        /* INKEY(20): wait up to 0.2 s for a key */
        if (_swix(OS_Byte, _INR(0, 2) | _OUTR(1, 2), 129, 20, 0, &key, &status))
            return -1;
        if (status == 0)
            return key & 255;
        if (status == 27)
            return -1;
        if ((int32_t)(ro_time() - start) > limit * 100)
            return -1;
    }
}

/*
 * Read one line of input.  From the keyboard if stdin is a terminal (with
 * line-editing and, if echo is false, no echo at all); from stdin otherwise.
 * Returns the length of the line, or -1 on end of file or Escape.
 */
int ro_read_line(char *buf, size_t size, bool echo)
{
    size_t n = 0;

    if (size == 0)
        return -1;

    if (!isatty(0)) {
        if (!fgets(buf, (int)size, stdin))
            return -1;
        n = strcspn(buf, "\r\n");
        buf[n] = '\0';
        return (int)n;
    }

    for (;;) {
        int c = read_char();
        if (c < 0) {
            buf[n] = '\0';
            return -1;                 /* Escape pressed, or an error */
        }
        if (c == 13 || c == 10)
            break;
        if (c == 8 || c == 127) {      /* Backspace or Delete */
            if (n > 0) {
                n--;
                if (echo) {
                    fputs("\b \b", stderr);
                    fflush(stderr);
                }
            }
            continue;
        }
        if (c == 21) {                 /* Ctrl-U: kill the line */
            while (n > 0) {
                n--;
                if (echo)
                    fputs("\b \b", stderr);
            }
            fflush(stderr);
            continue;
        }
        if (c < 32 || c > 255)
            continue;                  /* function keys, cursor keys, ... */
        if (n + 1 < size) {
            buf[n++] = (char)c;
            if (echo) {
                fputc(c, stderr);
                fflush(stderr);
            }
        }
    }
    buf[n] = '\0';
    fputc('\n', stderr);
    fflush(stderr);
    return (int)n;
}

/*
 * Helper function to print the message from a SeatDialogText.  Returns the
 * final prompt to print on the input line, or NULL if a batch-mode abort is
 * needed.  In the latter case it will have printed the abort text already.
 */
static const char *console_print_seatdialogtext(SeatDialogText *text)
{
    const char *prompt = NULL;
    stdio_sink errsink[1];
    stdio_sink_init(errsink, stderr);

    for (SeatDialogTextItem *item = text->items,
             *end = item+text->nitems; item < end; item++) {
        switch (item->type) {
          case SDT_PARA:
            wordwrap(BinarySink_UPCAST(errsink),
                     ptrlen_from_asciz(item->text), 60);
            fputc('\n', stderr);
            break;
          case SDT_DISPLAY:
            fprintf(stderr, "  %s\n", item->text);
            break;
          case SDT_SCARY_HEADING:
            /* Can't change font size or weight in this context */
            fprintf(stderr, "%s\n", item->text);
            break;
          case SDT_BATCH_ABORT:
            if (console_batch_mode) {
                fprintf(stderr, "%s\n", item->text);
                fflush(stderr);
                return NULL;
            }
            break;
          case SDT_PROMPT:
            prompt = item->text;
            break;
          default:
            break;
        }
    }

    assert(prompt); /* something in the SeatDialogText should have set this */
    return prompt;
}

SeatPromptResult console_confirm_ssh_host_key(
    Seat *seat, const char *host, int port, const char *keytype,
    char *keystr, SeatDialogText *text, HelpCtx helpctx,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    char line[32];

    const char *prompt = console_print_seatdialogtext(text);
    if (!prompt)
        return SPR_SW_ABORT("Cannot confirm a host key in batch mode");

    while (true) {
        fprintf(stderr,
                "%s (y/n, Return cancels connection, i for more info) ",
                prompt);
        fflush(stderr);

        line[0] = '\0';
        ro_read_line(line, sizeof(line), true);

        if (line[0] == 'i' || line[0] == 'I') {
            for (SeatDialogTextItem *item = text->items,
                     *end = item+text->nitems; item < end; item++) {
                switch (item->type) {
                  case SDT_MORE_INFO_KEY:
                    fprintf(stderr, "%s", item->text);
                    break;
                  case SDT_MORE_INFO_VALUE_SHORT:
                    fprintf(stderr, ": %s\n", item->text);
                    break;
                  case SDT_MORE_INFO_VALUE_BLOB:
                    fprintf(stderr, ":\n%s\n", item->text);
                    break;
                  default:
                    break;
                }
            }
        } else {
            break;
        }
    }

    /* In case of misplaced reflexes from another program, also recognise 'q'
     * as 'abandon connection rather than trust this key' */
    if (line[0] != '\0' && line[0] != '\r' && line[0] != '\n' &&
        line[0] != 'q' && line[0] != 'Q') {
        if (line[0] == 'y' || line[0] == 'Y')
            store_host_key(seat, host, port, keytype, keystr);
        return SPR_OK;
    } else {
        fputs(console_abandoned_msg, stderr);
        return SPR_USER_ABORT;
    }
}

SeatPromptResult console_confirm_weak_crypto_primitive(
    Seat *seat, SeatDialogText *text,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    char line[32];

    const char *prompt = console_print_seatdialogtext(text);
    if (!prompt)
        return SPR_SW_ABORT("Cannot confirm a weak crypto primitive "
                            "in batch mode");

    fprintf(stderr, "%s (y/n) ", prompt);
    fflush(stderr);

    line[0] = '\0';
    ro_read_line(line, sizeof(line), true);

    if (line[0] == 'y' || line[0] == 'Y') {
        return SPR_OK;
    } else {
        fputs(console_abandoned_msg, stderr);
        return SPR_USER_ABORT;
    }
}

SeatPromptResult console_confirm_weak_cached_hostkey(
    Seat *seat, SeatDialogText *text,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    char line[32];

    const char *prompt = console_print_seatdialogtext(text);
    if (!prompt)
        return SPR_SW_ABORT("Cannot confirm a weak cached host key "
                            "in batch mode");

    fprintf(stderr, "%s (y/n) ", prompt);
    fflush(stderr);

    line[0] = '\0';
    ro_read_line(line, sizeof(line), true);

    if (line[0] == 'y' || line[0] == 'Y') {
        return SPR_OK;
    } else {
        fputs(console_abandoned_msg, stderr);
        return SPR_USER_ABORT;
    }
}

/*
 * Ask whether to wipe a session log file before writing to it.
 * Returns 2 for wipe, 1 for append, 0 for cancel (don't log).
 */
int console_askappend(LogPolicy *lp, Filename *filename,
                      void (*callback)(void *ctx, int result), void *ctx)
{
    static const char msgtemplate[] =
        "The session log file \"%.*s\" already exists.\n"
        "You can overwrite it with a new session log,\n"
        "append your session log to the end of it,\n"
        "or disable session logging for this session.\n"
        "Enter \"y\" to wipe the file, \"n\" to append to it,\n"
        "or just press Return to disable logging.\n"
        "Wipe the log file? (y/n, Return cancels logging) ";

    static const char msgtemplate_batch[] =
        "The session log file \"%.*s\" already exists.\n"
        "Logging will not be enabled.\n";

    char line[32];

    if (console_batch_mode) {
        fprintf(stderr, msgtemplate_batch, FILENAME_MAX, filename->path);
        fflush(stderr);
        return 0;
    }
    fprintf(stderr, msgtemplate, FILENAME_MAX, filename->path);
    fflush(stderr);

    line[0] = '\0';
    ro_read_line(line, sizeof(line), true);

    if (line[0] == 'y' || line[0] == 'Y')
        return 2;
    else if (line[0] == 'n' || line[0] == 'N')
        return 1;
    else
        return 0;
}

bool console_antispoof_prompt = true;

void console_set_trust_status(Seat *seat, bool trusted)
{
    /* Do nothing in response to a change of trust status, because there's
     * nothing we can do in a console environment.  However, the query function
     * below will make a fiddly decision about whether to tell the backend to
     * enable fallback handling. */
}

bool console_can_set_trust_status(Seat *seat)
{
    if (console_batch_mode) {
        /*
         * In batch mode, we don't need to worry about the server mimicking our
         * interactive authentication, because the user already knows not to
         * expect any.
         */
        return true;
    }

    return false;
}

bool console_has_mixed_input_stream(Seat *seat)
{
    if (!is_interactive() || !console_antispoof_prompt) {
        /*
         * If standard input isn't connected to a terminal, then even if the
         * server did send a spoof authentication prompt, the user couldn't
         * respond to it via the terminal anyway.
         *
         * We also pretend this is true if the user has purposely disabled the
         * antispoof prompt.
         */
        return false;
    }

    return true;
}

/*
 * Warn about the obsolescent key file format.
 */
void old_keyfile_warning(void)
{
    static const char message[] =
        "You are loading an SSH-2 private key which has an\n"
        "old version of the file format. This means your key\n"
        "file is not fully tamperproof. Future versions of\n"
        "PuTTY may stop supporting this private key format,\n"
        "so we recommend you convert your key to the new\n"
        "format.\n"
        "\n"
        "Once the key is loaded into PuTTYgen, you can perform\n"
        "this conversion simply by saving it again.\n";

    fputs(message, stderr);
}

void console_logging_error(LogPolicy *lp, const char *string)
{
    /* Errors setting up logging are considered important, so they're
     * displayed to standard error even when not in verbose mode */
    fprintf(stderr, "%s\n", string);
    fflush(stderr);
}

void console_eventlog(LogPolicy *lp, const char *string)
{
    /* Ordinary Event Log entries are displayed in the same way as logging
     * errors, but only in verbose mode */
    if (lp_verbose(lp))
        console_logging_error(lp, string);
}

StripCtrlChars *console_stripctrl_new(
    Seat *seat, BinarySink *bs_out, SeatInteractionContext sic)
{
    return stripctrl_new(bs_out, false, 0);
}

/*
 * Special functions to read and print to the console for password prompts and
 * the like.  Prompts go to stderr and answers come from the keyboard; the text
 * is also sanitised of escape sequences, on the basis that it might have been
 * sent by a hostile SSH server doing malicious keyboard-interactive.
 */
static void console_write(FILE *outfp, ptrlen data)
{
    fwrite(data.ptr, 1, data.len, outfp);
    fflush(outfp);
}

SeatPromptResult console_get_userpass_input(prompts_t *p)
{
    size_t curr_prompt;
    FILE *outfp = stderr;

    /* Zero all the results, in case we abort half-way through. */
    {
        size_t i;
        for (i = 0; i < p->n_prompts; i++)
            prompt_set_result(p->prompts[i], "");
    }

    if (p->n_prompts && console_batch_mode)
        return SPR_SW_ABORT("Cannot answer interactive prompts "
                            "in batch mode");

    /* Preamble. */
    /* We only print the `name' caption if we have to... */
    if (p->name_reqd && p->name) {
        ptrlen plname = ptrlen_from_asciz(p->name);
        console_write(outfp, plname);
        if (!ptrlen_endswith(plname, PTRLEN_LITERAL("\n"), NULL))
            console_write(outfp, PTRLEN_LITERAL("\n"));
    }
    /* ...but we always print any `instruction'. */
    if (p->instruction) {
        ptrlen plinst = ptrlen_from_asciz(p->instruction);
        console_write(outfp, plinst);
        if (!ptrlen_endswith(plinst, PTRLEN_LITERAL("\n"), NULL))
            console_write(outfp, PTRLEN_LITERAL("\n"));
    }

    for (curr_prompt = 0; curr_prompt < (size_t)p->n_prompts; curr_prompt++) {
        prompt_t *pr = p->prompts[curr_prompt];
        char line[1024];
        int ret;

        console_write(outfp, ptrlen_from_asciz(pr->prompt));

        ret = ro_read_line(line, sizeof line, pr->echo);
        if (ret < 0) {
            /* Regard end of input or Escape as a deliberate user abort */
            smemclr(line, sizeof line);
            return SPR_USER_ABORT;
        }
        put_data(pr->result, line, ret);
        smemclr(line, sizeof line);
    }

    return SPR_OK;
}

bool is_interactive(void)
{
    return isatty(0);
}

bool console_set_stdio_prompts(bool newvalue)
{
    /* Sending prompts to stdio in place of the keyboard is not supported. */
    return false;
}

bool set_legacy_charset_handling(bool newvalue)
{
    /* This probably _will_ need to be supported, but isn't yet. */
    return false;
}
