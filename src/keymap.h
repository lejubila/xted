#ifndef KED_KEYMAP_H
#define KED_KEYMAP_H

#include "term.h"

#include <stddef.h>

/* Keys as written in the configuration: modifiers joined by '+', then the key:
 *   ctrl+s  alt+/  alt+shift+up  ctrl+tab  shift+f3  f8  alt+,
 * Key names: up down left right home end pgup pgdn del ins tab enter esc space
 * backspace f1..f12; anything else is a single character. */

/* Parses spec into a normalised key. Returns 0, or -1 with a message in err. */
int key_parse(const char *spec, Key *out, char *err, size_t errlen);
/* Writes the spec of k ("ctrl+s") or, with pretty, the label ("Ctrl+S"). */
void key_format(Key k, int pretty, char *out, size_t n);
/* The form in which keys are stored and compared: Ctrl+letter is the control code
 * without the Ctrl flag, Alt+capital is Alt+Shift+lowercase. */
Key key_norm(Key k);

typedef struct {
    Key key;
    int cmd;                /* index in commands[], or -1: the key is unbound */
} Bind;

typedef struct {
    Bind *b;
    size_t n, cap;
} Keymap;

/* The default bindings: the shortcut of every command plus a few extra keys. */
void keymap_init(Keymap *m);
void keymap_free(Keymap *m);
/* The command bound to k, or -1. */
int keymap_find(const Keymap *m, Key k);
/* Binds k to command cmd (cmd == -1 unbinds). Replaces what k did before. */
void keymap_bind(Keymap *m, Key k, int cmd);
/* The first key bound to cmd as a label ("Ctrl+S"); empty if none. */
void keymap_label(const Keymap *m, int cmd, char *out, size_t n);

/* Index in commands[] of the command with this id, or -1. */
int command_by_id(const char *id);

#endif
