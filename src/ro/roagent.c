/*
 * roagent.c - the key agent.  See roagent.h.
 *
 * Upstream's pageant.c is the agent proper: it holds the keys and answers the SSH agent protocol.  Here it
 * runs inside the program, so there is nothing to connect to.  This file is the glue the rest of PuTTY
 * expects of a platform (agent_exists, agent_query: the SSH code asks them for the keys it can sign with, and
 * for a signature, and agent forwarding hands the other end's requests to them) and the Keys window.
 *
 * The glue.  pageant_handle_msg() answers through a callback some time after it is called, from PuTTY's
 * top level callbacks, so agent_query() hands back a pending query and calls the SSH code back from a callback
 * of its own once the answer is in.  The agent's own cleaning up after a request runs after it has called us,
 * so a query is never freed from inside the answer.  Keys are held decrypted, so no request ever has to wait
 * for a passphrase (a request that would is refused).
 *
 * The window.  A list of the keys (the type, size and comment of each, a tick box to choose some, and the
 * fingerprint of the one last chosen) with Add key..., Remove, Remove all and Close.  Adding asks for the
 * passphrase in a box if the key has one; a key file can also be dropped on the window or on the icon bar icon.
 * "Start-up list..." opens the text file of keys to add whenever the program starts (roagent_load_startup).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "ssh.h"
#include "pageant.h"
#include "ro.h"
#include "rowimp.h"
#include "rodlg.h"
#include "rodialog.h"
#include "roagent.h"

/* ------------------------------------------------------------------------ */
/* the glue: PuTTY's idea of an agent                                       */

struct agent_pending_query {
    PageantClient pc;
    PageantClientRequestId reqid;
    strbuf *request;                 /* a copy of the request (the caller frees its own) */
    void (*callback)(void *, void *, int);
    void *callback_ctx;
    bool answered;
    void *reply;                     /* the answer with its length on the front; NULL if there was none */
    int replylen;
};

static void query_log(PageantClient *pc, PageantClientRequestId *reqid, const char *fmt, va_list ap)
{
}

static void query_finish(void *vctx);

static void query_got_response(PageantClient *pc, PageantClientRequestId *reqid, ptrlen response)
{
    agent_pending_query *q = container_of(pc, agent_pending_query, pc);
    unsigned char *r = snewn(response.len + 4, unsigned char);

    PUT_32BIT_MSB_FIRST(r, (uint32_t)response.len);
    memcpy(r + 4, response.ptr, response.len);
    q->reply = r;
    q->replylen = (int)(response.len + 4);
    q->answered = true;
    if (q->callback)
        queue_toplevel_callback(query_finish, q);
}

static bool query_ask_passphrase(PageantClient *pc, PageantClientDialogId *dlgid, const char *comment)
{
    return false;                    /* keys are held decrypted: nothing waits for a passphrase */
}

static const PageantClientVtable query_vtable = {
    .log = query_log,
    .got_response = query_got_response,
    .ask_passphrase = query_ask_passphrase,
};

static void query_free(agent_pending_query *q)
{
    pageant_unregister_client(&q->pc);
    strbuf_free(q->request);
    sfree(q);
}

/* The answer is in: tell the SSH code that asked. */
static void query_finish(void *vctx)
{
    agent_pending_query *q = (agent_pending_query *)vctx;
    void (*callback)(void *, void *, int) = q->callback;
    void *ctx = q->callback_ctx;
    void *reply = q->reply;
    const int replylen = q->replylen;

    q->reply = NULL;
    query_free(q);
    callback(ctx, reply, reply ? replylen : 0);
}

/* The agent has keys to offer.  (An empty agent is not "running": the log would only say it has none.) */
bool agent_exists(void)
{
    return pageant_count_ssh1_keys() + pageant_count_ssh2_keys() > 0;
}

agent_pending_query *agent_query(strbuf *request, void **out, int *outlen,
                                 void (*callback)(void *, void *, int), void *callback_ctx)
{
    agent_pending_query *q = snew(agent_pending_query);

    memset(q, 0, sizeof *q);
    q->pc.vt = &query_vtable;
    q->pc.suppress_logging = true;
    q->callback = callback;
    q->callback_ctx = callback_ctx;
    q->request = strbuf_dup(ptrlen_from_strbuf(request));
    pageant_register_client(&q->pc);
    if (request->len < 4) {                              /* not even a length field: nothing to answer */
        *out = NULL;
        *outlen = 0;
        query_free(q);
        return NULL;
    }
    pageant_handle_msg(&q->pc, &q->reqid, make_ptrlen(q->request->s + 4, q->request->len - 4));
    if (callback)
        return q;

    /* the caller will not wait for a callback: run what is pending until the answer is in */
    while (!q->answered && toplevel_callback_pending())
        run_toplevel_callbacks();
    *out = q->reply;
    *outlen = q->reply ? q->replylen : 0;
    q->reply = NULL;
    query_free(q);
    return NULL;
}

