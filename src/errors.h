#ifndef XTED_ERRORS_H
#define XTED_ERRORS_H

#include <stddef.h>

/* Recognises a compiler/grep/traceback location in one line of output:
 *   path:LINE[:COL][: ...]        gcc, clang, grep -n, tsc --pretty false
 *   path(LINE[,COL])[: ...]       tsc
 *   File "path", line LINE        Python
 * On success sets the path (an offset and length into line) and the 1-based
 * line and column (0 when there is none) and returns 1. Only the text is
 * judged; whether the file exists is up to the caller. */
int err_parse(const char *line, size_t len, size_t *path_off, size_t *path_len, size_t *lineno,
              size_t *col);

#endif
