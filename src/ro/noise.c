/*
 * noise.c: entropy for PuTTY's cryptographic random number generator.
 *
 * Upstream's Unix port reads /dev/urandom.  RISC OS has no such thing, and (as
 * far as I could find) no operating-system entropy source at all, so what we
 * can offer depends on the machine:
 *
 *  - If the CryptRandom module is available (Theo Markettos's module, which UnixLib
 *    also uses for /dev/random; version 0.09 or later; it is normally in !System.310.
 *    Modules, from where we load it if it is not running), CryptRandom_Block gives us
 *    random numbers gathered from things like mouse movements, key presses and
 *    the timing of the HAL counter, and kept across restarts in a seed file.  That
 *    is the good case.  (Not CryptRandom_Word: in 0.13 it returns one random byte
 *    four times over, so a word holds 8 bits of randomness, not 32.)
 *
 *  - Whether or not it is loaded we also gather timing jitter ourselves (measured
 *    with the HAL's microsecond counter, and the time of every keypress and
 *    network event to the same accuracy) and system state, so a weakness in one
 *    source does not sink the other.  On its own this is WEAK: perhaps a hundred
 *    bits at best, and an attacker who knew roughly when a session started could
 *    search for them.  ro_entropy_is_weak() says whether the module is working,
 *    so that the front ends can tell the user instead of quietly shipping weak
 *    keys.
 *
 * PuTTY's own seed file (read_random_seed) is always mixed in as well; if it
 * was written by a machine with a good source, copying it across carries that
 * strength over.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "putty.h"
#include "ssh.h"
#include "storage.h"
#include "ro.h"

/* The module's SWIs (the numbers are in UnixLib's swis.h). */
#define CryptRandom_Stir_SWI   0x051981   /* no registers: mix in more noise */
#define CryptRandom_Block_SWI  0x051983   /* R0 -> buffer, R1 = number of bytes */

static bool have_cryptrandom;
static bool probed;

/* Fill buf with n bytes from the CryptRandom module; false if it is unavailable. */
static bool cryptrandom_bytes(void *buf, size_t n)
{
    return !_swix(CryptRandom_Block_SWI, _INR(0, 1), buf, (int)n);
}

/*
 * Is the module there, and is it really handing out random bytes?
 *
 * PackMan's CryptRandom package only puts the file in !System.310.Modules; nothing
 * loads it at boot, so (as UnixLib does for /dev/random) we load it from
 * System:Modules if it is not running yet.  RMEnsure also makes sure it is version
 * 0.09 or later: before that Block could leave the end of a big request unfilled.
 * Then the SWIs are tried, and what comes back is checked: bytes that are still as we
 * left them, all zero or all one, or the same twice, are not entropy (an unexpected
 * module), and then we say so and rely on our own sources.
 */
#define ENSURE_VERSION "RMEnsure CryptRandom 0.09 "

/* True if the command ran without an error (an RMEnsure that has to give up is an error). */
static bool cli_ok(const char *cmd)
{
    return !_swix(OS_CLI, _IN(0), cmd);
}

static bool module_ok(void)
{
    return cli_ok(ENSURE_VERSION "Error CryptRandom 0.09 or later is needed") &&
           !_swix(CryptRandom_Stir_SWI, 0);
}

static void probe(void)
{
    unsigned char a[16], b[16];
    size_t i;
    bool all_zero = true, all_ones = true;

    if (probed)
        return;
    probed = true;
    if (!module_ok()) {
        cli_ok(ENSURE_VERSION "RMLoad System:Modules.CryptRand");          /* errors do not matter */
        if (!module_ok())
            return;
    }
    memset(a, 0x00, sizeof a);
    memset(b, 0xFF, sizeof b);
    if (!cryptrandom_bytes(a, sizeof a) || !cryptrandom_bytes(b, sizeof b))
        return;
    for (i = 0; i < sizeof a; i++) {
        if (a[i] != 0x00) all_zero = false;
        if (b[i] != 0xFF) all_ones = false;
    }
    if (all_zero || all_ones || !memcmp(a, b, sizeof a))
        return;
    have_cryptrandom = true;
}

bool ro_entropy_is_weak(void)
{
    probe();
    return !have_cryptrandom;
}

