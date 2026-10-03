/*
 * rokeygen.c - the key generator window.  See rokeygen.h.
 *
 * This is PuTTYgen's job done in the Wimp; PuTTY's own code (keygen/, sshpubk.c, import.c) makes,
 * reads and writes the keys, and what is here is the window, and the order things happen in,
 * which follows windows/puttygen.c.
 *
 * Making a key takes seconds to minutes and PuTTY's generators cannot be interrupted, so they
 * run as an ordinary function call that lets the desktop go on between steps of its progress
 * reports (roapp_yield), and gives up by jumping out of the call (longjmp) if Cancel is
 * clicked: the few kilobytes of half-built key are lost, which is acceptable for something
 * that is done once in a long while.
 *
 * Randomness.  With the CryptRandom module (noise.c) a key is seeded from it, as the puttygen
 * command does.  Without it the person has to supply the randomness by moving the mouse over
 * the window, as PuTTYgen does everywhere: the pointer's position and the time are collected
 * until there is as much as the key needs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <errno.h>

#include "putty.h"
#include "ssh.h"
#include "storage.h"
#include "sshkeygen.h"
#include "mpint.h"
#include "ro.h"
#include "rowimp.h"
#include "rodlg.h"
#include "rodialog.h"
#include "romenu.h"
#include "roapp.h"
#include "roclip.h"
#include "rosave.h"
#include "rokeygen.h"

#define TITLE      "Key generator"
#define MAX_BITS   16384

enum {
    K_RSA = 1, K_DSA, K_ECDSA, K_EDDSA, K_BITS, K_HINT, K_GENERATE, K_STATUS, K_BAR,
    K_KEYHEAD, K_PUBKEY, K_FINGERPRINT, K_COMMENT, K_PASS1, K_PASS2,
    K_LOAD, K_COPY, K_SAVEPUB, K_SAVEPRIV,
    K_CLOSE                         /* (not an icon: what the close icon does) */
};

typedef enum { KIND_RSA, KIND_DSA, KIND_ECDSA, KIND_EDDSA } Kind;
typedef enum { S_IDLE, S_ENTROPY, S_GENERATING } State;
typedef enum { SK_PPK, SK_PUB_OPENSSH, SK_PUB_RFC4716, SK_OPENSSH, SK_OPENSSH_NEW, SK_SSHCOM } SaveKind;

/* ---- progress reports from the generators: the bar's length, phase by phase ---- */

#define PROGRESS_RANGE 1000

struct phase {
    double startpoint, total;
    double exp_probability, exp_current_value;      /* for phases that repeat until they succeed */
};

struct progress {
    ProgressReceiver rec;
    size_t nphases, phasessize;
    struct phase *phases, *curr;
    double scale;
};

static struct {
    dlg        *d;
    State       state;
    Kind        kind;                   /* what is being generated */
    int         bits;
    ssh_key    *gen;                    /* ... and the result of it */
    ssh2_userkey key;                   /* the key on show (key.key is NULL if there is none) */
    bool        dirty;                  /* made or changed since it was last saved */
    FingerprintType fptype;
    bool        random_held;
    bool        cancel, close_after;
    jmp_buf     abort_env;
    struct progress prog;
    Kind        pending_kind;           /* a generation waiting for the person to confirm it */
    int         pending_bits;

    strbuf     *entropy;                /* the mouse, while we wait for randomness */
    size_t      entropy_got, entropy_required;
    uint32_t    entropy_prev;
    int         prev_x, prev_y;

    struct {                            /* a key being loaded, waiting for its passphrase */
        char *path;
        int   type, realtype;
        char *comment;
        bool  retry;
    } load;


    menu       *menu, *export_menu, *fp_menu;
} kg;

static Kind shown_kind = KIND_RSA;      /* the kind the size field and hint were last set for */

/* ------------------------------------------------------------------------ */
/* small things                                                             */

static void wipe_free(char *s)
{
    if (s) {
        smemclr(s, strlen(s));
        sfree(s);
    }
}

static void set_status(const char *fmt, ...)
{
    va_list ap;
    char *s;
    if (!kg.d) return;
    va_start(ap, fmt);
    s = dupvprintf(fmt, ap);
    va_end(ap);
    dlg_set_text(kg.d, K_STATUS, s);
    sfree(s);
}

static void notice(const char *fmt, ...)
{
    va_list ap;
    char *s;
    va_start(ap, fmt);
    s = dupvprintf(fmt, ap);
    va_end(ap);
    ro_notice(&kg, TITLE, s);
    sfree(s);
}

static void free_key(void)
{
    if (kg.key.key) ssh_key_free(kg.key.key);
    sfree(kg.key.comment);
    kg.key.key = NULL;
    kg.key.comment = NULL;
}

static void hold_random(void)
{
    if (!kg.random_held) {
        random_ref();
        kg.random_held = true;
    }
}

static void release_random(void)
{
    if (kg.random_held) {
        random_unref();
        kg.random_held = false;
    }
}

static int default_bits(Kind k)
{
    return k == KIND_ECDSA ? 384 : k == KIND_EDDSA ? 255 : 2048;
}

static const char *hint_for(Kind k)
{
    switch (k) {
      case KIND_RSA:   return "bits: 2048 or more is best";
      case KIND_DSA:   return "bits: 1024 is all OpenSSH allows";
      case KIND_ECDSA: return "curve size: 256, 384 or 521";
      default:         return "255 is Ed25519, 448 is Ed448";
    }
}

