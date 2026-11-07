#ifndef KED_UTIL_H
#define KED_UTIL_H

#include <stddef.h>

#ifdef __GNUC__
#define KED_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define KED_PRINTF(a, b)
#endif

void die(const char *fmt, ...) KED_PRINTF(1, 2);
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

/* Growable byte string. Not NUL-terminated. */
typedef struct {
    char *p;
    size_t len, cap;
} SBuf;

#define SBUF_INIT { NULL, 0, 0 }

void sbuf_free(SBuf *b);
void sbuf_reserve(SBuf *b, size_t extra);
void sbuf_add(SBuf *b, const char *s, size_t n);
void sbuf_adds(SBuf *b, const char *s);
void sbuf_addf(SBuf *b, const char *fmt, ...) KED_PRINTF(2, 3);

#endif
