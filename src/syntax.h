#ifndef XTED_SYNTAX_H
#define XTED_SYNTAX_H

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
/* Symbol rules of the language (directives decl/include/bind/attr, see langs.c and symbols.c).
 * pat is the pattern text; for RULE_CLASS..RULE_NS it describes a definition. */
enum { RULE_CLASS, RULE_FUNC, RULE_METHOD, RULE_PROP, RULE_CONST, RULE_VAR, RULE_NS,
       RULE_INCLUDE, RULE_BIND, RULE_ATTR, RULE_ALIAS, RULE_DOC,
       RULE_RECV };            /* decl recv: a method written outside its type (Go), % the type */
typedef struct {
    int kind;
    const char *pat;
} SynRule;
const SynRule *syn_rules(const Syntax *s, size_t *n);
size_t syn_rule_count(const Syntax *s);
/* 1 if blocks are marked by indentation (Python), 0 for braces. */
int syn_indent_scope(const Syntax *s);
/* The operators that open a member list ("-> ::" for PHP), blank separated; NULL if none. */
const char *syn_member_seps(const Syntax *s);
/* Calls fn for every keyword/type/constant of the language. */
void syn_each_word(const Syntax *s, void (*fn)(const char *w, size_t n, int cls, void *ctx),
                   void *ctx);

/* Adds a language written in the format of langs.c, searched before the built-in ones (the
 * same name replaces one). Returns 0, or -1 with a message in err. */
int syn_add_user(const char *text, size_t n, char *err, size_t errlen);
/* Adds every file of dir (in alphabetical order); a bad file is skipped and reported through
 * report(). Returns how many were added. */
size_t syn_load_dir(const char *dir, void (*report)(const char *msg, void *ctx), void *ctx);
void syn_clear_user(void);

/* Picks the language for a file from its name, first line (shebang) and
 * modelines in the first/last 5 lines. NULL means plain text. */
Syntax *syn_detect(const Buffer *b);

/* Classifies one line: cls[i] receives the class of byte i (cls may be NULL
 * when only the state is wanted). Returns the state at the end of the line:
 * 0, 1 + the index of the multi-line region left open, or (bit 15) a state inside an
 * embedded language, see the "embed" directive in langs.c. */
uint16_t syn_line(Syntax *s, uint16_t state_in, const char *text, size_t len, uint8_t *cls);

/* Comment markers for line y: those of the embedded language when the line starts inside one
 * (line comments inside <script>, block comments inside <style>), else the document's.
 * Otherwise the same as syn_comment. */
int syn_comment_at(Syntax *s, Buffer *b, size_t y, const char **open, const char **close);

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