static Kind selected_kind(void)
{
    if (dlg_get_selected(kg.d, K_DSA)) return KIND_DSA;
    if (dlg_get_selected(kg.d, K_ECDSA)) return KIND_ECDSA;
    if (dlg_get_selected(kg.d, K_EDDSA)) return KIND_EDDSA;
    return KIND_RSA;
}

/* ------------------------------------------------------------------------ */
/* what the window shows                                                    */

/* Shade what cannot be used now: while a key is being made nothing can be changed, and
 * without a key there is nothing to save. */
static void apply_state(void)
{
    static const int pickers[] = { K_RSA, K_DSA, K_ECDSA, K_EDDSA, K_BITS, K_LOAD };
    static const int keyed[] = { K_COMMENT, K_PASS1, K_PASS2, K_COPY, K_SAVEPUB, K_SAVEPRIV };
    const bool busy = kg.state != S_IDLE;
    const bool have = kg.key.key != NULL;
    size_t i;

    if (!kg.d) return;
    for (i = 0; i < sizeof pickers / sizeof pickers[0]; i++)
        dlg_set_shaded(kg.d, pickers[i], busy);
    for (i = 0; i < sizeof keyed / sizeof keyed[0]; i++)
        dlg_set_shaded(kg.d, keyed[i], busy || !have);
    dlg_set_text(kg.d, K_GENERATE, busy ? "Cancel" : "Generate");
}

static void refresh_public_line(void)
{
    char *s = ssh2_pubkey_openssh_str(&kg.key);
    dlg_set_text(kg.d, K_PUBKEY, s);
    sfree(s);
}

/* Show the key's public key, fingerprint and comment. */
static void show_key(void)
{
    char *fp;
    const char *sp;

    if (!kg.d) return;
    if (!kg.key.key) {
        dlg_set_text(kg.d, K_PUBKEY, "");
        dlg_set_text(kg.d, K_FINGERPRINT, "");
        dlg_set_text(kg.d, K_COMMENT, "");
        dlg_set_text(kg.d, K_PASS1, "");
        dlg_set_text(kg.d, K_PASS2, "");
        return;
    }
    refresh_public_line();
    fp = ssh2_fingerprint(kg.key.key, kg.fptype);
    sp = strrchr(fp, ' ');                  /* "ssh-rsa 2048 SHA256:..." : show the hash itself */
    dlg_set_text(kg.d, K_FINGERPRINT, sp ? sp + 1 : fp);
    sfree(fp);
    dlg_set_text(kg.d, K_COMMENT, kg.key.comment ? kg.key.comment : "");
}

/* "ssh-rsa, 2048 bits", for saying what a key is. */
static char *key_description(void)
{
    char *fp = ssh2_fingerprint(kg.key.key, SSH_FPTYPE_DEFAULT), *sp = strrchr(fp, ' '), *ret;
    if (sp) *sp = '\0';                         /* "ssh-rsa 2048 SHA256:..." -> "ssh-rsa 2048" */
    sp = strrchr(fp, ' ');
    if (sp) {
        *sp++ = '\0';
        ret = dupprintf("%s, %s bits", fp, sp);
    } else {
        ret = dupstr(fp);
    }
    sfree(fp);
    return ret;
}

/* The comment is edited in its field, and the Wimp does not say when; take it from there. */
static void sync_comment(void)
{
    const char *text;
    if (!kg.d || !kg.key.key) return;
    text = dlg_get_text(kg.d, K_COMMENT);
    if (!kg.key.comment || strcmp(text, kg.key.comment)) {
        sfree(kg.key.comment);
        kg.key.comment = dupstr(text);
        kg.dirty = true;
        refresh_public_line();
    }
}

/* ------------------------------------------------------------------------ */
/* progress reports                                                         */

static void pump(void)
{
    if (roapp_yield()) kg.cancel = true;            /* the program is being quit */
    if (kg.cancel) longjmp(kg.abort_env, 1);
}

static ProgressPhase prog_add_linear(ProgressReceiver *prog, double overall_cost)
{
    struct progress *p = container_of(prog, struct progress, rec);
    ProgressPhase ph;
    int phase;

    sgrowarray(p->phases, p->phasessize, p->nphases);
    phase = (int)p->nphases++;
    p->phases[phase].total = overall_cost;
    ph.n = phase;
    return ph;
}

static ProgressPhase prog_add_probabilistic(ProgressReceiver *prog, double cost_per_attempt,
                                            double probability)
{
    struct progress *p = container_of(prog, struct progress, rec);
    ProgressPhase ph;
    int phase;

    sgrowarray(p->phases, p->phasessize, p->nphases);
    phase = (int)p->nphases++;
    p->phases[phase].exp_probability = 1.0 - probability;
    p->phases[phase].exp_current_value = 1.0;
    p->phases[phase].total = cost_per_attempt / probability;        /* expected number of attempts = 1 / probability */
    ph.n = phase;
    return ph;
}

static void prog_ready(ProgressReceiver *prog)
{
    struct progress *p = container_of(prog, struct progress, rec);
    double total = 0;
    size_t i;

    for (i = 0; i < p->nphases; i++) {
        p->phases[i].startpoint = total;
        total += p->phases[i].total;
    }
    p->scale = PROGRESS_RANGE / total;
}

static void prog_start_phase(ProgressReceiver *prog, ProgressPhase phase)
{
    struct progress *p = container_of(prog, struct progress, rec);
    p->curr = &p->phases[phase.n];
}

static void prog_update(struct progress *p, double phasepos)
{
    double position = (p->curr->startpoint + p->curr->total * phasepos) * p->scale;
    if (position < 0) position = 0;
    if (position > PROGRESS_RANGE) position = PROGRESS_RANGE;
    dlg_set_bar(kg.d, K_BAR, (int)position);
    pump();
}

