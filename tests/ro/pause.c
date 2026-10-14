/*
 * pause.c - waits a number of centiseconds, for an Obey file that has to give the desktop time between two commands.
 *
 *     pause 300
 *
 * Run in a task window or from an Obey file started by the Filer (both pre-emptive), it does not hold up the desktop.
 */
#include <stdlib.h>
#include "kernel.h"
#include "swis.h"

int main(int argc, char **argv)
{
    int start = 0, now = 0;
    const int cs = argc > 1 ? atoi(argv[1]) : 100;

    _swix(OS_ReadMonotonicTime, _OUT(0), &start);
    do {
        _swix(OS_ReadMonotonicTime, _OUT(0), &now);
    } while (now - start < cs);
    return 0;
}
