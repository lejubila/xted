#ifndef XTED_SYMBOLS_H
#define XTED_SYMBOLS_H

#include "doc.h"
#include "stdsym.h"

#include <stddef.h>

/* ---- patterns ----------------------------------------------------------- */

enum { PM_NAME = 64, PM_PARAMS = 128, PM_PATH = 256 };

/* What a pattern captured. */
typedef struct {
    char name[PM_NAME];     /* @ */
    char type[PM_NAME];     /* % (last segment of a qualified name) */
    char qual[PM_PATH];     /* % the whole qualified name, without generics */
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
    int hidden;                 /* private or protected */
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

/* The scans that read the project (its files, its classes) are cut after ms milliseconds and go
 * on at the next call, so that typing never waits for a big tree: call before a group of
 * queries (0: no limit, the default). sym_busy tells whether, since then, one was cut short: the
 * results may be incomplete and the same query should be repeated. */
void sym_budget(SymCache *c, long ms);
int sym_busy(const SymCache *c);

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
/* 1 if the document is C++ rather than C (by its extension, or by what it includes and uses). */
int sym_is_cxx(const Doc *d);
void sym_globals(SymCache *c, Doc *const *docs, size_t ndocs, Doc *cur, SymFn fn, void *ctx);

/* The symbols of the other files of the project that cur does not reach, with how cur would write
 * the include or import of their file. C and C++: the functions, types, macros and variables of
 * the headers ("buffer.h", "../src/buffer.h"). JavaScript and TypeScript: what the modules export
 * ("./util", "../lib/api"; a default export comes as "=./util"). The files are read a little at
 * each call (sym_busy). */
typedef void (*HdrFn)(void *ctx, const char *name, int kind, const char *sig, const char *ret,
                      const char *spec);
void sym_project_symbols(SymCache *c, Doc *const *docs, size_t ndocs, Doc *cur, HdrFn fn, void *ctx);

/* PHP: the classes of the project that can be imported with "use": config/app.php aliases, then
 * Composer's classmap, then the PSR-4 directories of the project. A class is reported with its
 * qualified name when its last segment starts with prefix (case ignored), or, if full is set,
 * when the qualified name does. rank: 0 the last segment is the prefix, 1 it starts with it,
 * 2 only the namespace matches. */
typedef void (*ClassFn)(void *ctx, const char *fq, int rank);
void sym_classes(SymCache *c, Doc *cur, const char *prefix, size_t plen, int full, ClassFn fn,
                 void *ctx);

/* PHP: the qualified name of the class known as name (an alias of config/app.php, else a class
 * of the project or of vendor with that last segment, the project's first). Returns 1 if found. */
int sym_class_fq(SymCache *c, Doc *cur, const char *name, char *out, size_t n);

/* Go: the import path ("example.com/app/store") of the package of cur's module called name, the
 * one cur already imports if two have that name; not cur's own. Returns 1 if there is one. */
int sym_go_package(SymCache *c, Doc *cur, const char *name, char *out, size_t n);

/* Java and C#: the import or using spec of the project's class called name ("com.acme.Foo",
 * "Acme.Core"), the one that would come with it in sym_project_symbols. Returns 1 if one is
 * needed. */
int sym_project_class(SymCache *c, Doc *const *docs, size_t ndocs, Doc *cur, const char *name, char *out,
                      size_t n);

/* C#: 1 if namespace wanted needs no using in cur: the file's namespace holds it, or the project
 * gives it (implicit usings of the .csproj, <Using Include>, global using). */
int sym_cs_using_known(SymCache *c, Doc *cur, const char *wanted);

/* The type name (class) of the expression before the operator at (y, sep), "" if unknown;
 * writes at most n bytes. Used by the tests. */
int sym_type_before(SymCache *c, Doc *const *docs, size_t ndocs, Doc *cur, size_t y, size_t sep,
                    char *out, size_t n);


/* ---- calls, parameters and local variables ------------------------------ */

/* The parenthesis that is open at a position: a call "foo(a, |" or the parameters of a
 * definition. */
typedef struct {
    char name[PM_NAME];     /* what precedes the parenthesis ("" for a plain one) */
    int arg;                /* index of the argument at the position (commas at its level) */
    int decl;               /* the parameters of a definition (function, def, "int foo(") */
    int typepos;            /* declaration, and the word at the position is where a type goes */
    int isnew;              /* "new Name(" */
    size_t y, x;            /* the parenthesis */
    size_t name_x;          /* where the name starts on line y */
} SymCall;

/* Looks back (at most 40 lines, outside strings and comments) from byte x of line y for the
 * unclosed "(". Returns 1 and fills o if there is one, else 0. */
int sym_call_at(Syntax *syn, const Buffer *b, size_t y, size_t x, SymCall *o);
/* Where parameter number arg of the list sig starts and ends (variadic ones take the rest). */
int sym_param_span(const char *sig, int arg, size_t *from, size_t *to);
/* For a call to a known function or method, "name(params)" in out and the byte range of the
 * current parameter (both 0 if none). Returns 1 if the callee was found. */
int sym_call_hint(SymCache *c, Doc *const *docs, size_t ndocs, Doc *cur, size_t y, size_t x,
                  char *out, size_t cap, size_t *hl_from, size_t *hl_to);
/* The parameters of the function around line y and the variables it declares above that point
 * (only up to byte xlimit of line y), reported as RULE_VAR with no signature. */
void sym_locals(SymCache *c, Doc *cur, size_t y, size_t xlimit, SymFn fn, void *ctx);

/* The definitions of the document (classes, functions, methods, properties, constants) in file
 * order, as kind (RULE_*), name (members qualified by their class), parameters, and where the
 * name is (line, byte). Members that only a doc comment declares are left out. Returns how
 * many. */
typedef void (*OutlineFn)(void *ctx, int kind, const char *name, const char *sig, size_t y,
                          size_t x);
int sym_outline(SymCache *c, Doc *cur, OutlineFn fn, void *ctx);

/* ---- environment variables -------------------------------------------- */

/* The names the .env files define (.env, .env.local, .env.example ...) in the folder of cur and
 * in those above it up to the project's root, nearest first, each once with its file's name. */
typedef void (*EnvFn)(void *ctx, const char *name, const char *file);
void sym_env_keys(SymCache *c, Doc *cur, EnvFn fn, void *ctx);

#endif