static void prog_report(ProgressReceiver *prog, double progress)
{
    prog_update(container_of(prog, struct progress, rec), progress);
}

static void prog_report_attempt(ProgressReceiver *prog)
{
    struct progress *p = container_of(prog, struct progress, rec);
    p->curr->exp_current_value *= p->curr->exp_probability;
    prog_update(p, 1.0 - p->curr->exp_current_value);
}

static void prog_report_phase_complete(ProgressReceiver *prog)
{
    prog_update(container_of(prog, struct progress, rec), 1.0);
}

static const ProgressReceiverVtable progress_vt = {
    .add_linear = prog_add_linear,
    .add_probabilistic = prog_add_probabilistic,
    .ready = prog_ready,
    .start_phase = prog_start_phase,
    .report = prog_report,
    .report_attempt = prog_report_attempt,
    .report_phase_complete = prog_report_phase_complete,
};

/* ------------------------------------------------------------------------ */
/* making a key                                                             */

static void close_window(void);

static char *default_comment(Kind k, int bits)
{
    char buf[40];
    struct tm tm = ltime();
    const char *fmt = k == KIND_DSA ? "dsa-key-%Y%m%d" : k == KIND_ECDSA ? "ecdsa-key-%Y%m%d" :
                      k == KIND_EDDSA ? (bits == 255 ? "ed25519-key-%Y%m%d" : "eddsa-key-%Y%m%d") :
                      "rsa-key-%Y%m%d";
    strftime(buf, sizeof buf, fmt, &tm);
    return dupstr(buf);
}

/* The generators, with the progress bar and the way out. */
static void generate_into(PrimeGenerationContext *pgc)
{
    ProgressReceiver *rec = &kg.prog.rec;

    switch (kg.kind) {
      case KIND_DSA: {
        struct dsa_key *k = snew(struct dsa_key);
        dsa_generate(k, kg.bits, pgc, rec);
        kg.gen = &k->sshk;
        break;
      }
      case KIND_ECDSA: {
        struct ecdsa_key *k = snew(struct ecdsa_key);
        ecdsa_generate(k, kg.bits);
        kg.gen = &k->sshk;
        break;
      }
      case KIND_EDDSA: {
        struct eddsa_key *k = snew(struct eddsa_key);
        eddsa_generate(k, kg.bits);
        kg.gen = &k->sshk;
        break;
      }
      default: {
        RSAKey *k = snew(RSAKey);
        rsa_generate(k, kg.bits, false, pgc, rec);
        k->comment = NULL;
        kg.gen = &k->sshk;
        break;
      }
    }
}

static void run_generation(void)
{
    PrimeGenerationContext *pgc;

    if (!roapp_op_begin()) {
        notice("Another job is running; try again when it has finished.");
        kg.state = S_IDLE;
        apply_state();
        return;
    }
    kg.state = S_GENERATING;
    kg.cancel = false;
    kg.gen = NULL;
    apply_state();
    set_status("Generating the key. Please wait...");
    dlg_set_bar(kg.d, K_BAR, 0);

    memset(&kg.prog, 0, sizeof kg.prog);
    kg.prog.rec.vt = &progress_vt;
    pgc = primegen_new_context(&primegen_probabilistic);
    if (setjmp(kg.abort_env) == 0) {
        generate_into(pgc);
        primegen_free_context(pgc);
    } else {
        kg.gen = NULL;                  /* cancelled: what was half made is abandoned */
    }
    sfree(kg.prog.phases);
    kg.prog.phases = NULL;
    roapp_op_end();

    kg.state = S_IDLE;
    if (kg.gen) {
        char *desc;
        free_key();
        kg.key.key = kg.gen;
        kg.key.comment = default_comment(kg.kind, kg.bits);
        kg.gen = NULL;
        kg.dirty = true;
        dlg_set_bar(kg.d, K_BAR, PROGRESS_RANGE);
        dlg_set_text(kg.d, K_PASS1, "");
        dlg_set_text(kg.d, K_PASS2, "");
        show_key();
        desc = key_description();
        set_status("Made a new key (%s). Save it, and copy or save its public key.", desc);
        sfree(desc);
    } else {
        dlg_set_bar(kg.d, K_BAR, 0);
        set_status("Cancelled: no key was made.");
    }
    apply_state();
    if (kg.close_after) close_window();
}

/* Seed the generator from the CryptRandom module, as the puttygen command does. */
static bool seed_from_module(void)
{
    unsigned char buf[64];
    size_t n = random_seed_bits() / 8;

    if (n < 32) n = 32;
    if (n > sizeof buf) n = sizeof buf;
    if (!ro_cryptrandom_bytes(buf, n)) return false;
    random_reseed(make_ptrlen(buf, n));
    smemclr(buf, sizeof buf);
    return true;
}

static bool want_mouse_randomness(void)
{
    const char *force = getenv("PuTTY$ForceMouseEntropy");      /* (to test the mouse path) */
    return (force && *force) || ro_entropy_is_weak();
}

static void cancel_entropy(void)
{
    if (kg.entropy) {
        strbuf_free(kg.entropy);
        kg.entropy = NULL;
    }
    if (kg.state == S_ENTROPY) {
        kg.state = S_IDLE;
        dlg_set_bar(kg.d, K_BAR, 0);
        set_status("Cancelled: no key was made.");
        apply_state();
    }
}

