/*
 * rolocal.c - the local terminal: a RISC OS task window behind a PuTTY terminal.
 *
 * The TaskWindow module runs the supervisor prompt, or a command, as a task of its own and hands whatever the task
 * writes to the screen to a "parent" task in Wimp messages; the parent hands it what is typed.  Here the parent is
 * PuTTY and the screen is one of its terminal windows, so a program that wants an xterm - bash, vim, anything that
 * draws with escape sequences - has one, and the prompt has a window with a scroll back and a clipboard.  It is a
 * Backend like Telnet or Raw: PuTTY's terminal and line discipline do not know the difference.
 *
 * The messages (PRM volume 3, "TaskWindow"; DeskLib's TaskWindow library does the same; tests/ro/twprobe.c is a
 * script-driven parent that logs them, and what it showed is what this relies on):
 *
 *   &808C0 Input   parent -> task   word 5 = the number of bytes, the bytes from word 6 (232 at most)
 *   &808C1 Output  task -> parent   the same layout; every one is acknowledged (the task waits for that)
 *   &808C2 Ego     task -> parent   word 5 = the number given with -txt; the sender is the task's handle
 *   &808C3 Morio   task -> parent   the task has finished
 *   &808C4 Morite  parent -> task   kill it
 *
 * The task is started with   TaskWindow ["command"] -ctrl -display -task &<me> -txt &<id> ...   (-ctrl lets the
 * control characters through).  With no command it shows the supervisor prompt, "*", and goes on until it is told
 * to quit; with one it runs it and finishes (-quit).  What a program writes comes through as it was written, and
 * what is typed goes in as the characters OS_ReadC would give: a program that has turned the terminal raw (UnixLib's
 * termios does, with the Escape key's own handling off) reads exactly the bytes PuTTY's terminal sends, escape
 * sequences for the cursor keys included, and the prompt's line editor takes what it understands.  The Escape
 * character does what the Escape key does, interrupts the program.
 *
 * Input is handed over as the module can take it.  Its buffer is small - a burst of Input messages (3000 bytes sent back
 * to back, 13 messages) arrives short by a few bytes, 6000 by about 90, and 500 bytes sent to a program that is not
 * reading yet lose the end - so what is typed is queued, a few bytes at a time go as Input messages, and anything more
 * goes by the Wimp's data transfer protocol, which the module answers like a RAM-fetching receiver: we send
 * Message_DataSave, it answers with Message_RAMFetch for a 1040 byte buffer, we Wimp_TransferBlock into that and send
 * Message_RAMTransmit.  It takes one block per DataSave (it does not ask again when a full block was sent), but it does
 * not answer a DataSave until the program has used up what it was given before, and that is the flow control: a
 * second block offered to a program that was not reading was fetched only once it began to (tests/ro/twprobe.c, ds).
 * The two ways in do not keep their order: a few Input messages sent while a block was waiting for the program to
 * read it overtook it (a test that typed a command to quit after 500 bytes had the command read first).  So after a
 * block another Message_DataSave goes out at once, empty-handed if need be: its answer says the block has been used,
 * and until it comes whatever is typed is queued for it, not sent.  If the module never answers the first (an older
 * one), the queue is given out in Input messages, paced.  A lone Escape character (the Escape key) jumps the queue and
 * empties it: it interrupts.
 *
 * What differs from an xterm is the screen's own control codes: the prompt and BASIC write VDU codes (Delete is 127,
 * CLS is 12, colours are 17, 31 puts the cursor somewhere), and a terminal emulator would show them as nothing or as
 * mess.  The few that matter are turned into their ANSI equivalents on the way in (filter_output); the rest are
 * dropped with their parameters.  Escape sequences, Tab, Return, Backspace and the bell pass through as they are.
 *
 * A UnixLib program learns the terminal from its environment, which UnixLib makes from the system variables called
 * UnixEnv$NAME: TERM from the session's terminal type (xterm), COLUMNS and LINES from the window's size (these
 * are what its TIOCGWINSZ answers inside a task window).  They are set when the first local terminal starts and
 * kept up to date as the window is resized (a program already running does not hear of it; one started afterwards
 * does), and put back when the last one ends.
 *
 * PuTTY$LocalSlot (kilobytes) sets the memory of the task window, which is 4096 by default, since the Unix
 * programs that want a terminal are not small.  PuTTY$LocalType, for tests, is typed into the window as it starts
 * (or, if it starts with @, the contents of that file; see type_script for the escapes in it).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "putty.h"
#include "ro.h"
#include "rowimp.h"
#include "roapp.h"
#include "rolocal.h"

#define MSG_TW_INPUT   0x808C0
#define MSG_TW_MORITE  0x808C4

#define TW_MAX_DATA    232             /* bytes in one Input or Output message */
#define LOCAL_MAX_BACKLOG 4096         /* the terminal is this far behind: hold the acknowledgement */
#define DEFAULT_SLOT_K 4096
#define TICK_MS        50              /* the pace of Input messages when there is no data transfer */
#define XFER_WAIT_MS   3000            /* how long the task has to answer the first Message_DataSave */

