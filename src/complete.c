#include "complete.h"
#include "fuzzy.h"
#include "snippet.h"
#include "utf8.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RAW 2048

typedef struct {
    char *w, *sig;
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
    int use_ctx;            /* after PHP's `use`: qualified names are the candidates */
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

static void consider_sig(Gather *g, const char *w, size_t n, int kind, size_t dist,
                         const char *sig)
{
    char tmp[256];
    int score;

    if (n < g->plen || (n == g->plen && kind != CAND_SNIPPET && !(kind == CAND_CLASS && sig)) ||
        n >= sizeof tmp ||
        g->n >= MAX_RAW)
        return;
    memcpy(tmp, w, n);
    tmp[n] = '\0';
    if (g->plen > 0 && !first_char_ok(g, tmp))
        return;
    score = fuzzy_score(g->prefix, tmp);
    if (score < 0 || !set_add(g, w, n))
        return;
    /* the standard library is big: it must not bury the file's own names and the keywords */
    if (kind >= CAND_FUNC && dist >= 2)
        score -= 8;
    g->v[g->n].w = xstrdup(tmp);
    g->v[g->n].sig = sig && sig[0] ? xstrdup(sig) : NULL;
    g->v[g->n].kind = kind;
    g->v[g->n].score = score;
    g->v[g->n].dist = dist;
    g->n++;
}

static void consider(Gather *g, const char *w, size_t n, int kind, size_t dist)
{
    consider_sig(g, w, n, kind, dist, NULL);
}

/* A symbol from the index or the library: "name(params): type", the kind decides the letter. */
static void sym_cb(void *ctx, const char *name, int kind, const char *sig, const char *ret,
                   int rank)
{
    char buf[PM_PARAMS + PM_NAME + 96];
    int ck;
    size_t n;
    const Gather *g = ctx;

    /* most of the library is rejected by the first letter: do not format it */
    if (g->plen > 0 && (strlen(name) < g->plen || !first_char_ok(g, name)))
        return;
    switch (kind) {
    case RULE_CLASS:
        ck = CAND_CLASS;
        break;
    case RULE_METHOD:
        ck = CAND_METHOD;
        break;
    case RULE_PROP:
    case RULE_VAR:
        ck = CAND_PROP;
        break;
    case RULE_CONST:
        ck = CAND_CONST;
        break;
    default:
        ck = CAND_FUNC;
        break;
    }
    n = (size_t)snprintf(buf, sizeof buf, "%s", name);
    if (ck == CAND_FUNC || ck == CAND_METHOD)
        n += (size_t)snprintf(buf + n, sizeof buf - n, "(%s)", sig);
    if (ck != CAND_CLASS && ret[0] && n < sizeof buf)
        snprintf(buf + n, sizeof buf - n, ": %s", ret);
    consider_sig(ctx, name, strlen(name), ck, (size_t)rank, ck == CAND_CLASS && rank < 2 ? NULL : buf);
}

/* A class of the project for the suggestions: after `use` the qualified name itself, elsewhere
 * the short name with the qualified one as its signature (so accepting it can add the use). */
static void class_cb(void *ctx, const char *fq, int rank)
{
    Gather *g = ctx;

    if (g->use_ctx) {
        if (g->n < MAX_RAW && set_add(g, fq, strlen(fq))) {
            g->v[g->n].w = xstrdup(fq);
            g->v[g->n].sig = NULL;
            g->v[g->n].kind = CAND_CLASS;
            /* the closer the last segment is to what was typed, the better; the project first */
            {
                const char *last = strrchr(fq, '\\');
                size_t extra = strlen(last ? last + 1 : fq);

                g->v[g->n].score = 100 - rank * 20 - (int)(extra > 15 ? 15 : extra) +
                                   (strncmp(fq, "App\\", 4) == 0 ? 4 : 0);
            }
            g->v[g->n].dist = 1;
            g->n++;
        }
    } else {
        const char *last = strrchr(fq, '\\');

        last = last ? last + 1 : fq;
        /* the same class already found in a file the document includes (`new Foo` without its
         * use) carries no qualified name: give it this one, so that accepting it adds the use */
        {
            size_t i, ll = strlen(last);

            for (i = 0; i < g->n; i++)
                if (g->v[i].kind == CAND_CLASS && !g->v[i].sig && g->v[i].dist >= 1 &&
                    strlen(g->v[i].w) == ll && memcmp(g->v[i].w, last, ll) == 0) {
                    g->v[i].sig = xstrdup(fq);
                    return;
                }
        }
        consider_sig(g, last, strlen(last), CAND_CLASS, 2, fq);
    }
}

/* True if the word at [start, x) of a PHP line follows `use` (maybe qualified: use A\B\C);
 * *qs is where the qualified name begins. */
static int use_context(const Buffer *b, size_t y, size_t start, size_t *qs)
{
    const Line *l = &b->lines[y];
    size_t s = start, k;

    while (s > 0 && (l->s[s - 1] == '\\' || (l->s[s - 1] & 0x80) || isalnum((unsigned char)l->s[s - 1]) ||
                     l->s[s - 1] == '_'))
        s--;
    k = s;
    while (k > 0 && l->s[k - 1] == ' ')
        k--;
    if (k == s || k < 3 || memcmp(l->s + k - 3, "use", 3) != 0)
        return 0;
    for (s = 0; s < k - 3; s++)
        if (l->s[s] != ' ' && l->s[s] != '\t')
            return 0;
    *qs = start;
    while (*qs > 0 && (l->s[*qs - 1] == '\\' || isalnum((unsigned char)l->s[*qs - 1]) ||
                       l->s[*qs - 1] == '_'))
        (*qs)--;
    return 1;
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

    for (i = 0; i < c->n; i++) {
        free(c->v[i].text);
        free(c->v[i].sig);
    }
    c->n = 0;
    c->on = 0;
    c->picked = 0;
    c->sel = c->top = 0;
    c->rows = 0;
    c->scanning = 0;
}

void complete_free(Complete *c)
{
    complete_close(c);
    free(c->v);
    sym_cache_free(c->sc);
    memset(c, 0, sizeof *c);
}

int complete_open(Complete *c, Doc *const *docs, size_t ndocs, Doc *cur, size_t y, size_t x)
{
    Gather g;
    const Buffer *b = &cur->buf;
    size_t start, plen, i, budget = COMPLETE_SCAN_LINES, d, take, sep = (size_t)-1, qs = 0;
    int php = cur->syn && strncmp(syn_name(cur->syn), "PHP", 3) == 0, use_ctx = 0;

    complete_close(c);
    plen = complete_prefix(b, y, x, &start);
    if (php && use_context(b, y, start, &qs)) {
        use_ctx = 1;
        start = qs;
        plen = x - qs;
    } else if (syn_rule_count(cur->syn) > 0) {
        sep = sym_member_at(cur->syn, b, y, start);
    }
    if (plen >= sizeof g.prefix ||
        (!use_ctx && sep == (size_t)-1 &&
         plen < (c->min_prefix ? c->min_prefix : COMPLETE_MIN_PREFIX)))
        return 0;
    memset(&g, 0, sizeof g);
    g.use_ctx = use_ctx;
    memcpy(g.prefix, b->lines[y].s + start, plen);
    g.prefix[plen] = '\0';
    g.plen = plen;
    g.v = xmalloc(MAX_RAW * sizeof *g.v);
    g.setsize = 8192;
    g.set = xmalloc(g.setsize * sizeof *g.set);
    memset(g.set, 0, g.setsize * sizeof *g.set);

    if (use_ctx) {
        if (!c->sc)
            c->sc = sym_cache_new();
        sym_budget(c->sc, COMPLETE_SCAN_MS);
        sym_classes(c->sc, cur, g.prefix, plen, 1, class_cb, &g);
        budget = 0;
    } else if (sep != (size_t)-1) {
        if (!c->sc)
            c->sc = sym_cache_new();
        sym_budget(c->sc, COMPLETE_SCAN_MS);
        sym_members(c->sc, docs, ndocs, cur, y, sep, sym_cb, &g);
        budget = 0;                 /* after an operator only members make sense */
    } else {
        size_t ns, k;
        const SnipDef *sd = snip_lang(cur->syn ? syn_name(cur->syn) : NULL, &ns);

        /* snippets first: a snippet named like a keyword wins */
        for (k = 0; k < ns; k++)
            consider(&g, sd[k].name, strlen(sd[k].name), CAND_SNIPPET, 0);
        if (syn_rule_count(cur->syn) > 0) {
            if (!c->sc)
                c->sc = sym_cache_new();
            sym_budget(c->sc, COMPLETE_SCAN_MS);
            sym_globals(c->sc, docs, ndocs, cur, sym_cb, &g);
            if (php && plen >= 3 && (start == 0 || b->lines[y].s[start - 1] != '$'))
                sym_classes(c->sc, cur, g.prefix, plen, 0, class_cb, &g);
        }
        /* after the symbols: a library class called like a keyword (Exception) keeps its use */
        syn_each_word(cur->syn, kw_cb, &g);
    }
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
        c->v[c->n].sig = g.v[i].sig;
        c->v[c->n].kind = g.v[i].kind;
        c->n++;
    }
    for (i = take; i < g.n; i++) {
        free(g.v[i].w);
        free(g.v[i].sig);
    }
    for (i = 0; i < g.setsize; i++)
        free(g.set[i]);
    free(g.set);
    free(g.v);
    c->y = y;
    c->x = start;
    c->on = c->n > 0;
    c->scanning = c->sc && sym_busy(c->sc);
    c->scan_doc = cur;
    c->scan_y = y;
    c->scan_x = x;
    c->scan_gen = cur->buf.gen;
    return c->on;
}

