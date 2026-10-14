/*
 * twprobe.c - the parent side of the TaskWindow protocol, for finding out what the module really does.
 *
 *     WimpTask Run <dir>.twprobe <script> <log>
 *
 * It must be started as a Wimp task (the module sends it messages).  The script is a text file, one command
 * to a line:
 *
 *     start [command]     start "TaskWindow [command] -ctrl -display -task &me -txt &id" (no command: the prompt)
 *     startq command      the same with -quit as well
 *     send TEXT           send TEXT as input to the child (escapes \r \n \e \\ \xHH)
 *     sendn N TEXT        the same, but tell the module the data is N bytes long (to try the size field)
 *     wait CS             wait that many centiseconds, logging what arrives
 *     dsrep N TEXT        the same, with TEXT repeated N times (for amounts the child's buffer cannot hold)
 *     ds TEXT             give the child TEXT by the data transfer protocol instead (Message_DataSave, then the
 *                         RAMFetch it answers with is served by Wimp_TransferBlock and RAMTransmit); the log shows
 *                         every message it sends back
 *     kill                send Message_TaskWindowMorite
 *     suspend / resume    Message_TaskWindowSuspend / Resume
 *     end                 close down
 *
 * Every message from the child is logged, with the bytes of output in hex and as text.  Output messages are
 * acknowledged, as the protocol wants.  It ends by itself 50 cs after the child's Morio, or at "end".
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "swis.h"

#define MSG_INPUT   0x808C0
#define MSG_OUTPUT  0x808C1
#define MSG_EGO     0x808C2
#define MSG_MORIO   0x808C3
#define MSG_MORITE  0x808C4
#define MSG_SUSPEND 0x808C6
#define MSG_RESUME  0x808C7

static FILE *logf;
static int me, child = -1, txt = 0x1234;
static unsigned char ds_buf[4096];                 /* what "ds" is giving the child */
static int ds_len, ds_sent;

static int now_cs(void)
{
    int t = 0;
    _swix(OS_ReadMonotonicTime, _OUT(0), &t);
    return t;
}

static void lg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(logf, "%6d ", now_cs());
    vfprintf(logf, fmt, ap);
    fputc('\n', logf);
    fflush(logf);
    va_end(ap);
}

static void dump(const unsigned char *p, int n)
{
    int i;
    fprintf(logf, "       hex:");
    for (i = 0; i < n; i++) fprintf(logf, " %02x", p[i]);
    fprintf(logf, "\n       txt: ");
    for (i = 0; i < n; i++) fputc(p[i] >= 32 && p[i] < 127 ? p[i] : '.', logf);
    fputc('\n', logf);
    fflush(logf);
}

static int unescape(const char *s, unsigned char *out)
{
    int n = 0;
    while (*s) {
        if (*s == '\\' && s[1]) {
            s++;
            switch (*s) {
              case 'r': out[n++] = 13; break;
              case 'n': out[n++] = 10; break;
              case 'e': out[n++] = 27; break;
              case 't': out[n++] = 9; break;
              case 'x': {
                unsigned v = 0;
                sscanf(s + 1, "%2x", &v);
                out[n++] = (unsigned char)v;
                if (s[1]) s++;
                if (s[1]) s++;
                break;
              }
              default: out[n++] = (unsigned char)*s; break;
            }
            s++;
        } else {
            out[n++] = (unsigned char)*s++;
        }
    }
    return n;
}

static void send_msg(int action, const unsigned char *data, int n, int declared)
{
    int blk[64];
    int size = 24 + n;
    memset(blk, 0, sizeof blk);
    size = (size + 3) & ~3;
    blk[0] = size;
    blk[3] = 0;
    blk[4] = action;
    if (action == MSG_INPUT) {
        blk[5] = declared >= 0 ? declared : n;
        memcpy((char *)blk + 24, data, n);
    } else {
        blk[0] = 20;
    }
    if (child < 0) { lg("no child to send to"); return; }
    _swix(Wimp_SendMessage, _INR(0, 2), 17, blk, child);
    lg("sent %s to %d (size %d, declared %d)", action == MSG_INPUT ? "Input" : "message", child, blk[0], blk[5]);
}

static void handle(int reason, int *b)
{
    if (reason == 17 || reason == 18 || reason == 19) {
        int action = b[4];
        if (action == MSG_OUTPUT) {
            int n = b[5];
            lg("Output from %d: size field %d, message size %d, ref %d", b[1], n, b[0], b[2]);
            if (n > 0 && n <= 232) dump((unsigned char *)b + 24, n);
            else dump((unsigned char *)b + 24, 16);
            {
                int ack[64];
                memcpy(ack, b, b[0] < 256 ? b[0] : 256);
                ack[3] = ack[2];
                _swix(Wimp_SendMessage, _INR(0, 2), 19, ack, b[1]);
            }
        } else if (action == MSG_EGO) {
            lg("Ego from %d: txt %x (message size %d)", b[1], b[5], b[0]);
            child = b[1];
        } else if (action == MSG_MORIO) {
            lg("Morio from %d (message size %d, words %x %x)", b[1], b[0], b[5], b[6]);
        } else if (action == 0) {
            lg("Quit");
        } else if (action == 6) {                  /* RAMFetch: the child's buffer is at b[5], b[6] bytes */
            int n = ds_len - ds_sent, reply[16];
            lg("RAMFetch from %d: buffer %x, size %d (your_ref %d, my_ref %d)", b[1], b[5], b[6], b[3], b[2]);
            if (n > b[6]) n = b[6];
            if (n > 0) {
                _kernel_oserror *e = _swix(Wimp_TransferBlock, _INR(0, 4), me, ds_buf + ds_sent, b[1], b[5], n);
                lg("Wimp_TransferBlock of %d bytes: %s", n, e ? e->errmess : "ok");
            }
            memset(reply, 0, sizeof reply);
            reply[0] = 28;
            reply[3] = b[2];
            reply[4] = 7;                          /* RAMTransmit */
            reply[5] = b[5];
            reply[6] = n > 0 ? n : 0;
            _swix(Wimp_SendMessage, _INR(0, 2), 17, reply, b[1]);
            ds_sent += n > 0 ? n : 0;
            lg("sent RAMTransmit of %d bytes (%d of %d given)", n, ds_sent, ds_len);
        } else if (action == 2) {
            lg("DataSaveAck from %d: file %s", b[1], (const char *)&b[11]);
        } else {
            lg("reason %d action &%x from %d size %d", reason, action, b[1], b[0]);
        }
    } else if (reason != 0) {
        lg("event %d", reason);
    }
}

