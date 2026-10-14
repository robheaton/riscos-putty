/*
 * wimprun.c - runs a script of commands as a desktop task, with proper waits in it.
 *
 *     WimpTask Run <dir>.wimprun <script>
 *
 * An Obey file cannot do this job: run from the Filer it is a task window, the programs it starts with WimpTask do not
 * begin until it has finished, and so do not see the variables it set for each of them, whatever it waits for in between.
 * This is a task of its own that calls Wimp_PollIdle while it waits, so what it starts runs at once.
 *
 * Script lines (blank lines and lines starting with | are ignored):
 *
 *     set NAME value       a system variable
 *     unset NAME
 *     run COMMAND...       Wimp_StartTask, as WimpTask would (a program is started with   run Run <path> args )
 *     wait CS              wait that many centiseconds, giving the desktop its turn
 *     quit HEXHANDLE       send Message_Quit to the task with that handle (tests/ro/tasks.c lists the handles; PuTTY's
 *                          is also what PuTTYlocal$Trace logs as -task)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "swis.h"

static void wait_cs(int cs)
{
    int blk[64], reason = 0, now = 0, end;

    _swix(OS_ReadMonotonicTime, _OUT(0), &now);
    end = now + cs;
    while (now < end) {
        _swix(Wimp_PollIdle, _INR(0, 2) | _OUT(0), 0, blk, now + 5, &reason);
        if (reason == 17 && blk[4] == 0) return;                  /* Message_Quit */
        _swix(OS_ReadMonotonicTime, _OUT(0), &now);
    }
}

int main(int argc, char **argv)
{
    static const int msgs[] = { 0 };
    char line[600];
    FILE *fp;
    int version = 0, me = 0;

    if (argc < 2 || !(fp = fopen(argv[1], "r"))) {
        printf("usage: wimprun script\n");
        return 1;
    }
    if (_swix(Wimp_Initialise, _INR(0, 3) | _OUTR(0, 1), 310, 0x4B534154, "wimprun", msgs, &version, &me))
        return 1;
    while (fgets(line, sizeof line, fp)) {
        char *p = line + strlen(line), *arg;
        while (p > line && (p[-1] == '\n' || p[-1] == '\r')) *--p = 0;
        if (!line[0] || line[0] == '|') continue;
        arg = strchr(line, ' ');
        if (arg) *arg++ = 0;
        else arg = line + strlen(line);
        if (!strcmp(line, "set")) {
            char *value = strchr(arg, ' ');
            if (value) *value++ = 0;
            else value = arg + strlen(arg);
            _swix(OS_SetVarVal, _INR(0, 4), arg, value, (int)strlen(value), 0, 0);
        } else if (!strcmp(line, "unset")) {
            _swix(OS_SetVarVal, _INR(0, 4), arg, "", -1, 0, 0);
        } else if (!strcmp(line, "run")) {
            _swix(Wimp_StartTask, _IN(0), arg);
        } else if (!strcmp(line, "wait")) {
            wait_cs(atoi(arg));
        } else if (!strcmp(line, "quit")) {
            int blk[8];
            memset(blk, 0, sizeof blk);
            blk[0] = 20;
            _swix(Wimp_SendMessage, _INR(0, 2), 17, blk, (int)strtoul(arg, NULL, 16));
        }
    }
    fclose(fp);
    _swix(Wimp_CloseDown, _INR(0, 1), me, 0x4B534154);
    return 0;
}
