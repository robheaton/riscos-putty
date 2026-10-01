/*
 * roconnect.c - the Connect dialogue.
 *
 * The dialogue holds the handful of settings that decide where a session goes (host
 * name, port, protocol, login name, private key); everything else comes from a
 * working Conf, which starts as the defaults and is replaced when a saved session
 * is loaded.  Open starts a session from a copy of that Conf with the dialogue's
 * fields applied.  The full configuration dialogue will edit the same Conf.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "storage.h"
#include "ro.h"
#include "rowimp.h"
#include "rodlg.h"
#include "romenu.h"
#include "rodialog.h"
#include "roterm.h"
#include "roapp.h"
#include "roconnect.h"
#include "roconfig.h"

enum {
    C_HOST = 1, C_PORT, C_SSH, C_TELNET, C_RLOGIN, C_RAW, C_USER, C_KEY,
    C_SESSION, C_LIST, C_LOAD, C_SAVE, C_DELETE, C_OPEN, C_CANCEL, C_SETTINGS
};

#define DEFAULT_SETTINGS "Default Settings"

static dlg  *d_conn;
static Conf *work;                     /* the settings a new session starts from */
static int   last_proto = PROT_SSH;    /* the protocol the port field was last matched to */

/* the saved sessions, as listed in the menu on screen */
static menu  *sess_menu;
static char **sess_names;
static int    sess_count;

/* ------------------------------------------------------------------------ */
/* fields <-> Conf                                                          */

static int default_port_for(int proto)
{
    switch (proto) {
      case PROT_SSH:    return 22;
      case PROT_TELNET: return 23;
      case PROT_RLOGIN: return 513;
      default:          return 0;
    }
}

static int protocol_from_fields(void)
{
    if (dlg_get_selected(d_conn, C_TELNET)) return PROT_TELNET;
    if (dlg_get_selected(d_conn, C_RLOGIN)) return PROT_RLOGIN;
    if (dlg_get_selected(d_conn, C_RAW))    return PROT_RAW;
    return PROT_SSH;
}

static void fields_from_conf(Conf *conf)
{
    char buf[16];
    int proto = conf_get_int(conf, CONF_protocol);
    const Filename *key = conf_get_filename(conf, CONF_keyfile);

    if (proto != PROT_TELNET && proto != PROT_RLOGIN && proto != PROT_RAW) proto = PROT_SSH;
    dlg_set_text(d_conn, C_HOST, conf_get_str(conf, CONF_host));
    snprintf(buf, sizeof buf, "%d", conf_get_int(conf, CONF_port));
    dlg_set_text(d_conn, C_PORT, buf);
    dlg_set_selected(d_conn, C_SSH, proto == PROT_SSH);
    dlg_set_selected(d_conn, C_TELNET, proto == PROT_TELNET);
    dlg_set_selected(d_conn, C_RLOGIN, proto == PROT_RLOGIN);
    dlg_set_selected(d_conn, C_RAW, proto == PROT_RAW);
    dlg_set_text(d_conn, C_USER, conf_get_str_ambi(conf, CONF_username, NULL));
    dlg_set_text(d_conn, C_KEY, key ? filename_to_str(key) : "");
    last_proto = proto;
}

/* Copy the text of a field with surrounding blanks removed. */
static char *field_text(int id)
{
    const char *s = dlg_get_text(d_conn, id);
    size_t n;
    while (*s == ' ') s++;
    n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') n--;
    return dupprintf("%.*s", (int)n, s);
}

