/*
 * inecho.c - counts what a program in a task window is sent, to see whether input is lost.
 *
 *     inecho [seconds]  (UnixLib, like xtest)
 *
 * Puts the terminal in raw mode, reads bytes until it reads 'Q', and prints how many it read and their sum.  A test
 * types a known amount at it (PuTTY$LocalType=@file) and compares.  With a number of seconds it waits that long
 * before it reads anything, so what is typed meanwhile shows how much the task window's input buffer holds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include "kernel.h"
#include "swis.h"

int main(int argc, char **argv)
{
    struct termios o, t;
    unsigned long n = 0, sum = 0;
    int c;

    tcgetattr(0, &o);
    t = o;
    t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t.c_oflag &= ~OPOST;
    t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    printf("inecho: ready\r\n");
    fflush(stdout);
    if (argc > 1) {
        _kernel_swi_regs r;
        int until;
        _kernel_swi(OS_ReadMonotonicTime, &r, &r);
        until = r.r[0] + atoi(argv[1]) * 100;
        do {
            _kernel_swi(OS_ReadMonotonicTime, &r, &r);
        } while (r.r[0] - until < 0);
    }
    while ((c = getchar()) != EOF && c != 'Q') {
        n++;
        sum += (unsigned)c;
    }
    tcsetattr(0, TCSANOW, &o);
    printf("inecho: count=%lu sum=%lu\r\n", n, sum);
    return 0;
}
