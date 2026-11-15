#ifndef XTED_GIT_H
#define XTED_GIT_H

#include <stddef.h>
#include <stdint.h>

/* Marks of Line.git. */
enum { GIT_NONE = 0, GIT_ADD = 1, GIT_MOD = 2, GIT_DEL = 3 };

/* Reads the hunk headers of a `git diff -U0` and marks the lines (0-based, nlines of them):
 * added, modified, and for a pure deletion the line above it. */
void git_parse_diff(const char *diff, size_t n, uint8_t *marks, size_t nlines);

/* Marks of the file at path against HEAD (the index in a repository without commits), as it is
 * on disk. Returns 1 with *marks (malloc'ed, nlines bytes), 0 if the file is not tracked by a
 * repository, -1 if git cannot be run. */
int git_file_marks(const char *path, size_t nlines, uint8_t **marks);

#endif