typedef struct Local Local;
struct Local {
    Local *next;
    Seat *seat;
    LogContext *logctx;
    Conf *conf;
    int  txt;                          /* what -txt gave: the module reports the task by it */
    int  child;                        /* the task's Wimp handle, 0 if it has none (yet or any more) */
    bool finished;
    int  width, height;                /* of the terminal, in characters */
    size_t backlog;                    /* what the terminal has not dealt with yet */

    /* the control code filter: a multi-byte command being collected */
    unsigned char vdu_cmd;
    int  vdu_need, vdu_have;
    unsigned char vdu_arg[10];

    /* what was typed and the task has not been given yet (see local_input) */
    unsigned char *outq;
    size_t outq_len, outq_cap;
    bool xfer;                         /* a Message_DataSave is out, and the task has not asked for the block yet */
    bool ram_ok;                       /* ... it has answered one, so it does data transfer (and may be slow to answer) */
    bool unconsumed;                   /* a block has been given and the program may not have used it up yet */
    unsigned long xfer_started;
    int  xfer_ref;                     /* my_ref of the Message_DataSave, to know it if it bounces */
    bool no_ram;                       /* the task window does not do data transfer: paced Input messages instead */
    bool tick_pending;

    /* what PuTTY$LocalType has still to type (see type_script) */
    char *script;
    const char *script_pos;

    /* an Output message the terminal was too far behind to be acknowledged */
    bool ack_held;
    int  ack[64];

    Backend backend;
};

static Local *locals;
static int txt_counter = 0x5075;       /* "Pu" */

/* ------------------------------------------------------------------------ */
/* the environment of the programs in the task window                       */

static const char *const env_names[3] = { "UnixEnv$TERM", "UnixEnv$COLUMNS", "UnixEnv$LINES" };

static struct {
    int  users;                        /* local terminals alive */
    bool had[3];                       /* the variable was set before we set it ... */
    char old[3][80];                   /* ... to this */
} env;

static void set_var(const char *name, const char *value)
{
    _swix(OS_SetVarVal, _INR(0, 4), name, value, (int)strlen(value), 0, 0);
}

static void unset_var(const char *name)
{
    _swix(OS_SetVarVal, _INR(0, 4), name, "", -1, 0, 0);
}

static void env_acquire(const char *term, int cols, int rows)
{
    char num[16];
    int i;

    if (env.users++ == 0) {
        for (i = 0; i < 3; i++) {
            int len = 0;
            env.had[i] = !_swix(OS_ReadVarVal, _INR(0, 4) | _OUT(2), env_names[i], env.old[i],
                                (int)sizeof env.old[i] - 1, 0, 0, &len) && len >= 0;
            if (env.had[i]) env.old[i][len] = 0;
        }
    }
    set_var(env_names[0], term && term[0] ? term : "xterm");
    snprintf(num, sizeof num, "%d", cols);
    set_var(env_names[1], num);
    snprintf(num, sizeof num, "%d", rows);
    set_var(env_names[2], num);
}

