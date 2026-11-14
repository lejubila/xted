#ifndef KED_SYMBOLS_H
#define KED_SYMBOLS_H

#include "doc.h"
#include "stdsym.h"

#include <stddef.h>

/* ---- patterns ----------------------------------------------------------- */

enum { PM_NAME = 64, PM_PARAMS = 128, PM_PATH = 256 };

/* What a pattern captured. */
typedef struct {
    char name[PM_NAME];     /* @ */
    char type[PM_NAME];     /* % (last segment of a qualified name) */
    char params[PM_PARAMS]; /* ^ (text up to the matching parenthesis) */
    char path[PM_PATH];     /* ~ a quoted string, & a dotted module path */
    size_t end;             /* where the match ends */
} PatMatch;

/* Matches the pattern (items separated by blanks; see the header of langs.c) on s[from, len),
 * anchored at from after blanks. Returns 1 and fills m, or 0. */
int pat_match(const char *pat, const char *s, size_t len, size_t from, PatMatch *m);

/* ---- index of a file ---------------------------------------------------- */

typedef struct {
    char *name, *sig, *ret;     /* ret: the type; for a class, its base class */
    int kind;                   /* RULE_CLASS, RULE_FUNC, RULE_METHOD, RULE_PROP, RULE_CONST, RULE_VAR */
    long owner;                 /* index of the class a member belongs to, -1 for none */
    size_t y0, y1;              /* lines of a class (its block) */
} Sym;

typedef struct SymIndex SymIndex;
struct SymIndex {
    char *key;                  /* real path of the file (NULL: a document without one) */
    const Buffer *buf;          /* the open document it was built from (NULL: read from disk) */
    unsigned long gen;          /* Buffer.gen it reflects */
    long built_us, last_us;     /* how long the last build took, and when it ended */
    long checked;               /* disk files: when mtime was last looked at */
    long mtime;
    long size;
    Sym *v;
    size_t n, cap;
    char **inc;                 /* include specs: a path as written, or ":Name" for a class to find */
    size_t ninc;
    char **incp;                /* inc resolved to real paths (NULL entries: not found) */
    int resolved;
    unsigned long used;
};

/* Builds the index of buffer b with the rules of syn. */
SymIndex *sym_build(const Buffer *b, Syntax *syn);
void sym_index_free(SymIndex *x);

/* ---- completion queries ------------------------------------------------- */

typedef struct SymCache SymCache;
SymCache *sym_cache_new(void);
void sym_cache_free(SymCache *c);

/* kind: RULE_* (the standard library reports its own kinds mapped to these). rank: 0 current
 * file, 1 included or open file, 2 standard library. */
typedef void (*SymFn)(void *ctx, const char *name, int kind, const char *sig, const char *ret,
                      int rank);

/* If the text before byte `start` of line y ends with a member operator of the language
 * (-> :: .), returns its position; otherwise (size_t)-1. */
size_t sym_member_at(Syntax *s, const Buffer *b, size_t y, size_t start);

/* Members of the object before the operator at (y, sep): the type is found by following the
 * chain of calls and properties. Returns 1 if the type was resolved, else 0 after reporting
 * every member of every known class. */
int sym_members(SymCache *c, Doc *const *docs, size_t ndocs, Doc *cur, size_t y, size_t sep,
                SymFn fn, void *ctx);
/* Functions, classes, constants and variables visible from cur: the file, the open documents,
 * the files it includes, the standard library. */
void sym_globals(SymCache *c, Doc *const *docs, size_t ndocs, Doc *cur, SymFn fn, void *ctx);

/* The type name (class) of the expression before the operator at (y, sep), "" if unknown;
 * writes at most n bytes. Used by the tests. */
int sym_type_before(SymCache *c, Doc *const *docs, size_t ndocs, Doc *cur, size_t y, size_t sep,
                    char *out, size_t n);

#endif