static void start_entropy(void)
{
    size_t need = ((size_t)kg.bits / 2) * 2;                    /* as PuTTYgen does */

    if (need > random_seed_bits()) need = random_seed_bits();   /* more is no use to the generator */
    kg.entropy_required = need;
    kg.entropy_got = 0;
    kg.entropy = strbuf_new_nm();
    kg.entropy_prev = ro_time();
    kg.prev_x = kg.prev_y = -1;
    kg.state = S_ENTROPY;
    dlg_set_bar(kg.d, K_BAR, 0);
    set_status("Move the mouse around over this window to gather randomness.");
    apply_state();
}

/* Watch the pointer while the person waves it about. */
static void collect_mouse(void)
{
    int x = 0, y = 0, buttons = 0, t = 0;
    wpointer p;

    if (!kg.entropy || _swix(OS_Mouse, _OUTR(0, 3), &x, &y, &buttons, &t)) return;
    if (_swix(Wimp_GetPointerInfo, _IN(1), &p) || p.window != dlg_window(kg.d)) return;
    if (x == kg.prev_x && y == kg.prev_y) return;
    kg.prev_x = x;
    kg.prev_y = y;
    put_uint32(kg.entropy, (uint32_t)x);
    put_uint32(kg.entropy, (uint32_t)y);
    put_uint32(kg.entropy, (uint32_t)t);
    noise_ultralight(NOISE_SOURCE_MOUSEPOS, (unsigned long)((x << 16) ^ y));
    if (ro_time() != kg.entropy_prev) {                /* at most once a centisecond: successive samples are alike */
        kg.entropy_got += 2;
        kg.entropy_prev = ro_time();
    }
    dlg_set_bar(kg.d, K_BAR, (int)(kg.entropy_got * PROGRESS_RANGE / kg.entropy_required));
    if (kg.entropy_got >= kg.entropy_required) {
        random_reseed(ptrlen_from_strbuf(kg.entropy));
        strbuf_free(kg.entropy);
        kg.entropy = NULL;
        run_generation();
    }
}

static void begin_generation(void)
{
    kg.kind = kg.pending_kind;
    kg.bits = kg.pending_bits;
    hold_random();
    if (!want_mouse_randomness() && seed_from_module())
        run_generation();
    else
        start_entropy();
}

static void unsaved_answered(void *ctx, int button)
{
    (void)ctx;
    if (button == 0 && kg.d && kg.state == S_IDLE) begin_generation();
}

/* The size is acceptable; make sure a key that has not been saved is not thrown away. */
static void confirm_replace(void)
{
    static const char *const buttons[] = { "Generate", "Cancel" };
    if (kg.key.key && kg.dirty)
        ro_msgbox(&kg, TITLE, "The key now in the window has not been saved.\nMake a new key anyway?",
                  buttons, 2, 1, 1, unsaved_answered, NULL);
    else
        begin_generation();
}

static void size_answered(void *ctx, int button)
{
    (void)ctx;
    if (button == 0 && kg.d && kg.state == S_IDLE) confirm_replace();
}

static void on_generate(void)
{
    static const char *const buttons[] = { "Generate", "Cancel" };
    const char *text;
    char *end;
    long bits;
    Kind k;

    if (kg.state == S_GENERATING) {                 /* Cancel (the generator notices at its next report) */
        kg.cancel = true;
        set_status("Cancelling...");
        return;
    }
    if (kg.state == S_ENTROPY) { cancel_entropy(); return; }

    k = selected_kind();
    text = dlg_get_text(kg.d, K_BITS);
    bits = strtol(text, &end, 10);
    if (end == text || *end || bits < 1 || bits > MAX_BITS) {
        notice("The size must be a number of bits from 1 to %d.", MAX_BITS);
        dlg_focus(kg.d, K_BITS);
        return;
    }
    if (k == KIND_ECDSA || k == KIND_EDDSA) {
        const struct ec_curve *curve;
        const ssh_keyalg *alg;
        const bool ok = k == KIND_ECDSA ? ec_nist_alg_and_curve_by_bits((int)bits, &curve, &alg)
                                        : ec_ed_alg_and_curve_by_bits((int)bits, &curve, &alg);
        if (!ok) {
            notice(k == KIND_ECDSA ? "An ECDSA key must be 256, 384 or 521 bits."
                                   : "An EdDSA key must be 255 bits (Ed25519) or 448 (Ed448).");
            dlg_focus(kg.d, K_BITS);
            return;
        }
    } else if (bits < 256) {
        notice("The key generator will not make a key shorter than 256 bits.");
        dlg_focus(kg.d, K_BITS);
        return;
    }
    kg.pending_kind = k;
    kg.pending_bits = (int)bits;
    if ((k == KIND_RSA || k == KIND_DSA) && bits < 2048)
        ro_msgbox(&kg, TITLE, "Keys shorter than 2048 bits are not recommended.\nReally generate this key?",
                  buttons, 2, 1, 1, size_answered, NULL);
    else
        confirm_replace();
}

static void kind_changed(void)
{
    const Kind k = selected_kind();
    char old_default[16];

    snprintf(old_default, sizeof old_default, "%d", default_bits(shown_kind));
    if (!dlg_get_text(kg.d, K_BITS)[0] || !strcmp(dlg_get_text(kg.d, K_BITS), old_default)) {
        char buf[16];
        snprintf(buf, sizeof buf, "%d", default_bits(k));
        dlg_set_text(kg.d, K_BITS, buf);
    }
    dlg_set_text(kg.d, K_HINT, hint_for(k));
    shown_kind = k;
}

/* ------------------------------------------------------------------------ */
/* loading a key                                                            */

