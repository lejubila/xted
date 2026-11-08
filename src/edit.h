#ifndef KED_EDIT_H
#define KED_EDIT_H

#include "editor.h"

/* Editing commands on the editor state. Every command that changes the text
 * is one undo group, except typing/Backspace/Delete, which merge until
 * another kind of key closes the group (see handle_key in editor.c). */

/* ---- selection ---- */

/* True if the selection is not empty. */
int edit_sel_active(const Editor *e);
/* Ordered ends of the selection; only meaningful when it is active. */
void edit_sel_range(const Editor *e, size_t *y0, size_t *x0, size_t *y1, size_t *x1);
/* The part of line y that is selected in a view: bytes [from,to); *nl is set
 * when the selection goes on past the end of the line. Returns 0 if none. */
int edit_view_sel_line(const Buffer *b, const View *v, size_t y, size_t *from, size_t *to,
                       int *nl);
int edit_sel_line(const Editor *e, size_t y, size_t *from, size_t *to, int *nl);
/* Drops the selection, leaving the cursor at its start or end. */
void edit_sel_collapse(Editor *e, int to_end);
void edit_select_all(Editor *e);
void edit_select_line(Editor *e);
void edit_select_word(Editor *e);

/* ---- typing; a selection is replaced ---- */

void edit_insert_cp(Editor *e, uint32_t cp);
/* Keeps the indentation of the current line, up to the cursor. */
void edit_newline(Editor *e);
void edit_backspace(Editor *e);
void edit_delete(Editor *e);
/* Tab indents the selected lines, or inserts a tab without selection. */
void edit_tab(Editor *e);
/* Removes one tab (or up to tabw spaces) from the start of the selected lines. */
void edit_dedent(Editor *e);

/* ---- clipboard ---- */

/* Without a selection, copy and cut take the whole line. */
void edit_copy(Editor *e);
void edit_cut(Editor *e);
void edit_paste(Editor *e);
/* Replaces the text of the last paste with the next older kill ring entry. */
void edit_paste_older(Editor *e);
/* Text from a bracketed paste: no autoindent, control characters dropped. */
void edit_paste_text(Editor *e, const char *s, size_t n);

void edit_undo(Editor *e);
void edit_redo(Editor *e);

/* ---- lines (the lines touched by the selection, or the cursor line) ---- */

void edit_move_lines(Editor *e, int down);
void edit_duplicate_lines(Editor *e, int down);
void edit_delete_lines(Editor *e);
/* Joins each line of the selection with the next one (the cursor line if none). */
void edit_join_lines(Editor *e);

#endif