int complete_resume(Complete *c, Doc *const *docs, size_t ndocs, Doc *cur, size_t y, size_t x)
{
    char *keep = NULL;
    size_t top = c->top, picked = c->picked, i;
    unsigned spin = c->spin + 1;

    if (!c->scanning || c->scan_doc != cur || c->scan_y != y || c->scan_x != x ||
        c->scan_gen != cur->buf.gen) {
        c->scanning = 0;
        return 0;
    }
    if (c->on && c->sel < c->n)
        keep = xstrdup(c->v[c->sel].text);
    complete_open(c, docs, ndocs, cur, y, x);
    c->spin = spin;
    if (keep) {
        for (i = 0; i < c->n; i++)
            if (strcmp(c->v[i].text, keep) == 0) {
                c->sel = i;
                c->top = top <= i ? top : i;
                c->picked = (int)picked;
                break;
            }
        free(keep);
    }
    return 1;
}

static int param_optional(const char *p, size_t n)
{
    size_t i;
    int d = 0;

    if (n > 0 && p[0] == '[')
        return 1;
    if (n > 0 && p[0] == '*')
        return 1;
    for (i = 0; i < n; i++) {
        if (p[i] == '(' || p[i] == '[' || p[i] == '{' || p[i] == '<')
            d++;
        else if ((p[i] == ')' || p[i] == ']' || p[i] == '}' || p[i] == '>') && d > 0)
            d--;
        else if (d == 0 && p[i] == '=')
            return 1;
        else if (d == 0 && p[i] == '?' && i + 1 < n && p[i + 1] == ':')
            return 1;
        else if (p[i] == '.' && i + 2 < n && p[i + 1] == '.' && p[i + 2] == '.')
            return 1;
    }
    return 0;
}

