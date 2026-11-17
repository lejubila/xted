#include "md.h"
#include "i18n.h"
#include "syntax.h"
#include "undo.h"
#include "utf8.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Text with a style and a source line for every byte: a paragraph before it is wrapped. */
typedef struct {
    SBuf t, s;              /* bytes, styles */
    size_t *src;
    size_t n, cap;
} Run;

/* The lines being written. */
typedef struct {
    SBuf text;              /* every line, '\n' after each */
    uint8_t **st;
    size_t *srcline;
    size_t n, cap;
    int width, ascii;
    int last_blank;
} Out;

static void run_add(Run *r, const char *t, size_t n, uint8_t st, size_t src)
{
    size_t k;

    if (r->n + n > r->cap) {
        r->cap = (r->n + n) * 2 + 16;
        r->src = xrealloc(r->src, r->cap * sizeof *r->src);
    }
    sbuf_add(&r->t, t, n);
    for (k = 0; k < n; k++) {
        sbuf_add(&r->s, (const char *)&st, 1);
        r->src[r->n++] = src;
    }
}

/* Appends r's bytes [from, to) with their styles and source lines. */
static void run_copy(Run *dst, const Run *r, size_t from, size_t to)
{
    size_t k;

    for (k = from; k < to; k++)
        run_add(dst, r->t.p + k, 1, (uint8_t)r->s.p[k], r->src[k]);
}

static void run_clear(Run *r)
{
    r->t.len = r->s.len = 0;
    r->n = 0;
}

static void run_free(Run *r)
{
    sbuf_free(&r->t);
    sbuf_free(&r->s);
    free(r->src);
}

static void out_line(Out *o, const char *t, const uint8_t *s, size_t n, size_t src)
{
    if (o->n == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 64;
        o->st = xrealloc(o->st, o->cap * sizeof *o->st);
        o->srcline = xrealloc(o->srcline, o->cap * sizeof *o->srcline);
    }
    o->st[o->n] = xmalloc(n ? n : 1);
    if (n)
        memcpy(o->st[o->n], s, n);
    o->srcline[o->n++] = src;
    sbuf_add(&o->text, t, n);
    sbuf_adds(&o->text, "\n");
    o->last_blank = n == 0;
}

static void blank(Out *o, size_t src)
{
    if (o->n > 0 && !o->last_blank)
        out_line(o, "", NULL, 0, src);
}

static size_t cols_of(const char *s, size_t n)
{
    return utf8_cols(s, n, n, 8);
}

/* A run of n copies of the UTF-8 character c, in style st. */
static void repeat(Run *r, const char *c, size_t times, uint8_t st, size_t src)
{
    size_t k;

    for (k = 0; k < times; k++)
        run_add(r, c, strlen(c), st, src);
}

static void emit_run(Out *o, const Run *r)
{
    out_line(o, r->t.p ? r->t.p : "", (const uint8_t *)(r->s.p ? r->s.p : ""), r->n, r->n ? r->src[0] : 0);
}

/* ---- inline text --------------------------------------------------------- */

/* The closing mark of the same kind after k (a ** or * that ends an emphasis). */
static int closes(const char *s, size_t n, size_t k, const char *mark)
{
    size_t ml = strlen(mark), i;

    for (i = k + ml; i + ml <= n; i++) {
        if (s[i] == '\\') {
            i++;
            continue;
        }
        if (memcmp(s + i, mark, ml) == 0 && i > k + ml && s[i - 1] != ' ')
            return 1;
    }
    return 0;
}

static int word_ch(char c)
{
    return isalnum((unsigned char)c) || (unsigned char)c >= 0x80;
}

/* Renders the inline Markdown of s into r: emphasis, code spans, links, images, escapes. A '\n'
 * in s is a hard line break and is kept. */
