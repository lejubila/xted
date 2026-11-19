#include "search.h"
#include "files.h"
#include "utf8.h"
#include "util.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HIT_TEXT_MAX 160

/* ---- compile ----------------------------------------------------------- */

#ifndef XTED_NO_REGEX
/* Rewrites \d \D \w \W \s \S as POSIX classes; brackets are copied as they are. */
static char *translate(const char *p, size_t n)
{
    SBuf o = SBUF_INIT;
    size_t i = 0;

    while (i < n) {
        if (p[i] == '[') {
            size_t j = i + 1;

            sbuf_add(&o, "[", 1);
            if (j < n && p[j] == '^')
                sbuf_add(&o, &p[j++], 1);
            if (j < n && p[j] == ']')
                sbuf_add(&o, &p[j++], 1);
            while (j < n && p[j] != ']') {
                if (p[j] == '[' && j + 1 < n && (p[j + 1] == ':' || p[j + 1] == '.' ||
                                                 p[j + 1] == '=')) {
                    char kind = p[j + 1];
                    size_t k = j + 2;

                    while (k + 1 < n && !(p[k] == kind && p[k + 1] == ']'))
                        k++;
                    k = k + 2 <= n ? k + 2 : n;
                    sbuf_add(&o, p + j, k - j);
                    j = k;
                } else {
                    sbuf_add(&o, &p[j++], 1);
                }
            }
            if (j < n)
                sbuf_add(&o, &p[j++], 1);
            i = j;
        } else if (p[i] == '\\' && i + 1 < n) {
            const char *cls = NULL;

            switch (p[i + 1]) {
            case 'd': cls = "[[:digit:]]"; break;
            case 'D': cls = "[^[:digit:]]"; break;
            case 'w': cls = "[[:alnum:]_]"; break;
            case 'W': cls = "[^[:alnum:]_]"; break;
            case 's': cls = "[[:space:]]"; break;
            case 'S': cls = "[^[:space:]]"; break;
            default: break;
            }
            if (cls)
                sbuf_adds(&o, cls);
            else
                sbuf_add(&o, p + i, 2);
            i += 2;
        } else {
            sbuf_add(&o, &p[i++], 1);
        }
    }
    sbuf_add(&o, "", 1);
    return sbuf_steal(&o);
}
#endif

void sm_free(Matcher *m)
{
#ifndef XTED_NO_REGEX
    if (m->has_re)
        regfree(&m->re);
#endif
    free(m->raw);
    free(m->fold);
    free(m->line);
    memset(m, 0, sizeof *m);
}

int sm_compile(Matcher *m, const char *pat, size_t n, int regex, int cs, int word)
{
    sm_free(m);
    m->regex = regex;
    m->cs = cs;
    m->word = word;
    if (n == 0)
        return -1;
    if (regex) {
#ifdef XTED_NO_REGEX
        snprintf(m->err, sizeof m->err, "%s", "regular expressions are not available");
        return -1;
#else
        char *tr = translate(pat, n);
        int rc = regcomp(&m->re, tr, REG_EXTENDED | (cs ? 0 : REG_ICASE));

        free(tr);
        if (rc != 0) {
            regerror(rc, &m->re, m->err, sizeof m->err);
            return -1;
        }
        m->has_re = 1;
#endif
    } else {
        size_t i = 0;

        m->raw = xmalloc(n * sizeof *m->raw);
        m->fold = xmalloc(n * sizeof *m->fold);
        while (i < n) {
            uint32_t cp;

            i += (size_t)utf8_decode(pat + i, n - i, &cp);
            m->raw[m->ncp] = cp;
            m->fold[m->ncp] = cp_fold(cp);
            m->ncp++;
        }
    }
    m->valid = 1;
    return 0;
}

/* ---- one line ---------------------------------------------------------- */

static void load_line(Matcher *m, const char *s, size_t len)
{
    if (m->linecap < len + 1) {
        m->linecap = (len + 1) * 2;
        m->line = xrealloc(m->line, m->linecap);
    }
    memcpy(m->line, s, len);
    m->line[len] = '\0';
}

static size_t char_len(const char *s, size_t len, size_t i)
{
    uint32_t cp;

    return i < len ? (size_t)utf8_decode(s + i, len - i, &cp) : 1;
}

static int word_ok(const char *s, size_t len, size_t a, size_t b)
{
    uint32_t cp;

    if (a > 0) {
        size_t p = utf8_prev(s, a);

        utf8_decode(s + p, len - p, &cp);
        if (cp_is_word(cp))
            return 0;
    }
    if (b < len) {
        utf8_decode(s + b, len - b, &cp);
        if (cp_is_word(cp))
            return 0;
    }
    return 1;
}

