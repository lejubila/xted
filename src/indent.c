#include "indent.h"

#define SCAN_LINES 2000

int indent_detect(const Buffer *b, int *tabs, int *width)
{
    size_t y, ntab = 0, nsp = 0;
    size_t hist[9] = { 0 };
    size_t prev = 0, least = 0;
    size_t i, best = 0;

    for (y = 0; y < b->nlines && y < SCAN_LINES; y++) {
        const Line *l = &b->lines[y];
        size_t n = 0;

        while (n < l->len && l->s[n] == ' ')
            n++;
        if (n == l->len)
            continue;                   /* empty or blank */
        if (l->s[0] == '\t') {
            ntab++;
            prev = 0;
            continue;
        }
        /* " * text" inside a block comment is aligned with the star, not indented */
        if (n % 2 == 1 && l->s[n] == '*')
            continue;
        if (n > 0) {
            nsp++;
            if (least == 0 || n < least)
                least = n;
            if (n > prev && n - prev <= 8)
                hist[n - prev]++;
        }
        prev = n;
    }
    if (ntab + nsp < 2)
        return 0;
    if (ntab > nsp) {
        *tabs = 1;
        *width = 0;
        return 1;
    }
    for (i = 2; i <= 8; i++)
        if (hist[i] > hist[best] || (best == 0 && hist[i] > 0))
            best = i;
    /* a step of 1 is an alignment more often than an indentation */
    if (best == 0 && hist[1] > 0)
        best = 1;
    if (best == 0)
        best = least <= 8 ? least : 0;
    if (best == 0)
        return 0;
    *tabs = 0;
    *width = (int)best;
    return 1;
}