static void inline_md(Run *r, const char *s, size_t n, const size_t *src, uint8_t base)
{
    size_t i = 0;
    int bold = 0, ital = 0, strike = 0;

    while (i < n) {
        char c = s[i];
        uint8_t st = (uint8_t)(base | (bold ? MD_BOLD : 0) | (ital ? MD_ITALIC : 0));

        if (strike)
            st = (uint8_t)((st & ~MD_CLS) | SC_COMMENT);
        if (c == '\\' && i + 1 < n && ispunct((unsigned char)s[i + 1])) {
            run_add(r, s + i + 1, 1, st, src[i]);
            i += 2;
        } else if (c == '`') {
            size_t ticks = 0, b, e;

            while (i + ticks < n && s[i + ticks] == '`')
                ticks++;
            for (e = i + ticks; e + ticks <= n; e++)
                if (memcmp(s + e, "``````````", ticks < 10 ? ticks : 10) == 0 &&
                    (e + ticks == n || s[e + ticks] != '`'))
                    break;
            if (e + ticks > n) {
                run_add(r, s + i, ticks, st, src[i]);
                i += ticks;
                continue;
            }
            {
                size_t ce = e;          /* where the closing backticks are */

                b = i + ticks;
                if (e > b + 1 && s[b] == ' ' && s[e - 1] == ' ') {
                    b++;
                    e--;
                }
                run_add(r, s + b, e - b, (uint8_t)((st & ~MD_CLS) | SC_CODE), src[i]);
                i = ce + ticks;
            }
        } else if ((c == '*' || c == '_') && i + 1 < n && s[i + 1] == c &&
                   (bold || (i + 2 < n && s[i + 2] != ' ' && closes(s, n, i, c == '*' ? "**" : "__")))) {
            bold = !bold;
            i += 2;
        } else if ((c == '*' || c == '_') &&
                   (ital || (i + 1 < n && s[i + 1] != ' ' && closes(s, n, i, c == '*' ? "*" : "_"))) &&
                   !(c == '_' && ((i > 0 && word_ch(s[i - 1]) && !ital) || (ital && i + 1 < n && word_ch(s[i + 1]))))) {
            ital = !ital;
            i++;
        } else if (c == '~' && i + 1 < n && s[i + 1] == '~' && (strike || closes(s, n, i, "~~"))) {
            strike = !strike;
            i += 2;
        } else if ((c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '[')) && i + 1 < n) {
            /* [text](url) and ![alt](src) */
            size_t t0 = i + (c == '!' ? 2 : 1), t1, u0, u1;
            int depth = 1;

            for (t1 = t0; t1 < n && depth > 0; t1++)
                if (s[t1] == '[')
                    depth++;
                else if (s[t1] == ']')
                    depth--;
            if (depth != 0 || t1 >= n || s[t1] != '(') {
                run_add(r, s + i, 1, st, src[i]);
                i++;
                continue;
            }
            t1--;                       /* the ']' */
            u0 = t1 + 2;
            for (u1 = u0; u1 < n && s[u1] != ')'; u1++)
                ;
            if (u1 >= n) {
                run_add(r, s + i, 1, st, src[i]);
                i++;
                continue;
            }
            if (c == '!') {
                char lab[80];

                snprintf(lab, sizeof lab, "[%s: ", tr("image"));
                run_add(r, lab, strlen(lab), (uint8_t)SC_COMMENT, src[i]);
                inline_md(r, s + t0, t1 - t0, src + t0, SC_COMMENT);
                run_add(r, "]", 1, (uint8_t)SC_COMMENT, src[i]);
            } else {
                size_t ue = u0;

                while (ue < u1 && s[ue] != ' ')
                    ue++;                   /* (url "title") */
                inline_md(r, s + t0, t1 - t0, src + t0, (uint8_t)(SC_LINK | MD_UNDER));
                /* the address, when the text does not say it */
                if (ue > u0 && !(ue - u0 == t1 - t0 && memcmp(s + u0, s + t0, ue - u0) == 0) && s[u0] != '#') {
                    run_add(r, " (", 2, (uint8_t)SC_COMMENT, src[i]);
                    run_add(r, s + u0, ue - u0, (uint8_t)SC_COMMENT, src[i]);
                    run_add(r, ")", 1, (uint8_t)SC_COMMENT, src[i]);
                }
            }
            i = u1 + 1;
        } else if (c == '<' && i + 1 < n && (strncmp(s + i + 1, "http", 4) == 0 || strncmp(s + i + 1, "mailto:", 7) == 0)) {
            size_t e = i + 1;

            while (e < n && s[e] != '>' && s[e] != ' ')
                e++;
            if (e < n && s[e] == '>') {
                run_add(r, s + i + 1, e - i - 1, (uint8_t)(SC_LINK | MD_UNDER), src[i]);
                i = e + 1;
            } else {
                run_add(r, s + i, 1, st, src[i]);
                i++;
            }
        } else {
            uint32_t cp;
            int k = utf8_decode(s + i, n - i, &cp);

            if (k < 1)
                k = 1;
            run_add(r, s + i, (size_t)k, st, src[i]);
            i += (size_t)k;
        }
    }
}