void agent_cancel_query(agent_pending_query *q)
{
    delete_callbacks_for_context(q);
    sfree(q->reply);
    query_free(q);
}

/* There is no stream to talk to the agent over; forwarding falls back to handing it each request. */
Socket *agent_connect(Plug *plug)
{
    return new_error_socket_fmt(plug, "the key agent is inside the program");
}

/* ------------------------------------------------------------------------ */
/* the keys                                                                 */

typedef struct AgKey {
    struct pageant_pubkey *key;      /* what the agent is told to identify the key by */
    char *label;                     /* "ssh-ed25519 255  comment" */
    char *fingerprint;               /* "SHA256:..." */
    bool ticked;
} AgKey;

#define ROWS 6
#define LABEL_LEN 120

enum {
    A_ADD = 1, A_REMOVE, A_REMOVEALL, A_CLOSE, A_PREV, A_NEXT, A_COUNT, A_FPLABEL, A_FP, A_HINT, A_HEAD, A_NONE,
    A_STARTUP,
    A_ROW0 = 20                      /* one tick box for each visible row */
};

static struct {
    dlg     *d;
    AgKey   *keys;
    size_t   n;
    int      top;                    /* the first key shown */
    int      shown;                  /* the key whose fingerprint is shown, or -1 */
    struct {                         /* a key being added, waiting for its passphrase */
        char *path;
        char *comment;
        bool retry;
    } add;
    char   **startq;                 /* key files still to be added from the start-up list */
    size_t   nstart, startpos;
} ag = { .shown = -1 };

static void free_keys(void)
{
    size_t i;

    for (i = 0; i < ag.n; i++) {
        pageant_pubkey_free(ag.keys[i].key);
        sfree(ag.keys[i].label);
        sfree(ag.keys[i].fingerprint);
    }
    sfree(ag.keys);
    ag.keys = NULL;
    ag.n = 0;
}

static void note_key(void *ctx, char **fingerprints, const char *comment, uint32_t flags,
                     struct pageant_pubkey *key)
{
    const FingerprintType ft = ssh2_pick_default_fingerprint(fingerprints);
    char *full = dupstr(fingerprints[ft]), *word[3] = { NULL, NULL, NULL };
    char *p = full;
    int nwords = 0;
    AgKey *k;
    size_t cap = ag.n + 1;

    (void)ctx; (void)flags;
    while (nwords < 3) {                                   /* "ssh-ed25519 255 SHA256:..." or "1024 12:34:..." */
        word[nwords++] = p;
        p = strchr(p, ' ');
        if (!p) break;
        *p++ = '\0';
    }
    ag.keys = sresize(ag.keys, cap, AgKey);
    k = &ag.keys[ag.n++];
    memset(k, 0, sizeof *k);
    k->key = pageant_pubkey_copy(key);
    if (nwords == 3) {
        k->label = dupprintf("%s %s  %s", word[0], word[1], comment);
        k->fingerprint = dupstr(word[2]);
    } else {
        k->label = dupprintf("ssh-1 %s  %s", word[0], comment);
        k->fingerprint = dupstr(nwords > 1 ? word[1] : "");
    }
    sfree(full);
}

static void load_keys(void)
{
    char *err = NULL;

    free_keys();
    if (pageant_enum_keys(note_key, NULL, &err) != PAGEANT_ACTION_OK) {
        ro_notice(&ag, "Keys", err ? err : "The keys could not be listed.");
        free_keys();
    }
    sfree(err);
}

/* ------------------------------------------------------------------------ */
/* the window                                                               */

