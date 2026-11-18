#ifndef XTED_TERM_H
#define XTED_TERM_H

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
    KEY_FOCUS_IN, KEY_FOCUS_OUT,    /* the terminal window gained / lost focus */
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_PASTE,      /* bracketed paste: the text is in term_paste_text() */
    KEY_MOUSE,      /* a mouse event: see term_mouse_event() */
    KEY_EOF,
    KEY_CMD         /* a button of the message row for a command with no key: its index in mods */
};

#define CTRL(c) ((c) & 0x1f)

typedef struct {
    uint32_t code;
    int mods;
} Key;

typedef enum { MOUSE_PRESS, MOUSE_RELEASE, MOUSE_DRAG, MOUSE_WHEEL_UP, MOUSE_WHEEL_DOWN } MouseKind;

typedef struct {
    MouseKind kind;
    int button;         /* 0 left, 1 middle, 2 right (not meaningful for the wheel) */
    int x, y;           /* 0-based cell */
    int mods;           /* MOD_* */
} MouseEv;

enum { TERM_EV_KEY = 1, TERM_EV_RESIZE = 2, TERM_EV_FD = 4 };

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
/* Also wakes term_wait when fd is readable (TERM_EV_FD); -1 stops watching. One at a time. */
void term_watch(int fd);
/* Reads one key from the buffered input; code is KEY_NONE for sequences
 * that are ignored, KEY_EOF when stdin is closed. */
void term_read_key(Key *k);
/* Text of the last KEY_PASTE; valid until the next one. Never NULL. */
const char *term_paste_text(size_t *n);
/* Reports clicks, drags and the wheel (xterm SGR protocol) as KEY_MOUSE; off by default. */
void term_mouse(int on);
/* The event of the last KEY_MOUSE. */
MouseEv term_mouse_event(void);
void term_write(const char *s, size_t n);

#endif
