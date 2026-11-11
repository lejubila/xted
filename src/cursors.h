#ifndef KED_CURSORS_H
#define KED_CURSORS_H

#include "editor.h"

/* Multi-cursor editing. The primary cursor is Editor.cy/cx/sel_*; the others
 * are Editor.xc[]. Commands written for one cursor run on every cursor by
 * loading each into the primary fields in turn (cur_each). */

static inline int cur_multi(const Editor *e)
{
    return e->nxc > 0;
}

/* Drops the extra cursors. */
void cur_clear(Editor *e);

/* Ctrl+D: selects the word under the cursor, then adds the next occurrence of
 * the selected text as a new cursor. Alt+D: adds all of them. */
void cur_add_next(Editor *e);
void cur_add_all(Editor *e);
/* Adds a cursor at byte x of line y and makes it the primary one (Alt+click). Does nothing
 * if a cursor is already there. */
void cur_add_at(Editor *e, size_t y, size_t x);
/* Adds a cursor on the line above/below the primary, in the same column. */
void cur_add_vertical(Editor *e, int down);

/* Calls fn once per cursor, from the last in the text to the first, with each
 * loaded into the primary fields (idx is its place in text order). All edits
 * make one undo group. Afterwards the cursors are ordered and identical ones merged. */
void cur_each(Editor *e, void (*fn)(Editor *e, void *arg, size_t idx), void *arg);

/* Selections of all cursors joined by newlines (malloc'ed); NULL if none selects. */
char *cur_selected_text(const Editor *e, size_t *n);
/* Number of cursors, primary included. */
size_t cur_count(const Editor *e);

#endif