static void show(void)
{
    char text[64];
    size_t i;
    bool any_ticked = false;

    if (!ag.d) return;
    if (ag.top >= (int)ag.n) ag.top = ag.n > ROWS ? (int)ag.n - ROWS : 0;
    if (ag.top < 0) ag.top = 0;
    for (i = 0; i < ROWS; i++) {
        const int id = A_ROW0 + (int)i;
        const size_t k = (size_t)ag.top + i;
        if (k < ag.n) {
            dlg_set_text(ag.d, id, ag.keys[k].label);
            dlg_set_selected(ag.d, id, ag.keys[k].ticked);
            dlg_set_hidden(ag.d, id, 0);
        } else {
            dlg_set_hidden(ag.d, id, 1);
        }
    }
    dlg_set_text(ag.d, A_NONE, ag.n == 0 ? "No keys are loaded." : "");
    for (i = 0; i < ag.n; i++)
        if (ag.keys[i].ticked) any_ticked = true;
    if (ag.n == 0)
        snprintf(text, sizeof text, "No keys");
    else if (ag.n <= ROWS)
        snprintf(text, sizeof text, "%u key%s", (unsigned)ag.n, ag.n == 1 ? "" : "s");
    else
        snprintf(text, sizeof text, "Keys %d to %d of %u", ag.top + 1,
                 (int)(ag.top + ROWS < (int)ag.n ? ag.top + ROWS : (int)ag.n), (unsigned)ag.n);
    dlg_set_text(ag.d, A_COUNT, text);
    dlg_set_shaded(ag.d, A_PREV, ag.top == 0);
    dlg_set_shaded(ag.d, A_NEXT, ag.top + ROWS >= (int)ag.n);
    dlg_set_shaded(ag.d, A_REMOVE, !any_ticked);
    dlg_set_shaded(ag.d, A_REMOVEALL, ag.n == 0);
    dlg_set_text(ag.d, A_FP, ag.shown >= 0 && (size_t)ag.shown < ag.n ? ag.keys[ag.shown].fingerprint : "");
}

/* The agent's keys have changed (this is the front end's hook, which pageant.c calls). */
void keylist_update(void)
{
    if (!ag.d) return;
    load_keys();
    ag.shown = -1;
    show();
}

static void forget_add(void)
{
    sfree(ag.add.path);
    sfree(ag.add.comment);
    memset(&ag.add, 0, sizeof ag.add);
    pageant_forget_passphrases();
}

static void startup_continue(void);
static void startup_callback(void *ctx) { (void)ctx; startup_continue(); }

/* One key has been dealt with (added, refused or given up on): go on to the next of the start-up list. */
static void add_over(void)
{
    forget_add();
    if (ag.startpos < ag.nstart)
        queue_toplevel_callback(startup_callback, NULL);
}

static void notice(const char *fmt, ...)
{
    va_list ap;
    char *s;
    va_start(ap, fmt);
    s = dupvprintf(fmt, ap);
    va_end(ap);
    ro_notice(&ag, "Keys", s);
    sfree(s);
}

static void try_add(const char *passphrase);

static void passphrase_answered(void *ctx, const char *answer)
{
    (void)ctx;
    if (!answer || !ag.add.path) {                         /* cancelled */
        add_over();
        return;
    }
    try_add(answer);
}

static void ask_passphrase(void)
{
    char *msg = dupprintf("%sEnter the passphrase to unlock the key\n%s",
                          ag.add.retry ? "That passphrase was wrong.\n" : "",
                          ag.add.comment ? ag.add.comment : "");
    ro_prompt(&ag, "Key passphrase", msg, true, "", passphrase_answered, NULL);
    sfree(msg);
}

/* Offer the agent the key file, with the passphrase if one has been typed. */
static void try_add(const char *passphrase)
{
    Filename *fn = filename_from_str(ag.add.path);
    char *ret = NULL;
    const int result = pageant_add_keyfile(fn, passphrase, &ret, false);

    filename_free(fn);
    switch (result) {
      case PAGEANT_ACTION_NEED_PP:
        sfree(ag.add.comment);
        ag.add.comment = ret;
        ret = NULL;
        ag.add.retry = passphrase != NULL;                 /* (it asked again: the one we gave was wrong) */
        ask_passphrase();
        return;
      case PAGEANT_ACTION_FAILURE:
        notice("The key %s could not be added.\n%s", ag.add.path, ret ? ret : "");
        break;
      case PAGEANT_ACTION_WARNING:
        notice("The key %s was added, but:\n%s", ag.add.path, ret ? ret : "");
        break;
      default:
        break;
    }
    sfree(ret);
    keylist_update();
    add_over();
}

static void begin_add(const char *path)
{
    forget_add();
    ag.add.path = dupstr(path);
    try_add(NULL);
}