/* Writes the styled run r wrapped at the output's width: the first line after first (in style
 * pst), the others after more. A '\n' in r breaks the line. */
static void wrap(Out *o, const Run *r, const char *first, const char *more, uint8_t pst)
{
    size_t fw = cols_of(first, strlen(first)), mw = cols_of(more, strlen(more));
    size_t i = 0, w = (size_t)o->width;
    Run line;
    int firstline = 1;

    memset(&line, 0, sizeof line);
    while (i < r->n || firstline) {
        const char *pre = firstline ? first : more;
        size_t room = w > (firstline ? fw : mw) + 4 ? w - (firstline ? fw : mw) : 4, used = 0, cut = (size_t)-1;
        size_t start = i, k = i, srcl = i < r->n ? r->src[i] : (r->n ? r->src[r->n - 1] : 0);

        run_clear(&line);
        run_add(&line, pre, strlen(pre), pst, srcl);
        /* as many words as fit */
        while (k < r->n && r->t.p[k] != '\n') {
            uint32_t cp;
            int b = utf8_decode(r->t.p + k, r->n - k, &cp);
            size_t cw;

            if (b < 1)
                b = 1;
            cw = (size_t)(cp_width(cp) > 0 ? cp_width(cp) : 0);
            if (used + cw > room && k > start)
                break;
            if (r->t.p[k] == ' ')
                cut = k;
            used += cw;
            k += (size_t)b;
        }
        if (k < r->n && r->t.p[k] != '\n' && cut != (size_t)-1 && cut > start)
            k = cut;                    /* break at the last space */
        run_copy(&line, r, start, k);
        if (line.n == 0)
            out_line(o, "", NULL, 0, srcl);
        else
            emit_run(o, &line);
        firstline = 0;
        i = k;
        while (i < r->n && (r->t.p[i] == ' ' || r->t.p[i] == '\n'))
            if (r->t.p[i++] == '\n')
                break;
        if (i >= r->n)
            break;
    }
    run_free(&line);
}

/* ---- blocks -------------------------------------------------------------- */

static size_t lead(const Line *l)
{
    size_t k = 0;

    while (k < l->len && (l->s[k] == ' ' || l->s[k] == '\t'))
        k++;
    return k;
}

static int is_blank_line(const Line *l)
{
    return lead(l) == l->len;
}

/* --- *** ___ alone on a line */
static int is_rule(const Line *l)
{
    size_t k = lead(l), count = 0;
    char c;

    if (k > 3 || k >= l->len)
        return 0;
    c = l->s[k];
    if (c != '-' && c != '*' && c != '_')
        return 0;
    for (; k < l->len; k++) {
        if (l->s[k] == c)
            count++;
        else if (l->s[k] != ' ' && l->s[k] != '\t')
            return 0;
    }
    return count >= 3;
}

/* === or --- under a paragraph: its level, 0 if not. */
static int setext(const Line *l)
{
    size_t k = lead(l);
    char c;

    if (k > 3 || k >= l->len || (l->s[k] != '=' && l->s[k] != '-'))
        return 0;
    c = l->s[k];
    while (k < l->len && l->s[k] == c)
        k++;
    while (k < l->len && (l->s[k] == ' ' || l->s[k] == '\t'))
        k++;
    return k == l->len ? (c == '=' ? 1 : 2) : 0;
}

