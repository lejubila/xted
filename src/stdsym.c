#include "stdsym.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

#include "stdsym_data.inc"

typedef struct {
    size_t name, nl;        /* offsets into text */
    size_t base, bl;
    size_t from, to;        /* the section's entries: text[from, to) */
} Sect;

struct StdLib {
    char *text;
    size_t len;
    Sect *sec;
    size_t nsec;
};

/* Unpacks the format described in tools/packlib.py; NULL if the data is corrupt. */
static char *unpack(const unsigned char *z, size_t zn, size_t *outlen)
{
    size_t n, o = 0, i = 4;
    char *out;

    if (zn < 4)
        return NULL;
    n = (size_t)z[0] | (size_t)z[1] << 8 | (size_t)z[2] << 16 | (size_t)z[3] << 24;
    out = xmalloc(n + 1);
    while (i < zn && o < n) {
        unsigned c = z[i++];

        if (c < 128) {
            size_t k = c + 1;

            if (i + k > zn || o + k > n)
                goto bad;
            memcpy(out + o, z + i, k);
            i += k;
            o += k;
        } else {
            size_t len = (c & 127) + 4, off, k;

            if (i + 2 > zn)
                goto bad;
            off = (size_t)z[i] | (size_t)z[i + 1] << 8;
            i += 2;
            if (off == 0 || off > o || o + len > n)
                goto bad;
            for (k = 0; k < len; k++, o++)
                out[o] = out[o - off];      /* may overlap: byte by byte */
        }
    }
    if (o != n)
        goto bad;
    out[n] = '\0';
    *outlen = n;
    return out;
bad:
    free(out);
    return NULL;
}

static void index_sections(StdLib *l)
{
    size_t i = 0, cap = 0;
    int open = 0;

    /* text starts with "#..." (the global section) */
    while (i < l->len) {
        size_t e = i;
        const char *line = l->text + i;

        while (e < l->len && l->text[e] != '\n')
            e++;
        if (line[0] == '#') {
            Sect *s;
            size_t k;

            if (open)
                l->sec[l->nsec - 1].to = i;
            if (l->nsec == cap) {
                cap = cap ? cap * 2 : 64;
                l->sec = xrealloc(l->sec, cap * sizeof *l->sec);
            }
            s = &l->sec[l->nsec++];
            memset(s, 0, sizeof *s);
            s->name = i + 1;
            for (k = 1; k < e - i && line[k] != ':'; k++)
                ;
            s->nl = k - 1;
            if (k < e - i) {
                s->base = i + k + 1;
                s->bl = e - i - k - 1;
            }
            s->from = e + 1 < l->len ? e + 1 : l->len;
            s->to = l->len;
            open = 1;
        }
        i = e + 1;
    }
}

StdLib *std_open(const char *id)
{
    const unsigned char *z = NULL;
    size_t zn = 0;
    StdLib *l;

    if (strcmp(id, "php") == 0) {
        z = std_php;
        zn = sizeof std_php;
    } else if (strcmp(id, "c") == 0) {
        z = std_c;
        zn = sizeof std_c;
    } else if (strcmp(id, "cpp") == 0) {
        z = std_cpp;
        zn = sizeof std_cpp;
    } else if (strcmp(id, "py") == 0) {
        z = std_py;
        zn = sizeof std_py;
    } else if (strcmp(id, "js") == 0) {
        z = std_js;
        zn = sizeof std_js;
    } else {
        return NULL;
    }
    l = xmalloc(sizeof *l);
    memset(l, 0, sizeof *l);
    l->text = unpack(z, zn, &l->len);
    if (!l->text) {
        free(l);
        return NULL;
    }
    index_sections(l);
    return l;
}

void std_close(StdLib *l)
{
    if (!l)
        return;
    free(l->text);
    free(l->sec);
    free(l);
}

/* Splits one entry line and calls fn. */
static void entry(const StdLib *l, const char *ln, size_t n, StdFn fn, void *ctx)
{
    const char *colon, *open, *ret = "", *rp = NULL;
    size_t nl, pl = 0, rl = 0, k;
    const char *params = "";
    int kind;

    (void)l;
    for (k = 0; k < n && ln[k] != '(' && ln[k] != ':'; k++)
        ;
    nl = k;
    if (nl == 0)
        return;
    if (k < n && ln[k] == ':') {
        colon = ln + k;
        ret = colon + 1;
        rl = n - k - 1;
        kind = STD_VAR;
        for (k = 0; k < nl; k++)
            if ((ln[k] < 'A' || ln[k] > 'Z') && ln[k] != '_' && (ln[k] < '0' || ln[k] > '9'))
                break;
        if (k == nl)
            kind = STD_CONST;
        fn(ctx, ln, nl, "", 0, ret, rl, kind);
        return;
    }
    kind = STD_FUNC;
    if (k < n) {
        size_t j, close = n;

        open = ln + k;
        for (j = n; j > k + 1; j--)
            if (ln[j - 1] == '>' && ln[j - 2] == ')') {
                rp = ln + j;
                close = j - 2;
                break;
            }
        if (!rp) {
            for (j = n; j > k + 1 && ln[j - 1] != ')'; j--)
                ;
            close = j > k + 1 ? j - 1 : n;
        }
        params = open + 1;
        pl = close > k + 1 ? close - k - 1 : 0;
        if (rp) {
            ret = rp;
            rl = (size_t)(ln + n - rp);
        }
    }
    fn(ctx, ln, nl, params, pl, ret, rl, kind);
}

static void each_entry(const StdLib *l, const Sect *s, StdFn fn, void *ctx)
{
    size_t i = s->from;

    while (i < s->to) {
        size_t e = i;

        while (e < s->to && l->text[e] != '\n')
            e++;
        if (e > i && l->text[i] != '#')
            entry(l, l->text + i, e - i, fn, ctx);
        i = e + 1;
    }
}

void std_globals(const StdLib *l, StdFn fn, void *ctx)
{
    size_t i;

    for (i = 0; i < l->nsec; i++) {
        const Sect *s = &l->sec[i];

        if (s->nl == 0)
            each_entry(l, s, fn, ctx);
        else
            fn(ctx, l->text + s->name, s->nl, "", 0, "", 0, STD_CLASS);
    }
}

static const Sect *find_sect(const StdLib *l, const char *cls, size_t cl)
{
    size_t i;

    for (i = 0; i < l->nsec; i++)
        if (l->sec[i].nl == cl && cl > 0 && memcmp(l->text + l->sec[i].name, cls, cl) == 0)
            return &l->sec[i];
    return NULL;
}

int std_class(const StdLib *l, const char *cls, size_t cl)
{
    return find_sect(l, cls, cl) != NULL;
}

const char *std_base(const StdLib *l, const char *cls, size_t cl, size_t *bl)
{
    const Sect *s = find_sect(l, cls, cl);

    if (!s || s->bl == 0)
        return NULL;
    *bl = s->bl;
    return l->text + s->base;
}

void std_members(const StdLib *l, const char *cls, size_t cl, StdFn fn, void *ctx)
{
    int depth;

    for (depth = 0; depth < 8 && cl > 0; depth++) {
        const Sect *s = find_sect(l, cls, cl);

        if (!s)
            return;
        each_entry(l, s, fn, ctx);
        cls = l->text + s->base;
        cl = s->bl;
    }
}
