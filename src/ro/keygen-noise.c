/*
 * keygen-noise.c: where puttygen gets the entropy for a new key.
 *
 * Upstream's Unix version reads /dev/urandom, or whatever --random-device names.  A key
 * is the one thing here that must not come from weak randomness: it is used for years and
 * nothing would ever warn the person using it.  So the only sources are the CryptRandom
 * module (see noise.c) or a file of random bytes supplied with --random-device (made
 * on another machine, say).  The timing jitter that is all a machine without the module
 * has is mixed into every random number the tools use, but it is not enough to be the
 * only thing a new private key rests on, so puttygen says why and stops.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "putty.h"
#include "ro.h"

char *get_random_data(int len, const char *device)
{
    char *buf = snewn(len, char);

    if (device) {
        FILE *fp = fopen(device, "rb");
        size_t got;

        if (!fp) {
            fprintf(stderr, "puttygen: %s: open: %s\n", device, strerror(errno));
            sfree(buf);
            return NULL;
        }
        got = fread(buf, 1, (size_t)len, fp);
        fclose(fp);
        if (got != (size_t)len) {
            fprintf(stderr, "puttygen: %s: too short: %d bytes of random data are needed "
                    "for this key, and the file has %d\n", device, len, (int)got);
            smemclr(buf, len);
            sfree(buf);
            return NULL;
        }
        return buf;
    }

    if (!ro_cryptrandom_bytes(buf, (size_t)len)) {
        fprintf(stderr,
                "puttygen: there is no good source of random numbers to make a key from.\n"
                "The CryptRandom module is not loaded and could not be found in System:Modules.\n"
                "Install the CryptRandom package with !PackMan (System section), or use\n"
                "--random-device FILE to give a file of random bytes made elsewhere.\n");
        sfree(buf);
        return NULL;
    }
    return buf;
}
