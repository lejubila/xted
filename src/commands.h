#ifndef XTED_COMMANDS_H
#define XTED_COMMANDS_H

#include "editor.h"

/* Every user-visible command. The palette lists them by name; keymap.c binds keys to them
 * (the default shortcut here, then the user's [keys] configuration). */
/* Palette sections: the actions first, then the settings that change how the editor behaves. */
enum { G_FILE, G_BUFFERS, G_SEARCH, G_EDIT, G_BLOCKS, G_RUN, G_OTHER, G_SETTINGS, G_COUNT };

/* What a setting command shows in the palette (see command_state). */
enum { ST_NONE, ST_MOUSE, ST_SUGGEST, ST_PAIR, ST_GIT, ST_CLIP, ST_TABS, ST_SP2, ST_SP4, ST_SP8,
       ST_THEME, ST_LANG, ST_COLORS, ST_GLYPHS };

typedef struct {
    const char *id;         /* stable English name used in the configuration */
    const char *name;       /* name shown in the palette */
    const char *key;        /* default shortcut as written in the configuration, "" for none */
    void (*fn)(Editor *e);
    int modifies;           /* changes the text: refused in a read-only document */
    int vertical;           /* a vertical move: keeps the wanted column */
    int group;              /* G_*: commands of a group are listed together, in table order */
    int state;              /* ST_*: the current value this command sets, ST_NONE for an action */
} Command;

extern const Command commands[];
extern const size_t ncommands;

extern const char *const command_groups[G_COUNT];

/* The current value of a setting command: -1 if idx is an action; else 1 (on) or 0 (off) for
 * the marker, or 2 for a value without a marker, with the text to show in val (the theme's
 * name, "attivo", ...). */
int command_state(const Editor *e, int idx, char *val, size_t n);

/* Runs command idx as a key or the palette would: read-only check, undo group, wanted column. */
void command_run(Editor *e, int idx);

#endif