/*
 * The HAL's free-running counter (OS_Hardware 0, HAL routine 21) counts down
 * through each centisecond tick at the rate of the machine's timer (1 or 2 MHz),
 * so it times events far more finely than OS_ReadMonotonicTime.  Returns 0 if the
 * call is not available.
 */
static uint32_t hal_counter(void)
{
    static bool unavailable;
    unsigned v = 0;
    if (!unavailable && _swix(OS_Hardware, _IN(8) | _IN(9) | _OUT(0), 0, 21, &v))
        unavailable = true;
    return v;
}

/* A grab-bag of changeable system state. */
static void snapshot(void (*func)(void *, int))
{
    struct {
        uint32_t cs;
        time_t t;
        clock_t c;
        int mouse[4];
        unsigned long ticks;
        uint32_t hires[6];
    } snap;
    int i;

    memset(&snap, 0, sizeof snap);
    snap.cs = ro_time();
    snap.t = time(NULL);
    snap.c = clock();
    _swix(OS_Mouse, _OUTR(0, 3), &snap.mouse[0], &snap.mouse[1],
          &snap.mouse[2], &snap.mouse[3]);
    snap.ticks = getticks();
    for (i = 0; i < 6; i++)
        snap.hires[i] = hal_counter();
    func(&snap, sizeof snap);
}

/*
 * Time some memory-bound work with the HAL counter.  How long it takes wobbles
 * with cache state, interrupts, the bus and whatever else the machine is doing,
 * which is a small amount of real unpredictability - perhaps a bit or two a sample,
 * and none at all if the counter is missing.
 */
static void jitter(void (*func)(void *, int), int samples)
{
    static volatile unsigned char scratch[8192];
    uint32_t acc = ro_time();
    int i, j;

    for (i = 0; i < samples; i++) {
        volatile unsigned long count = 0;
        uint32_t a, b, d;
        uint32_t t0 = ro_time();

        if (i % 16 == 0)               /* now and then, wait out a tick too */
            while (ro_time() == t0)
                count++;
        a = hal_counter();
        for (j = 0; j < 400; j++) {
            acc = acc * 1664525u + 1013904223u;
            count += scratch[(acc >> 12) & 8191];
            scratch[(acc >> 5) & 8191] = (unsigned char)acc;
        }
        b = hal_counter();
        d = a - b;
        func(&d, sizeof d);
        func((void *)&count, sizeof count);
    }
}

/*
 * Called once, at PuTTY startup.
 */
void noise_get_heavy(void (*func) (void *, int))
{
    unsigned char buf[64];

    probe();
    if (have_cryptrandom && cryptrandom_bytes(buf, sizeof buf))
        func(buf, sizeof buf);

    snapshot(func);
    jitter(func, 160);                 /* about a tenth of a second; always, as well as the module */

    /* (No random_save_seed() here: the PRNG is still being built when we are
     * called.  cleanup_exit() saves the seed when the program ends.) */
    read_random_seed(func);
}

void noise_get_light(void (*func) (void *, int))
{
    snapshot(func);
}

static void add_proctime(void *data, int len)
{
    random_add_noise(NOISE_SOURCE_PROCTIME, data, len);
}

/*
 * Called on a timer, and grabs as much changeable system data as it can
 * quickly get its hands on.
 */
void noise_regular(void)
{
    unsigned char buf[16];
    if (have_cryptrandom && cryptrandom_bytes(buf, sizeof buf))
        random_add_noise(NOISE_SOURCE_PROCTIME, buf, sizeof buf);
    snapshot(add_proctime);
}

/*
 * Called on every keypress, mouse move and network event: adds the current
 * time to the noise pool, and whatever the caller passed in as well.
 */
void noise_ultralight(NoiseSourceId id, unsigned long data)
{
    uint32_t t[2];
    t[0] = ro_time();
    t[1] = hal_counter();              /* where in the tick the event fell */
    random_add_noise(NOISE_SOURCE_TIME, t, sizeof t);
    random_add_noise(id, &data, sizeof data);
}

uint64_t prng_reseed_time_ms(void)
{
    return (uint64_t)time(NULL) * 1000 + (ro_time() % 100) * 10;
}