/* ``` or ~~~: the length of the fence, 0 if none; *info is the language word after it. */
static size_t fence(const Line *l, char *ch, char *info, size_t in)
{
    size_t k = lead(l), n = 0, e;

    if (k > 3 || k >= l->len || (l->s[k] != '`' && l->s[k] != '~'))
        return 0;
    *ch = l->s[k];
    while (k + n < l->len && l->s[k + n] == *ch)
        n++;
    if (n < 3)
        return 0;
    k += n;
    while (k < l->len && l->s[k] == ' ')
        k++;
    for (e = k; e < l->len && l->s[e] != ' ' && l->s[e] != '{' && l->s[e] != ','; e++)
        ;
    snprintf(info, in, "%.*s", (int)(e - k < in - 1 ? e - k : in - 1), l->s + k);
    return n;
}

/* A list item: its indentation, the marker ("-", "3."), where its text starts. */
static int list_item(const Line *l, size_t *indent, char *mark, size_t mn, size_t *text)
{
    size_t k = lead(l), e = k;

    if (k >= l->len)
        return 0;
    if ((l->s[k] == '-' || l->s[k] == '*' || l->s[k] == '+') && k + 1 < l->len && l->s[k + 1] == ' ') {
        e = k + 1;
    } else {
        while (e < l->len && isdigit((unsigned char)l->s[e]) && e - k < 9)
            e++;
        if (e == k || e >= l->len || (l->s[e] != '.' && l->s[e] != ')') || e + 1 >= l->len || l->s[e + 1] != ' ')
            return 0;
        e++;
    }
    *indent = k;
    snprintf(mark, mn, "%.*s", (int)(e - k), l->s + k);
    *text = e + 1;
    while (*text < l->len && l->s[*text] == ' ')
        (*text)++;
    return 1;
}

/* | a | b | with a row of dashes below it. */
static int is_table_sep(const Line *l)
{
    size_t k;
    int dash = 0, bar = 0;

    for (k = 0; k < l->len; k++) {
        char c = l->s[k];

        if (c == '-')
            dash = 1;
        else if (c == '|')
            bar = 1;
        else if (c != ':' && c != ' ' && c != '\t')
            return 0;
    }
    return dash && bar;
}

static int has_bar(const Line *l)
{
    return l->len > 0 && memchr(l->s, '|', l->len) != NULL;
}

/* The cells of a table row: [from, to) of each, the outer bars left out. */
static size_t cells(const Line *l, size_t from[], size_t to[], size_t max)
{
    size_t k = lead(l), e = l->len, n = 0, b;

    while (e > k && (l->s[e - 1] == ' ' || l->s[e - 1] == '\t'))
        e--;
    if (k < e && l->s[k] == '|')
        k++;
    if (e > k && l->s[e - 1] == '|' && !(e >= 2 && l->s[e - 2] == '\\'))
        e--;
    b = k;
    for (; k <= e && n < max; k++) {
        if (k == e || (l->s[k] == '|' && !(k > 0 && l->s[k - 1] == '\\'))) {
            size_t f = b, t = k;

            while (f < t && l->s[f] == ' ')
                f++;
            while (t > f && l->s[t - 1] == ' ')
                t--;
            from[n] = f;
            to[n++] = t;
            b = k + 1;
        }
    }
    return n;
}

#define TCOLS 16

