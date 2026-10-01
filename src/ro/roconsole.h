/*
 * roconsole.h - helpers shared by the RISC OS console tools.
 */
#ifndef PUTTY_ROCONSOLE_H
#define PUTTY_ROCONSOLE_H

#include <stddef.h>
#include <stdbool.h>

/*
 * Read one line from the keyboard (with line editing; no echo if echo is
 * false) or from stdin if that has been redirected.  Returns the length, or
 * -1 on end of file or Escape.
 */
int ro_read_line(char *buf, size_t size, bool echo);

/* True if the entropy source is only timing jitter (see noise.c). */
bool ro_entropy_is_weak(void);

#endif /* PUTTY_ROCONSOLE_H */