static void env_size(int cols, int rows)
{
    char num[16];
    snprintf(num, sizeof num, "%d", cols);
    set_var(env_names[1], num);
    snprintf(num, sizeof num, "%d", rows);
    set_var(env_names[2], num);
}

static void env_release(void)
{
    int i;
    if (env.users <= 0 || --env.users > 0) return;
    for (i = 0; i < 3; i++) {
        if (env.had[i]) set_var(env_names[i], env.old[i]);
        else unset_var(env_names[i]);
    }
}

/* ------------------------------------------------------------------------ */
/* talking to the task                                                      */

static void send_to_child(const Local *l, int action, const void *data, size_t n)
{
    int blk[64];

    if (!l->child) return;
    memset(blk, 0, sizeof blk);
    if (action == MSG_TW_INPUT) {
        if (n > TW_MAX_DATA) n = TW_MAX_DATA;
        blk[0] = (int)((24 + n + 3) & ~3u);
        blk[5] = (int)n;
        memcpy(&blk[6], data, n);
    } else {
        blk[0] = 20;
    }
    blk[4] = action;
    _swix(Wimp_SendMessage, _INR(0, 2), EV_MESSAGE, blk, l->child);
}

static void queue_add(Local *l, const char *buf, size_t len)
{
    if (l->outq_len + len > l->outq_cap) {
        l->outq_cap = (l->outq_len + len) * 2 + 256;
        l->outq = sresize(l->outq, l->outq_cap, unsigned char);
    }
    memcpy(l->outq + l->outq_len, buf, len);
    l->outq_len += len;
}

static void queue_drop(Local *l, size_t n)
{
    memmove(l->outq, l->outq + n, l->outq_len - n);
    l->outq_len -= n;
}

/* Up to max bytes of the queue, as Input messages. */
static void send_messages(Local *l, size_t max)
{
    while (l->outq_len > 0 && max > 0) {
        size_t n = l->outq_len < TW_MAX_DATA ? l->outq_len : TW_MAX_DATA;
        if (n > max) n = max;
        send_to_child(l, MSG_TW_INPUT, l->outq, n);
        queue_drop(l, n);
        max -= n;
    }
}

static void tick(void *ctx, unsigned long now);

static void ensure_tick(Local *l)
{
    if (!l->tick_pending) {
        l->tick_pending = true;
        schedule_timer(TICK_MS, tick, l);
    }
}

/* Offer the queue to the task by the data transfer protocol; it answers with a Message_RAMFetch (ram_fetch). */
static void start_transfer(Local *l)
{
    int blk[24];

    memset(blk, 0, sizeof blk);
    blk[0] = 52;
    blk[4] = MSG_DATA_SAVE;
    blk[5] = -1;                                       /* no window, no icon: it is not a drop */
    blk[6] = -1;
    blk[9] = (int)l->outq_len;                         /* its estimated size */
    blk[10] = 0xFFF;                                   /* Text */
    memcpy(&blk[11], "Input", 6);
    if (_swix(Wimp_SendMessage, _INR(0, 2), EV_MESSAGE_RECORDED, blk, l->child)) {
        l->no_ram = true;
        return;
    }
    l->xfer = true;
    l->xfer_started = GETTICKCOUNT();
    l->xfer_ref = blk[2];
    ro_trace("PuTTYlocal$Trace", "DataSave of %d bytes sent (ref %d)", (int)l->outq_len, blk[2]);
    if (!l->ram_ok) ensure_tick(l);                    /* (to give up if the first one is never answered) */
}

/* Give the task what is waiting, in the way it can take it. */
static void pump(Local *l)
{
    if (l->finished || !l->child || l->xfer) return;
    if (l->no_ram) {
        if (l->outq_len) {
            send_messages(l, TW_MAX_DATA);             /* a message every tick: 4.6 KB a second */
            if (l->outq_len) ensure_tick(l);
        }
    } else if (l->outq_len > 0 || l->unconsumed) {
        if (!l->unconsumed && l->outq_len <= TW_MAX_DATA) {
            send_messages(l, l->outq_len);             /* a few bytes and nothing in front of them: just send them */
        } else {
            start_transfer(l);
            if (l->no_ram) pump(l);
        }
    }
}

