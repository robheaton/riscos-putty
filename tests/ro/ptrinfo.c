/*
 * ptrinfo.c - where the system thinks the pointer is.
 *
 *     ptrinfo
 *
 * Prints the pointer position from OS_Mouse and from Wimp_GetPointerInfo (OS units, origin bottom left),
 * the buttons, and the window and icon under it.  A GUI test that drives the emulator over VNC can then check
 * that the pointer it asked for is where the desktop sees it (in a 1 OS unit per pixel mode it was not: see
 * modeinfo.c).
 */
#include <stdio.h>
#include "kernel.h"
#include "swis.h"

int main(void)
{
    int mx = -1, my = -1, mb = -1, mt = -1;
    int blk[5] = { 0 };

    _swix(OS_Mouse, _OUT(0) | _OUT(1) | _OUT(2) | _OUT(3), &mx, &my, &mb, &mt);
    _swix(Wimp_GetPointerInfo, _IN(1), blk);
    printf("OS_Mouse: x %d y %d buttons %d\n", mx, my, mb);
    printf("Wimp_GetPointerInfo: x %d y %d buttons %d window %d icon %d\n", blk[0], blk[1], blk[2], blk[3], blk[4]);
    return 0;
}
