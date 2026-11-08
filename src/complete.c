#include "complete.h"
#include "fuzzy.h"
#include "snippet.h"
#include "utf8.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

#define MAX_RAW 2048

typedef struct {
    char *w;
    int kind, score;
    size_t dist;            /* lines from the cursor; documents other than the current one: far */
} Raw;

typedef struct {
    Raw *v;
    size_t n;
    char **set;             /* every distinct word seen, to count each once */
    size_t setsize, setn;
    char prefix[256];
    size_t plen;
} Gather;

static int word_cp(const char *s, size_t len, size_t i, size_t *n)
{
    uint32_t cp;

    *n = (size_t)utf8_decode(s + i, len - i, &cp);
    return cp_is_word(cp);
}

size_t complete_prefix(const Buffer *b, size_t y, size_t x, size_t *start)
{
    const Line *l = &b->lines[y];
    size_t s = x, n;

    while (s > 0) {
        size_t p = utf8_prev(l->s, s);

        if (!word_cp(l->s, l->len, p, &n))
            break;
        s = p;
    }
    *start = s;
    return x - s;
}

/* True if w was not in the set; adds it. */
static int set_add(Gather *g, const char *w, size_t n)
{
    size_t h = 2166136261u, i;
    char *copy;

    for (i = 0; i < n; i++)
        h = (h ^ (unsigned char)w[i]) * 16777619u;
    h &= g->setsize - 1;
    while (g->set[h]) {
        if (strlen(g->set[h]) == n && memcmp(g->set[h], w, n) == 0)
            return 0;
        h = (h + 1) & (g->setsize - 1);
    }
    if (g->setn * 2 >= g->setsize)
        return 0;           /* full: ignore further words rather than grow without bound */
    copy = xmalloc(n + 1);
    memcpy(copy, w, n);
    copy[n] = '\0';
    g->set[h] = copy;
    g->setn++;
    return 1;
}

static int first_char_ok(const Gather *g, const char *w)
{
    char a = w[0], b = g->prefix[0];

    if (a >= 'A' && a <= 'Z')
        a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z')
        b = (char)(b + 32);
    return a == b;
}

static void consider(Gather *g, const char *w, size_t n, int kind, size_t dist)
{
    char tmp[256];
    int score;

    if (n < g->plen || (n == g->plen && kind != CAND_SNIPPET) || n >= sizeof tmp ||
        g->n >= MAX_RAW)
        return;
    memcpy(tmp, w, n);
    tmp[n] = '\0';
    if (!first_char_ok(g, tmp))
        return;
    score = fuzzy_score(g->prefix, tmp);
    if (score < 0 || !set_add(g, w, n))
        return;
    g->v[g->n].w = xstrdup(tmp);
    g->v[g->n].kind = kind;
    g->v[g->n].score = score;
    g->v[g->n].dist = dist;
    g->n++;
}

static void scan_line(Gather *g, const Line *l, size_t dist, int is_cursor_line, size_t skip_x)
{
    size_t i = 0;

    while (i < l->len) {
        size_t n, start;

        if (!word_cp(l->s, l->len, i, &n)) {
            i += n;
            continue;
        }
        start = i;
        while (i < l->len && word_cp(l->s, l->len, i, &n))
            i += n;
        /* the word being typed is not a suggestion for itself */
        if (is_cursor_line && start == skip_x)
            continue;
        consider(g, l->s + start, i - start, CAND_WORD, dist);
    }
}

static void kw_cb(const char *w, size_t n, int cls, void *ctx)
{
    (void)cls;
    consider(ctx, w, n, CAND_KEYWORD, 0);
}

static int raw_cmp(const void *pa, const void *pb)
{
    const Raw *a = pa, *b = pb;

    if (a->score != b->score)
        return b->score - a->score;
    if (a->dist != b->dist)
        return a->dist < b->dist ? -1 : 1;
    return strcmp(a->w, b->w);
}

void complete_close(Complete *c)
{
    size_t i;

    for (i = 0; i < c->n; i++)
        free(c->v[i].text);
    c->n = 0;
    c->on = 0;
    c->picked = 0;
    c->sel = c->top = 0;
}

void complete_free(Complete *c)
{
    complete_close(c);
    free(c->v);
    memset(c, 0, sizeof *c);
}

int complete_open(Complete *c, Doc *const *docs, size_t ndocs, const Doc *cur, size_t y,
                  size_t x)
{
    Gather g;
    const Buffer *b = &cur->buf;
    size_t start, plen, i, budget = COMPLETE_SCAN_LINES, d, take;

    complete_close(c);
    plen = complete_prefix(b, y, x, &start);
    if (plen < COMPLETE_MIN_PREFIX || plen >= sizeof g.prefix)
        return 0;
    memset(&g, 0, sizeof g);
    memcpy(g.prefix, b->lines[y].s + start, plen);
    g.prefix[plen] = '\0';
    g.plen = plen;
    g.v = xmalloc(MAX_RAW * sizeof *g.v);
    g.setsize = 8192;
    g.set = xmalloc(g.setsize * sizeof *g.set);
    memset(g.set, 0, g.setsize * sizeof *g.set);

    {
        size_t ns, k;
        const SnipDef *sd = snip_lang(cur->syn ? syn_name(cur->syn) : NULL, &ns);

        /* snippets first: a snippet named like a keyword wins */
        for (k = 0; k < ns; k++)
            consider(&g, sd[k].name, strlen(sd[k].name), CAND_SNIPPET, 0);
    }
    syn_each_word(cur->syn, kw_cb, &g);
    /* the current document outward from the cursor line */
    for (d = 0; budget > 0; d++) {
        int more = 0;

        if (d <= y) {
            scan_line(&g, &b->lines[y - d], d, d == 0, start);
            budget--;
            more = 1;
        }
        if (d > 0 && y + d < b->nlines && budget > 0) {
            scan_line(&g, &b->lines[y + d], d, 0, 0);
            budget--;
            more = 1;
        }
        if (!more)
            break;
    }
    for (i = 0; i < ndocs && budget > 0; i++) {
        size_t k;

        if (docs[i] == cur)
            continue;
        for (k = 0; k < docs[i]->buf.nlines && budget > 0; k++, budget--)
            scan_line(&g, &docs[i]->buf.lines[k], (size_t)1 << 30, 0, 0);
    }

    qsort(g.v, g.n, sizeof *g.v, raw_cmp);
    take = g.n;
    if (take > 64)
        take = 64;
    for (i = 0; i < take; i++) {
        if (c->n == c->cap) {
            c->cap = c->cap ? c->cap * 2 : 16;
            c->v = xrealloc(c->v, c->cap * sizeof *c->v);
        }
        c->v[c->n].text = g.v[i].w;
        c->v[c->n].kind = g.v[i].kind;
        c->n++;
    }
    for (i = take; i < g.n; i++)
        free(g.v[i].w);
    for (i = 0; i < g.setsize; i++)
        free(g.set[i]);
    free(g.set);
    free(g.v);
    c->y = y;
    c->x = start;
    c->on = c->n > 0;
    return c->on;
}

void complete_move(Complete *c, long delta)
{
    long s;

    if (c->n == 0)
        return;
    s = (long)c->sel + delta;
    if (s < 0)
        s = (long)c->n - 1;
    if (s >= (long)c->n)
        s = 0;
    c->sel = (size_t)s;
    c->picked = 1;
    if (c->sel < c->top)
        c->top = c->sel;
    if (c->sel >= c->top + COMPLETE_ROWS)
        c->top = c->sel - COMPLETE_ROWS + 1;
}
