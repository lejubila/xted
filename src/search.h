#ifndef XTED_SEARCH_H
#define XTED_SEARCH_H

#include "buffer.h"
#include "util.h"

#include <stddef.h>
#include <stdint.h>

/* XTED_NO_REGEX (the Windows build): no regular expressions, only plain text; the groups keep
 * the shape of POSIX's for the callers */
#ifndef XTED_NO_REGEX
#include <regex.h>
#else
typedef long regoff_t;
typedef struct { regoff_t rm_so, rm_eo; } regmatch_t;
#endif

/* A compiled search pattern: plain text or an extended regular expression
 * (POSIX ERE, matched one line at a time). Matches are never empty. */
typedef struct {
    int valid;
    int regex, cs, word;    /* regular expression, match case, whole words only */
    uint32_t *raw, *fold;   /* plain text: the pattern's code points, as is and lower-cased */
    size_t ncp;
#ifndef XTED_NO_REGEX
    regex_t re;
#endif
    int has_re;
    char *line;             /* scratch: NUL-terminated copy of the line being searched */
    size_t linecap;
    char err[128];
} Matcher;

/* Returns 0 on success; -1 with m->err set (empty for an empty pattern) and m left unusable. */
int sm_compile(Matcher *m, const char *pat, size_t n, int regex, int cs, int word);
void sm_free(Matcher *m);

/* First match that starts at or after byte `from`. grp (10 entries, optional)
 * receives the groups of a regex match; grp[0] is always the whole match. */
int sm_find(Matcher *m, const char *s, size_t len, size_t from, size_t *ms, size_t *me,
            regmatch_t *grp);
/* Last match that starts before byte `before`. */
int sm_find_last(Matcher *m, const char *s, size_t len, size_t before, size_t *ms, size_t *me);
/* All matches of a line as [start, end) pairs; returns how many (at most max). */
size_t sm_line_matches(Matcher *m, const char *s, size_t len, size_t (*rng)[2], size_t max);

/* ---- in a buffer ---- */

typedef struct {
    size_t y0, x0, y1, x1;  /* the part of the text searched */
} Span;

typedef struct {
    size_t y, x, end;       /* line, start and end byte of a match */
} Hit;

/* Next match at or after (y,x) going forward, or the last one that starts
 * before (y,x) going backward. With wrap it continues from the other end of
 * the text; *wrapped says whether it had to. lim, if given, restricts the search. */
int search_next(Matcher *m, const Buffer *b, size_t y, size_t x, int forward, int wrap,
                const Span *lim, Hit *out, int *wrapped);
/* Number of matches (at most cap) and how many start before (y,x). */
void search_stats(Matcher *m, const Buffer *b, size_t y, size_t x, size_t cap, size_t *before,
                  size_t *total);

/* ---- replacing ---- */

/* Appends repl to out with \0..\9 (regex mode), \t and \\ expanded. */
void search_expand(const Matcher *m, const char *line, const regmatch_t *grp, const char *repl,
                   SBuf *out);
/* Replaces the match `h`; returns the offset just after the new text. */
size_t search_replace_at(Buffer *b, Matcher *m, const Hit *h, const char *repl);
/* Replaces every match in lim (or everywhere); lim->x1 follows the edits.
 * Returns the number of replacements. The caller opens the undo group. */
size_t search_replace_all(Buffer *b, Matcher *m, const char *repl, Span *lim);

/* ---- in files ---- */

typedef struct {
    char *path;             /* root joined with the file's path (or the buffer's own name) */
    size_t line, col;       /* 0-based line, byte offset of the match */
    char *text;             /* the line, trimmed and shortened for display */
} FileHit;

typedef struct {
    FileHit *v;
    size_t n, cap;
    int truncated;          /* more matches existed than max */
} FileHits;

#define FILE_MAX_BYTES (20L * 1024 * 1024)

/* Adds the matches of a block of text (with '\n' or "\r\n" line ends). */
void search_text(Matcher *m, const char *path, const char *data, size_t len, FileHits *out,
                 size_t max);
void search_buffer(Matcher *m, const char *path, const Buffer *b, FileHits *out, size_t max);
/* Skips files for which skip(st_dev, st_ino, ctx) is true (already searched in memory). */
void search_dir(Matcher *m, const char *root, int (*skip)(unsigned long dev, unsigned long ino,
                                                          void *ctx),
                void *ctx, FileHits *out, size_t max);
void filehits_free(FileHits *h);

/* "12", "12:5" or ":5": a 1-based line (0 if absent) and column (0 if absent).
 * Returns -1 if the text is not of that form. */
int search_parse_goto(const char *s, size_t *line, size_t *col);

#endif
