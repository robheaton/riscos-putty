/*
 * modeinfo.c - what the screen mode is, and how big the desktop font draws text in it.
 *
 *     modeinfo [-box]
 *
 * Prints the mode's size in pixels and OS units, its eigen factors (OS units per pixel: 2 in a
 * "90 dpi" mode, 1 in a "180 dpi" one), its depth, and the width in OS units that the Wimp gives a few
 * strings (Wimp_TextOp 1), which is what the dialogues' layouts have to fit.  Use it to check that a
 * layout means the same in every mode: OS units are 1/180 inch whatever the mode, and the desktop font
 * is a fixed number of OS units tall, so the strings measure the same in any mode.
 *
 * With -box it also sets the pointer's bounding box to the whole screen (OS_Word 21,1).  After a switch to a
 * 1 OS unit per pixel mode with WimpMode, RPCEmu's pointer would not go above about half way up the screen
 * (a stale box from the larger mode): the clicks a GUI test sends over VNC then land in the wrong place.
 */
#include <stdio.h>
#include <string.h>
#include "kernel.h"
#include "swis.h"

static int mode_var(int var)
{
    int v = 0;
    _swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, var, &v);
    return v;
}

static int text_width(const char *s)
{
    int w = -1;
    _swix(Wimp_TextOp, _INR(0, 2) | _OUT(0), 1, s, 0, &w);
    return w;
}

int main(int argc, char **argv)
{
    static const char *const samples[] = {
        "Host name:", "Drag a private key file onto this window to fill it in.", "0123456789", "WWWWWWWWWW", "iiiiiiiiii"
    };
    const int xe = mode_var(4), ye = mode_var(5), w = mode_var(11) + 1, h = mode_var(12) + 1;
    size_t i;

    if (argc > 1 && !strcmp(argv[1], "-box")) {
        unsigned char blk[9];
        const int right = (w << xe) - 1, top = (h << ye) - 1;

        blk[0] = 1;                                              /* the OS_Word 21,1 block: 1, then left, bottom, */
        blk[1] = blk[2] = blk[3] = blk[4] = 0;                   /* right and top as 16 bit values (left and bottom 0) */
        blk[5] = (unsigned char)(right & 0xFF); blk[6] = (unsigned char)(right >> 8);
        blk[7] = (unsigned char)(top & 0xFF); blk[8] = (unsigned char)(top >> 8);
        _swix(OS_Word, _INR(0, 1), 21, blk);
        printf("pointer box set to 0,0 - %d,%d\n", right, top);
    }
    printf("%d x %d pixels, eigen factors %d,%d (%d,%d OS units per pixel), %d x %d OS units\n", w, h, xe, ye, 1 << xe,
           1 << ye, w << xe, h << ye);
    printf("log2 bits per pixel %d, colours %d (NColour)\n", mode_var(9), mode_var(3));
    for (i = 0; i < sizeof samples / sizeof samples[0]; i++)
        printf("  %4d OS units  \"%s\"\n", text_width(samples[i]), samples[i]);
    return 0;
}