static int find_lit(const Matcher *m, const char *s, size_t len, size_t from, size_t *ms,
                    size_t *me)
{
    size_t i = from;
    const uint32_t *want = m->cs ? m->raw : m->fold;

    while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80)
        i++;
    while (i < len) {
        size_t j = i, k = 0;

        while (k < m->ncp && j < len) {
            uint32_t cp;
            size_t n;

            if ((unsigned char)s[j] < 0x80) {
                cp = (unsigned char)s[j];
                n = 1;
            } else {
                n = (size_t)utf8_decode(s + j, len - j, &cp);
            }
            if ((m->cs ? cp : cp_fold(cp)) != want[k])
                break;
            j += n;
            k++;
        }
        if (k == m->ncp) {
            *ms = i;
            *me = j;
            return 1;
        }
        i += char_len(s, len, i);
    }
    return 0;
}

/* m->line must hold the line (regex mode). */
static int find_re(Matcher *m, size_t len, size_t from, size_t *ms, size_t *me, regmatch_t *grp)
{
#ifdef XTED_NO_REGEX
    (void)m; (void)len; (void)from; (void)ms; (void)me; (void)grp;
    return 0;
#else
    regmatch_t pm[10];
    int i;

    if (from > len)
        return 0;
    if (regexec(&m->re, m->line + from, 10, pm, from > 0 ? REG_NOTBOL : 0) != 0 ||
        pm[0].rm_so < 0)
        return 0;
    *ms = from + (size_t)pm[0].rm_so;
    *me = from + (size_t)pm[0].rm_eo;
    if (grp)
        for (i = 0; i < 10; i++) {
            grp[i].rm_so = pm[i].rm_so < 0 ? -1 : pm[i].rm_so + (regoff_t)from;
            grp[i].rm_eo = pm[i].rm_eo < 0 ? -1 : pm[i].rm_eo + (regoff_t)from;
        }
    return 1;
#endif
}

/* Non-empty, whole-word-checked match at or after `from`. */
static int find_core(Matcher *m, const char *s, size_t len, size_t from, size_t *ms, size_t *me,
                     regmatch_t *grp)
{
    size_t pos = from;

    while (pos <= len) {
        size_t a, b;
        int ok = m->regex ? find_re(m, len, pos, &a, &b, grp) : find_lit(m, s, len, pos, &a, &b);

        if (!ok)
            return 0;
        if (b > a && (!m->word || word_ok(s, len, a, b))) {
            *ms = a;
            *me = b;
            if (!m->regex && grp) {
                int i;

                for (i = 0; i < 10; i++)
                    grp[i].rm_so = grp[i].rm_eo = -1;
                grp[0].rm_so = (regoff_t)a;
                grp[0].rm_eo = (regoff_t)b;
            }
            return 1;
        }
        if (a >= len)
            return 0;
        pos = a + char_len(s, len, a);
    }
    return 0;
}

int sm_find(Matcher *m, const char *s, size_t len, size_t from, size_t *ms, size_t *me,
            regmatch_t *grp)
{
    if (!m->valid)
        return 0;
    if (m->regex)
        load_line(m, s, len);
    return find_core(m, s, len, from, ms, me, grp);
}

int sm_find_last(Matcher *m, const char *s, size_t len, size_t before, size_t *ms, size_t *me)
{
    size_t pos = 0, a, b;
    int found = 0;

    if (!m->valid)
        return 0;
    if (m->regex)
        load_line(m, s, len);
    while (pos <= len && find_core(m, s, len, pos, &a, &b, NULL) && a < before) {
        *ms = a;
        *me = b;
        found = 1;
        pos = a + char_len(s, len, a);
    }
    return found;
}

size_t sm_line_matches(Matcher *m, const char *s, size_t len, size_t (*rng)[2], size_t max)
{
    size_t pos = 0, n = 0, a, b;

    if (!m->valid || len == 0)
        return 0;
    if (m->regex)
        load_line(m, s, len);
    while (n < max && pos <= len && find_core(m, s, len, pos, &a, &b, NULL)) {
        rng[n][0] = a;
        rng[n][1] = b;
        n++;
        pos = b;
    }
    return n;
}

/* ---- in a buffer ------------------------------------------------------- */

static void bounds(const Buffer *b, const Span *lim, size_t y, size_t *lo, size_t *hi)
{
    size_t len = b->lines[y].len;

    *lo = lim && y == lim->y0 ? lim->x0 : 0;
    *hi = lim && y == lim->y1 ? lim->x1 : len;
    if (*hi > len)
        *hi = len;
}

static int in_span(const Span *lim, size_t y)
{
    return !lim || (y >= lim->y0 && y <= lim->y1);
}