static void abort_load(void)
{
    sfree(kg.load.path);
    sfree(kg.load.comment);
    memset(&kg.load, 0, sizeof kg.load);
}

static const char *leaf_of(const char *path)
{
    const char *p = strrchr(path, '.'), *c;
    p = p ? p + 1 : path;
    c = strrchr(p, ':');
    return c ? c + 1 : p;
}

/* A key has been read: put it in the window. */
static void adopt_key(ssh2_userkey *nk, const char *passphrase)
{
    char *desc;
    const bool foreign = kg.load.realtype != kg.load.type;
    char *foreign_name = foreign ? dupstr(key_type_to_str(kg.load.realtype)) : NULL;
    char *leaf = dupstr(leaf_of(kg.load.path));

    free_key();
    kg.key = *nk;                                       /* (a structure copy: the key and comment move) */
    sfree(nk);
    kg.dirty = false;
    dlg_set_text(kg.d, K_PASS1, passphrase);
    dlg_set_text(kg.d, K_PASS2, passphrase);
    show_key();
    dlg_set_bar(kg.d, K_BAR, 0);
    desc = key_description();
    set_status("Loaded the key (%s) from %s.", desc, leaf);
    sfree(desc);
    sfree(leaf);
    apply_state();
    if (foreign_name) {
        notice("Successfully imported foreign key\n(%s).\nTo use this key with PuTTY, use the \"Save private "
               "key\" button to save it in PuTTY's own format.", foreign_name);
        sfree(foreign_name);
    }
}

static void ask_passphrase(void);

static void load_finish(const char *passphrase)
{
    Filename *fn = filename_from_str(kg.load.path);
    const char *err = NULL;
    ssh2_userkey *nk;

    if (kg.load.type == kg.load.realtype)
        nk = ppk_load_f(fn, passphrase, &err);
    else
        nk = import_ssh2(fn, kg.load.realtype, (char *)passphrase, &err);
    filename_free(fn);

    if (nk == SSH2_WRONG_PASSPHRASE) {
        kg.load.retry = true;
        ask_passphrase();
        return;
    }
    if (!nk) {
        notice("Couldn't load private key (%s).", err ? err : "unknown error");
    } else if (kg.d) {
        adopt_key(nk, passphrase);
    }
    abort_load();
}

static void passphrase_answered(void *ctx, const char *answer)
{
    (void)ctx;
    if (!answer || !kg.d || !kg.load.path) {
        abort_load();
        return;
    }
    load_finish(answer);
}

static void ask_passphrase(void)
{
    char *msg = dupprintf("%sEnter the passphrase to unlock the key\n%s",
                          kg.load.retry ? "That passphrase was wrong.\n" : "",
                          kg.load.comment ? kg.load.comment : "");
    ro_prompt(&kg, "Key passphrase", msg, true, "", passphrase_answered, NULL);
    sfree(msg);
}

static void load_start(const char *path)
{
    Filename *fn;
    int type, realtype;
    char *comment = NULL;
    bool encrypted;

    if (!kg.d) return;
    if (kg.state != S_IDLE) {
        notice("Please wait until the key has been made.");
        return;
    }
    abort_load();
    fn = filename_from_str(path);
    type = realtype = key_type(fn);
    if (type == SSH_KEYTYPE_UNOPENABLE) {
        notice("The file %s could not be opened.", path);
        filename_free(fn);
        return;
    }
    if (type != SSH_KEYTYPE_SSH1 && type != SSH_KEYTYPE_SSH2 && !import_possible(type)) {
        notice("Couldn't load private key (%s).", key_type_to_str(type));
        filename_free(fn);
        return;
    }
    if (type != SSH_KEYTYPE_SSH1 && type != SSH_KEYTYPE_SSH2)
        type = import_target_type(type);
    if (type == SSH_KEYTYPE_SSH1) {
        notice("That is an SSH-1 key. The key generator window only handles SSH-2 keys; the puttygen "
               "command (in !PuTTY.Tools) can convert SSH-1 keys.");
        filename_free(fn);
        return;
    }
    encrypted = (type == realtype) ? ppk_encrypted_f(fn, &comment) : import_encrypted(fn, realtype, &comment);
    filename_free(fn);

    kg.load.path = dupstr(path);
    kg.load.type = type;
    kg.load.realtype = realtype;
    kg.load.comment = comment;
    kg.load.retry = false;
    if (encrypted) ask_passphrase();
    else load_finish("");
}

static void load_path_answered(void *ctx, const char *answer)
{
    (void)ctx;
    if (answer && *answer && kg.d) load_start(answer);
}

static void on_load(void)
{
    if (kg.state != S_IDLE) return;
    ro_prompt(&kg, "Load key", "Type the pathname of the private key file to load.\n"
              "(Or drag the file onto the key generator window.)", false, "", load_path_answered, NULL);
}

static bool is_key_file(const char *path)
{
    Filename *fn = filename_from_str(path);
    const int t = key_type(fn);
    filename_free(fn);
    return t != SSH_KEYTYPE_UNOPENABLE && t != SSH_KEYTYPE_UNKNOWN;
}

/* ------------------------------------------------------------------------ */
/* saving and copying                                                       */

/* Say that a file could not be written, and why if the system said. */
static void save_failed(const char *what)
{
    const int err = errno;
    if (err != 0)
        notice("Unable to save the %s.\n(%s)", what, strerror(err));
    else
        notice("Unable to save the %s.", what);
}

