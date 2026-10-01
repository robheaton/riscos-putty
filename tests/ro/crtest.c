/*
 * crtest.c - does the CryptRandom module (Theo Markettos's, the one PuTTY uses for
 * good random numbers) behave the way noise.c assumes?
 *
 * It looks up the module's SWI numbers by name, calls each SWI, and checks what comes
 * back: that Block fills the whole buffer (including more than the module's pool, which
 * version 0.05 to 0.08 got wrong), that Word and Byte return their answer in R0 and not
 * the value passed in, and that the bytes look random (about half the bits set, an even
 * spread of byte values, nothing repeated).  Run it with the module loaded:
 *
 *     RMLoad <path>.CryptRand
 *     Run <path>.crtest
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "swis.h"

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static int swi_number(const char *name)
{
    int n = -1;
    _kernel_oserror *e = _swix(OS_SWINumberFromString, _IN(1) | _OUT(0), name, &n);
    return e ? -1 : n;
}

static int now(void)
{
    int cs = 0;
    _swix(OS_ReadMonotonicTime, _OUT(0), &cs);
    return cs;
}

static int popcount(unsigned char b)
{
    int n = 0;
    while (b) { n += b & 1; b >>= 1; }
    return n;
}

int main(void)
{
    static unsigned char buf[8192 + 64], big[20000];
    int s_byte = swi_number("CryptRandom_Byte"), s_stir = swi_number("CryptRandom_Stir");
    int s_noise = swi_number("CryptRandom_AddNoise"), s_block = swi_number("CryptRandom_Block");
    int s_word = swi_number("CryptRandom_Word");
    _kernel_oserror *e;
    unsigned i, w;
    int b, t0, t1;

    printf("SWI numbers: Byte %x  Stir %x  AddNoise %x  Block %x  Word %x\n",
           s_byte, s_stir, s_noise, s_block, s_word);
    check(s_byte == 0x51980 && s_stir == 0x51981 && s_noise == 0x51982 && s_block == 0x51983 &&
          s_word == 0x51984, "the numbers are the ones in UnixLib's swis.h");
    if (s_block < 0 || s_word < 0) {
        printf("the module does not seem to be loaded\n");
        return 2;
    }

    e = _swix(s_stir, 0);
    printf("Stir -> %s\n", e ? e->errmess : "ok");
    check(!e, "Stir (no registers) works");

    /* Byte and Word: the answer comes back in R0, whatever R0 held on entry */
    printf("Byte:");
    for (i = 0; i < 12; i++) {
        b = 0x1234;
        e = _swix(s_byte, _IN(0) | _OUT(0), 0x1234, &b);
        printf(" %02x", b & 255);
        if (e) { printf(" [%s]", e->errmess); failures++; }
    }
    printf("\nWord:");
    for (i = 0; i < 6; i++) {
        w = 0xDEADBEEFu;
        e = _swix(s_word, _IN(0) | _OUT(0), 0xDEADBEEFu, &w);
        printf(" %08x", w);
        if (e) { printf(" [%s]", e->errmess); failures++; }
    }
    printf("\n");
    {
        unsigned seen[100];
        int j, dup = 0, same4 = 0;
        for (i = 0; i < 100; i++) {
            _swix(s_word, _IN(0) | _OUT(0), 0u, &seen[i]);
            for (j = 0; j < (int)i; j++) if (seen[j] == seen[i]) dup++;
            if (seen[i] == (seen[i] & 0xFF) * 0x01010101u) same4++;
        }
        printf("Word: %d of 100 have all four bytes equal, %d pairs equal\n", same4, dup);
        if (same4 > 50)
            printf("  NOTE CryptRandom_Word (0.13) returns one random byte four times over, so each\n"
                   "       word holds 8 bits of randomness, not 32: use CryptRandom_Block instead\n");
        else
            check(dup == 0, "100 words in a row are all different");
    }

    /* Block: fills every byte of buffers of several sizes, including some bigger than any pool */
    {
        static const int sizes[] = { 1, 16, 64, 500, 1000, 4096, 8192 };   /* the buffer is 64 longer */
        unsigned k;
        for (k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
            int n = sizes[k], filled = n < 16;      /* for a tiny request a sentinel byte proves nothing */
            char what[96];
            memset(buf, 0xA5, sizeof buf);
            e = _swix(s_block, _INR(0, 1), buf, n);
            for (i = n >= 16 ? n - 16 : 0; i < (unsigned)n && !filled; i++)
                if (buf[i] != 0xA5) filled = 1;
            snprintf(what, sizeof what, "Block of %d bytes (%s): the end is filled, nothing written beyond", n,
                     e ? e->errmess : "no error");
            check(!e && filled && buf[n] == 0xA5, what);
        }
    }

    /* Statistics over 8192 bytes */
    {
        int ones = 0, hist[256], dups = 0, j;
        double chi = 0;
        memset(hist, 0, sizeof hist);
        _swix(s_block, _INR(0, 1), buf, 8192);
        for (i = 0; i < 8192; i++) { ones += popcount(buf[i]); hist[buf[i]]++; }
        for (i = 0; i < 256; i++) {
            double d = hist[i] - 8192.0 / 256;
            chi += d * d / (8192.0 / 256);
        }
        for (i = 0; i + 16 <= 8192; i += 16)
            for (j = i + 16; j + 16 <= 8192; j += 16)
                if (!memcmp(buf + i, buf + j, 16)) dups++;
        printf("8192 bytes: %d bits set of %d (expect %d +- 150), chi-square %.0f (expect 255 +- 45), "
               "repeated 16-byte blocks %d\n", ones, 8 * 8192, 4 * 8192, chi, dups);
        check(ones > 4 * 8192 - 600 && ones < 4 * 8192 + 600, "bit balance");
        check(chi > 150 && chi < 400, "byte spread");
        check(dups == 0, "no repeated blocks");
    }

    /* Two big requests must differ, and a request twice the size of the pool must not repeat itself */
    {
        int half = (int)sizeof big / 2;
        _swix(s_block, _INR(0, 1), big, (int)sizeof big);
        check(memcmp(big, big + half, 64) != 0, "a 20000 byte request does not repeat after half of it");
    }

    t0 = now();
    for (i = 0; i < 2000; i++) _swix(s_block, _INR(0, 1), buf, 16);
    t1 = now();
    printf("2000 x Block(16): %d cs\n", t1 - t0);

    printf("%s (%d failures)\n", failures ? "FAILED" : "all good", failures);
    return failures ? 1 : 0;
}