static void table(Out *o, const Buffer *b, size_t y0, size_t y1, const char *bar, const char *hline,
                  const char *cross)
{
    size_t w[TCOLS] = { 0 }, ncol = 0, y, c;
    int align[TCOLS] = { 0 };   /* 0 left, 1 centre, 2 right */
    Run cell[TCOLS], row;

    memset(cell, 0, sizeof cell);
    memset(&row, 0, sizeof row);
    /* the alignments, then the widths of the rendered cells */
    {
        size_t f[TCOLS], t[TCOLS], n = cells(&b->lines[y0 + 1], f, t, TCOLS);

        for (c = 0; c < n; c++) {
            const char *s = b->lines[y0 + 1].s;
            int l = f[c] < t[c] && s[f[c]] == ':', r = t[c] > f[c] && s[t[c] - 1] == ':';

            align[c] = l && r ? 1 : r ? 2 : 0;
        }
    }
    for (y = y0; y < y1; y++) {
        size_t f[TCOLS], t[TCOLS], n, k;

        if (y == y0 + 1)
            continue;
        n = cells(&b->lines[y], f, t, TCOLS);
        if (n > ncol)
            ncol = n;
        for (k = 0; k < n; k++) {
            Run tmp;
            size_t *src = xmalloc((t[k] - f[k] + 1) * sizeof *src), q;

            memset(&tmp, 0, sizeof tmp);
            for (q = 0; q <= t[k] - f[k]; q++)
                src[q] = y;
            inline_md(&tmp, b->lines[y].s + f[k], t[k] - f[k], src, (uint8_t)(y == y0 ? MD_BOLD : 0));
            if (cols_of(tmp.t.p ? tmp.t.p : "", tmp.n) > w[k])
                w[k] = cols_of(tmp.t.p ? tmp.t.p : "", tmp.n);
            free(src);
            run_free(&tmp);
        }
    }
    for (y = y0; y < y1; y++) {
        size_t f[TCOLS], t[TCOLS], n = cells(&b->lines[y], f, t, TCOLS), k;

        run_clear(&row);
        if (y == y0 + 1) {
            for (k = 0; k < ncol; k++) {
                if (k)
                    run_add(&row, cross, strlen(cross), (uint8_t)SC_COMMENT, y);
                repeat(&row, hline, w[k] + 2, (uint8_t)SC_COMMENT, y);
            }
            emit_run(o, &row);
            continue;
        }
        for (k = 0; k < ncol; k++) {
            Run tmp;
            size_t cw = 0, pad, left;

            memset(&tmp, 0, sizeof tmp);
            if (k < n) {
                size_t *src = xmalloc((t[k] - f[k] + 1) * sizeof *src), q;

                for (q = 0; q <= t[k] - f[k]; q++)
                    src[q] = y;
                inline_md(&tmp, b->lines[y].s + f[k], t[k] - f[k], src, (uint8_t)(y == y0 ? MD_BOLD : 0));
                free(src);
                cw = cols_of(tmp.t.p ? tmp.t.p : "", tmp.n);
            }
            if (k)
                run_add(&row, bar, strlen(bar), (uint8_t)SC_COMMENT, y);
            pad = w[k] - cw;
            left = align[k] == 2 ? pad : align[k] == 1 ? pad / 2 : 0;
            repeat(&row, " ", 1 + left, 0, y);
            if (tmp.n) {
                sbuf_add(&row.t, tmp.t.p, tmp.n);
                sbuf_add(&row.s, tmp.s.p, tmp.n);
                if (row.n + tmp.n > row.cap) {
                    row.cap = (row.n + tmp.n) * 2;
                    row.src = xrealloc(row.src, row.cap * sizeof *row.src);
                }
                for (cw = 0; cw < tmp.n; cw++)
                    row.src[row.n++] = y;
            }
            repeat(&row, " ", 1 + pad - left, 0, y);
            run_free(&tmp);
        }
        emit_run(o, &row);
    }
    run_free(&row);
}

void md_free(MdPreview *m)
{
    size_t i;

    for (i = 0; i < m->n; i++)
        free(m->st[i]);
    free(m->st);
    free(m->srcline);
    m->st = NULL;
    m->srcline = NULL;
    m->n = 0;
}

const uint8_t *md_styles(const MdPreview *m, size_t y)
{
    return y < m->n ? m->st[y] : NULL;
}

size_t md_line_for(const MdPreview *m, size_t y)
{
    size_t i;

    for (i = 0; i < m->n; i++)
        if (m->srcline[i] >= y)
            return i;
    return m->n ? m->n - 1 : 0;
}

/* The paragraph being gathered: its source text (lines joined, '\n' for a hard break), the
 * source line of each byte, and how its lines start. */
