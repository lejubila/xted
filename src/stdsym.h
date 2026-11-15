#ifndef XTED_STDSYM_H
#define XTED_STDSYM_H

#include <stddef.h>

/* The standard library of a language as symbols: global functions, constants and classes (or
 * modules) with their members, compressed in the binary and unpacked on first use.
 * Entries come from the lists in tools/lib, packed by tools/packlib.py. */
typedef struct StdLib StdLib;

enum { STD_FUNC, STD_VAR, STD_CONST, STD_CLASS };

/* id: "php", "c", "cpp", "py" or "js". NULL for an unknown id. */
StdLib *std_open(const char *id);
void std_close(StdLib *l);

/* name/params/ret are not NUL-terminated; params and ret may be empty. */
typedef void (*StdFn)(void *ctx, const char *name, size_t nl, const char *params, size_t pl,
                      const char *ret, size_t rl, int kind);

/* Global functions and constants, and every class by name. */
void std_globals(const StdLib *l, StdFn fn, void *ctx);
/* The header that declares a global function, constant or type of the C library (the lines
 * "@stdio.h" of the list), NULL if the list has none for it; *hl is its length. */
const char *std_header(const StdLib *l, const char *name, size_t nl, size_t *hl);
/* 1 if a class (or module) of that name exists. */
int std_class(const StdLib *l, const char *cls, size_t cl);
/* Members of a class followed by those of its base classes. */
void std_members(const StdLib *l, const char *cls, size_t cl, StdFn fn, void *ctx);
/* The base class of cls (NULL if none), with its length. */
const char *std_base(const StdLib *l, const char *cls, size_t cl, size_t *bl);

#endif