static void tick(void *ctx, unsigned long now)
{
    Local *l = (Local *)ctx;

    l->tick_pending = false;
    if (l->finished) return;
    if (l->xfer && !l->ram_ok && now - l->xfer_started > XFER_WAIT_MS) {
        ro_trace("PuTTYlocal$Trace", "no answer to the first DataSave: paced Input messages from now on");
        l->xfer = false;                               /* it never answered the first: it does not do this */
        l->unconsumed = false;
        l->no_ram = true;
    }
    pump(l);
    if ((l->no_ram && l->outq_len) || (l->xfer && !l->ram_ok)) ensure_tick(l);
}

/* The task wants the next block of a transfer: m is its Message_RAMFetch, with the address of its buffer and its size. */
static void ram_fetch(Local *l, const wmessage *m)
{
    const int buf = m->data[0], size = m->data[1];
    size_t n = l->outq_len < (size_t)size ? l->outq_len : (size_t)size;
    int reply[16];

    ro_trace("PuTTYlocal$Trace", "RAMFetch: buffer %x, size %d; queue %d bytes", buf, size, (int)l->outq_len);
    l->xfer = false;                                   /* (one block to a Message_DataSave: see the top) */
    l->ram_ok = true;
    if (n && _swix(Wimp_TransferBlock, _INR(0, 4), roapp_task_handle(), l->outq, l->child, buf, (int)n)) {
        l->no_ram = true;                              /* it cannot be done: end this block and use Input messages */
        n = 0;
    }
    memset(reply, 0, sizeof reply);
    reply[0] = 28;
    reply[3] = m->my_ref;
    reply[4] = MSG_RAM_TRANSMIT;
    reply[5] = buf;
    reply[6] = (int)n;
    _swix(Wimp_SendMessage, _INR(0, 2), EV_MESSAGE, reply, m->sender);
    queue_drop(l, n);
    /* A block given: the program may not have used it up, so the next Message_DataSave goes out at once (it carries the
     * rest, or nothing, and is answered when the program has used this up).  An empty answer: all has been used. */
    l->unconsumed = n > 0;
    pump(l);
}

/* What the user typed (or pasted): to the task, now if it is only a little and nothing is waiting. */
static void local_input(Local *l, const char *buf, size_t len)
{
    if (l->finished || !len) return;
    if (len == 1 && buf[0] == 27) {                    /* the Escape key interrupts: it does not wait, and what was */
        l->outq_len = 0;                               /* waiting (a paste, say) is given up */
        send_to_child(l, MSG_TW_INPUT, buf, 1);
        return;
    }
    if (l->outq_len == 0 && !l->xfer && !l->unconsumed && len <= TW_MAX_DATA) {
        send_to_child(l, MSG_TW_INPUT, buf, len);
        return;
    }
    queue_add(l, buf, len);
    pump(l);
}

static void kill_child(Local *l)
{
    if (l->child && !l->finished) send_to_child(l, MSG_TW_MORITE, NULL, 0);
}

/*
 * What PuTTY$LocalType says is typed as the task window starts, for tests: backslash escapes stand for the characters
 * that cannot be in a system variable (\r \n \e \xHH \\), and \p waits a second and a half before going on (the module
 * takes an Escape character as the Escape key and throws away what is waiting to be read, so a test cannot send a
 * program's name and the keys it is to be given in one go).
 */
static void type_timer(void *ctx, unsigned long now);

static void type_script(Local *l)
{
    char out[8192];
    size_t n = 0;
    const char *s = l->script_pos;
    bool pause = false;

    if (!s) return;
    while (*s && n < sizeof out && !pause) {
        if (*s == '\\' && s[1]) {
            s++;
            switch (*s) {
              case 'r': out[n++] = 13; break;
              case 'n': out[n++] = 10; break;
              case 'e': out[n++] = 27; break;
              case 't': out[n++] = 9; break;
              case 'p': pause = true; break;
              case 'x': {
                unsigned v = 0;
                sscanf(s + 1, "%2x", &v);
                out[n++] = (char)v;
                if (s[1]) s++;
                if (s[1]) s++;
                break;
              }
              default: out[n++] = *s; break;
            }
            s++;
        } else {
            out[n++] = *s++;
        }
    }
    local_input(l, out, n);
    l->script_pos = *s ? s : NULL;
    if (l->script_pos)
        schedule_timer(1500, type_timer, l);
}