int search_next(Matcher *m, const Buffer *b, size_t y, size_t x, int forward, int wrap,
                const Span *lim, Hit *out, int *wrapped)
{
    size_t yy, lo, hi, ms, me;
    int pass;

    if (wrapped)
        *wrapped = 0;
    if (!m->valid || y >= b->nlines)
        return 0;
    for (pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            if (!wrap)
                return 0;
            if (wrapped)
                *wrapped = 1;
        }
        if (forward) {
            size_t first = pass == 0 ? y : 0, last = pass == 0 ? b->nlines - 1 : y;

            for (yy = first; yy <= last; yy++) {
                if (!in_span(lim, yy))
                    continue;
                bounds(b, lim, yy, &lo, &hi);
                {
                    size_t from = pass == 0 && yy == y && x > lo ? x : lo;

                    if (from > hi || !sm_find(m, b->lines[yy].s, hi, from, &ms, &me, NULL))
                        continue;
                    if (pass == 1 && yy == y && ms >= x)
                        continue;
                    out->y = yy;
                    out->x = ms;
                    out->end = me;
                    return 1;
                }
            }
        } else {
            size_t first = pass == 0 ? y : b->nlines - 1, last = pass == 0 ? 0 : y;

            for (yy = first;; yy--) {
                if (in_span(lim, yy)) {
                    bounds(b, lim, yy, &lo, &hi);
                    {
                        size_t before = pass == 0 && yy == y ? x : hi + 1;

                        if (sm_find_last(m, b->lines[yy].s, hi, before, &ms, &me) && ms >= lo &&
                            !(pass == 1 && yy == y && ms < x)) {
                            out->y = yy;
                            out->x = ms;
                            out->end = me;
                            return 1;
                        }
                    }
                }
                if (yy == last)
                    break;
            }
        }
    }
    return 0;
}

void search_stats(Matcher *m, const Buffer *b, size_t y, size_t x, size_t cap, size_t *before,
                  size_t *total)
{
    size_t yy, n = 0, bef = 0;

    if (m->valid)
        for (yy = 0; yy < b->nlines && n < cap; yy++) {
            const Line *l = &b->lines[yy];
            size_t pos = 0, ms, me;

            if (l->len == 0)
                continue;
            if (m->regex)
                load_line(m, l->s, l->len);
            while (n < cap && pos <= l->len && find_core(m, l->s, l->len, pos, &ms, &me, NULL)) {
                n++;
                if (yy < y || (yy == y && ms < x))
                    bef++;
                pos = me;
            }
        }
    *before = bef;
    *total = n;
}

/* ---- replacing --------------------------------------------------------- */

void search_expand(const Matcher *m, const char *line, const regmatch_t *grp, const char *repl,
                   SBuf *out)
{
    const char *p;

    for (p = repl; *p; p++) {
        if (*p == '\\' && p[1]) {
            if (m->regex && isdigit((unsigned char)p[1])) {
                int g = p[1] - '0';

                if (grp && grp[g].rm_so >= 0)
                    sbuf_add(out, line + grp[g].rm_so, (size_t)(grp[g].rm_eo - grp[g].rm_so));
                p++;
                continue;
            }
            if (p[1] == 't') {
                sbuf_add(out, "\t", 1);
                p++;
                continue;
            }
            if (p[1] == '\\') {
                sbuf_add(out, "\\", 1);
                p++;
                continue;
            }
        }
        sbuf_add(out, p, 1);
    }
}

size_t search_replace_at(Buffer *b, Matcher *m, const Hit *h, const char *repl)
{
    const Line *l = &b->lines[h->y];
    regmatch_t grp[10];
    size_t ms, me;
    SBuf out = SBUF_INIT;
    size_t newend;

    if (!sm_find(m, l->s, l->len, h->x, &ms, &me, grp) || ms != h->x)
        return h->end;
    search_expand(m, l->s, grp, repl, &out);
    buf_delete(b, h->y, ms, me - ms);
    if (out.len > 0)
        buf_insert(b, h->y, ms, out.p, out.len);
    newend = ms + out.len;
    sbuf_free(&out);
    return newend;
}

size_t search_replace_all(Buffer *b, Matcher *m, const char *repl, Span *lim)
{
    Hit h;
    size_t count = 0, y = lim ? lim->y0 : 0, x = lim ? lim->x0 : 0;

    while (search_next(m, b, y, x, 1, 0, lim, &h, NULL)) {
        size_t oldlen = h.end - h.x, newend = search_replace_at(b, m, &h, repl);
        size_t newlen = newend - h.x;

        count++;
        if (lim && h.y == lim->y1)
            lim->x1 = lim->x1 + newlen - oldlen;
        y = h.y;
        x = newend;
    }
    return count;
}

/* ---- in files ---------------------------------------------------------- */

