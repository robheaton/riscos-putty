/*
 * xtest.c - what a UnixLib program sees in a task window, and what it can do there.
 *
 *     xtest            (build it WITHOUT -mlibscl: it needs UnixLib; see the Makefile rule for xtest)
 *
 * Prints TERM, COLUMNS, LINES and what TIOCGWINSZ says, puts the terminal in raw mode, draws a few ANSI
 * escape sequences (clear, colours, cursor movement), then echoes the codes of the keys you type (each as hex)
 * until it reads 'q'.  Run it from a PuTTY local terminal (or from twprobe's script) to see the real behaviour of
 * input and output between a program and the terminal.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>

static volatile int sigints;
static void on_int(int sig) { sigints++; }

int main(int argc, char **argv)
{
    struct winsize ws;
    struct termios o, t;
    const char *term, *cols, *lines;
    int c, n = 0;

    memset(&ws, 0, sizeof ws);
    ioctl(0, TIOCGWINSZ, &ws);
    term = getenv("TERM");
    cols = getenv("COLUMNS");
    lines = getenv("LINES");
    printf("xtest: TERM=%s COLUMNS=%s LINES=%s winsize=%dx%d isatty(0)=%d isatty(1)=%d\r\n",
           term ? term : "(unset)", cols ? cols : "(unset)", lines ? lines : "(unset)", ws.ws_col, ws.ws_row,
           isatty(0), isatty(1));
    fflush(stdout);

    tcgetattr(0, &o);
    t = o;
    t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t.c_oflag &= ~OPOST;
    t.c_lflag &= ~(ECHO | ECHONL | ICANON | IEXTEN);
    if (argc < 2) t.c_lflag &= ~ISIG;                   /* "xtest isig": keep signals on, as bash does */
    else signal(SIGINT, on_int);
    t.c_cflag |= CS8;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);

    printf("\033[2J\033[H\033[1;31mred\033[0m \033[42mgreen bg\033[0m\033[3;5Hat 3,5\033[10;1H");
    printf("tab:\tx\r\nbox: \033(0lqqk\033(B  bell:\007  type keys, q quits\r\n");
    fflush(stdout);

    while ((c = getchar()) != EOF && c != 'q') {
        printf("%02x%s ", c, sigints ? "(SIGINT)" : "");
        if (++n % 16 == 0) printf("\r\n");
        fflush(stdout);
    }
    tcsetattr(0, TCSANOW, &o);
    printf("\r\nbye\r\n");
    return 0;
}
