#ifndef KED_FIND_H
#define KED_FIND_H

#include "editor.h"

/* Search, replace, go to line and search in files, on the editor state.
 * The search bar is a small state machine on the message row: while
 * Editor.find.mode is not FIND_OFF every key goes to find_handle_key. */

/* Ctrl+F / Ctrl+R. */
void find_open(Editor *e, int replace);
/* Alt+F: search the files of the project. */
void find_files_open(Editor *e);
void find_handle_key(Editor *e, Key k);
/* F3 / Shift+F3 with the bar closed. */
void find_next(Editor *e, int forward);
/* Ctrl+G. */
void find_goto(Editor *e);
/* Opens result idx of the last search in files. */
void find_open_result(Editor *e, size_t idx);
/* F12: the definition of the word under the cursor, from the tags file. */
void find_tag(Editor *e);
/* Opens definition idx of the last F12. */
void find_open_tag(Editor *e, size_t idx);
void find_free(Editor *e);

#endif
