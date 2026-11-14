#ifndef KED_COMPLETE_H
#define KED_COMPLETE_H

#include "doc.h"
#include "symbols.h"
#include "util.h"

#include <stddef.h>

#define COMPLETE_ROWS 8
#define COMPLETE_MIN_PREFIX 2
#define COMPLETE_SCAN_LINES 100000

enum { CAND_WORD = 0, CAND_KEYWORD, CAND_SNIPPET, CAND_FUNC, CAND_CLASS, CAND_METHOD, CAND_PROP,
       CAND_CONST };

typedef struct {
    char *text;             /* what is inserted (for a snippet: its name) */
    char *sig;              /* signature shown for the highlighted candidate (may be NULL) */
    int kind;               /* CAND_* */
} Cand;

/* The suggestion popup: candidates for the word before the cursor, best first. */
typedef struct {
    int on;
    Cand *v;
    size_t n, cap;
    size_t sel, top;        /* highlighted candidate, first visible one */
    size_t min_prefix;      /* letters needed before candidates are looked up (0: the default) */
    int picked;             /* the user chose with the arrows: Enter then accepts */
    size_t y, x;            /* where the word being completed starts */
    size_t rows;            /* rows the renderer could show last time (0: COMPLETE_ROWS) */
    SymCache *sc;           /* indexes of the symbols of files and of the standard library */
} Complete;

/* Length in bytes of the word that ends at (y,x); *start is where it begins. */
size_t complete_prefix(const Buffer *b, size_t y, size_t x, size_t *start);

/* Collects the candidates for the word before (y,x) of cur: after a member operator (-> :: .)
 * the members of the object before it; otherwise the symbols of the file, of the files it
 * includes and of the standard library, words of every open document (nearest first), the
 * language's keywords and its snippets. Leaves the popup open (on) if there is at least one.
 * Returns that. */
int complete_open(Complete *c, Doc *const *docs, size_t ndocs, Doc *cur, size_t y, size_t x);
/* For a function or method candidate with parameters: the snippet body "name(${1:a}, ${2:b})$0"
 * that inserts a call with its mandatory arguments as placeholders (optional ones, with a
 * default or "...", are left out). Returns 1 for that, 2 for a function without parameters
 * (body is the plain text "name()"), 0 (body untouched) for other candidates. */
int complete_call_body(const Cand *c, SBuf *body);
void complete_move(Complete *c, long delta);
void complete_close(Complete *c);
void complete_free(Complete *c);

#endif
