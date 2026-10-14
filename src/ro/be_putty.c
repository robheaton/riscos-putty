/*
 * be_putty.c - the list of backends of the PuTTY program: what upstream's be_list.c (compiled with SSH, no serial
 * and the other network protocols) gives, and the local terminal (rolocal.c) as well.
 *
 * Upstream's be_list.c is "rebuilt per application" for this, but offers no way to add a backend of one's own, so
 * the list is repeated here.  The order is the one in the configuration dialogue: the first n_ui_backends have radio
 * buttons (SSH), the rest are in the "Other" drop-down list in this order, so Telnet stays the first.
 */
#include <stdio.h>
#include "putty.h"
#include "rolocal.h"

const char *const appname = "PuTTY";

const int be_default_protocol = PROT_SSH;

const struct BackendVtable *const backends[] = {
    &ssh_backend,
    &telnet_backend,
    &rlogin_backend,
    &supdup_backend,
    &raw_backend,
    &local_backend,
    &sshconn_backend,
    NULL
};

const size_t n_ui_backends = 1;
