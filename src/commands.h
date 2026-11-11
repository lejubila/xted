#ifndef KED_COMMANDS_H
#define KED_COMMANDS_H

#include "editor.h"

/* Every user-visible command. The palette lists them by name; keymap.c binds keys to them
 * (the default shortcut here, then the user's [keys] configuration). */
typedef struct {
    const char *id;         /* stable English name used in the configuration */
    const char *name;       /* name shown in the palette */
    const char *key;        /* default shortcut as written in the configuration, "" for none */
    void (*fn)(Editor *e);
    int modifies;           /* changes the text: refused in a read-only document */
    int vertical;           /* a vertical move: keeps the wanted column */
} Command;

extern const Command commands[];
extern const size_t ncommands;

/* Runs command idx as a key or the palette would: read-only check, undo group, wanted column. */
void command_run(Editor *e, int idx);

#endif
