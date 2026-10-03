/*
 * rokeygen.h - the key generator window (PuTTYgen's job, in the Wimp).
 *
 * Makes SSH key pairs (RSA, DSA, ECDSA, EdDSA), loads a private key from a file - PuTTY's .ppk,
 * or one made by OpenSSH or ssh.com, dropped on the window or its icon bar icon, or named in a
 * box - lets the comment and passphrase be changed, and saves the private key, the public key
 * or a converted private key through the save box (rosave.h).  The public key can also be put
 * on the clipboard, ready to paste into a server's authorized_keys file.
 */
#ifndef PUTTY_ROKEYGEN_H
#define PUTTY_ROKEYGEN_H

#include <stdbool.h>
#include "rowimp.h"

/* Open the window, or bring it to the front. */
void rokeygen_open(void);

/* Open the window and load this key file into it. */
void rokeygen_load(const char *path);

/* A key file dropped on the window or the icon bar icon (Message_DataLoad); returns 1 if the
 * message was one for us. */
int  rokeygen_message(const wmessage *m);

/* Is there a key on show that was made (or changed) and has not been saved?  Quitting would lose it. */
bool rokeygen_unsaved(void);

/* While a key is shown (so a change to its comment can be noticed) or the mouse is being watched
 * for randomness, the main loop must wake now and then and call rokeygen_idle. */
bool rokeygen_idle_wanted(void);
void rokeygen_idle(void);

#endif /* PUTTY_ROKEYGEN_H */
