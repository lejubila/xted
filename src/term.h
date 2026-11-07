#ifndef KED_TERM_H
#define KED_TERM_H

#include <stddef.h>
#include <stdint.h>

enum { MOD_SHIFT = 1, MOD_ALT = 2, MOD_CTRL = 4 };

/* Key codes below 0x110000 are Unicode code points (control characters
 * included, e.g. Ctrl+S is 0x13); special keys live above KEY_SPECIAL. */
enum {
    KEY_NONE = 0,
    KEY_TAB = 9,
    KEY_ENTER = 13,
    KEY_ESC = 27,
    KEY_BACKSPACE = 127,
    KEY_SPECIAL = 0x200000,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_DEL, KEY_INS,
    KEY_EOF
};

#define CTRL(c) ((c) & 0x1f)

typedef struct {
    uint32_t code;
    int mods;
} Key;

enum { TERM_EV_KEY = 1, TERM_EV_RESIZE = 2 };

/* Raw mode + alternate screen; the terminal is restored on exit, on fatal
 * signals and on SIGTERM/SIGHUP. Returns -1 if stdin/stdout is not a tty. */
int term_init(void);
/* Async-signal-safe and idempotent. */
void term_restore(void);
void term_size(int *rows, int *cols);

/* Blocks until input or a resize arrives (timeout_ms < 0: no timeout).
 * Returns a mask of TERM_EV_*, 0 on timeout. */
int term_wait(int timeout_ms);
int term_has_input(void);
/* Reads one key from the buffered input; code is KEY_NONE for sequences
 * that are ignored, KEY_EOF when stdin is closed. */
void term_read_key(Key *k);
void term_write(const char *s, size_t n);

#endif