typedef struct {
    SBuf t;
    size_t *src;
    size_t n, cap;
    char first[96], more[96];
    uint8_t pst, base;
    int open;
} Para;

static void para_add(Para *p, const char *s, size_t n, size_t y)
{
    size_t k;

    if (p->n + n + 1 > p->cap) {
        p->cap = (p->n + n + 1) * 2 + 32;
        p->src = xrealloc(p->src, p->cap * sizeof *p->src);
    }
    sbuf_add(&p->t, s, n);
    for (k = 0; k < n; k++)
        p->src[p->n++] = y;
}

static void para_flush(Out *o, Para *p)
{
    Run r;

    if (!p->open)
        return;
    memset(&r, 0, sizeof r);
    inline_md(&r, p->t.p ? p->t.p : "", p->n, p->src, p->base);
    wrap(o, &r, p->first, p->more, p->pst);
    run_free(&r);
    p->t.len = 0;
    p->n = 0;
    p->open = 0;
}

/* A line of paragraph text: joined to the one before with a space, or a hard break when that
 * one ended with two spaces or a backslash. */
static void para_line(Para *p, const char *s, size_t n, size_t y)
{
    size_t e = n;
    int brk = 0;

    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t'))
        e--;
    if (n - e >= 2)
        brk = 1;
    if (e > 0 && s[e - 1] == '\\') {
        e--;
        brk = 1;
    }
    if (p->n > 0 && p->t.p[p->n - 1] != '\n')
        para_add(p, " ", 1, y);
    para_add(p, s, e, y);
    if (brk)
        para_add(p, "\n", 1, y);
    p->open = 1;
}

static void heading(Out *o, const char *s, size_t n, size_t y, int level, int ascii)
{
    Run r, ul;
    size_t *src = xmalloc((n + 1) * sizeof *src), k, w;

    memset(&r, 0, sizeof r);
    memset(&ul, 0, sizeof ul);
    for (k = 0; k <= n; k++)
        src[k] = y;
    inline_md(&r, s, n, src, (uint8_t)(SC_HEADING | MD_BOLD));
    free(src);
    wrap(o, &r, "", "", (uint8_t)(SC_HEADING | MD_BOLD));
    if (level <= 2) {
        w = cols_of(r.t.p ? r.t.p : "", r.n);
        if (w > (size_t)o->width)
            w = (size_t)o->width;
        repeat(&ul, level == 1 ? (ascii ? "=" : "\xe2\x95\x90") : (ascii ? "-" : "\xe2\x94\x80"), w,
               (uint8_t)SC_HEADING, y);
        emit_run(o, &ul);
    }
    run_free(&r);
    run_free(&ul);
}

