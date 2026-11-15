#ifndef XTED_INDENT_H
#define XTED_INDENT_H

#include "buffer.h"

/* Finds how a text is indented: tab characters or spaces, and how many spaces per level.
 * Looks at the first lines only. Returns 0 when there is not enough to tell (an empty file, no
 * indented lines); otherwise 1, with *tabs set for tab indentation and *width the number of
 * spaces of a level (meaningful only when *tabs is 0). */
int indent_detect(const Buffer *b, int *tabs, int *width);

#endif
