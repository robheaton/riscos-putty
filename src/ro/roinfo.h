/*
 * roinfo.h - the standard "About this program" box, shared by the PuTTY and PuTTYgen tasks.
 */
#ifndef PUTTY_ROINFO_H
#define PUTTY_ROINFO_H

#include "rodlg.h"

/*
 * The box the Toolbox's ProgInfo and the ROM applications show (Edit, Paint): a title bar and
 * nothing else, a right-aligned label and a sunken read-only field for each of name, purpose,
 * author, port, licence and version.  It is the submenu of the Info item of the icon bar menu,
 * so the Wimp opens and closes it: give dlg_window() of the result to menu_set_window().
 * Returns NULL if it could not be made.
 */
dlg *roinfo_new(const char *name, const char *purpose);

#endif /* PUTTY_ROINFO_H */