/* Apply the fields to conf.  Returns false (after saying why) if they will not do. */
static bool conf_from_fields(Conf *conf, bool need_host)
{
    char *host = field_text(C_HOST), *port = field_text(C_PORT);
    char *user = field_text(C_USER), *key = field_text(C_KEY);
    const int proto = protocol_from_fields();
    int portnum = default_port_for(proto);
    bool ok = true;

    if (*port) {
        char *end;
        long v = strtol(port, &end, 10);
        if (*end || v < 1 || v > 65535) {
            ro_notice(NULL, "PuTTY", "The port must be a number from 1 to 65535.");
            dlg_focus(d_conn, C_PORT);
            ok = false;
        } else {
            portnum = (int)v;
        }
    }
    if (ok && need_host && !*host) {
        ro_notice(NULL, "PuTTY", "Type the name or address of the computer to connect to in the "
                  "Host name box.");
        dlg_focus(d_conn, C_HOST);
        ok = false;
    }
    if (ok) {
        Filename *fn = filename_from_str(key);
        conf_set_str(conf, CONF_host, host);
        conf_set_int(conf, CONF_port, portnum);
        conf_set_int(conf, CONF_protocol, proto);
        conf_set_str(conf, CONF_username, user);
        conf_set_filename(conf, CONF_keyfile, fn);
        filename_free(fn);
    }
    sfree(host);
    sfree(port);
    sfree(user);
    sfree(key);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* saved sessions                                                           */

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void free_session_list(void)
{
    int i;
    for (i = 0; i < sess_count; i++) sfree(sess_names[i]);
    sfree(sess_names);
    sess_names = NULL;
    sess_count = 0;
    menu_free(sess_menu);
    sess_menu = NULL;
}

/* Read the saved sessions, sorted, with "Default Settings" first. */
static void read_session_list(void)
{
    settings_e *e = enum_settings_start();
    strbuf *sb = strbuf_new();
    int cap = 16;

    free_session_list();
    sess_names = snewn(cap, char *);
    sess_names[sess_count++] = dupstr(DEFAULT_SETTINGS);
    if (e) {
        while (enum_settings_next(e, sb)) {
            if (!strcmp(sb->s, DEFAULT_SETTINGS)) {
                strbuf_clear(sb);
                continue;
            }
            if (sess_count == cap) {
                cap *= 2;
                sess_names = sresize(sess_names, cap, char *);
            }
            sess_names[sess_count++] = dupstr(sb->s);
            strbuf_clear(sb);
        }
        enum_settings_finish(e);
    }
    strbuf_free(sb);
    qsort(sess_names + 1, (size_t)(sess_count - 1), sizeof *sess_names, name_cmp);
}

static void load_session(const char *name)
{
    const bool is_default = !strcmp(name, DEFAULT_SETTINGS);
    if (!*name) return;
    conf_free(work);
    work = conf_new();
    /* "Default Settings" always exists: it is the built-in defaults until somebody saves it */
    if (!do_defaults(is_default ? NULL : name, work) && !is_default) {
        char *msg = dupprintf("There is no saved session called \"%s\".", name);
        ro_notice(NULL, "PuTTY", msg);
        sfree(msg);
        do_defaults(NULL, work);
    }
    fields_from_conf(work);
}

static void session_menu_selection(const int *sel)
{
    if (sel[0] >= 0 && sel[0] < sess_count && d_conn) {
        dlg_set_text(d_conn, C_SESSION, sess_names[sel[0]]);
        load_session(sess_names[sel[0]]);
    }
}

static void open_session_menu(void)
{
    wpointer p;
    int i;

    read_session_list();
    sess_menu = menu_new("Sessions");
    if (!sess_menu) return;
    for (i = 0; i < sess_count && i < MAX_MENU_ITEMS; i++) menu_add(sess_menu, sess_names[i], 0);
    if (_swix(Wimp_GetPointerInfo, _IN(1), &p)) return;
    roapp_menu_opened(session_menu_selection);
    menu_open(sess_menu, p.x - 64, p.y);
}

static void save_session(void)
{
    char *name = field_text(C_SESSION), *err;
    Conf *c;

    if (!*name) {
        ro_notice(NULL, "PuTTY", "Type a name for the session in the Session box, then press Save.");
        dlg_focus(d_conn, C_SESSION);
        sfree(name);
        return;
    }
    c = conf_copy(work);
    if (!conf_from_fields(c, false)) {
        conf_free(c);
        sfree(name);
        return;
    }
    err = save_settings(!strcmp(name, DEFAULT_SETTINGS) ? NULL : name, c);
    if (err) {
        ro_notice(NULL, "PuTTY", err);
        sfree(err);
    } else {
        conf_free(work);
        work = c;
        c = NULL;
    }
    if (c) conf_free(c);
    sfree(name);
}

static void delete_session(void)
{
    char *name = field_text(C_SESSION);
    if (*name && strcmp(name, DEFAULT_SETTINGS)) {
        del_settings(name);
        dlg_set_text(d_conn, C_SESSION, "");
    }
    sfree(name);
}

/* ------------------------------------------------------------------------ */
/* the dialogue                                                             */

static void open_session(int buttons)
{
    Conf *c = conf_copy(work);
    if (!conf_from_fields(c, true)) {
        conf_free(c);
        return;
    }
    if (!(buttons & BTN_ADJUST)) dlg_close(d_conn);      /* Adjust keeps the box open */
    if (!roterm_new(c)) conf_free(c);
}

/* The full configuration dialogue, on a copy of the working settings with the
 * fields applied; its Open starts the session. */
static void settings_done(void *ctx, bool ok, Conf *conf)
{
    if (!ok) return;
    if (d_conn) dlg_close(d_conn);
    if (!roterm_new(conf)) conf_free(conf);
}

static char settings_owner;          /* tells the configuration dialogue from the Settings button apart */

static void open_settings(void)
{
    Conf *c;
    if (roconfig_raise(&settings_owner)) return;        /* already open */
    c = conf_copy(work);
    if (!conf_from_fields(c, false)) {
        conf_free(c);
        return;
    }
    roconfig_open(c, false, 0, 0, "PuTTY Configuration", settings_done, &settings_owner);
}

static void connect_click(dlg *d, int id, int buttons, void *ud)
{
    char buf[16];
    int proto;
    (void)ud;

    switch (id) {
      case C_SSH: case C_TELNET: case C_RLOGIN: case C_RAW:
        /* a port that was the old protocol's default becomes the new one's */
        proto = protocol_from_fields();
        if (proto != last_proto) {
            const int old_default = default_port_for(last_proto);
            char *port = field_text(C_PORT);
            if (!*port || atoi(port) == old_default) {
                snprintf(buf, sizeof buf, "%d", default_port_for(proto));
                dlg_set_text(d, C_PORT, default_port_for(proto) ? buf : "");
            }
            sfree(port);
            last_proto = proto;
        }
        break;
      case C_LIST:
        open_session_menu();
        break;
      case C_LOAD: {
        char *name = field_text(C_SESSION);
        load_session(name);
        sfree(name);
        break;
      }
      case C_SAVE:
        save_session();
        break;
      case C_DELETE:
        delete_session();
        break;
      case C_SETTINGS:
        open_settings();
        break;
      case C_OPEN:
        open_session(buttons);
        break;
      case C_CANCEL:
        dlg_close(d);
        break;
    }
}

static void build_dialog(void)
{
    static const dlg_item items[] = {
        { DI_RLABEL,    0,         10,  16,  96, 22, "Host name:", 0, 0, NULL },
        { DI_FIELD,     C_HOST,   112,  14, 292, 26, "", 128, 0,
          "The name or address of the computer to connect to, e.g. pi.local or 192.168.1.5." },
        { DI_RLABEL,    0,        408,  16,  40, 22, "Port:", 0, 0, NULL },
        { DI_FIELD,     C_PORT,   452,  14,  56, 26, "22", 8, 0,
          "The port number on that computer (22 for SSH, 23 for Telnet)." },

        { DI_RLABEL,    0,         10,  52,  96, 22, "Connection:", 0, 0, NULL },
        { DI_RADIO,     C_SSH,    112,  50,  64, 24, "SSH", 0, 1, "Use the Secure Shell protocol (recommended)." },
        { DI_RADIO,     C_TELNET, 182,  50,  80, 24, "Telnet", 0, 1, "Use Telnet: nothing is encrypted." },
        { DI_RADIO,     C_RLOGIN, 268,  50,  80, 24, "Rlogin", 0, 1, "Use Rlogin: nothing is encrypted." },
        { DI_RADIO,     C_RAW,    354,  50,  60, 24, "Raw", 0, 1, "Make a plain connection to the port, with no protocol." },

        { DI_RLABEL,    0,         10,  88,  96, 22, "Login name:", 0, 0, NULL },
        { DI_FIELD,     C_USER,   112,  86, 292, 26, "", 128, 0,
          "The name to log in as. Leave it empty to be asked when you connect." },

        { DI_RLABEL,    0,         10, 124,  96, 22, "Key file:", 0, 0, NULL },
        { DI_FIELD,     C_KEY,    112, 122, 396, 26, "", 256, 0,
          "A private key (.ppk) to log in with. Drag the key's file here." },
        { DI_LABEL,     0,        112, 152, 396, 20, "Drag a private key file onto this window to fill it in.", 0, 0, NULL },

        { DI_RLABEL,    0,         10, 188,  96, 22, "Session:", 0, 0, NULL },
        { DI_FIELD,     C_SESSION,112, 186, 292, 26, "", 128, 0,
          "The name to save these settings under, or of a saved session to load." },
        { DI_BUTTON,    C_LIST,   410, 186,  98, 26, "Sessions...", 0, 0, "Choose one of the saved sessions." },
        { DI_BUTTON,    C_LOAD,   112, 220,  72, 28, "Load", 0, 0, "Load the saved session named above." },
        { DI_BUTTON,    C_SAVE,   190, 220,  72, 28, "Save", 0, 0, "Save these settings as the session named above." },
        { DI_BUTTON,    C_DELETE, 268, 220,  72, 28, "Delete", 0, 0, "Delete the saved session named above." },

        { DI_BUTTON,    C_SETTINGS, 14, 272, 104, 30, "Settings...", 0, 0,
          "Change every other setting of the session: fonts, colours, SSH options and more." },
        { DI_BUTTON,    C_CANCEL, 342, 272,  78, 30, "Cancel", 0, 0, "Close this window without connecting." },
        { DI_DEFBUTTON, C_OPEN,   430, 272,  78, 30, "Open", 0, 0,
          "Connect. Adjust-click to keep this window open." },
    };

    d_conn = dlg_new("PuTTY - connect", 520, 316, items, (int)(sizeof items / sizeof items[0]),
                     connect_click, NULL);
    if (d_conn) {
        dlg_set_keys(d_conn, C_OPEN, C_CANCEL);
        dlg_set_help(d_conn, "Choose a computer to connect to with PuTTY.");
    }
}

void roconnect_open(void)
{
    if (!d_conn) {
        build_dialog();
        if (!d_conn) return;
    }
    if (!work) {
        work = conf_new();
        do_defaults(NULL, work);
    }
    if (!dlg_is_open(d_conn)) {
        fields_from_conf(work);
        dlg_set_text(d_conn, C_SESSION, "");
    }
    dlg_open(d_conn);
    dlg_focus(d_conn, C_HOST);
}

int roconnect_message(const wmessage *m)
{
    char path[200];

    /* a file dropped on the dialogue is a private key */
    if (m->action != MSG_DATA_LOAD || !d_conn || m->data[0] != dlg_window(d_conn))
        return 0;
    wimp_msg_path(m, path, sizeof path);
    if (path[0]) {
        dlg_set_text(d_conn, C_KEY, path);
        wimp_send_load_ack(m, path);
    }
    return 1;
}
