#ifndef XTED_LANGS_H
#define XTED_LANGS_H

#include <stddef.h>

/* A built-in language: its definition as an array of lines, NULL-terminated. */
typedef struct {
    const char *name;
    const char *const *def;
} LangDef;

extern const LangDef lang_defs[];
extern const size_t lang_ndefs;

#endif