static void type_timer(void *ctx, unsigned long now)
{
    type_script((Local *)ctx);
}

/* Read what PuTTY$LocalType says, as the session starts (so that a test can start several one after another with a
 * different text each); it is typed when the task window has announced itself (Ego). */
static void load_test_input(Local *l)
{
    const char *s = getenv("PuTTY$LocalType");

    if (!s) return;
    if (s[0] == '@') {                                  /* "@file": what is in the file */
        FILE *fp = fopen(s + 1, "rb");
        char *text = snewn(16384, char);
        size_t got = 0;
        if (fp) {
            got = fread(text, 1, 16383, fp);
            fclose(fp);
        }
        text[got] = 0;
        l->script = text;
    } else {
        l->script = dupstr(s);
    }
    l->script_pos = l->script;
}

/* ------------------------------------------------------------------------ */
/* the screen's control codes                                               */

typedef struct Sink {
    Local *l;
    unsigned char b[1024];
    size_t n;
} Sink;

static void sink_flush(Sink *s)
{
    if (s->n) {
        s->l->backlog = seat_stdout(s->l->seat, s->b, s->n);
        s->n = 0;
    }
}

static void put(Sink *s, const char *text)
{
    size_t len = strlen(text);
    if (s->n + len > sizeof s->b) sink_flush(s);
    memcpy(s->b + s->n, text, len);
    s->n += len;
}

static void putc_(Sink *s, unsigned c)
{
    if (s->n + 1 > sizeof s->b) sink_flush(s);
    s->b[s->n++] = (unsigned char)c;
}

/* A VDU command whose parameters have all arrived. */
static void vdu_command(Local *l, Sink *s)
{
    const unsigned char *a = l->vdu_arg;
    char buf[24];

    switch (l->vdu_cmd) {
      case 17: {                       /* COLOUR: a text colour, or with bit 7 a background one */
        const int n = a[0] & 15;
        const int base = (a[0] & 128) ? (n < 8 ? 40 : 92) : (n < 8 ? 30 : 82);   /* 40-47 / 100-107, 30-37 / 90-97 */
        snprintf(buf, sizeof buf, "\033[%dm", base + n);
        put(s, buf);
        break;
      }
      case 22:                         /* MODE: the screen is cleared */
        put(s, "\033[2J\033[H");
        break;
      case 31:                         /* TAB(x,y): the cursor goes there (the first column and row are 0) */
        snprintf(buf, sizeof buf, "\033[%d;%dH", a[1] + 1, a[0] + 1);
        put(s, buf);
        break;
      default:                         /* printer, graphics, palette, windows: nothing a terminal can do */
        break;
    }
}

/* How many parameter bytes follow a VDU code (0 if it is not one with parameters). */
static int vdu_params(unsigned c)
{
    switch (c) {
      case 1: case 17: case 22: return 1;
      case 18: return 2;
      case 31: return 2;
      case 19: case 25: return 5;
      case 28: case 29: return 4;
      case 24: return 8;
      case 23: return 9;
      default: return 0;
    }
}