static void pump(int cs)
{
    int end = now_cs() + cs;
    int b[64];
    while (now_cs() < end) {
        int reason = 0;
        _swix(Wimp_PollIdle, _INR(0, 2) | _OUT(0), 0, b, now_cs() + 5, &reason);
        handle(reason, b);
    }
}

int main(int argc, char **argv)
{
    FILE *sf;
    char line[512];
    static const int msgs[] = { MSG_OUTPUT, MSG_EGO, MSG_MORIO, 2, 6, 0 };
    int ver = 0;

    if (argc < 3) { printf("usage: twprobe script log\n"); return 1; }
    logf = fopen(argv[2], "w");
    sf = fopen(argv[1], "r");
    if (!logf || !sf) { printf("cannot open files\n"); return 1; }
    if (_swix(Wimp_Initialise, _INR(0, 3) | _OUTR(0, 1), 310, 0x4B534154, "twprobe", msgs, &ver, &me)) {
        lg("Wimp_Initialise failed");
        return 1;
    }
    lg("task handle %d, Wimp %d", me, ver);
    while (fgets(line, sizeof line, sf)) {
        char *p = line + strlen(line);
        while (p > line && (p[-1] == '\n' || p[-1] == '\r')) *--p = 0;
        if (!line[0] || line[0] == '#') continue;
        lg("> %s", line);
        if (!strncmp(line, "start", 5)) {
            const int quit = !strncmp(line, "startq", 6);
            const char *cmd = line + (quit ? 6 : 5);
            char full[600];
            int h = -1;
            while (*cmd == ' ') cmd++;
            sprintf(full, "TaskWindow %s%s%s -ctrl -display %s-task &%x -txt &%x -name \"twprobe\"",
                    *cmd ? "\"" : "", cmd, *cmd ? "\"" : "", quit ? "-quit " : "", me, txt);
            lg("starting: %s", full);
            if (_swix(Wimp_StartTask, _IN(0) | _OUT(0), full, &h)) lg("Wimp_StartTask failed");
            else { lg("Wimp_StartTask gave %d", h); child = h; }
        } else if (!strncmp(line, "sendn ", 6)) {
            unsigned char buf[300];
            int declared = atoi(line + 6), n;
            const char *t = strchr(line + 6, ' ');
            n = unescape(t ? t + 1 : "", buf);
            send_msg(MSG_INPUT, buf, n, declared);
        } else if (!strncmp(line, "ds ", 3) || !strncmp(line, "dsrep ", 6)) {
            int blk[24];
            if (line[2] == 'r') {
                const int reps = atoi(line + 6);
                const char *t = strchr(line + 6, ' ');
                unsigned char one[300];
                int len = unescape(t ? t + 1 : "", one), i;
                ds_len = 0;
                for (i = 0; i < reps && ds_len + len <= (int)sizeof ds_buf; i++) {
                    memcpy(ds_buf + ds_len, one, len);
                    ds_len += len;
                }
            } else {
                ds_len = unescape(line + 3, ds_buf);
            }
            ds_sent = 0;
            memset(blk, 0, sizeof blk);
            blk[0] = 52;
            blk[4] = 1;                            /* Message_DataSave */
            blk[5] = -1;                           /* window */
            blk[6] = -1;                           /* icon */
            blk[9] = ds_len;                       /* estimated size */
            blk[10] = 0xFFF;                       /* text */
            memcpy(&blk[11], "Input", 6);
            if (child < 0) lg("no child to send to");
            else {
                _swix(Wimp_SendMessage, _INR(0, 2), 18, blk, child);
                lg("sent DataSave of %d bytes to %d", ds_len, child);
            }
        } else if (!strncmp(line, "send ", 5)) {
            unsigned char buf[300];
            int n = unescape(line + 5, buf);
            send_msg(MSG_INPUT, buf, n, -1);
        } else if (!strncmp(line, "wait ", 5)) {
            pump(atoi(line + 5));
        } else if (!strcmp(line, "kill")) {
            send_msg(MSG_MORITE, NULL, 0, -1);
        } else if (!strcmp(line, "suspend")) {
            send_msg(MSG_SUSPEND, NULL, 0, -1);
        } else if (!strcmp(line, "resume")) {
            send_msg(MSG_RESUME, NULL, 0, -1);
        } else if (!strcmp(line, "end")) {
            break;
        }
    }
    pump(50);
    lg("closing down");
    _swix(Wimp_CloseDown, _INR(0, 1), me, 0x4B534154);
    fclose(logf);
    return 0;
}
