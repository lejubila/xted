#ifndef KED_BUFFER_H
#define KED_BUFFER_H

#include "undo.h"

#include <stddef.h>
#include <stdint.h>

/* One line of raw UTF-8 bytes, without terminator. */
typedef struct {
    char *s;
    size_t len, cap;
    uint16_t hl;        /* opaque derived state (syntax), valid below Buffer.stable */
} Line;

/* Text of one file. There is always at least one (possibly empty) line.
 * All edits go through buf_insert/delete/split/join (directly or through the
 * helpers below, which are built on them): that is where undo records, and
 * where the representation could change without touching callers. */
typedef struct {
    Line *lines;
    size_t nlines, cap;
    char *path;
    int modified;
    int crlf;           /* write CRLF line endings */
    int final_newline;  /* file ends with a newline */
    int has_nul;        /* loaded data contained NUL bytes */
    int is_new;         /* path named a file that does not exist yet */
    size_t stable;      /* lines [0, stable) unchanged since their derived state was computed */
    int empty_origin;   /* started as a new or 0-byte file: an empty buffer saves as 0 bytes */
    Undo undo;
} Buffer;

void buf_init(Buffer *b);
void buf_free(Buffer *b);

/* Returns 0 on success (also for a nonexistent path: the buffer is then
 * empty and flagged is_new), -1 with a message in err. */
int buf_load(Buffer *b, const char *path, char *err, size_t errlen);
/* Like buf_load, from an open descriptor: the buffer gets no name. */
int buf_load_fd(Buffer *b, int fd, char *err, size_t errlen);
/* Atomic save (temp file + rename), keeping mode and, if possible, owner.
 * path == NULL saves to b->path. Returns bytes written, or -1 with err. */
long buf_save(Buffer *b, const char *path, char *err, size_t errlen);

/* Primitives. s must not contain '\n'. */
void buf_insert(Buffer *b, size_t y, size_t x, const char *s, size_t n);
void buf_delete(Buffer *b, size_t y, size_t x, size_t n);
/* Breaks line y at byte x; the tail becomes line y + 1. */
void buf_split(Buffer *b, size_t y, size_t x);
/* Appends line y + 1 to line y. */
void buf_join(Buffer *b, size_t y);

/* Undo groups. begin remembers the cursor to restore; operations made until
 * close are undone together (typing and deleting merge while a group stays open). */
void buf_undo_begin(Buffer *b, size_t cy, size_t cx);
void buf_undo_cursor(Buffer *b, size_t cy, size_t cx);
void buf_undo_close(Buffer *b);
/* Return 0 if there is nothing to undo/redo, else 1 with the cursor to restore. */
int buf_undo(Buffer *b, size_t *cy, size_t *cx);
int buf_redo(Buffer *b, size_t *cy, size_t *cx);

/* Text from (y0,x0) to (y1,x1), lines separated by '\n'. The result is
 * malloc'ed and not NUL-terminated. */
char *buf_get_range(const Buffer *b, size_t y0, size_t x0, size_t y1, size_t x1, size_t *n);
void buf_delete_range(Buffer *b, size_t y0, size_t x0, size_t y1, size_t x1);
/* Inserts s (which may contain '\n') at (*y,*x) and moves them to its end. */
void buf_insert_text(Buffer *b, size_t *y, size_t *x, const char *s, size_t n);
/* Removes lines y0..y1 (inclusive); an emptied buffer keeps one empty line. */
void buf_delete_lines(Buffer *b, size_t y0, size_t y1);
/* Inserts s as new line(s) before or after line y. */
void buf_insert_lines(Buffer *b, size_t y, int after, const char *s, size_t n);

#endif