static bool write_public(const char *path, int format)
{
    Filename *fn = filename_from_str(path);
    FILE *fp;
    strbuf *blob;

    errno = 0;
    fp = f_open(fn, "w", false);
    filename_free(fn);
    if (!fp) {
        save_failed("public key file");
        return false;
    }
    blob = strbuf_new();
    ssh_key_public_blob(kg.key.key, BinarySink_UPCAST(blob));
    ssh2_write_pubkey(fp, kg.key.comment && *kg.key.comment ? kg.key.comment : NULL,
                      blob->u, (int)blob->len, format);
    strbuf_free(blob);
    if (fclose(fp) < 0) {
        save_failed("public key file");
        return false;
    }
    return true;
}

/* One save in progress: what is to be written, and the passphrase it is to be locked with (taken when the
 * save box is opened, so that a second box cannot disturb it). */
typedef struct {
    SaveKind kind;
    char    *pass;                      /* NULL: no passphrase */
} SaveJob;

static bool key_save_fn(void *ctx, const char *path)
{
    SaveJob *job = (SaveJob *)ctx;
    Filename *fn;
    bool ok;
    int type;

    if (!kg.d || !kg.key.key) return false;
    sync_comment();
    switch (job->kind) {
      case SK_PUB_OPENSSH:
        return write_public(path, SSH_KEYTYPE_SSH2_PUBLIC_OPENSSH);
      case SK_PUB_RFC4716:
        return write_public(path, SSH_KEYTYPE_SSH2_PUBLIC_RFC4716);
      case SK_PPK: {
        char *bad = ppk_params_bad(&ppk_save_default_parameters, job->pass != NULL,
                                   job->pass ? strlen(job->pass) : 0);
        if (bad) {
            notice("The key file parameters are not valid: %s", bad);
            sfree(bad);
            return false;
        }
        fn = filename_from_str(path);
        errno = 0;
        ok = ppk_save_f(fn, &kg.key, job->pass, &ppk_save_default_parameters);
        filename_free(fn);
        if (!ok) save_failed("private key file");
        else kg.dirty = false;
        return ok;
      }
      default:
        type = job->kind == SK_OPENSSH ? SSH_KEYTYPE_OPENSSH_AUTO :
               job->kind == SK_OPENSSH_NEW ? SSH_KEYTYPE_OPENSSH_NEW : SSH_KEYTYPE_SSHCOM;
        fn = filename_from_str(path);
        errno = 0;
        ok = export_ssh2(fn, type, &kg.key, job->pass);
        filename_free(fn);
        if (!ok) save_failed("converted key file");
        return ok;
    }
}

static void key_save_done(void *ctx, bool saved)
{
    SaveJob *job = (SaveJob *)ctx;
    (void)saved;
    wipe_free(job->pass);
    sfree(job);
}

/* The default name for an exported private key: what OpenSSH itself would call it. */
static const char *export_leaf(void)
{
    const char *id = ssh_key_ssh_id(kg.key.key);
    if (!strncmp(id, "ssh-rsa", 7)) return "id_rsa";
    if (!strncmp(id, "ssh-dss", 7)) return "id_dsa";
    if (!strncmp(id, "ecdsa-", 6)) return "id_ecdsa";
    if (!strncmp(id, "ssh-ed25519", 11)) return "id_ed25519";
    if (!strncmp(id, "ssh-ed448", 9)) return "id_ed448";
    return "id_key";
}

static void open_save_box(SaveJob *job)
{
    const char *title, *leaf;

    switch (job->kind) {
      case SK_PPK:         title = "Save private key";           leaf = "PrivateKey"; break;
      case SK_PUB_OPENSSH: title = "Save public key";            leaf = "PublicKey"; break;
      case SK_PUB_RFC4716: title = "Save public key (RFC 4716)"; leaf = "PublicKey"; break;
      case SK_OPENSSH:     title = "Export OpenSSH key";         leaf = export_leaf(); break;
      case SK_OPENSSH_NEW: title = "Export OpenSSH key (new)";   leaf = export_leaf(); break;
      default:             title = "Export ssh.com key";         leaf = "SSHComKey"; break;
    }
    rosave_open(title, leaf, FILETYPE_TEXT, key_save_fn, key_save_done, job);
}

static void nopass_answered(void *ctx, int button)
{
    SaveJob *job = (SaveJob *)ctx;
    if (button == 0 && kg.d && kg.key.key) open_save_box(job);
    else key_save_done(job, false);
}

static void start_save(SaveKind kind)
{
    SaveJob *job;

    if (!kg.d || !kg.key.key || kg.state != S_IDLE) return;
    sync_comment();
    job = snew(SaveJob);
    job->kind = kind;
    job->pass = NULL;

    if (kind != SK_PUB_OPENSSH && kind != SK_PUB_RFC4716) {       /* a private key: the passphrase protects it */
        const char *p1 = dlg_get_text(kg.d, K_PASS1), *p2 = dlg_get_text(kg.d, K_PASS2);
        if (strcmp(p1, p2)) {
            notice("The two passphrases given do not match.");
            sfree(job);
            return;
        }
        if (*p1) job->pass = dupstr(p1);
        if (!*p1) {
            static const char *const buttons[] = { "Save", "Cancel" };
            ro_msgbox(&kg, TITLE, "Are you sure you want to save this key\nwithout a passphrase to protect it?",
                      buttons, 2, 1, 1, nopass_answered, job);
            return;
        }
    }
    open_save_box(job);
}

