#ifndef KED_SNIPPET_H
#define KED_SNIPPET_H

#include "util.h"

#include <stddef.h>

#define SNIP_MAX 10

/* A snippet is a name and a body. In the body: $1..$9 and ${1:default} are
 * placeholders visited in order with Tab, $0 (or ${0:text}) is where the
 * cursor ends, "$$" is a literal dollar. A snippet with no $0 ends at its
 * end. Mirrors (the same number twice) are not supported: only the first is a stop. */
typedef struct {
    const char *name;
    const char *body;
} SnipDef;

/* Built-in snippets of a language (by Syntax name); NULL, *n = 0 if none. */
const SnipDef *snip_lang(const char *lang, size_t *n);

/* Stops of an expanded snippet as byte offsets in the plain text, in visiting order. */
typedef struct {
    size_t off, len;
} SnipStop;

typedef struct {
    SnipStop stop[SNIP_MAX];
    size_t n;
} SnipStops;

/* Expands body into plain text (appended to text) and its stops. */
void snip_parse(const char *body, SBuf *text, SnipStops *out);

/* A running session: the stops as positions in the buffer. */
typedef struct {
    int on;
    struct { size_t y, x, len; } stop[SNIP_MAX];
    size_t n, cur;
} SnipSession;

/* After a key that may have edited line cy (now cursor cx), with the line count and the
 * length of the current stop's line before and after: keeps the stops in step.
 * Returns 0, and ends the session, if the edit left the current stop. */
int snip_adjust(SnipSession *s, size_t nlines_before, size_t nlines_now, size_t len_before,
                size_t len_now, size_t cy, size_t cx);

#endif
