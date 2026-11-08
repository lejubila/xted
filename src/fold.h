#ifndef KED_FOLD_H
#define KED_FOLD_H

#include "editor.h"

/* Code folding. A folded line (Line.fold = N) hides the N lines after it; the
 * screen shows "… N righe" at its end. Lines are always in text order, so every
 * helper below is a walk over the visible lines; with no folds (Buffer.folded
 * == 0) they cost nothing. The cursor is never left inside a fold. */

/* The line shown after y / before y (y itself when there is none before). */
size_t fold_next(const Buffer *b, size_t y);
size_t fold_prev(const Buffer *b, size_t y);
/* y moved n visible lines down / up (clamped to the first / last visible line). */
size_t fold_advance(const Buffer *b, size_t y, size_t n);
size_t fold_back(const Buffer *b, size_t y, size_t n);
/* The k-th visible line after from, or Buffer.nlines if the text ends first. */
size_t fold_nth(const Buffer *b, size_t from, size_t k);
/* Visible lines in [from, to). */
size_t fold_count(const Buffer *b, size_t from, size_t to);
/* The folded line that hides y, or y itself when it is visible. */
size_t fold_head(const Buffer *b, size_t y);

/* The block that can be folded starting at line y: it hides lines y+1..*last.
 * By matching bracket when the line ends with one, otherwise by indentation. */
int fold_range(Syntax *s, Buffer *b, size_t y, size_t *last);

/* Alt+Z: folds the block at the cursor (the innermost one that contains the
 * cursor line), or unfolds the folded line it is on. */
void fold_toggle(Editor *e);
void fold_all(Editor *e);
void unfold_all(Editor *e);
/* Unfolds whatever hides the cursor or the selection anchor. */
void fold_reveal(Editor *e);

#endif