static void body_add(SBuf *b, const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (s[i] == '$')
            sbuf_adds(b, "$$");
        else if (s[i] != '}')
            sbuf_add(b, s + i, 1);
    }
}

int complete_call_body(const Cand *c, SBuf *body)
{
    const char *sg = c->sig, *p, *q, *start;
    size_t nparams = 0, nstops = 0, name_len;
    int depth = 0, any = 0;
    SBuf out = SBUF_INIT;

    if ((c->kind != CAND_FUNC && c->kind != CAND_METHOD) || !sg)
        return 0;
    name_len = strlen(c->text);
    if (strncmp(sg, c->text, name_len) != 0 || sg[name_len] != '(')
        return 0;
    start = sg + name_len + 1;
    for (q = start; *q; q++) {
        if (*q == '(' || *q == '[' || *q == '{')
            depth++;
        else if ((*q == ')' || *q == ']' || *q == '}') && depth-- == 0)
            break;
    }
    if (*q != ')')
        return 0;
    /* [start, q) are the parameters */
    for (p = start; p < q && (*p == ' ' || *p == '\t'); p++)
        ;
    if (p == q) {
        /* no parameters: a plain call, no placeholders */
        body_add(body, c->text, name_len);
        sbuf_adds(body, "()");
        return 2;
    }
    body_add(&out, c->text, name_len);
    sbuf_adds(&out, "(");
    depth = 0;
    for (p = start; p <= q; p++) {
        if (p < q && (*p == '(' || *p == '[' || *p == '{' || *p == '<'))
            depth++;
        else if (p < q && (*p == ')' || *p == ']' || *p == '}' || (*p == '>' && p[-1] != '-')) &&
                 depth > 0)
            depth--;
        if (p == q || (*p == ',' && depth == 0)) {
            const char *a = start, *z = p;

            while (a < z && (*a == ' ' || *a == '\t'))
                a++;
            while (z > a && (z[-1] == ' ' || z[-1] == '\t'))
                z--;
            if (z > a) {
                nparams++;
                if (!param_optional(a, (size_t)(z - a))) {
                    if (any)
                        sbuf_adds(&out, ", ");
                    if (nstops < 8) {
                        sbuf_addf(&out, "${%zu:", ++nstops);
                        body_add(&out, a, (size_t)(z - a));
                        sbuf_adds(&out, "}");
                    } else {
                        body_add(&out, a, (size_t)(z - a));
                    }
                    any = 1;
                }
            }
            start = p + 1;
        }
    }
    if (!any)
        sbuf_adds(&out, nparams ? "$1" : "");
    sbuf_adds(&out, ")$0");
    sbuf_add(body, out.p, out.len);
    sbuf_free(&out);
    return 1;
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
    {
        size_t rows = c->rows ? c->rows : COMPLETE_ROWS;

        if (c->sel < c->top)
            c->top = c->sel;
        if (c->sel >= c->top + rows)
            c->top = c->sel - rows + 1;
    }
}
