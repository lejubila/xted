#ifndef KED_SYNTAX_H
#define KED_SYNTAX_H

#include "buffer.h"

#include <stddef.h>
#include <stdint.h>

typedef enum {
    SC_NORMAL, SC_COMMENT, SC_STRING, SC_KEYWORD, SC_TYPE, SC_CONSTANT, SC_NUMBER,
    SC_PREPROC, SC_FUNCTION, SC_OPERATOR, SC_VARIABLE, SC_HEADING, SC_EMPHASIS,
    SC_CODE, SC_LINK,
    SC_COUNT
} SynClass;

/* A language: regions, word lists and a few flags, built from a text definition. */
typedef struct Syntax Syntax;

/* Parses a definition (see langs.c for the format). Returns NULL with a
 * message in err, including the line number, on a malformed definition. */
Syntax *syn_parse(const char *text, size_t n, char *err, size_t errlen);
void syn_free(Syntax *s);
const char *syn_name(const Syntax *s);
/* Comment markers of the language: *close is NULL for line comments.
 * Returns 0 if it has none (or s is NULL). */
int syn_comment(const Syntax *s, const char **open, const char **close);
/* Calls fn for every keyword/type/constant of the language. */
void syn_each_word(const Syntax *s, void (*fn)(const char *w, size_t n, int cls, void *ctx),
                   void *ctx);

/* Picks the language for a file from its name, first line (shebang) and
 * modelines in the first/last 5 lines. NULL means plain text. */
Syntax *syn_detect(const Buffer *b);

/* Classifies one line: cls[i] receives the class of byte i (cls may be NULL
 * when only the state is wanted). Returns the state at the end of the line:
 * 0, or 1 + the index of the multi-line region left open. */
uint16_t syn_line(const Syntax *s, uint16_t state_in, const char *text, size_t len, uint8_t *cls);

/* Makes the states of lines [0, last] valid (Buffer.stable > last). */
void syn_update(Syntax *s, Buffer *b, size_t last);
/* Classes of line y, valid until the next call. Needs syn_update(b, y - 1)
 * to have run. The pointer is NULL for an empty line. */
const uint8_t *syn_classes(Syntax *s, const Buffer *b, size_t y);

/* If the cursor is on a bracket, or just after one, finds its partner,
 * skipping brackets inside strings and comments. Returns 1 and sets both
 * positions: (*py,*px) is the bracket under/before the cursor, (*my,*mx) its partner. */
int syn_match_bracket(Syntax *s, Buffer *b, size_t y, size_t x,
                      size_t *py, size_t *px, size_t *my, size_t *mx);

#endif
