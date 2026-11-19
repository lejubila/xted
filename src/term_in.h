#ifndef XTED_TERM_IN_H
#define XTED_TERM_IN_H

#include <stddef.h>

/* Between term_keys.c, which makes keys of the bytes the terminal sends, and the terminal of the
 * system (term.c on POSIX, term_win.c on Windows), which puts them in the buffer. */

#define TERM_INBUF 4096

extern unsigned char term_inbuf[TERM_INBUF];
extern size_t term_inpos, term_inlen;   /* the bytes not read yet: [term_inpos, term_inlen) */
extern int term_in_eof;                 /* the input is closed */

/* Waits up to timeout_ms (0: does not wait) for input and puts it in the buffer, which is empty;
 * 0 when none came (term_in_eof set if it is closed). */
int term_fill(int timeout_ms);

#endif
