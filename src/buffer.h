#ifndef KED_BUFFER_H
#define KED_BUFFER_H

#include <stddef.h>

/* One line of raw UTF-8 bytes, without terminator. */
typedef struct {
    char *s;
    size_t len, cap;
} Line;

/* Text of one file. There is always at least one (possibly empty) line.
 * All edits go through buf_insert/delete/split/join, so that undo can
 * hook in there and the representation can change without touching callers. */
typedef struct {
    Line *lines;
    size_t nlines, cap;
    char *path;
    int modified;
    int crlf;           /* write CRLF line endings */
    int final_newline;  /* file ends with a newline */
    int has_nul;        /* loaded data contained NUL bytes */
    int is_new;         /* path named a file that does not exist yet */
    int empty_origin;   /* started as a new or 0-byte file: an empty buffer saves as 0 bytes */
} Buffer;

void buf_init(Buffer *b);
void buf_free(Buffer *b);

/* Returns 0 on success (also for a nonexistent path: the buffer is then
 * empty and flagged is_new), -1 with a message in err. */
int buf_load(Buffer *b, const char *path, char *err, size_t errlen);
/* Atomic save (temp file + rename), keeping mode and, if possible, owner.
 * path == NULL saves to b->path. Returns bytes written, or -1 with err. */
long buf_save(Buffer *b, const char *path, char *err, size_t errlen);

/* s must not contain '\n'. */
void buf_insert(Buffer *b, size_t y, size_t x, const char *s, size_t n);
void buf_delete(Buffer *b, size_t y, size_t x, size_t n);
/* Breaks line y at byte x; the tail becomes line y + 1. */
void buf_split(Buffer *b, size_t y, size_t x);
/* Appends line y + 1 to line y. */
void buf_join(Buffer *b, size_t y);

#endif
