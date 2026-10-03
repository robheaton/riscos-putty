/*
 * keyscan.c - which keys does the keyboard say are held down?
 *
 *     keyscan [seconds]
 *
 * Prints the numbers of every key held, found two ways: by repeated OS_Byte 121 scans ("scan"), and by
 * asking OS_Byte 129 about every number as the terminal does ("inkey": INKEY(-n)).  The same key is one
 * higher in the second list than the first (Page Up is scan 63 and INKEY(-64)): OS_Byte 129 wants the
 * second.  Needed because the keys that generate other keys' codes - Page Up arrives as Shift-Up, Home as
 * Ctrl-^ - can only be told apart by asking the keyboard.
 *
 * Without an argument it looks once.  With one it keeps looking for that many seconds and prints a line
 * whenever the keys held change, so a script can hold keys through the emulator's VNC
 * (tools/vncdrive.py keydown) while this runs under riscos_run, and find a key's number.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "swis.h"

#define MAXKEYS 24

/* The keys held, by an OS_Byte 121 scan. */
static int by_scan(int *keys)
{
    int start = 0, found, n = 0;

    while (start < 128 && n < MAXKEYS) {
        found = -1;
        _swix(OS_Byte, _INR(0, 1) | _OUT(1), 121, start, &found);
        found &= 0xFF;
        if (found == 0xFF || found > 127) break;
        keys[n++] = found;
        start = found + 1;
    }
    return n;
}

/* ... and by INKEY(-n) for each n. */
static int by_inkey(int *keys)
{
    int i, n = 0;

    for (i = 1; i < 128 && n < MAXKEYS; i++) {
        int r1 = 0, r2 = 0;
        _swix(OS_Byte, _INR(0, 2) | _OUT(1) | _OUT(2), 129, (-i) & 0xFF, 0xFF, &r1, &r2);
        if ((r1 & 0xFF) == 0xFF && (r2 & 0xFF) == 0xFF) keys[n++] = i;
    }
    return n;
}

static void show(const char *what, const int *keys, int n)
{
    int i;
    printf("  %s:", what);
    for (i = 0; i < n; i++) printf(" %d", keys[i]);
    if (!n) printf(" -");
}

int main(int argc, char **argv)
{
    const int seconds = argc > 1 ? atoi(argv[1]) : 0;
    int prev_a[MAXKEYS], prev_b[MAXKEYS], na, nb, pna = -1, pnb = -1, start = 0, now;

    _swix(OS_ReadMonotonicTime, _OUT(0), &start);
    do {
        int a[MAXKEYS], b[MAXKEYS];
        na = by_scan(a);
        nb = by_inkey(b);
        _swix(OS_ReadMonotonicTime, _OUT(0), &now);
        if (seconds == 0 || na != pna || nb != pnb || memcmp(a, prev_a, sizeof(int) * (size_t)na) ||
            memcmp(b, prev_b, sizeof(int) * (size_t)nb)) {
            if (seconds) printf("%5.2fs", (now - start) / 100.0);
            show("scan", a, na);
            show("inkey", b, nb);
            printf("\n");
            memcpy(prev_a, a, sizeof a);
            memcpy(prev_b, b, sizeof b);
            pna = na;
            pnb = nb;
        }
    } while (seconds && now - start < seconds * 100);
    return 0;
}