static void filter_output(Local *l, const unsigned char *p, size_t n, Sink *s)
{
    for (; n; n--, p++) {
        const unsigned c = *p;
        int np;

        if (l->vdu_need) {
            l->vdu_arg[l->vdu_have++] = (unsigned char)c;
            if (--l->vdu_need == 0) vdu_command(l, s);
            continue;
        }
        if ((np = vdu_params(c)) != 0) {
            l->vdu_cmd = (unsigned char)c;
            l->vdu_need = np;
            l->vdu_have = 0;
            continue;
        }
        switch (c) {
          case 0: case 2: case 3: case 4: case 5: case 6:      /* NUL, printer on/off, text/graphics cursor, VDU on */
          case 16: case 21: case 26:                           /* CLG, VDU off, reset windows */
            break;
          case 11: put(s, "\033[A"); break;                    /* cursor up */
          case 12: put(s, "\033[2J\033[H"); break;             /* CLS */
          case 20: put(s, "\033[39;49m"); break;               /* the default colours */
          case 30: put(s, "\033[H"); break;                    /* home */
          case 127: put(s, "\b \b"); break;                    /* what the line editor echoes for Delete */
          default: putc_(s, c); break;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* the messages                                                             */

static Local *find_child(int task)
{
    Local *l;
    for (l = locals; l; l = l->next)
        if (l->child == task) return l;
    return NULL;
}

/* An Output message is acknowledged by sending it back with your_ref set to its my_ref (as DeskLib does); until
 * then the task waits to send more. */
static void make_ack(const wmessage *m, int *ack)
{
    const int size = m->size < 256 ? m->size : 256;
    memset(ack, 0, 256);
    memcpy(ack, m, (size_t)size);
    ack[3] = m->my_ref;
}

bool rolocal_message(const wmessage *m)
{
    Local *l;

    if (m->action == MSG_TW_EGO || m->action == MSG_TW_OUTPUT || m->action == MSG_TW_MORIO)
        ro_trace("PuTTYlocal$Trace", "message &%x from task %d: size %d, word %d", m->action, m->sender, m->size,
                 m->data[0]);
    switch (m->action) {
      case MSG_TW_EGO:
        for (l = locals; l; l = l->next)
            if (l->txt == m->data[0]) {
                l->child = m->sender;
                type_script(l);
                break;
            }
        return true;

      case MSG_TW_OUTPUT: {
        int ack[64];
        int n = m->data[0];
        const int room = m->size - 24;

        make_ack(m, ack);
        if (n > room) n = room;
        if (n > TW_MAX_DATA) n = TW_MAX_DATA;
        l = find_child(m->sender);
        if (l && n > 0) {
            Sink s;
            s.l = l;
            s.n = 0;
            filter_output(l, (const unsigned char *)&m->data[1], (size_t)n, &s);
            sink_flush(&s);
        }
        if (l && l->backlog > LOCAL_MAX_BACKLOG) {
            memcpy(l->ack, ack, sizeof ack);                   /* held until the terminal has caught up: unthrottle */
            l->ack_held = true;
        } else {
            _swix(Wimp_SendMessage, _INR(0, 2), EV_MESSAGE_ACK, ack, m->sender);
        }
        return true;
      }

      case MSG_RAM_FETCH:                                      /* the task asking for the next block of what we give it */
        l = find_child(m->sender);
        if (!l || !l->xfer) return false;
        ram_fetch(l, m);
        return true;

      case MSG_DATA_SAVE_ACK:                                  /* ... or asking for a file, which we do not do */
        l = find_child(m->sender);
        if (!l || !l->xfer) return false;
        l->xfer = false;
        l->unconsumed = false;
        l->no_ram = true;
        pump(l);
        return true;

      case MSG_TW_MORIO:
        l = find_child(m->sender);
        if (l && !l->finished) {
            l->xfer = false;
            l->outq_len = 0;
            static const char note[] = "\r\n[The task window has finished]\r\n";
            l->finished = true;
            l->child = 0;
            logevent(l->logctx, "The task window has finished");
            seat_stdout(l->seat, note, sizeof note - 1);
            seat_notify_remote_exit(l->seat);
        }
        return true;

      default:
        return false;
    }
}

/* A message of ours that nobody answered (the Message_DataSave to a task window that is not there to take it). */
bool rolocal_bounced(const wmessage *m)
{
    Local *l;

    if (m->action != MSG_DATA_SAVE) return false;
    for (l = locals; l; l = l->next)
        if (l->xfer && l->xfer_ref == m->my_ref) {
            l->xfer = false;
            l->unconsumed = false;
            l->no_ram = true;
            pump(l);
            return true;
        }
    return false;
}

void rolocal_shutdown(void)
{
    Local *l;
    for (l = locals; l; l = l->next) kill_child(l);
    while (env.users > 0) env_release();
}

/* ------------------------------------------------------------------------ */
/* the Backend                                                              */

/*
 * Upstream's conf_launchable (utils/conf_launchable.c) says a session with no host name cannot be started, which a
 * local terminal never has.  This one takes its place in the program (the linker finds it first, so the library's
 * is not used; the other programs keep upstream's).
 */
bool conf_launchable(Conf *conf)
{
    const int protocol = conf_get_int(conf, CONF_protocol);

    if (protocol == PROT_LOCAL)
        return true;
    if (protocol == PROT_SERIAL)
        return conf_get_str(conf, CONF_serline)[0] != 0;
    return conf_get_str(conf, CONF_host)[0] != 0;
}

/*
 * The memory for the task window: what PuTTY$LocalSlot asks for (4096K if it says nothing), but no more than the
 * free memory leaves room for.  A task that cannot have its slot makes the Wimp put up an error box that stops the whole
 * desktop until it is answered, so asking for too much is not something to leave to chance.
 */
static int slot_kilobytes(void)
{
    const char *v = getenv("PuTTY$LocalSlot");
    int want = v && atoi(v) > 0 ? atoi(v) : DEFAULT_SLOT_K;
    int cur = 0, next = 0, avail = 0;

    if (!_swix(Wimp_SlotSize, _INR(0, 1) | _OUTR(0, 2), -1, -1, &cur, &next, &avail)) {
        const int room = (avail - 512 * 1024) / 1024;
        ro_trace("PuTTYlocal$Trace", "memory: slot %dK, next %dK, free %dK; wanted %dK", cur / 1024, next / 1024,
                 avail / 1024, want);
        if (want > room) want = room < 256 ? 256 : room;
    }
    return want;
}

/* "..." for the CLI: a quote inside is doubled. */
static char *quoted(const char *s)
{
    size_t n = 0;
    const char *p;
    char *out, *q;

    for (p = s; *p; p++) n += (*p == '"') ? 2 : 1;
    q = out = snewn(n + 3, char);
    *q++ = '"';
    for (p = s; *p; p++) {
        if (*p == '"') *q++ = '"';
        *q++ = *p;
    }
    *q++ = '"';
    *q = 0;
    return out;
}

static char *local_init(const BackendVtable *vt, Seat *seat, Backend **backend_handle, LogContext *logctx,
                        Conf *conf, const char *host, int port, char **realhost, bool nodelay, bool keepalive)
{
    Local *l = snew(Local);
    const char *command = conf_get_str_ambi(conf, CONF_remote_cmd, NULL);
    char *cmd = NULL, full[1024];
    int started = 0, len;
    _kernel_oserror *e;

    memset(l, 0, sizeof *l);
    l->backend.vt = vt;
    l->backend.interactor = NULL;
    l->seat = seat;
    l->logctx = logctx;
    l->conf = conf_copy(conf);
    l->txt = txt_counter++;
    l->width = conf_get_int(conf, CONF_width);
    l->height = conf_get_int(conf, CONF_height);

    if (command[0]) cmd = quoted(command);
    len = snprintf(full, sizeof full, "TaskWindow %s%s-ctrl -display -task &%x -txt &%x -name \"PuTTY local\" -wimpslot %dK",
                   cmd ? cmd : "", cmd ? " -quit " : "", roapp_task_handle(), l->txt, slot_kilobytes());
    sfree(cmd);
    if (len < 0 || len >= (int)sizeof full) {
        conf_free(l->conf);
        sfree(l);
        return dupstr("The command is too long.");
    }

    env_acquire(conf_get_str(conf, CONF_termtype), l->width, l->height);
    load_test_input(l);
    l->no_ram = getenv("PuTTY$LocalNoRAM") != NULL;           /* for tests of the paced fallback */
    ro_trace("PuTTYlocal$Trace", "starting: %s", full);
    if ((e = _swix(Wimp_StartTask, _IN(0) | _OUT(0), full, &started)) != NULL) {
        char *msg = dupprintf("Could not start a task window: %s", e->errmess);
        ro_trace("PuTTYlocal$Trace", "Wimp_StartTask failed: %s", e->errmess);
        env_release();
        conf_free(l->conf);
        sfree(l->script);
        sfree(l);
        return msg;
    }
    ro_trace("PuTTYlocal$Trace", "Wimp_StartTask gave task %d", started);
    l->child = started;
    l->next = locals;
    locals = l;
    logevent(logctx, command[0] ? "Started a task window running a command" : "Started a task window (the supervisor prompt)");

    *backend_handle = &l->backend;
    *realhost = dupstr(command[0] ? command : "Local terminal");     /* the window's title, until a program sets its own */
    seat_set_trust_status(seat, false);                        /* nothing to log in to: no authentication phase */
    return NULL;
}

static void local_free(Backend *be)
{
    Local *l = container_of(be, Local, backend);
    Local **pp;

    kill_child(l);
    for (pp = &locals; *pp; pp = &(*pp)->next)
        if (*pp == l) { *pp = l->next; break; }
    expire_timer_context(l);
    sfree(l->outq);
    sfree(l->script);
    env_release();
    conf_free(l->conf);
    sfree(l);
}

static void local_reconfig(Backend *be, Conf *conf)
{
    Local *l = container_of(be, Local, backend);
    conf_copy_into(l->conf, conf);
}

static void local_send(Backend *be, const char *buf, size_t len)
{
    Local *l = container_of(be, Local, backend);
    local_input(l, buf, len);
}

static size_t local_sendbuffer(Backend *be)
{
    Local *l = container_of(be, Local, backend);
    return l->outq_len;
}

static void local_size(Backend *be, int width, int height)
{
    Local *l = container_of(be, Local, backend);
    l->width = width;
    l->height = height;
    env_size(width, height);
}

static const SessionSpecial local_specials[] = {
    { "Interrupt (Escape)", SS_IP, 0 },
    { "Quit task window", SS_ABORT, 0 },
    { NULL, SS_EXITMENU, 0 }
};

static void local_special(Backend *be, SessionSpecialCode code, int arg)
{
    Local *l = container_of(be, Local, backend);
    if (code == SS_IP)
        local_input(l, "\033", 1);
    else if (code == SS_ABORT)
        kill_child(l);
}

static const SessionSpecial *local_get_specials(Backend *be)
{
    return local_specials;
}

static bool local_connected(Backend *be)
{
    Local *l = container_of(be, Local, backend);
    return !l->finished;
}

static bool local_sendok(Backend *be)
{
    Local *l = container_of(be, Local, backend);
    return !l->finished;
}

static void local_unthrottle(Backend *be, size_t backlog)
{
    Local *l = container_of(be, Local, backend);
    l->backlog = backlog;
    if (l->ack_held && backlog <= LOCAL_MAX_BACKLOG) {
        l->ack_held = false;
        _swix(Wimp_SendMessage, _INR(0, 2), EV_MESSAGE_ACK, l->ack, l->ack[1]);
    }
}

static bool local_ldisc(Backend *be, int option)
{
    return false;                                              /* the program echoes and edits for itself */
}

static void local_provide_ldisc(Backend *be, Ldisc *ldisc)
{
}

static int local_exitcode(Backend *be)
{
    Local *l = container_of(be, Local, backend);
    return l->finished ? 0 : -1;                               /* the module does not say how the task ended */
}

static int local_cfg_info(Backend *be)
{
    return 0;
}

const BackendVtable local_backend = {
    .init = local_init,
    .free = local_free,
    .reconfig = local_reconfig,
    .send = local_send,
    .sendbuffer = local_sendbuffer,
    .size = local_size,
    .special = local_special,
    .get_specials = local_get_specials,
    .connected = local_connected,
    .exitcode = local_exitcode,
    .sendok = local_sendok,
    .ldisc_option_state = local_ldisc,
    .provide_ldisc = local_provide_ldisc,
    .unthrottle = local_unthrottle,
    .cfg_info = local_cfg_info,
    .id = "local",
    .displayname_tc = "Local",
    .displayname_lc = "local",
    .protocol = PROT_LOCAL,
    .default_port = 0,
};