/* Keys waiting to be added, one at a time: the start-up list, or a key dropped while another is being unlocked. */
static void queue_key(const char *path)
{
    ag.startq = sresize(ag.startq, ag.nstart + 1, char *);
    ag.startq[ag.nstart++] = dupstr(path);
}

/* Add the next queued key, if none is being added now (add_over comes back here when it is over). */
static void startup_continue(void)
{
    if (ag.add.path) return;
    if (ag.startpos < ag.nstart) {
        char *path = ag.startq[ag.startpos];
        ag.startq[ag.startpos++] = NULL;
        begin_add(path);
        sfree(path);
    }
    if (ag.startpos >= ag.nstart) {                        /* the queue is used up */
        sfree(ag.startq);
        ag.startq = NULL;
        ag.nstart = ag.startpos = 0;
    }
}

void roagent_add_file(const char *path)
{
    roagent_open();
    queue_key(path);
    startup_continue();
}

/* Add the keys the start-up list names (they ask for their passphrases in turn). */
void roagent_load_startup(void)
{
    char *fname = ro_agentkeys_path(false), *line;
    FILE *fp = fopen(fname, "r");

    sfree(fname);
    if (!fp) return;
    while ((line = fgetline(fp)) != NULL) {
        char *p = line, *end;
        while (*p == ' ' || *p == '\t') p++;
        end = p + strlen(p);
        while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) *--end = '\0';
        if (*p && *p != '#')
            queue_key(p);
        sfree(line);
    }
    fclose(fp);
    startup_continue();
}

/* Open the start-up list in whatever edits text. */
static void edit_startup_list(void)
{
    char *fname = ro_agentkeys_path(true);
    char *cmd = dupprintf("Filer_Run %s", fname);
    _kernel_oserror *e = _swix(OS_CLI, _IN(0), cmd);

    if (e) notice("The start-up list could not be opened.\n%s", e->errmess);
    sfree(cmd);
    sfree(fname);
}

static void path_answered(void *ctx, const char *answer)
{
    (void)ctx;
    if (answer && *answer && ag.d) roagent_add_file(answer);
}

static void remove_ticked(void)
{
    size_t i;
    char *err = NULL;

    for (i = 0; i < ag.n; i++) {
        if (!ag.keys[i].ticked) continue;
        if (pageant_delete_key(ag.keys[i].key, &err) != PAGEANT_ACTION_OK) {
            notice("A key could not be removed.\n%s", err ? err : "");
            sfree(err);
            break;
        }
    }
    keylist_update();
}

static void remove_all_confirmed(void *ctx, int button)
{
    char *err = NULL;
    (void)ctx;
    if (button != 0) return;
    if (pageant_delete_all_keys(&err) != PAGEANT_ACTION_OK) {
        notice("The keys could not be removed.\n%s", err ? err : "");
        sfree(err);
    }
    keylist_update();
}

static void ag_click(dlg *d, int id, int buttons, void *ud)
{
    (void)d; (void)ud;
    if (!(buttons & (BTN_SELECT | BTN_CLICK_SELECT | BTN_ADJUST | BTN_CLICK_ADJUST))) return;
    if (id >= A_ROW0 && id < A_ROW0 + ROWS) {              /* a tick box was clicked: the key it is for is the one described */
        const size_t k = (size_t)ag.top + (size_t)(id - A_ROW0);
        if (k < ag.n) {
            ag.keys[k].ticked = dlg_get_selected(ag.d, id) != 0;
            ag.shown = (int)k;
            show();
        }
        return;
    }
    switch (id) {
      case A_ADD:
        ro_prompt(&ag, "Add key", "Type the pathname of the private key file to add.\n"
                  "(Or drag the file onto this window, or onto the icon bar icon.)", false, "", path_answered, NULL);
        break;
      case A_REMOVE:
        remove_ticked();
        break;
      case A_REMOVEALL: {
        static const char *const buttons[] = { "Remove all", "Cancel" };
        ro_msgbox(&ag, "Keys", "Remove all the keys from the agent?\n(The key files themselves are not touched.)",
                  buttons, 2, 1, 1, remove_all_confirmed, NULL);
        break;
      }
      case A_STARTUP:
        edit_startup_list();
        break;
      case A_PREV:
        ag.top -= ROWS;
        show();
        break;
      case A_NEXT:
        ag.top += ROWS;
        show();
        break;
      case A_CLOSE:
        dlg_close(ag.d);
        break;
    }
}