static void copy_public(void)
{
    char *s;
    wchar_t *w;
    size_t i, n;

    if (!kg.d || !kg.key.key) return;
    sync_comment();
    s = ssh2_pubkey_openssh_str(&kg.key);
    n = strlen(s);
    w = snewn(n + 1, wchar_t);
    for (i = 0; i < n; i++) w[i] = (unsigned char)s[i];
    roclip_copy(w, n);
    sfree(w);
    sfree(s);
    set_status("The public key is on the clipboard.");
}

/* ------------------------------------------------------------------------ */
/* the menu                                                                 */

static void menu_selected(const int *sel)
{
    if (!kg.d) return;
    switch (sel[0]) {
      case 0: on_load(); break;
      case 1: start_save(SK_PUB_OPENSSH); break;
      case 2: start_save(SK_PPK); break;
      case 3: copy_public(); break;
      case 4:
        switch (sel[1]) {
          case 0: start_save(SK_OPENSSH); break;
          case 1: start_save(SK_OPENSSH_NEW); break;
          case 2: start_save(SK_SSHCOM); break;
          case 3: start_save(SK_PUB_RFC4716); break;
        }
        break;
      case 5:
        kg.fptype = sel[1] == 1 ? SSH_FPTYPE_MD5 : SSH_FPTYPE_SHA256;
        if (kg.key.key) show_key();
        break;
    }
}

static void on_menu_click(dlg *d, int x, int y, void *ud)
{
    const bool busy = kg.state != S_IDLE, have = kg.key.key != NULL;
    int i;
    (void)d; (void)ud;

    if (!kg.menu) {
        kg.export_menu = menu_new("Export");
        menu_add(kg.export_menu, "OpenSSH key", 0);
        menu_add(kg.export_menu, "OpenSSH (new format)", 0);
        menu_add(kg.export_menu, "ssh.com key", 0);
        menu_add(kg.export_menu, "Public key (RFC 4716)", 0);
        kg.fp_menu = menu_new("Fingerprint");
        menu_add(kg.fp_menu, "SHA256", 0);
        menu_add(kg.fp_menu, "MD5", 0);
        kg.menu = menu_new(TITLE);
        menu_add(kg.menu, "Load key...", 0);
        menu_add(kg.menu, "Save public key", 0);
        menu_add(kg.menu, "Save private key", 0);
        menu_add(kg.menu, "Copy public key", 0);
        menu_add(kg.menu, "Export", 0);
        menu_add(kg.menu, "Fingerprint", 0);
        menu_set_sub(kg.menu, 4, kg.export_menu);
        menu_set_sub(kg.menu, 5, kg.fp_menu);
    }
    menu_shade(kg.menu, 0, busy);
    for (i = 1; i <= 4; i++) menu_shade(kg.menu, i, busy || !have);
    menu_tick(kg.fp_menu, 0, kg.fptype != SSH_FPTYPE_MD5);
    menu_tick(kg.fp_menu, 1, kg.fptype == SSH_FPTYPE_MD5);
    roapp_menu_opened(menu_selected);
    menu_open(kg.menu, x - 64, y);
}

/* ------------------------------------------------------------------------ */
/* the window                                                               */

static void close_window(void)
{
    if (!kg.d) return;
    ro_msgbox_drop_owner(&kg);              /* its questions and prompts go with it */
    rosave_close();
    abort_load();
    if (kg.entropy) {
        strbuf_free(kg.entropy);
        kg.entropy = NULL;
    }
    free_key();
    dlg_free(kg.d);
    kg.d = NULL;
    kg.state = S_IDLE;
    kg.dirty = kg.cancel = kg.close_after = false;
    release_random();
}

static void close_answered(void *ctx, int button)
{
    (void)ctx;
    if (button == 0) close_window();
}

static void request_close(void)
{
    static const char *const buttons[] = { "Close", "Cancel" };

    if (kg.state == S_GENERATING) {         /* it closes when the generator has stopped */
        kg.cancel = true;
        kg.close_after = true;
        return;
    }
    if (kg.state == S_ENTROPY) cancel_entropy();
    if (kg.key.key && kg.dirty) {
        ro_msgbox(&kg, TITLE, "The key has not been saved.\nClose the window and lose it?", buttons, 2, 1, 1,
                  close_answered, NULL);
        return;
    }
    close_window();
}

static void keygen_click(dlg *d, int id, int buttons, void *ud)
{
    (void)d; (void)ud;
    switch (id) {
      case K_RSA: case K_DSA: case K_ECDSA: case K_EDDSA:
        kind_changed();
        break;
      case K_GENERATE:
        on_generate();
        break;
      case K_LOAD:
        on_load();
        break;
      case K_COPY:
        copy_public();
        break;
      case K_SAVEPUB:
        start_save(SK_PUB_OPENSSH);
        break;
      case K_SAVEPRIV:
        start_save(SK_PPK);
        break;
      case K_CLOSE:
        request_close();
        break;
    }
    (void)buttons;
}

