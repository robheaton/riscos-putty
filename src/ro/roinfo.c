/*
 * roinfo.c - the "About this program" box.  See roinfo.h.
 *
 * Laid out as !Edit's is, measured in pixels: 320 wide, rows 30 apart, fields 26 high,
 * labels 76 wide.
 */
#include <stdio.h>
#include <string.h>

#include "putty.h"
#include "roversion.h"
#include "roinfo.h"

enum { I_NAME = 10, I_PURPOSE, I_AUTHOR, I_PORT, I_LICENCE, I_VERSION };

dlg *roinfo_new(const char *name, const char *purpose)
{
    static const dlg_item items[] = {
        { DI_RLABEL,  0,         0,   4, 76, 26, "Name",    0, 0, NULL },
        { DI_DISPLAY, I_NAME,    77,  4, 238, 26, "",       0, 0, NULL },
        { DI_RLABEL,  0,         0,  34, 76, 26, "Purpose", 0, 0, NULL },
        { DI_DISPLAY, I_PURPOSE, 77, 34, 238, 26, "",       0, 0, NULL },
        { DI_RLABEL,  0,         0,  64, 76, 26, "Author",  0, 0, NULL },
        { DI_DISPLAY, I_AUTHOR,  77, 64, 238, 26, "",       0, 0, NULL },
        { DI_RLABEL,  0,         0,  94, 76, 26, "Port",    0, 0, NULL },
        { DI_DISPLAY, I_PORT,    77, 94, 238, 26, "",       0, 0, NULL },
        { DI_RLABEL,  0,         0, 124, 76, 26, "Licence", 0, 0, NULL },
        { DI_DISPLAY, I_LICENCE, 77, 124, 238, 26, "",      0, 0, NULL },
        { DI_RLABEL,  0,         0, 154, 76, 26, "Version", 0, 0, NULL },
        { DI_DISPLAY, I_VERSION, 77, 154, 238, 26, "",      0, 0, NULL },
    };
    const char *v = ver + (strncmp(ver, "Release ", 8) ? 0 : 8);       /* "Release 0.85" -> "0.85" */
    char text[64];
    dlg *d = dlg_new_style(DLG_MENU, "About this program", 320, 184, items,
                           (int)(sizeof items / sizeof items[0]), NULL, NULL);

    if (!d) return NULL;
    dlg_set_text(d, I_NAME, name);
    dlg_set_text(d, I_PURPOSE, purpose);
    dlg_set_text(d, I_AUTHOR, "Simon Tatham and others");
    dlg_set_text(d, I_PORT, "Rob Heaton and Claude, 2026");
    dlg_set_text(d, I_LICENCE, "MIT licence");
    snprintf(text, sizeof text, "%s (%s), PuTTY %.*s", RO_PORT_VERSION, RO_PORT_DATE,
             (int)strspn(v, "0123456789."), v);
    dlg_set_text(d, I_VERSION, text);
    dlg_set_help(d, "Information about this program: its name, purpose, author, licence and version.");
    return d;
}
