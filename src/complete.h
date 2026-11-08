#ifndef KED_COMPLETE_H
#define KED_COMPLETE_H

#include "doc.h"

#include <stddef.h>

#define COMPLETE_ROWS 8
#define COMPLETE_MIN_PREFIX 2
#define COMPLETE_SCAN_LINES 100000

enum { CAND_WORD = 0, CAND_KEYWORD, CAND_SNIPPET };

typedef struct {
    char *text;             /* what is inserted (for a snippet: its name) */
    int kind;               /* CAND_* */
} Cand;

/* The suggestion popup: candidates for the word before the cursor, best first. */
typedef struct {
    int on;
    Cand *v;
    size_t n, cap;
    size_t sel, top;        /* highlighted candidate, first visible one */
    int picked;             /* the user chose with the arrows: Enter then accepts */
    size_t y, x;            /* where the word being completed starts */
} Complete;

/* Length in bytes of the word that ends at (y,x); *start is where it begins. */
size_t complete_prefix(const Buffer *b, size_t y, size_t x, size_t *start);

/* Collects the candidates for the word before (y,x) of cur: words of every
 * open document (nearest first), the language's keywords and its snippets.
 * Leaves the popup open (on) if there is at least one. Returns that. */
int complete_open(Complete *c, Doc *const *docs, size_t ndocs, const Doc *cur, size_t y,
                  size_t x);
void complete_move(Complete *c, long delta);
void complete_close(Complete *c);
void complete_free(Complete *c);

#endif
