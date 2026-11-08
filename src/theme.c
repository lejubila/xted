#include "theme.h"

#include <stdlib.h>
#include <string.h>

static const Theme themes[] = {
    {
        "scuro", 0, { 212, 212, 212 }, { 0, 0, 0 },
        {
            /* normal */   { 212, 212, 212 },
            /* comment */  { 106, 153, 85 },
            /* string */   { 206, 145, 120 },
            /* keyword */  { 197, 134, 192 },
            /* type */     { 78, 201, 176 },
            /* constant */ { 86, 156, 214 },
            /* number */   { 181, 206, 168 },
            /* preproc */  { 155, 155, 155 },
            /* function */ { 220, 220, 170 },
            /* operator */ { 212, 212, 212 },
            /* variable */ { 156, 220, 254 },
            /* heading */  { 86, 156, 214 },
            /* emphasis */ { 220, 160, 90 },
            /* code */     { 206, 145, 120 },
            /* link */     { 79, 193, 255 },
        }
    },
    {
        "chiaro", 1, { 56, 58, 66 }, { 250, 250, 250 },
        {
            /* normal */   { 56, 58, 66 },
            /* comment */  { 110, 130, 100 },
            /* string */   { 163, 21, 21 },
            /* keyword */  { 175, 0, 219 },
            /* type */     { 38, 127, 153 },
            /* constant */ { 0, 0, 255 },
            /* number */   { 9, 134, 88 },
            /* preproc */  { 110, 110, 110 },
            /* function */ { 121, 94, 38 },
            /* operator */ { 56, 58, 66 },
            /* variable */ { 0, 16, 128 },
            /* heading */  { 0, 0, 255 },
            /* emphasis */ { 175, 90, 0 },
            /* code */     { 163, 21, 21 },
            /* link */     { 4, 81, 165 },
        }
    },
};

size_t theme_count(void)
{
    return sizeof themes / sizeof themes[0];
}

const Theme *theme_get(size_t i)
{
    return &themes[i % theme_count()];
}

int theme_detect_depth(void)
{
    const char *nc = getenv("NO_COLOR"), *ct = getenv("COLORTERM"), *t = getenv("TERM");

    if (nc && nc[0])
        return COLOR_NONE;
    if (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit")))
        return COLOR_TRUE;
    if (!t || strcmp(t, "dumb") == 0)
        return COLOR_NONE;
    if (strstr(t, "256"))
        return COLOR_256;
    return COLOR_16;
}

static int sq(int a)
{
    return a * a;
}

static int level(int v)
{
    static const int steps[6] = { 0, 95, 135, 175, 215, 255 };
    int i, best = 0;

    for (i = 1; i < 6; i++)
        if (abs(v - steps[i]) < abs(v - steps[best]))
            best = i;
    return best;
}

int theme_to_256(Rgb c)
{
    static const int steps[6] = { 0, 95, 135, 175, 215, 255 };
    int ri = level(c.r), gi = level(c.g), bi = level(c.b);
    int cube = 16 + 36 * ri + 6 * gi + bi;
    int avg = (c.r + c.g + c.b) / 3;
    int gi2 = (avg - 8 + 5) / 10, gray, dc, dg;

    if (gi2 < 0)
        gi2 = 0;
    if (gi2 > 23)
        gi2 = 23;
    gray = 8 + 10 * gi2;
    dc = sq(c.r - steps[ri]) + sq(c.g - steps[gi]) + sq(c.b - steps[bi]);
    dg = sq(c.r - gray) + sq(c.g - gray) + sq(c.b - gray);
    return dg < dc ? 232 + gi2 : cube;
}

int theme_to_16(Rgb c)
{
    /* xterm's palette in SGR order: 0-7 normal, 8-15 bright */
    static const Rgb pal[16] = {
        { 0, 0, 0 }, { 205, 0, 0 }, { 0, 205, 0 }, { 205, 205, 0 },
        { 0, 0, 238 }, { 205, 0, 205 }, { 0, 205, 205 }, { 229, 229, 229 },
        { 127, 127, 127 }, { 255, 0, 0 }, { 0, 255, 0 }, { 255, 255, 0 },
        { 92, 92, 255 }, { 255, 0, 255 }, { 0, 255, 255 }, { 255, 255, 255 },
    };
    int i, best = 0, bd = 1 << 30;

    for (i = 0; i < 16; i++) {
        int d = sq(c.r - pal[i].r) + sq(c.g - pal[i].g) + sq(c.b - pal[i].b);

        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

static void color(SBuf *o, int depth, Rgb c, int base)
{
    switch (depth) {
    case COLOR_TRUE:
        sbuf_addf(o, "\x1b[%d;2;%d;%d;%dm", base + 8, c.r, c.g, c.b);
        break;
    case COLOR_256:
        sbuf_addf(o, "\x1b[%d;5;%dm", base + 8, theme_to_256(c));
        break;
    case COLOR_16: {
        int i = theme_to_16(c);

        sbuf_addf(o, "\x1b[%dm", (i < 8 ? base : base + 60) + (i & 7));
        break;
    }
    default:
        break;
    }
}

void theme_fg(SBuf *o, int depth, Rgb c)
{
    color(o, depth, c, 30);
}

void theme_bg(SBuf *o, int depth, Rgb c)
{
    color(o, depth, c, 40);
}