static void build_window(void)
{
    static const dlg_item items[] = {
        { DI_BOX,     0,          8,  13, 644, 144, "", 0, 0, NULL },
        { DI_HEADING, 0,         22,   4, 160,  18, "Generate a new key", 0, 0, NULL },
        { DI_RLABEL,  0,         14,  36,  98,  22, "Key type:", 0, 0, NULL },
        { DI_RADIO,   K_RSA,    120,  34,  64,  24, "RSA", 0, 1,
          "RSA keys work with every SSH server." },
        { DI_RADIO,   K_DSA,    190,  34,  64,  24, "DSA", 0, 1,
          "DSA keys are old and many servers no longer accept them." },
        { DI_RADIO,   K_ECDSA,  260,  34,  80,  24, "ECDSA", 0, 1,
          "ECDSA keys are short and fast. Pick a curve size of 256, 384 or 521 bits." },
        { DI_RADIO,   K_EDDSA,  346,  34,  80,  24, "EdDSA", 0, 1,
          "EdDSA keys (Ed25519 is the usual one) are short, fast and modern." },
        { DI_RLABEL,  0,         14,  72,  98,  22, "Key size:", 0, 0, NULL },
        { DI_FIELD,   K_BITS,   120,  70,  64,  26, "2048", 8, 0,
          "The size of the key in bits (for ECDSA and EdDSA, which curve to use)." },
        { DI_LABEL,   K_HINT,   192,  72, 330,  22, "bits: 2048 or more is best", 40, 0, NULL },
        { DI_BUTTON,  K_GENERATE, 540, 68, 112, 30, "Generate", 0, 0,
          "Make a new key pair. Click again while it is working to cancel." },
        { DI_LABEL,   K_STATUS,  14, 108, 630,  22, "", 120, 0, NULL },
        { DI_BAR,     K_BAR,     14, 130, 630,  16, "", 0, 0, "How far the key has got." },

        { DI_BOX,     0,          8, 175, 644, 179, "", 0, 0, NULL },
        { DI_HEADING, K_KEYHEAD,  22, 166,  40,  18, "Key", 0, 0, NULL },
        { DI_RLABEL,  0,         14, 192,  98,  22, "Public key:", 0, 0, NULL },
        { DI_INFO,    K_PUBKEY, 120, 190, 524,  26, "", 1100, 0,
          "The public key, to paste into a server's authorized_keys file. Copy public key puts all of it on the clipboard." },
        { DI_RLABEL,  0,         14, 224,  98,  22, "Fingerprint:", 0, 0, NULL },
        { DI_INFO,    K_FINGERPRINT, 120, 222, 524, 26, "", 128, 0,
          "A short summary of the key, for checking that two copies are the same. Menu chooses SHA256 or MD5." },
        { DI_RLABEL,  0,         14, 256,  98,  22, "Comment:", 0, 0, NULL },
        { DI_FIELD,   K_COMMENT,120, 254, 524,  26, "", 200, 0,
          "A note kept with the key to tell it from others; you can change it." },
        { DI_RLABEL,  0,         14, 288,  98,  22, "Passphrase:", 0, 0, NULL },
        { DI_PASSWORD,K_PASS1,  120, 286, 524,  26, "", 200, 0,
          "Lock the private key with this passphrase (leave it empty for none)." },
        { DI_RLABEL,  0,         14, 320,  98,  22, "Confirm:", 0, 0, NULL },
        { DI_PASSWORD,K_PASS2,  120, 318, 524,  26, "", 200, 0, "Type the passphrase again." },

        { DI_BUTTON,  K_LOAD,     8, 366,  84,  30, "Load...", 0, 0,
          "Load a private key from a file, to change its passphrase or comment or to convert it." },
        { DI_BUTTON,  K_COPY,   180, 366, 148,  30, "Copy public key", 0, 0,
          "Put the public key on the clipboard, ready to paste into authorized_keys." },
        { DI_BUTTON,  K_SAVEPUB,338, 366, 148,  30, "Save public key", 0, 0,
          "Save the public key as a file (in OpenSSH's one-line format)." },
        { DI_BUTTON,  K_SAVEPRIV,496,366, 156,  30, "Save private key", 0, 0,
          "Save the private key as a PuTTY .ppk file. Keep it safe and secret." },
    };

    kg.d = dlg_new("PuTTY key generator", 660, 408, items, (int)(sizeof items / sizeof items[0]),
                   keygen_click, NULL);
    if (!kg.d) return;
    dlg_set_keys(kg.d, -1, -1);
    dlg_set_close_id(kg.d, K_CLOSE);
    dlg_set_menu_fn(kg.d, on_menu_click);
    dlg_set_help(kg.d, "The key generator makes SSH keys. Choose a type and size and click Generate, "
                 "or drag a key file here. Menu has more: converting keys, copying and fingerprints.");
    dlg_set_selected(kg.d, K_RSA, 1);
    shown_kind = KIND_RSA;
    kg.fptype = SSH_FPTYPE_DEFAULT;
    kg.state = S_IDLE;
    hold_random();
    set_status("Choose a key type and size, then click Generate.");
    apply_state();
}

void rokeygen_open(void)
{
    if (!kg.d) build_window();
    if (kg.d) dlg_open(kg.d);
}

void rokeygen_load(const char *path)
{
    rokeygen_open();
    load_start(path);
}

int rokeygen_message(const wmessage *m)
{
    char path[256];
    const int win = m->data[0];

    if (m->action != MSG_DATA_LOAD) return 0;
    if (!(kg.d && win == dlg_window(kg.d)) && win != -2) return 0;
    wimp_msg_path(m, path, sizeof path);
    if (!path[0]) return 0;
    if (win == -2) {                        /* on the icon bar icon: only if it is a key */
        if (!is_key_file(path)) return 0;
        rokeygen_open();
    }
    wimp_send_load_ack(m, path);
    load_start(path);
    return 1;
}

bool rokeygen_unsaved(void)
{
    return kg.d && kg.key.key && kg.dirty;
}

bool rokeygen_idle_wanted(void)
{
    return kg.d && (kg.state == S_ENTROPY || (kg.state == S_IDLE && kg.key.key));
}

void rokeygen_idle(void)
{
    if (!kg.d) return;
    if (kg.state == S_ENTROPY) collect_mouse();
    else if (kg.state == S_IDLE) sync_comment();
}