void md_build(MdPreview *m, const Buffer *b, Buffer *out, int width, int ascii)
{
    Out o;
    Para p;
    size_t y, oy = 0, ox = 0;
    char fch = 0, info[32];
    size_t flen = 0;
    Syntax *fsyn = NULL;
    uint16_t fst = 0;
    const char *bullet = ascii ? "*" : "\xe2\x80\xa2", *quote = ascii ? "| " : "\xe2\x94\x82 ";

    memset(&o, 0, sizeof o);
    memset(&p, 0, sizeof p);
    o.width = width < 20 ? 20 : width;
    o.ascii = ascii;
    for (y = 0; y < b->nlines; y++) {
        const Line *l = &b->lines[y];
        size_t ind, text, li;
        char mark[16], fc;

        if (flen) {
            /* inside a fenced code block: its lines as they are, coloured by their language */
            size_t k = lead(l), n = 0, w;
            uint8_t cls[1024], st[1024];
            Run r;

            while (k + n < l->len && l->s[k + n] == fch)
                n++;
            if (n >= flen && k <= 3 && k + n == l->len) {
                flen = 0;
                syn_free(fsyn);
                fsyn = NULL;
                continue;
            }
            memset(&r, 0, sizeof r);
            memset(cls, 0, sizeof cls);
            if (fsyn && l->len < sizeof cls)
                fst = syn_line(fsyn, fst, l->s, l->len, cls);
            for (k = 0; k < l->len && k < sizeof st; k++)
                st[k] = (uint8_t)(cls[k] | MD_CODEBG);
            run_add(&r, " ", 1, MD_CODEBG, y);
            if (l->len) {
                sbuf_add(&r.t, l->s, l->len);
                sbuf_add(&r.s, (const char *)st, l->len < sizeof st ? l->len : sizeof st);
                while (r.s.len < r.t.len)
                    sbuf_add(&r.s, (const char *)&st[0], 1);
                if (r.n + l->len > r.cap) {
                    r.cap = r.n + l->len + 8;
                    r.src = xrealloc(r.src, r.cap * sizeof *r.src);
                }
                for (k = 0; k < l->len; k++)
                    r.src[r.n++] = y;
            }
            w = cols_of(l->s, l->len) + 1;
            if (w < (size_t)o.width)
                repeat(&r, " ", (size_t)o.width - w, MD_CODEBG, y);
            emit_run(&o, &r);
            run_free(&r);
            continue;
        }
        if (is_blank_line(l)) {
            para_flush(&o, &p);
            blank(&o, y);
            continue;
        }
        if ((li = fence(l, &fc, info, sizeof info)) > 0) {
            para_flush(&o, &p);
            fch = fc;
            flen = li;
            fsyn = syn_by_name(info);
            fst = 0;
            continue;
        }
        ind = lead(l);
        if (ind <= 3 && l->s[ind] == '#') {
            size_t lv = 0, e = l->len;

            while (ind + lv < l->len && l->s[ind + lv] == '#')
                lv++;
            if (lv <= 6 && (ind + lv == l->len || l->s[ind + lv] == ' ')) {
                size_t t0 = ind + lv;

                para_flush(&o, &p);
                while (t0 < l->len && l->s[t0] == ' ')
                    t0++;
                while (e > t0 && (l->s[e - 1] == '#' || l->s[e - 1] == ' '))
                    e--;
                blank(&o, y);
                heading(&o, l->s + t0, e - t0, y, (int)lv, ascii);
                continue;
            }
        }
        if (p.open && p.first[0] == '\0' && p.base == 0 && setext(l)) {
            /* the paragraph above was a heading */
            Run r;

            memset(&r, 0, sizeof r);
            p.base = 0;
            p.open = 0;
            heading(&o, p.t.p, p.n, p.src[0], setext(l), ascii);
            p.t.len = 0;
            p.n = 0;
            run_free(&r);
            continue;
        }
        if (is_rule(l)) {
            Run r;

            para_flush(&o, &p);
            memset(&r, 0, sizeof r);
            repeat(&r, ascii ? "-" : "\xe2\x94\x80", (size_t)o.width, (uint8_t)SC_COMMENT, y);
            emit_run(&o, &r);
            run_free(&r);
            continue;
        }
        if (has_bar(l) && y + 1 < b->nlines && is_table_sep(&b->lines[y + 1])) {
            size_t e = y + 2;

            para_flush(&o, &p);
            while (e < b->nlines && has_bar(&b->lines[e]) && !is_blank_line(&b->lines[e]))
                e++;
            table(&o, b, y, e, ascii ? "|" : "\xe2\x94\x82", ascii ? "-" : "\xe2\x94\x80",
                  ascii ? "+" : "\xe2\x94\xbc");
            y = e - 1;
            continue;
        }
        if (ind <= 3 && l->s[ind] == '>') {
            /* a quote: its text after a bar, nested quotes after more */
            size_t k = ind, depth = 0;
            char pre[96] = "";

            while (k < l->len && (l->s[k] == '>' || l->s[k] == ' ')) {
                if (l->s[k] == '>')
                    depth++;
                k++;
            }
            while (depth-- > 0 && strlen(pre) + strlen(quote) < sizeof pre)
                strcat(pre, quote);
            if (!p.open || strcmp(p.first, pre) != 0) {
                para_flush(&o, &p);
                snprintf(p.first, sizeof p.first, "%s", pre);
                snprintf(p.more, sizeof p.more, "%s", pre);
                p.pst = SC_COMMENT;
                p.base = MD_ITALIC;
            }
            para_line(&p, l->s + k, l->len - k, y);
            continue;
        }
        if (list_item(l, &ind, mark, sizeof mark, &text)) {
            size_t lvl = ind / 2, k;
            char pre[20] = "", box[8] = "";
            const char *mk = (mark[0] == '-' || mark[0] == '*' || mark[0] == '+') ? bullet : mark;
            size_t pw;

            para_flush(&o, &p);
            if (lvl > 8)
                lvl = 8;
            /* a task: [ ] or [x] */
            if (text + 3 <= l->len && l->s[text] == '[' && l->s[text + 2] == ']' &&
                (l->s[text + 1] == ' ' || l->s[text + 1] == 'x' || l->s[text + 1] == 'X')) {
                snprintf(box, sizeof box, "%s", l->s[text + 1] == ' ' ? (ascii ? "[ ]" : "\xe2\x98\x90")
                                                                       : (ascii ? "[x]" : "\xe2\x98\x91"));
                text += 3;
                while (text < l->len && l->s[text] == ' ')
                    text++;
            }
            for (k = 0; k < lvl; k++)
                strcat(pre, "  ");
            /* "  • text", "  3. text", "  ☐ text": the next lines under the text */
            snprintf(p.first, sizeof p.first, "%.16s%.15s ", pre, box[0] ? box : mk);
            pw = cols_of(p.first, strlen(p.first));
            if (pw > sizeof p.more - 1)
                pw = sizeof p.more - 1;
            memset(p.more, ' ', pw);
            p.more[pw] = '\0';
            p.pst = SC_KEYWORD;
            p.base = 0;
            para_line(&p, l->s + text, l->len - text, y);
            continue;
        }
        if (ind >= 4 && !p.open) {
            /* indented code */
            Run r;
            size_t w = cols_of(l->s + 4, l->len - 4) + 1;

            memset(&r, 0, sizeof r);
            run_add(&r, " ", 1, MD_CODEBG, y);
            run_add(&r, l->s + 4, l->len - 4, (uint8_t)(SC_CODE | MD_CODEBG), y);
            if (w < (size_t)o.width)
                repeat(&r, " ", (size_t)o.width - w, MD_CODEBG, y);
            emit_run(&o, &r);
            run_free(&r);
            continue;
        }
        if (p.open && p.first[0] && ind >= 2) {
            /* the next line of a list item */
            para_line(&p, l->s + ind, l->len - ind, y);
            continue;
        }
        if (p.open && (p.first[0] || p.base)) {
            para_flush(&o, &p);
        }
        if (!p.open) {
            p.first[0] = p.more[0] = '\0';
            p.pst = 0;
            p.base = 0;
        }
        para_line(&p, l->s + ind, l->len - ind, y);
    }
    para_flush(&o, &p);
    syn_free(fsyn);
    /* no blank lines at the end */
    while (o.n > 1 && o.last_blank && o.text.len > 0 && o.text.p[o.text.len - 1] == '\n' &&
           (o.text.len == 1 || o.text.p[o.text.len - 2] == '\n')) {
        free(o.st[--o.n]);
        o.text.len--;
        o.last_blank = o.n > 0 && (o.text.len < 2 || o.text.p[o.text.len - 2] == '\n');
    }
    /* the text replaces the preview's, with no undo */
    if (out->nlines > 1 || out->lines[0].len > 0)
        buf_delete_range(out, 0, 0, out->nlines - 1, out->lines[out->nlines - 1].len);
    if (o.text.len > 0)
        o.text.len--;                   /* the last line's newline */
    buf_insert_text(out, &oy, &ox, o.text.p ? o.text.p : "", o.text.len);
    undo_free(&out->undo);
    out->modified = 0;
    md_free(m);
    m->st = o.st;
    m->srcline = o.srcline;
    m->n = o.n;
    m->width = width;
    m->glyphs = ascii;
    sbuf_free(&o.text);
    free(p.src);
    sbuf_free(&p.t);
}
