#include "util.h"
#include "term.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void die(const char *fmt, ...)
{
    va_list ap;

    term_restore();
    fputs("ked: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);

    if (!p)
        die("memoria esaurita");
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        die("memoria esaurita");
    return p;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;

    return memcpy(xmalloc(n), s, n);
}

void sbuf_free(SBuf *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

void sbuf_reserve(SBuf *b, size_t extra)
{
    size_t nc;

    if (b->len + extra <= b->cap)
        return;
    nc = b->cap ? b->cap : 256;
    while (nc < b->len + extra)
        nc *= 2;
    b->p = xrealloc(b->p, nc);
    b->cap = nc;
}

void sbuf_add(SBuf *b, const char *s, size_t n)
{
    if (n == 0)
        return;
    sbuf_reserve(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

void sbuf_adds(SBuf *b, const char *s)
{
    sbuf_add(b, s, strlen(s));
}

/* Formats in place and retries once with the exact size; a single
 * vsnprintf(NULL, 0, ...) probe trips -Wformat-truncation in GCC. */
void sbuf_addf(SBuf *b, const char *fmt, ...)
{
    va_list ap, ap2;
    int n;

    sbuf_reserve(b, 64);
    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n >= b->cap - b->len) {
        sbuf_reserve(b, (size_t)n + 1);
        n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap2);
    }
    va_end(ap2);
    if (n > 0)
        b->len += (size_t)n;
}