static void add_hit(FileHits *out, size_t max, const char *path, size_t line, size_t col,
                    const char *s, size_t len)
{
    FileHit *h;
    size_t i = 0, n;
    char *t;

    if (out->n >= max) {
        out->truncated = 1;
        return;
    }
    if (out->n == out->cap) {
        out->cap = out->cap ? out->cap * 2 : 64;
        out->v = xrealloc(out->v, out->cap * sizeof *out->v);
    }
    while (i < len && (s[i] == ' ' || s[i] == '\t'))
        i++;
    n = len - i;
    if (n > HIT_TEXT_MAX) {
        n = HIT_TEXT_MAX;
        while (n > 0 && ((unsigned char)s[i + n] & 0xC0) == 0x80)
            n--;
    }
    t = xmalloc(n + 1);
    memcpy(t, s + i, n);
    t[n] = '\0';
    for (i = 0; i < n; i++)
        if ((unsigned char)t[i] < 0x20 || t[i] == 0x7F)
            t[i] = ' ';
    h = &out->v[out->n++];
    h->path = xstrdup(path);
    h->line = line;
    h->col = col;
    h->text = t;
}

void search_text(Matcher *m, const char *path, const char *data, size_t len, FileHits *out,
                 size_t max)
{
    size_t pos = 0, line = 0;

    while (pos < len && !out->truncated) {
        const char *nl = memchr(data + pos, '\n', len - pos);
        size_t n = (size_t)((nl ? nl : data + len) - (data + pos)), ms, me;

        if (n > 0 && data[pos + n - 1] == '\r')
            n--;
        if (n > 0 && sm_find(m, data + pos, n, 0, &ms, &me, NULL))
            add_hit(out, max, path, line, ms, data + pos, n);
        line++;
        pos += (nl ? (size_t)(nl - (data + pos)) : len - pos) + 1;
    }
}

void search_buffer(Matcher *m, const char *path, const Buffer *b, FileHits *out, size_t max)
{
    size_t y, ms, me;

    for (y = 0; y < b->nlines && !out->truncated; y++) {
        const Line *l = &b->lines[y];

        if (l->len > 0 && sm_find(m, l->s, l->len, 0, &ms, &me, NULL))
            add_hit(out, max, path, y, ms, l->s, l->len);
    }
}

typedef struct {
    char **v;
    size_t n, cap;
} PathList;

static void collect_path(const char *rel, void *ctx)
{
    PathList *pl = ctx;

    if (pl->n == pl->cap) {
        pl->cap = pl->cap ? pl->cap * 2 : 256;
        pl->v = xrealloc(pl->v, pl->cap * sizeof *pl->v);
    }
    pl->v[pl->n++] = xstrdup(rel);
}

/* Whole file in memory, or NULL. */
static char *slurp(const char *path, size_t size, size_t *len)
{
    int fd = open(path, O_RDONLY);
    char *data;
    size_t got = 0;

    if (fd < 0)
        return NULL;
    data = xmalloc(size + 1);
    while (got < size) {
        ssize_t r = read(fd, data + got, size - got);

        if (r <= 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    *len = got;
    return data;
}

void search_dir(Matcher *m, const char *root, int (*skip)(unsigned long, unsigned long, void *),
                void *ctx, FileHits *out, size_t max)
{
    PathList pl = { NULL, 0, 0 };
    size_t i;

    files_scan(root, collect_path, &pl);
    for (i = 0; i < pl.n; i++) {
        char *full = files_join(root, pl.v[i]);
        struct stat st;

        if (!out->truncated && stat(full, &st) == 0 && S_ISREG(st.st_mode) &&
            st.st_size <= FILE_MAX_BYTES &&
            !(skip && skip((unsigned long)st.st_dev, (unsigned long)st.st_ino, ctx))) {
            size_t len;
            char *data = slurp(full, (size_t)st.st_size, &len);

            if (data) {
                if (!memchr(data, 0, len < 4096 ? len : 4096))
                    search_text(m, full, data, len, out, max);
                free(data);
            }
        }
        free(full);
        free(pl.v[i]);
    }
    free(pl.v);
}

void filehits_free(FileHits *h)
{
    size_t i;

    for (i = 0; i < h->n; i++) {
        free(h->v[i].path);
        free(h->v[i].text);
    }
    free(h->v);
    memset(h, 0, sizeof *h);
}

/* ---- go to line -------------------------------------------------------- */

int search_parse_goto(const char *s, size_t *line, size_t *col)
{
    char *end;
    unsigned long a = 0, b = 0;

    *line = *col = 0;
    while (*s == ' ' || *s == '\t')
        s++;
    if (!*s)
        return -1;
    if (isdigit((unsigned char)*s)) {
        a = strtoul(s, &end, 10);
        s = end;
    }
    if (*s == ':') {
        s++;
        if (!isdigit((unsigned char)*s))
            return -1;
        b = strtoul(s, &end, 10);
        s = end;
    }
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s)
        return -1;
    *line = (size_t)a;
    *col = (size_t)b;
    return 0;
}
