/*
 * hrtest.c - does the HAL's high resolution counter work from a user program?
 * OS_Hardware 0 calls a HAL routine; HAL_CounterRate is routine 19 and
 * HAL_CounterRead 21.  Prints the rate and a few readings with the centisecond clock.
 */
#include <stdio.h>
#include "kernel.h"
#include "swis.h"

static unsigned hal(int routine, int *err)
{
    unsigned r0 = 0;
    _kernel_oserror *e = _swix(OS_Hardware, _IN(8) | _IN(9) | _OUT(0), 0, routine, &r0);
    *err = e != NULL;
    if (e) printf("OS_Hardware %d: %s\n", routine, e->errmess);
    return r0;
}

int main(void)
{
    int i, err;
    unsigned rate = hal(19, &err);
    unsigned period = hal(20, &err);
    printf("counter rate %u Hz, period %u\n", rate, period);
    for (i = 0; i < 12; i++) {
        int cs = 0;
        unsigned c = hal(21, &err);
        _swix(OS_ReadMonotonicTime, _OUT(0), &cs);
        printf("cs %d  counter %u\n", cs, c);
        if (err) break;
    }
    return 0;
}
