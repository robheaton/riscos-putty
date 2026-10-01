/*
 * noise.c: entropy for PuTTY's cryptographic random number generator.
 *
 * Upstream's Unix port reads /dev/urandom.  RISC OS has no such thing, and (as
 * far as I could find) no operating-system entropy source at all, so what we
 * can offer depends on the machine:
 *
 *  - If the CryptRandom module is loaded, CryptRandom_Word gives us proper
 *    random numbers.  That is the good case.
 *
 *  - Otherwise we fall back on timing jitter (measured with the HAL's
 *    microsecond counter, and the time of every keypress and network event to
 *    the same accuracy) and system state.  This is WEAK: perhaps a hundred bits
 *    at best, and an attacker who knew roughly when a session started could
 *    search for them.  ro_entropy_is_weak() says which case we are in so that
 *    the front ends can tell the user instead of quietly shipping weak keys.
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

#define CryptRandom_Word_SWI 0x051984

static bool have_cryptrandom;
static bool probed;

/* Fill buf with n bytes from the CryptRandom module; false if unavailable. */
static bool cryptrandom_bytes(void *vbuf, size_t n)
{
    unsigned char *buf = vbuf;
    while (n > 0) {
        unsigned w = 0;
        size_t i, take = n < 4 ? n : 4;
        if (_swix(CryptRandom_Word_SWI, _OUT(0), &w))
            return false;
        for (i = 0; i < take; i++)
            buf[i] = (unsigned char)(w >> (8 * i));
        buf += take;
        n -= take;
    }
    return true;
}

static void probe(void)
{
    unsigned char test[4];
    if (probed)
        return;
    probed = true;
    have_cryptrandom = cryptrandom_bytes(test, sizeof test);
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
    if (!have_cryptrandom)
        jitter(func, 160);             /* about a tenth of a second */

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
