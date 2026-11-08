#ifndef KED_COMMANDS_H
#define KED_COMMANDS_H

#include "editor.h"

/* Every user-visible command, by name, for the palette (and, in Phase 7,
 * for key maps). keys is the default shortcut as text, "" if none. */
typedef struct {
    const char *name;
    const char *keys;
    void (*fn)(Editor *e);
    int modifies;           /* changes the text: refused in a read-only document */
} Command;

extern const Command commands[];
extern const size_t ncommands;

#endif
