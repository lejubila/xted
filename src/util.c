#include "util.h"
#include "i18n.h"
#include "term.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void die(const char *fmt, ...)
{
    va_list ap;

    term_restore();
    fputs("xted: ", stderr);
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
        die(tr("out of memory"));
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        die(tr("out of memory"));
    return p;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;

    return memcpy(xmalloc(n), s, n);
}

char *xstrndup(const char *s, size_t n)
{
    char *p;

    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        n--;
    p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void sbuf_free(SBuf *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

char *sbuf_steal(SBuf *b)
{
    char *p = b->p;

    b->p = NULL;
    b->len = b->cap = 0;
    return p;
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

char *app_dir(const char *base)
{
    size_t n = strlen(base) + 8;
    char *d = xmalloc(n), *old = xmalloc(n);
    struct stat st;

    snprintf(d, n, "%s/xted", base);
    snprintf(old, n, "%s/ked", base);
    if (stat(d, &st) != 0 && stat(old, &st) == 0 && S_ISDIR(st.st_mode)) {
        free(d);
        return old;
    }
    free(old);
    return d;
}
