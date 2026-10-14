/*
 * tasks.c - the desktop's tasks, as the Task Manager lists them (TaskManager_EnumerateTasks).
 *
 *     tasks
 *
 * Prints each task's handle, name and memory.  A test that has made a program quit can check that the task windows it
 * had started went with it.  (Run it from the command line; the list is the desktop's, not this program's.)
 */
#include <stdio.h>
#include "kernel.h"
#include "swis.h"

int main(void)
{
    int next = 0;

    while (next >= 0) {
        int buf[64], end = 0, i, words;
        if (_swix(0x42681, _INR(0, 2) | _OUTR(0, 1), next, buf, (int)sizeof buf, &next, &end)) {
            printf("TaskManager_EnumerateTasks failed\n");
            return 1;
        }
        words = (end - (int)buf) / 4;
        for (i = 0; i + 3 < words && i + 3 < 64; i += 4)
            printf("%08x %s (%d bytes, flags %x)\n", buf[i], (const char *)buf[i + 1], buf[i + 2], buf[i + 3]);
    }
    return 0;
}
