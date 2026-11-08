#include "clip.h"

#include <stdlib.h>

void clip_free(Clip *c)
{
    size_t i;

    for (i = 0; i < c->count; i++)
        free(c->e[i].text);
    c->count = 0;
}

void clip_push(Clip *c, char *text, size_t n, int whole_line)
{
    size_t i;

    if (c->count == CLIP_MAX) {
        free(c->e[CLIP_MAX - 1].text);
        c->count--;
    }
    for (i = c->count; i > 0; i--)
        c->e[i] = c->e[i - 1];
    c->e[0].text = text;
    c->e[0].n = n;
    c->e[0].whole_line = whole_line;
    c->count++;
}

const ClipEntry *clip_get(const Clip *c, size_t i)
{
    return i < c->count ? &c->e[i] : NULL;
}

size_t base64_encode(const char *in, size_t n, char *out)
{
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *u = (const unsigned char *)in;
    size_t i, o = 0;

    for (i = 0; i + 2 < n; i += 3) {
        unsigned v = (unsigned)u[i] << 16 | (unsigned)u[i + 1] << 8 | u[i + 2];

        out[o++] = tab[v >> 18];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = tab[(v >> 6) & 63];
        out[o++] = tab[v & 63];
    }
    if (n - i == 1) {
        unsigned v = (unsigned)u[i] << 16;

        out[o++] = tab[v >> 18];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (n - i == 2) {
        unsigned v = (unsigned)u[i] << 16 | (unsigned)u[i + 1] << 8;

        out[o++] = tab[v >> 18];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = tab[(v >> 6) & 63];
        out[o++] = '=';
    }
    return o;
}

int clip_osc52(SBuf *out, const char *text, size_t n)
{
    if (n > OSC52_MAX)
        return -1;
    sbuf_adds(out, "\x1b]52;c;");
    sbuf_reserve(out, 4 * ((n + 2) / 3));
    out->len += base64_encode(text, n, out->p + out->len);
    sbuf_adds(out, "\x07");
    return 0;
}