static void build_window(void)
{
    dlg_item items[ROWS + 13];
    int n = 0, i;
    static char rowhelp[] = "A key the agent holds. Tick it to remove it; its fingerprint is shown below.";

    memset(items, 0, sizeof items);
#define ITEM(t, i_, x_, y_, w_, h_, txt, max, hlp) \
    do { items[n].type = (t); items[n].id = (i_); items[n].x = (x_); items[n].y = (y_); items[n].w = (w_); \
         items[n].h = (h_); items[n].text = (txt); items[n].maxlen = (max); items[n].help = (hlp); n++; } while (0)
    ITEM(DI_LABEL, A_HEAD, 16, 10, 528, 22, "The agent holds these keys for logging in. Tick the ones to remove.", 0, NULL);
    ITEM(DI_LABEL, A_NONE, 16, 42, 528, 22, "", 40, NULL);
    for (i = 0; i < ROWS; i++)
        ITEM(DI_OPTION, A_ROW0 + i, 16, 40 + i * 26, 528, 24, "", LABEL_LEN, rowhelp);
    ITEM(DI_LABEL, A_COUNT, 16, 200, 300, 22, "", 64, NULL);
    ITEM(DI_BUTTON, A_PREV, 392, 198, 72, 26, "Previous", 0, "Show the keys before these.");
    ITEM(DI_BUTTON, A_NEXT, 472, 198, 72, 26, "Next", 0, "Show more keys.");
    ITEM(DI_LABEL, A_FPLABEL, 16, 236, 528, 22, "Fingerprint of the key you last ticked:", 0, NULL);
    ITEM(DI_INFO, A_FP, 16, 258, 528, 26, "", 100, "The fingerprint of the key you last ticked.");
    ITEM(DI_BUTTON, A_ADD, 16, 300, 104, 30, "Add key...", 0, "Add a private key (a .ppk file) to the agent.");
    ITEM(DI_BUTTON, A_REMOVE, 128, 300, 80, 30, "Remove", 0, "Take the ticked keys out of the agent.");
    ITEM(DI_BUTTON, A_REMOVEALL, 216, 300, 96, 30, "Remove all", 0, "Take every key out of the agent.");
    ITEM(DI_BUTTON, A_STARTUP, 320, 300, 124, 30, "Start-up list...", 0,
         "Edit the list of key files that are added each time PuTTY starts.");
    ITEM(DI_DEFBUTTON, A_CLOSE, 452, 300, 92, 30, "Close", 0, "Close this window. The keys stay in the agent.");
    ITEM(DI_LABEL, A_HINT, 16, 340, 528, 22, "Drag a private key file (.ppk) here to add it.", 0, NULL);
#undef ITEM
    ag.d = dlg_new("PuTTY keys", 560, 372, items, n, ag_click, NULL);
    if (ag.d) {
        dlg_set_keys(ag.d, A_CLOSE, A_CLOSE);
        dlg_set_close_id(ag.d, A_CLOSE);
    }
}

void roagent_open(void)
{
    if (!ag.d) build_window();
    if (!ag.d) return;
    load_keys();
    ag.shown = -1;
    show();
    dlg_open(ag.d);
}

/* A key file is a private key PuTTY can read (not a public key, not some other file). */
static bool is_private_key_file(const char *path)
{
    Filename *fn = filename_from_str(path);
    const int t = key_type(fn);
    filename_free(fn);
    return t == SSH_KEYTYPE_SSH1 || t == SSH_KEYTYPE_SSH2;
}

int roagent_message(const wmessage *m)
{
    char path[256];
    const int win = m->data[0];

    if (m->action != MSG_DATA_LOAD) return 0;
    if (!(ag.d && win == dlg_window(ag.d)) && win != -2) return 0;
    wimp_msg_path(m, path, sizeof path);
    if (!path[0]) return 0;
    if (win == -2 && !is_private_key_file(path)) return 0;     /* on the icon bar: only a key is ours */
    wimp_send_load_ack(m, path);
    roagent_add_file(path);
    return 1;
}

static bool started;

void roagent_init(void)
{
    if (started) return;
    started = true;
    pageant_init();
}

void roagent_shutdown(void)
{
    if (!started) return;
    started = false;
    forget_add();
    while (ag.startpos < ag.nstart) sfree(ag.startq[ag.startpos++]);
    sfree(ag.startq);
    ag.startq = NULL;
    ag.nstart = ag.startpos = 0;
    pageant_delete_all();
    free_keys();
    if (ag.d) {
        dlg_free(ag.d);
        ag.d = NULL;
    }
}
