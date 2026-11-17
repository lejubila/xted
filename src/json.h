#ifndef XTED_JSON_H
#define XTED_JSON_H

#include "util.h"

#include <stddef.h>

/* What is wrong with a JSON text, and where (line and byte column, from 0). */
enum {
    JE_NONE, JE_END, JE_CHAR, JE_STRING, JE_ESCAPE, JE_CONTROL, JE_NUMBER, JE_KEY, JE_COLON,
    JE_COMMA, JE_TRAIL, JE_EXTRA, JE_DEPTH, JE_COMMENT
};

typedef struct {
    int code;               /* JE_* */
    size_t line, col;
} JsonErr;

/* The message of an error code, in English (to be passed to tr()). */
const char *json_err_text(int code);

/* Checks the JSON text s[0, n). Comments (// and / * * /) are allowed; a comma before } or ]
 * only when lenient (JSONC, JSON5, tsconfig.json). Returns 0, or -1 with the first error in e. */
int json_check(const char *s, size_t n, int lenient, JsonErr *e);

/* Writes s[0, n) again into out: one member a line, indented by indent at each level, or all on
 * one line when indent is NULL; no final newline. Strings and numbers are kept as written.
 * Returns 0; -1 with the error in e; 1 when the text has comments (they would be lost: nothing
 * is written). */
int json_format(const char *s, size_t n, int lenient, const char *indent, SBuf *out, JsonErr *e);

#endif
