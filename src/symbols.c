#include "symbols.h"
#include "files.h"
#include "util.h"

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- patterns ----------------------------------------------------------- */

static int is_idc(unsigned char c)
{
    return isalnum(c) || c == '_' || c >= 0x80;
}

static int is_blank(char c)
{
    return c == ' ' || c == '\t';
}

/* Words that start a statement or an expression: never the name or the type of a definition. */
static int is_reject(const char *w, size_t n)
{
    static const char list[] =
        " if else elif elseif for foreach while switch catch return new throw delete typeof "
        "sizeof do try with match using case goto in of echo print yield await and or not as "
        "def class function fn var let const import from ";
    char tmp[34];

    if (n == 0)
        return 1;
    if (n > 12)                 /* longer than any word of the list */
        return 0;
    tmp[0] = ' ';
    memcpy(tmp + 1, w, n);
    tmp[n + 1] = ' ';
    tmp[n + 2] = '\0';
    return strstr(list, tmp) != NULL;
}

static void put(char *dst, size_t cap, const char *s, size_t n)
{
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, s, n);
    dst[n] = '\0';
}

/* Skips <...> on the line; i is at the '<'. */
static size_t skip_angle(const char *s, size_t len, size_t i)
{
    int depth = 0;
    size_t j;

    for (j = i; j < len; j++) {
        if (s[j] == '<')
            depth++;
        else if (s[j] == '>' && --depth == 0)
            return j + 1;
        else if (s[j] == ';' || s[j] == '{')
            break;
    }
    return i;
}

/* One literal item: alternatives a|b|c, each an identifier-like word or punctuation. */
static int lit_match(const char *p, size_t pl, const char *s, size_t len, size_t *i)
{
    size_t a = 0;

    while (a <= pl) {
        size_t e = a;

        while (e < pl && p[e] != '|')
            e++;
        if (e > a) {
            size_t al = e - a;

            if (*i + al <= len && memcmp(s + *i, p + a, al) == 0 &&
                (!is_idc((unsigned char)p[e - 1]) || *i + al == len ||
                 !is_idc((unsigned char)s[*i + al]))) {
                *i += al;
                return 1;
            }
        }
        a = e + 1;
    }
    return 0;
}

/* One qualified name at *pi (an optional "?" or leading "\\", then segments joined by "\\" or
 * "::"): *qs where it starts, *ls where its last segment starts. */
static int qual_name(const char *s, size_t len, size_t *pi, size_t *qs, size_t *ls)
{
    size_t i = *pi, st;

    if (i < len && s[i] == '?')
        i++;
    if (i < len && s[i] == '\\')
        i++;
    *qs = i;
    for (;;) {
        st = i;
        while (i < len && is_idc((unsigned char)s[i]))
            i++;
        if (i == st)
            return 0;
        *ls = st;
        if (i < len && s[i] == '\\')
            i++;
        else if (i + 1 < len && s[i] == ':' && s[i + 1] == ':' && i + 2 < len &&
                 is_idc((unsigned char)s[i + 2]))
            i += 2;
        else
            break;
    }
    *pi = i;
    return 1;
}

int pat_match(const char *pat, const char *s, size_t len, size_t from, PatMatch *m)
{
    const char *p = pat;
    size_t i = from;

    memset(m, 0, sizeof *m);
    while (*p) {
        const char *e;
        size_t il;

        while (*p == ' ')
            p++;
        if (!*p)
            break;
        for (e = p; *e && *e != ' '; e++)
            ;
        il = (size_t)(e - p);
        while (i < len && is_blank(s[i]))
            i++;
        if (il == 1 && *p == '@') {
            size_t st = i;

            if (i >= len || !is_idc((unsigned char)s[i]) || isdigit((unsigned char)s[i]))
                return 0;
            while (i < len && is_idc((unsigned char)s[i]))
                i++;
            if (is_reject(s + st, i - st))
                return 0;
            put(m->name, sizeof m->name, s + st, i - st);
        } else if (il == 1 && *p == '%') {
            size_t qs, ls, qe;
            int got = 0;

            /* a type, or a union (A|B|null) of which the first real type is kept */
            for (;;) {
                size_t from_bar = i;

                if (!qual_name(s, len, &i, &qs, &ls)) {
                    if (!got)
                        return 0;
                    i = from_bar - 1;
                    break;
                }
                qe = i;
                if (!is_reject(s + ls, qe - ls)) {
                    size_t n = qe - ls;
                    int nullish = (n == 4 && (memcmp(s + ls, "null", 4) == 0 ||
                                              memcmp(s + ls, "void", 4) == 0)) ||
                                  (n == 5 && memcmp(s + ls, "false", 5) == 0);

                    if (!got || (!nullish && m->type[0] != '\0' &&
                                 (strcmp(m->type, "null") == 0 || strcmp(m->type, "void") == 0 ||
                                  strcmp(m->type, "false") == 0))) {
                        put(m->type, sizeof m->type, s + ls, n);
                        put(m->qual, sizeof m->qual, s + qs, qe - qs);
                    }
                    got = 1;
                } else if (!got) {
                    return 0;
                }
                if (i < len && s[i] == '<')
                    i = skip_angle(s, len, i);
                if (i + 1 < len && s[i] == '|' &&
                    (is_idc((unsigned char)s[i + 1]) || s[i + 1] == '\\' || s[i + 1] == '?'))
                    i++;
                else
                    break;
            }
            while (i < len && (is_blank(s[i]) || s[i] == '*' || s[i] == '&'))
                i++;
        } else if (il == 1 && *p == '^') {
            int depth = 1;
            size_t st = i, k = 0;

            while (i < len) {
                if (s[i] == '(')
                    depth++;
                else if (s[i] == ')' && --depth == 0)
                    break;
                i++;
            }
            for (; st < i && k + 1 < sizeof m->params; st++) {
                if (is_blank(s[st]) && (k == 0 || m->params[k - 1] == ' '))
                    continue;
                m->params[k++] = is_blank(s[st]) ? ' ' : s[st];
            }
            while (k > 0 && m->params[k - 1] == ' ')
                k--;
            m->params[k] = '\0';
            if (i < len)
                i++;
        } else if (il == 1 && *p == '~') {
            size_t st;
            char q;

            while (i < len && s[i] != '\'' && s[i] != '"' && s[i] != '`')
                i++;
            if (i >= len)
                return 0;
            q = s[i++];
            st = i;
            while (i < len && s[i] != q)
                i++;
            if (i >= len)
                return 0;
            put(m->path, sizeof m->path, s + st, i - st);
            i++;
        } else if (il == 1 && *p == '&') {
            size_t st = i, k = 0, dots = 0;

            while (i < len && (is_idc((unsigned char)s[i]) || s[i] == '.'))
                i++;
            if (i == st)
                return 0;
            /* leading dots: one is the current directory, each more goes up */
            while (st < i && s[st] == '.') {
                st++;
                dots++;
            }
            if (dots > 0) {
                memcpy(m->path, "./", 2);
                k = 2;
                while (dots-- > 1 && k + 3 < sizeof m->path) {
                    memcpy(m->path + k, "../", 3);
                    k += 3;
                }
            }
            for (; st < i && k + 1 < sizeof m->path; st++)
                m->path[k++] = s[st] == '.' ? '/' : s[st];
            m->path[k] = '\0';
        } else if (il > 1 && p[0] == '!') {
            put(m->type, sizeof m->type, p + 1, il - 1);    /* sets the type: "x = 'a' !str" */
        } else if (il == 2 && p[0] == '$' && p[1] == '$') {
            if (i < len)
                return 0;               /* end of line */
        } else if (il > 1 && p[0] == '=') {
            size_t j = i;               /* look ahead without consuming */

            if (!lit_match(p + 1, il - 1, s, len, &j))
                return 0;
        } else if (il == 2 && p[0] == '<' && p[1] == '>') {
            if (i < len && s[i] == '<')
                i = skip_angle(s, len, i);
        } else {
            int mod = 0;
            size_t pl = il;

            if (is_idc((unsigned char)p[0]) && (p[il - 1] == '*' || p[il - 1] == '?')) {
                mod = p[il - 1];
                pl--;
            }
            if (mod) {
                /* a modifier word only counts when a name follows ("get x" but not "get(x)") */
                for (;;) {
                    size_t j = i, k;

                    if (!lit_match(p, pl, s, len, &j))
                        break;
                    k = j;
                    while (k < len && is_blank(s[k]))
                        k++;
                    if (k >= len || !(is_idc((unsigned char)s[k]) || s[k] == '$' || s[k] == '&' ||
                                      s[k] == '*' || s[k] == '\\' || s[k] == '?' || s[k] == '<'))
                        break;
                    i = k;
                    if (mod == '?')
                        break;
                }
            } else if (!lit_match(p, pl, s, len, &i)) {
                return 0;
            }
        }
        p = e;
    }
    m->end = i;
    return 1;
}

/* ---- building an index -------------------------------------------------- */

static long ix_add(SymIndex *x, int kind, const char *name, const char *sig, const char *ret,
                   long owner, size_t y)
{
    Sym *s;

    if (x->n == x->cap) {
        x->cap = x->cap ? x->cap * 2 : 64;
        x->v = xrealloc(x->v, x->cap * sizeof *x->v);
    }
    s = &x->v[x->n];
    s->name = xstrdup(name);
    s->sig = xstrdup(sig);
    s->ret = xstrdup(ret);
    s->kind = kind;
    s->owner = owner;
    s->hidden = 0;
    s->y0 = y;
    s->y1 = (size_t)-1;
    return (long)x->n++;
}

static void ix_inc(SymIndex *x, const char *spec)
{
    size_t i;

    for (i = 0; i < x->ninc; i++)
        if (strcmp(x->inc[i], spec) == 0)
            return;
    if (x->ninc >= 64)
        return;
    x->inc = xrealloc(x->inc, (x->ninc + 1) * sizeof *x->inc);
    x->inc[x->ninc++] = xstrdup(spec);
}

void sym_index_free(SymIndex *x)
{
    size_t i;

    if (!x)
        return;
    for (i = 0; i < x->n; i++) {
        free(x->v[i].name);
        free(x->v[i].sig);
        free(x->v[i].ret);
    }
    for (i = 0; i < x->ninc; i++) {
        free(x->inc[i]);
        if (x->incp)
            free(x->incp[i]);
    }
    free(x->v);
    free(x->inc);
    free(x->incp);
    free(x->key);
    free(x);
}

/* Reduces a type as written ("?Foo", "\App\Foo|null", "Foo[]") to a class name; "" if unknown. */
static void norm_type(const char *t, char *out, size_t cap)
{
    size_t i = 0, n = 0, st;

    out[0] = '\0';
    for (;;) {
        while (t[i] == '?' || t[i] == '\\' || t[i] == ' ')
            i++;
        st = i;
        while (is_idc((unsigned char)t[i]) || t[i] == '\\' || t[i] == '.' ||
               (t[i] == ':' && t[i + 1] == ':'))
            i += t[i] == ':' ? 2 : 1;
        n = i - st;
        if (n == 0 || t[i] == '[')
            return;
        /* the last segment of a qualified name */
        while (n > 0) {
            size_t k = n;

            while (k > 0 && t[st + k - 1] != '\\' && t[st + k - 1] != '.' && t[st + k - 1] != ':')
                k--;
            if (k == 0)
                break;
            st += k;
            n -= k;
        }
        if (!((n == 4 && memcmp(t + st, "null", 4) == 0) || (n == 4 && memcmp(t + st, "void", 4) == 0))) {
            put(out, cap, t + st, n);
            return;
        }
        if (t[i] != '|')
            return;
        i++;
    }
}

/* The type named by a @return / @var / @type tag in the comment lines right above line y. */
static void doc_type(const Buffer *b, size_t y, char *out, size_t cap)
{
    size_t k;

    out[0] = '\0';
    for (k = 1; k <= 12 && k <= y; k++) {
        const Line *l = &b->lines[y - k];
        size_t i = 0, j;

        while (i < l->len && is_blank(l->s[i]))
            i++;
        if (i >= l->len || (l->s[i] != '*' && l->s[i] != '/' && l->s[i] != '#'))
            break;
        for (j = i; j + 1 < l->len; j++) {
            static const char *const tags[] = { "return", "var", "type" };
            size_t t;

            if (l->s[j] != '@')
                continue;
            for (t = 0; t < 3; t++) {
                size_t tl = strlen(tags[t]), a = j + 1 + tl;

                if (a > l->len || memcmp(l->s + j + 1, tags[t], tl) != 0)
                    continue;
                while (a < l->len && (is_blank(l->s[a]) || l->s[a] == '{'))
                    a++;
                {
                    char tmp[PM_NAME];
                    size_t e = a, n = 0;

                    while (e < l->len && n + 1 < sizeof tmp &&
                           (is_idc((unsigned char)l->s[e]) || strchr("\\|?[]<>.:", l->s[e])))
                        tmp[n++] = l->s[e++];
                    tmp[n] = '\0';
                    norm_type(tmp, out, cap);
                    if (out[0])
                        return;
                }
            }
        }
    }
}

enum { F_GLOBAL, F_CLASS, F_OTHER };

typedef struct {
    int kind;
    long owner;                 /* the class a block belongs to (also for method bodies) */
    long indent;
} Frame;

#define MAX_FRAMES 48
#define DOC_MAX 96
static const char *last_segment(const char *q);
static int token_start(const char *s, size_t i);

typedef struct {
    SymIndex *x;
    const Buffer *b;
    const SynRule *r;
    size_t nr;
    int indent;
    Frame fr[MAX_FRAMES];
    size_t nf, over;
    int pend;                   /* the next '{' opens this kind of block (-1: none) */
    long pend_owner;
    size_t last_code;
    struct {                    /* members declared in the doc comment above the next class */
        int method;             /* 1 method, 0 property, 2 @see (sig: the qualified class) */
        char name[PM_NAME], sig[PM_PARAMS], ret[PM_NAME];
        size_t y;
    } doc[DOC_MAX];
    size_t ndoc;
    int has_doc;                /* the language has docmember rules */
} Build;

static void push(Build *B, int kind, long owner, long indent)
{
    if (B->nf < MAX_FRAMES) {
        B->fr[B->nf].kind = kind;
        B->fr[B->nf].owner = owner;
        B->fr[B->nf].indent = indent;
        B->nf++;
    } else {
        B->over++;
    }
}

/* Closes a block at line y; after is where the text following its '}' starts (typedef names). */
static void pop(Build *B, size_t y, const char *s, size_t len, size_t after)
{
    Frame f;

    if (B->over) {
        B->over--;
        return;
    }
    if (B->nf <= 1)
        return;
    f = B->fr[--B->nf];
    if (f.kind == F_CLASS && f.owner >= 0) {
        Sym *c = &B->x->v[f.owner];

        c->y1 = y;
        if (c->name[0] == '\0' && s) {
            PatMatch m;

            if (pat_match("@", s, len, after, &m)) {
                free(c->name);
                c->name = xstrdup(m.name);
            }
        }
    }
}

static void add_prop(Build *B, long owner, const char *name, const char *ret, size_t y)
{
    SymIndex *x = B->x;
    size_t i;

    for (i = (size_t)owner + 1; i < x->n; i++)
        if (x->v[i].owner == owner && x->v[i].kind == RULE_PROP &&
            strcmp(x->v[i].name, name) == 0) {
            if (ret[0] && !x->v[i].ret[0]) {
                free(x->v[i].ret);
                x->v[i].ret = xstrdup(ret);
            }
            return;
        }
    ix_add(x, RULE_PROP, name, "", ret, owner, y);
}

/* A definition starting at i. Returns its kind (RULE_*) or -1; *end is where it ends. */
static int try_decl(Build *B, size_t y, const char *s, size_t len, size_t i, size_t *end)
{
    const Frame *top = &B->fr[B->nf - 1];
    size_t k;
    PatMatch m;

    for (k = 0; k < B->nr; k++) {
        int kind = B->r[k].kind;
        long owner = top->kind == F_CLASS ? top->owner : -1;
        char ret[PM_NAME];

        if (kind > RULE_NS)
            continue;
        if ((kind == RULE_METHOD || kind == RULE_PROP) && top->kind != F_CLASS)
            continue;
        if (kind == RULE_VAR && top->kind != F_GLOBAL)
            continue;
        if (!pat_match(B->r[k].pat, s, len, i, &m))
            continue;
        if (kind != RULE_CLASS && kind != RULE_NS && m.name[0] == '\0')
            continue;
        *end = m.end;
        if (kind == RULE_NS) {
            B->pend = F_GLOBAL;
            B->pend_owner = -1;
            return kind;
        }
        if (kind == RULE_CLASS) {
            size_t d;

            B->pend_owner = ix_add(B->x, RULE_CLASS, m.name, "", m.type, -1, y);
            B->pend = F_CLASS;
            for (d = 0; d < B->ndoc; d++) {
                if (B->doc[d].method == 2) {
                    /* a Laravel facade forwards to the classes of its @see tags: they are
                     * loaded with the file and searched for members (nameless entry) */
                    char spec[PM_PARAMS + 1];

                    if (strcmp(m.type, "Facade") != 0)
                        continue;
                    spec[0] = ':';
                    put(spec + 1, PM_PARAMS, B->doc[d].sig, strlen(B->doc[d].sig));
                    ix_inc(B->x, spec);
                    ix_add(B->x, RULE_VAR, "", "@see", last_segment(B->doc[d].sig),
                           B->pend_owner, B->doc[d].y);
                } else if (B->doc[d].method)
                    ix_add(B->x, RULE_METHOD, B->doc[d].name, B->doc[d].sig, B->doc[d].ret,
                           B->pend_owner, B->doc[d].y);
                else
                    add_prop(B, B->pend_owner, B->doc[d].name, B->doc[d].ret, B->doc[d].y);
            }
            B->ndoc = 0;
            return kind;
        }
        B->ndoc = 0;
        if (kind == RULE_FUNC && owner >= 0)
            kind = RULE_METHOD;
        if (m.type[0])
            put(ret, sizeof ret, m.type, strlen(m.type));
        else
            doc_type(B->b, y, ret, sizeof ret);
        if (kind == RULE_PROP) {
            add_prop(B, owner, m.name, ret, y);
        } else {
            const char *sg = m.params;

            /* the receiver is not an argument the caller writes */
            if (strncmp(sg, "self", 4) == 0 && (sg[4] == ',' || sg[4] == '\0'))
                sg += sg[4] == ',' ? 5 : 4;
            while (*sg == ' ')
                sg++;
            {
                long ni = ix_add(B->x, kind, m.name, sg, ret, owner, y);
                size_t a;

                /* hidden from outside the class: a facade does not forward it */
                for (a = i; a < m.end && a < len; a++)
                    if (token_start(s, a) && (strncmp(s + a, "private", 7) == 0 ||
                                              strncmp(s + a, "protected", 9) == 0))
                        B->x->v[ni].hidden = 1;
            }
        }
        return kind;
    }
    return -1;
}

/* Includes and attributes (members created by assignments in methods) anywhere in a line. */
static int try_other(Build *B, size_t y, const char *s, size_t len, size_t i, size_t *end)
{
    const Frame *top = &B->fr[B->nf - 1];
    size_t k;
    PatMatch m;

    for (k = 0; k < B->nr; k++) {
        int kind = B->r[k].kind;

        if (kind != RULE_INCLUDE && kind != RULE_ATTR)
            continue;
        if (kind == RULE_ATTR && top->owner < 0)
            continue;
        if (!pat_match(B->r[k].pat, s, len, i, &m))
            continue;
        *end = m.end;
        if (kind == RULE_INCLUDE) {
            if (!m.path[0] && !m.type[0] && m.name[0] && m.name[0] >= 'A' && m.name[0] <= 'Z') {
                /* "Name ::": a class named by a static access (include @ ::) */
                put(m.type, sizeof m.type, m.name, strlen(m.name));
            }
            if (m.path[0]) {
                ix_inc(B->x, m.path);
            } else if (m.type[0]) {
                char spec[PM_PATH + 1];

                /* Foo:: names a class; self::, static:: and parent:: do not */
                if (strcmp(m.type, "self") == 0 || strcmp(m.type, "static") == 0 ||
                    strcmp(m.type, "parent") == 0)
                    continue;

                spec[0] = ':';
                put(spec + 1, sizeof spec - 1, m.qual[0] ? m.qual : m.type,
                    strlen(m.qual[0] ? m.qual : m.type));
                ix_inc(B->x, spec);
            } else {
                continue;
            }
        } else {
            if (m.name[0] == '\0')
                continue;
            add_prop(B, top->owner, m.name, m.type, y);
        }
        return 1;
    }
    return 0;
}

static int token_start(const char *s, size_t i);
static const char *last_segment(const char *q);

/* @method / @property tags in a comment line: the members of the class that follows. */
static void scan_doc(Build *B, size_t y, const char *s, size_t len, const uint8_t *cls)
{
    size_t i, k;
    PatMatch m;

    for (i = 0; i < len && B->ndoc < DOC_MAX; i++) {
        if (s[i] != '@' || cls[i] != SC_COMMENT)
            continue;
        for (k = 0; k < B->nr; k++) {
            int see;

            if (B->r[k].kind != RULE_DOC || !pat_match(B->r[k].pat, s, len, i, &m))
                continue;
            see = m.name[0] == '\0' && m.qual[0] != '\0';
            if (m.name[0] == '\0' && !see)
                continue;
            B->doc[B->ndoc].method = see ? 2 : strchr(B->r[k].pat, '(') != NULL;
            put(B->doc[B->ndoc].name, PM_NAME, m.name, strlen(m.name));
            put(B->doc[B->ndoc].sig, PM_PARAMS, see ? m.qual : m.params,
                strlen(see ? m.qual : m.params));
            put(B->doc[B->ndoc].ret, PM_NAME, m.type, strlen(m.type));
            B->doc[B->ndoc].y = y;
            B->ndoc++;
            break;
        }
    }
}

static int token_start(const char *s, size_t i)
{
    unsigned char c = (unsigned char)s[i];

    return (is_idc(c) || c == '#' || c == '$') && (i == 0 || !is_idc((unsigned char)s[i - 1]));
}

/* The rest of a line for the rules that may match anywhere. */
static void scan_rest(Build *B, size_t y, const char *s, size_t len, const uint8_t *cls, size_t i)
{
    while (i < len) {
        size_t end;

        if (cls[i] != SC_COMMENT && token_start(s, i) && try_other(B, y, s, len, i, &end) &&
            end > i) {
            i = end;
            continue;
        }
        i++;
    }
}

static void scan_braces(Build *B, size_t y, const char *s, size_t len, const uint8_t *cls)
{
    size_t i = 0;
    int stmt = 1;

    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        size_t end = 0;

        if (is_blank((char)c)) {
            i++;
        } else if (cls[i] == SC_COMMENT) {
            i++;
        } else if (cls[i] == SC_STRING) {
            while (i < len && cls[i] == SC_STRING)
                i++;
            stmt = 0;
        } else if (c == '{') {
            const Frame *top = &B->fr[B->nf - 1];

            if (B->pend >= 0)
                push(B, B->pend, B->pend_owner, 0);
            else
                push(B, F_OTHER, top->owner, 0);
            B->pend = -1;
            stmt = 1;
            i++;
        } else if (c == '}') {
            pop(B, y, s, len, i + 1);
            stmt = 1;
            i++;
        } else if (c == ';') {
            B->pend = -1;
            stmt = 1;
            i++;
        } else if (c == '<' && i + 1 < len && s[i + 1] == '?') {
            i += i + 4 < len && s[i + 2] == 'p' ? 5 : 2;     /* <?php, <?= */
            stmt = 1;
        } else if (c == '?' && i + 1 < len && s[i + 1] == '>') {
            i += 2;
            stmt = 1;
        } else {
            if (stmt && B->fr[B->nf - 1].kind != F_OTHER && B->over == 0 &&
                try_decl(B, y, s, len, i, &end) >= 0 && end > i) {
                i = end;
                stmt = 0;
                continue;
            }
            if (token_start(s, i) && try_other(B, y, s, len, i, &end) && end > i) {
                i = end;
                stmt = 0;
                continue;
            }
            if (is_idc(c))
                while (i < len && is_idc((unsigned char)s[i]))
                    i++;
            else
                i++;
            stmt = 0;
        }
    }
}

static void scan_indent(Build *B, size_t y, const char *s, size_t len, const uint8_t *cls)
{
    size_t first = 0, end = 0;
    long col = 0;
    int kind = -1;

    while (first < len && is_blank(s[first])) {
        col = s[first] == '\t' ? (col / 8 + 1) * 8 : col + 1;
        first++;
    }
    if (first >= len || cls[first] == SC_COMMENT || cls[first] == SC_STRING)
        return;
    while (B->nf > 1 && B->fr[B->nf - 1].indent >= col)
        pop(B, B->last_code, NULL, 0, 0);
    B->last_code = y;
    B->pend = -1;
    if (B->fr[B->nf - 1].kind != F_OTHER)
        kind = try_decl(B, y, s, len, first, &end);
    if (kind == RULE_CLASS) {
        push(B, F_CLASS, B->pend_owner, col);
    } else if (kind == RULE_FUNC || kind == RULE_METHOD) {
        push(B, F_OTHER, B->fr[B->nf - 1].owner, col);
    }
    B->pend = -1;
    scan_rest(B, y, s, len, cls, kind >= 0 && end > first ? end : first);
}

SymIndex *sym_build(const Buffer *b, Syntax *syn)
{
    SymIndex *x = xmalloc(sizeof *x);
    Build B;
    size_t y, i, clscap = 0;
    uint16_t st = 0;
    uint8_t *cls = NULL;

    memset(x, 0, sizeof *x);
    memset(&B, 0, sizeof B);
    B.x = x;
    B.b = b;
    B.r = syn_rules(syn, &B.nr);
    for (i = 0; i < B.nr; i++)
        if (B.r[i].kind == RULE_DOC)
            B.has_doc = 1;
    B.indent = syn_indent_scope(syn);
    B.pend = -1;
    B.nf = 1;
    B.fr[0].kind = F_GLOBAL;
    B.fr[0].owner = -1;
    B.fr[0].indent = -1;
    if (B.nr > 0) {
        for (y = 0; y < b->nlines; y++) {
            const Line *l = &b->lines[y];

            if (l->len + 1 > clscap) {
                clscap = l->len + 64;
                cls = xrealloc(cls, clscap);
            }
            st = syn_line(syn, st, l->s, l->len, cls);
            if (B.has_doc && l->len && memchr(l->s, '@', l->len))
                scan_doc(&B, y, l->s, l->len, cls);
            if (B.indent)
                scan_indent(&B, y, l->s, l->len, cls);
            else
                scan_braces(&B, y, l->s, l->len, cls);
        }
    }
    free(cls);
    for (i = 0; i < x->n; i++)
        if (x->v[i].y1 == (size_t)-1)
            x->v[i].y1 = B.indent ? B.last_code : b->nlines;
    x->gen = b->gen;
    return x;
}

/* ---- cache -------------------------------------------------------------- */

enum { LIB_PHP, LIB_C, LIB_CPP, LIB_PY, LIB_JS, LIB_N };
static const char *const lib_ids[LIB_N] = { "php", "c", "cpp", "py", "js" };

#define CACHE_MAX 48
#define SET_MAX 40
#define OPEN_MAX 8
#define INC_DEPTH 3
#define FILE_MAX_BYTES (2L * 1024 * 1024)
#define PROJECT_FILES_MAX 40000

struct SymCache {
    SymIndex **v;
    size_t n;
    unsigned long tick;
    unsigned long pin;              /* indexes used since this tick belong to the Env being built */
    char **files;
    size_t nfiles;
    int files_done;
    char **al_name, **al_qual;      /* class aliases of config/app.php (Laravel facades) */
    size_t nal;
    int al_done;
    char **ps_prefix, **ps_dir;     /* Composer's PSR-4 map: namespace prefix -> directory */
    size_t nps;
    int ps_done;
    char **cls;                     /* qualified names of the project's classes */
    size_t ncls, clscap;
    int cls_done;
    int cl_phase;                   /* load_classes: 0 start, 1 classmap, 2 PSR-4 directories */
    FILE *cl_file;                  /* the classmap being read */
    size_t cl_opt, cl_i;            /* its entries; the PSR-4 directory being walked */
    FScan *cl_fs;
    FScan *files_fs;                /* the walk that fills files */
    long deadline_us;               /* scans stop here (0: no limit) */
    int busy;                       /* a scan was cut short since sym_budget */
    char root[PATH_MAX];            /* the project (where vendor/ or composer.json is); "" = cwd */
    StdLib *lib[LIB_N];
    int lib_tried[LIB_N];
    struct Hdr *hdr;                /* C: the headers of the project and their top-level names */
    size_t nhdr;
};

/* A header of the project, kept while its modification time is the same: only what a file that
 * includes it can use (functions, types, macros, variables at the top level). */
typedef struct {
    char *name, *sig, *ret;
    int kind;
    int def;                        /* JS/TS: the default export of the module */
    char *ns;                       /* PHP: the namespace it is declared in (NULL: global) */
} HSym;

struct Hdr {
    char *path;                     /* real path */
    long mtime, size, checked;
    HSym *v;
    size_t n;
};

SymCache *sym_cache_new(void)
{
    SymCache *c = xmalloc(sizeof *c);

    memset(c, 0, sizeof *c);
    return c;
}

/* What was read from the project (file list, aliases, PSR-4 map, class names). */
static void free_project(SymCache *c)
{
    size_t i;

    for (i = 0; i < c->nfiles; i++)
        free(c->files[i]);
    for (i = 0; i < c->nal; i++) {
        free(c->al_name[i]);
        free(c->al_qual[i]);
    }
    for (i = 0; i < c->nps; i++) {
        free(c->ps_prefix[i]);
        free(c->ps_dir[i]);
    }
    for (i = 0; i < c->ncls; i++)
        free(c->cls[i]);
    for (i = 0; i < c->nhdr; i++) {
        size_t k;

        for (k = 0; k < c->hdr[i].n; k++) {
            free(c->hdr[i].v[k].name);
            free(c->hdr[i].v[k].sig);
            free(c->hdr[i].v[k].ret);
            free(c->hdr[i].v[k].ns);
        }
        free(c->hdr[i].v);
        free(c->hdr[i].path);
    }
    free(c->hdr);
    c->hdr = NULL;
    c->nhdr = 0;
    files_scan_free(c->files_fs);
    files_scan_free(c->cl_fs);
    if (c->cl_file)
        fclose(c->cl_file);
    c->files_fs = c->cl_fs = NULL;
    c->cl_file = NULL;
    c->cl_phase = 0;
    c->cl_opt = c->cl_i = 0;
    free(c->files);
    free(c->al_name);
    free(c->al_qual);
    free(c->ps_prefix);
    free(c->ps_dir);
    free(c->cls);
    c->files = NULL;
    c->al_name = c->al_qual = c->ps_prefix = c->ps_dir = c->cls = NULL;
    c->nfiles = c->nal = c->nps = c->ncls = c->clscap = 0;
    c->files_done = c->al_done = c->ps_done = c->cls_done = 0;
}

/* The project of the file at key: the nearest directory above it with Composer's vendor/ or a
 * composer.json, else the one with .git, so that the lookups work whatever directory the editor
 * was started in. */
static void set_root(SymCache *c, const char *key)
{
    char dir[PATH_MAX], probe_path[PATH_MAX + 40];
    char *sl;
    struct stat st;
    char found[PATH_MAX];

    found[0] = '\0';
    if (key && key[0] == '/') {
        snprintf(dir, sizeof dir, "%s", key);
        while ((sl = strrchr(dir, '/')) != NULL) {
            *sl = '\0';
            snprintf(probe_path, sizeof probe_path, "%s/vendor/composer/autoload_psr4.php", dir);
            if (stat(probe_path, &st) == 0) {
                snprintf(found, sizeof found, "%s", dir);
                break;
            }
            snprintf(probe_path, sizeof probe_path, "%s/composer.json", dir);
            if (stat(probe_path, &st) == 0) {
                snprintf(found, sizeof found, "%s", dir);
                break;
            }
            if (dir[0] == '\0')
                break;
        }
        /* no Composer: the folder of the Git repository (a C project, for its headers) */
        if (!found[0]) {
            snprintf(dir, sizeof dir, "%s", key);
            while ((sl = strrchr(dir, '/')) != NULL) {
                *sl = '\0';
                snprintf(probe_path, sizeof probe_path, "%s/.git", dir);
                if (stat(probe_path, &st) == 0) {
                    snprintf(found, sizeof found, "%s", dir);
                    break;
                }
                if (dir[0] == '\0')
                    break;
            }
        }
    }
    if (strcmp(found, c->root) == 0)
        return;
    free_project(c);
    memcpy(c->root, found, strlen(found) + 1);
}

/* rel (relative to the project) as a path usable from the working directory. */
static void rooted(const SymCache *c, char *out, size_t n, const char *rel)
{
    size_t rl = strlen(c->root), l = strlen(rel);

    if (rl + 1 + l + 1 > n) {
        out[0] = '\0';
        return;
    }
    if (rl) {
        memcpy(out, c->root, rl);
        out[rl++] = '/';
    }
    memcpy(out + rl, rel, l + 1);
}

void sym_cache_free(SymCache *c)
{
    size_t i;

    if (!c)
        return;
    for (i = 0; i < c->n; i++)
        sym_index_free(c->v[i]);
    free_project(c);
    for (i = 0; i < LIB_N; i++)
        std_close(c->lib[i]);
    free(c->v);
    free(c);
}

static long now_us(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000000L + t.tv_nsec / 1000;
}

void sym_budget(SymCache *c, long ms)
{
    c->busy = 0;
    c->deadline_us = ms > 0 ? now_us() + ms * 1000L : 0;
}

int sym_busy(const SymCache *c)
{
    return c->busy;
}

/* Microseconds a scan may still run: 0 for no limit, -1 when the time is up. */
static long scan_left(const SymCache *c)
{
    long l;

    if (!c->deadline_us)
        return 0;
    l = c->deadline_us - now_us();
    return l > 0 ? l : -1;
}

/* Keeps x; when the cache is full, the index of a file read from disk that was used least
 * recently goes. Never one of an open document, nor one the Env being built holds (used since
 * c->pin): with none left to drop, the cache grows for this build. */
static void cache_put(SymCache *c, SymIndex *x)
{
    size_t i, victim = (size_t)-1;

    if (c->n >= CACHE_MAX) {
        unsigned long best = (unsigned long)-1;

        for (i = 0; i < c->n; i++)
            if (!c->v[i]->buf && c->v[i]->used < c->pin && c->v[i]->used < best) {
                best = c->v[i]->used;
                victim = i;
            }
        if (victim != (size_t)-1) {
            sym_index_free(c->v[victim]);
            c->v[victim] = x;
            return;
        }
    }
    c->v = xrealloc(c->v, (c->n + 1) * sizeof *c->v);
    c->v[c->n++] = x;
}

static void carry_resolved(SymIndex *nx, const SymIndex *old)
{
    size_t i, k;

    if (!old || !old->incp)
        return;
    nx->incp = xmalloc((nx->ninc ? nx->ninc : 1) * sizeof *nx->incp);
    memset(nx->incp, 0, (nx->ninc ? nx->ninc : 1) * sizeof *nx->incp);
    nx->resolved = 1;
    for (i = 0; i < nx->ninc; i++) {
        for (k = 0; k < old->ninc; k++)
            if (strcmp(nx->inc[i], old->inc[k]) == 0) {
                nx->incp[i] = old->incp[k] ? xstrdup(old->incp[k]) : NULL;
                break;
            }
        if (k == old->ninc)
            nx->resolved = 0;
    }
}

/* The index of an open document, rebuilt when its text changed (not on every keystroke of a
 * big file: a rebuild is allowed once it is 20 times cheaper than the time since the last). */
static SymIndex *index_of_doc(SymCache *c, Doc *d)
{
    SymIndex *x = NULL, *nx;
    size_t i;
    long t = now_us();

    for (i = 0; i < c->n; i++)
        if (c->v[i]->buf == &d->buf) {
            x = c->v[i];
            break;
        }
    if (x && (x->gen == d->buf.gen || (x->built_us >= 3000 && t - x->last_us < 20 * x->built_us))) {
        x->used = ++c->tick;
        return x;
    }
    nx = sym_build(&d->buf, d->syn);
    nx->buf = &d->buf;
    nx->built_us = now_us() - t;
    nx->last_us = now_us();
    nx->used = ++c->tick;
    if (x) {
        nx->key = x->key;
        x->key = NULL;
        carry_resolved(nx, x);
        for (i = 0; i < c->n; i++)
            if (c->v[i] == x)
                c->v[i] = nx;
        sym_index_free(x);
    } else {
        char *rp = d->buf.path ? realpath(d->buf.path, NULL) : NULL;

        nx->key = rp;
        cache_put(c, nx);
    }
    return nx;
}

/* A file read from disk, kept while its modification time is the same. */
static SymIndex *index_of_path(SymCache *c, const char *real)
{
    SymIndex *x = NULL, *nx;
    struct stat st;
    Buffer b;
    char err[128];
    size_t i;
    long now = (long)time(NULL);

    for (i = 0; i < c->n; i++)
        if (c->v[i]->key && strcmp(c->v[i]->key, real) == 0) {
            x = c->v[i];
            break;
        }
    if (x && (x->buf || now - x->checked < 2)) {
        x->used = ++c->tick;
        return x;
    }
    if (stat(real, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > FILE_MAX_BYTES)
        return x;
    if (x && x->mtime == (long)st.st_mtime && x->size == (long)st.st_size) {
        x->checked = now;
        x->used = ++c->tick;
        return x;
    }
    buf_init(&b);
    if (buf_load(&b, real, err, sizeof err) != 0) {
        buf_free(&b);
        return x;
    }
    {
        Syntax *syn = syn_detect(&b);        /* a fresh one: it is ours to free */

        nx = sym_build(&b, syn);
        syn_free(syn);
    }
    buf_free(&b);
    nx->key = xstrdup(real);
    nx->mtime = (long)st.st_mtime;
    nx->size = (long)st.st_size;
    nx->checked = now;
    nx->used = ++c->tick;
    if (x) {
        carry_resolved(nx, x);
        for (i = 0; i < c->n; i++)
            if (c->v[i] == x)
                c->v[i] = nx;
        sym_index_free(x);
    } else {
        cache_put(c, nx);
    }
    return nx;
}

/* ---- includes ----------------------------------------------------------- */

static const char *ext_of(const char *path)
{
    const char *dot = path ? strrchr(path, '.') : NULL;
    const char *slash = path ? strrchr(path, '/') : NULL;

    return dot && (!slash || dot > slash) ? dot + 1 : "";
}

static void collect_file(const char *rel, void *ctx)
{
    SymCache *c = ctx;

    if (c->nfiles >= PROJECT_FILES_MAX)
        return;
    c->files = xrealloc(c->files, (c->nfiles + 1) * sizeof *c->files);
    c->files[c->nfiles++] = xstrdup(rel);
}

/* The files of the project (without vendor/). The walk goes on a little at each call: 0 while it
 * is not over (c->busy is set), 1 when c->files is complete. */
static int ensure_files(SymCache *c)
{
    static const char *const prune[] = { "vendor", NULL };
    long left;

    if (c->files_done)
        return 1;
    /* vendor is Composer's business (its map); walking it can take seconds */
    left = scan_left(c);
    if (left < 0) {
        c->busy = 1;
        return 0;
    }
    if (!c->files_fs) {
        char top[PATH_MAX];

        rooted(c, top, sizeof top, ".");
        c->files_fs = files_scan_begin(top, prune);
    }
    if (!files_scan_step(c->files_fs, collect_file, c, left)) {
        c->busy = 1;
        return 0;
    }
    files_scan_free(c->files_fs);
    c->files_fs = NULL;
    c->files_done = 1;
    return 1;
}

/* The last segment of a qualified name (A\\B\\Foo, A::Foo). */
static const char *last_segment(const char *q)
{
    const char *p, *r = q;

    for (p = q; *p; p++)
        if (*p == '\\' || *p == ':' || *p == '.')
            r = p + 1;
    return r;
}

/* How many trailing directories of path (before base) match the namespace of qual, ignoring
 * case: Illuminate\\Support\\Facades\\Route against .../Illuminate/Support/Facades/Route.php. */
static int ns_score(const char *path, const char *base, const char *qual)
{
    const char *seg[16];
    size_t len[16], n = 0, k;
    const char *p = qual, *name = last_segment(qual), *st = qual;
    int score = 0;
    const char *pe = base;

    for (; p < name && n < 16; p++)
        if (*p == '\\' || *p == ':' || *p == '.') {
            if (p > st) {
                seg[n] = st;
                len[n++] = (size_t)(p - st);
            }
            st = p + 1;
        }
    while (n > 0 && pe > path) {
        const char *ps = pe - 1;                /* pe is just after a '/' */
        size_t dl;

        while (ps > path && ps[-1] != '/')
            ps--;
        dl = (size_t)(pe - 1 - ps);
        n--;
        if (dl != len[n])
            break;
        for (k = 0; k < dl; k++)
            if (tolower((unsigned char)ps[k]) != tolower((unsigned char)seg[n][k]))
                return score;
        score++;
        pe = ps;
    }
    return score;
}

/* Composer's vendor/composer/autoload_psr4.php: 'App\\' => array($baseDir . '/app'), read once. */
static void load_psr4(SymCache *c)
{
    FILE *f;
    char line[1024];

    if (c->ps_done)
        return;
    c->ps_done = 1;
    rooted(c, line, sizeof line, "vendor/composer/autoload_psr4.php");
    f = fopen(line, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *p = line, *q, prefix[PM_PATH];
        size_t n = 0;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != '\'')
            continue;
        for (p++; *p && *p != '\'' && n + 1 < sizeof prefix; p++) {
            if (*p == '\\' && p[1] == '\\')
                p++;                    /* written doubled in PHP source */
            prefix[n++] = *p;
        }
        prefix[n] = '\0';
        if (*p != '\'')
            continue;
        for (q = strstr(p, "=>"); q && *q; ) {
            char *a = strstr(q, "Dir . '"), *b;
            char dir[PATH_MAX];

            if (!a)
                break;
            b = strchr(a + 7, '\'');
            if (!b)
                break;
            /* $vendorDir or $baseDir */
            if (a >= line + 6 && memcmp(a - 6, "vendor", 6) == 0)
                snprintf(dir, sizeof dir, "vendor%.*s", (int)(b - a - 7), a + 7);
            else
                snprintf(dir, sizeof dir, ".%.*s", (int)(b - a - 7), a + 7);
            c->ps_prefix = xrealloc(c->ps_prefix, (c->nps + 1) * sizeof *c->ps_prefix);
            c->ps_dir = xrealloc(c->ps_dir, (c->nps + 1) * sizeof *c->ps_dir);
            c->ps_prefix[c->nps] = xstrdup(prefix);
            c->ps_dir[c->nps++] = xstrdup(dir);
            q = b + 1;
        }
    }
    fclose(f);
}

/* The file of a namespaced class by the PSR-4 map: every matching prefix is tried, the longest
 * first (as Composer does). */
static char *psr4_file(SymCache *c, const char *qual)
{
    size_t i, last = (size_t)-1;

    load_psr4(c);
    for (;;) {
        size_t best = (size_t)-1, bl = 0;

        for (i = 0; i < c->nps; i++) {
            size_t pl = strlen(c->ps_prefix[i]);

            if (pl <= strlen(qual) && strncmp(c->ps_prefix[i], qual, pl) == 0 &&
                (last == (size_t)-1 || pl < last) && (best == (size_t)-1 || pl > bl)) {
                best = i;
                bl = pl;
            }
        }
        if (best == (size_t)-1)
            return NULL;
        for (i = 0; i < c->nps; i++) {
            char path[PATH_MAX], *r;
            struct stat st;
            size_t k, n;

            if (strlen(c->ps_prefix[i]) != bl || strcmp(c->ps_prefix[i], c->ps_prefix[best]) != 0)
                continue;
            rooted(c, path, sizeof path, c->ps_dir[i]);
            n = strlen(path);
            path[n++] = '/';
            for (k = bl; qual[k] && n + 5 < sizeof path; k++)
                path[n++] = qual[k] == '\\' ? '/' : qual[k];
            memcpy(path + n, ".php", 5);
            if (stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
                (r = realpath(path, NULL)) != NULL)
                return r;
        }
        last = bl;
    }
}

/* A project file called Name.<ext>, for a class mentioned by name (PHP `use`): when qual has
 * a namespace, the file whose directories best match it. */
static char *find_class_file(SymCache *c, const char *qual, const char *ext)
{
    size_t i, el = strlen(ext);
    const char *name = last_segment(qual);
    size_t nl = strlen(name);
    long best = -1;
    int best_score = -1;

    if (strcmp(ext, "php") == 0 && strchr(qual, '\\')) {
        char *f = psr4_file(c, qual);

        if (f)
            return f;
    }
    if (!ensure_files(c))
        return NULL;
    for (i = 0; i < c->nfiles; i++) {
        const char *f = c->files[i], *base = strrchr(f, '/');
        size_t bl;
        int sc;

        base = base ? base + 1 : f;
        bl = strlen(base);
        if (!(bl == nl + 1 + el && memcmp(base, name, nl) == 0 && base[nl] == '.' &&
              memcmp(base + nl + 1, ext, el) == 0))
            continue;
        sc = ns_score(f, base, qual);
        if (sc > best_score) {
            best_score = sc;
            best = (long)i;
        }
    }
    if (best < 0)
        return NULL;
    {
        char path[PATH_MAX];

        rooted(c, path, sizeof path, c->files[best]);
        return realpath(path, NULL);
    }
}

static char *probe(const char *dir, const char *spec, const char *ext)
{
    static const char *const web[] = { "", ".ts", ".tsx", ".js", ".jsx", ".mjs", "/index.ts",
                                       "/index.js", NULL };
    static const char *const py[] = { "", ".py", "/__init__.py", NULL };
    static const char *const php[] = { "", ".php", NULL };
    static const char *const plain[] = { "", NULL };
    const char *const *sfx = plain;
    const char *pre[4];
    size_t a, b, np = 0;
    char path[PATH_MAX];

    if (strcmp(ext, "py") == 0)
        sfx = py;
    else if (strcmp(ext, "php") == 0 || strcmp(ext, "phtml") == 0)
        sfx = php;
    else if (strncmp(ext, "js", 2) == 0 || strncmp(ext, "ts", 2) == 0 || strcmp(ext, "mjs") == 0 ||
             strcmp(ext, "cjs") == 0 || strcmp(ext, "mts") == 0)
        sfx = web;
    pre[np++] = dir;
    pre[np++] = ".";
    if (strcmp(ext, "h") == 0 || strcmp(ext, "c") == 0 || strncmp(ext, "c", 1) == 0 ||
        strncmp(ext, "h", 1) == 0) {
        pre[np++] = "include";
    }
    for (a = 0; a < np; a++)
        for (b = 0; sfx[b]; b++) {
            struct stat st;

            if (snprintf(path, sizeof path, "%s%s%s%s", pre[a], spec[0] == '/' ? "" : "/", spec,
                         sfx[b]) >= (int)sizeof path)
                continue;
            if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
                return realpath(path, NULL);
        }
    return NULL;
}

static void resolve_includes(SymIndex *x)
{
    size_t i;
    char dir[PATH_MAX];
    const char *ext;

    if (x->resolved)
        return;
    if (!x->incp) {
        x->incp = xmalloc((x->ninc ? x->ninc : 1) * sizeof *x->incp);
        memset(x->incp, 0, (x->ninc ? x->ninc : 1) * sizeof *x->incp);
    }
    ext = ext_of(x->key);
    strcpy(dir, ".");
    if (x->key) {
        char *sl;

        snprintf(dir, sizeof dir, "%s", x->key);
        sl = strrchr(dir, '/');
        if (sl)
            *sl = '\0';
    }
    for (i = 0; i < x->ninc; i++) {
        if (x->incp[i])
            continue;
        if (x->inc[i][0] != ':')
            x->incp[i] = probe(dir, x->inc[i], ext);
    }
    x->resolved = 1;
}

/* ---- the world visible from a document ---------------------------------- */

typedef struct {
    SymIndex *v[SET_MAX];
    int depth[SET_MAX];
    size_t n;
    StdLib *lib[2];
    size_t nlib;
    const Doc *cur;             /* for the aliases written in it */
    SymCache *c;
} Env;

static void env_add(Env *e, SymIndex *x, int depth)
{
    size_t i;

    if (!x || e->n >= SET_MAX)
        return;
    if (e->c)
        x->used = ++e->c->tick;         /* held by this Env: not to be dropped while it is built */
    for (i = 0; i < e->n; i++)
        if (e->v[i] == x || (x->key && e->v[i]->key && strcmp(x->key, e->v[i]->key) == 0))
            return;
    e->v[e->n] = x;
    e->depth[e->n++] = depth;
}

static StdLib *lib_get(SymCache *c, int id)
{
    if (!c->lib_tried[id]) {
        c->lib_tried[id] = 1;
        c->lib[id] = std_open(lib_ids[id]);
    }
    return c->lib[id];
}

int sym_is_cxx(const Doc *d)
{
    static const char *const cxx[] = { "cc", "cpp", "cxx", "hpp", "hh", "hxx", "c++", NULL };
    const char *ext = ext_of(d->buf.path);
    size_t i, y;

    for (i = 0; cxx[i]; i++)
        if (strcmp(ext, cxx[i]) == 0)
            return 1;
    if (strcmp(ext, "c") == 0)
        return 0;
    for (y = 0; y < d->buf.nlines && y < 200; y++) {
        const Line *l = &d->buf.lines[y];
        size_t k;

        for (k = 0; k + 5 < l->len; k++)
            if (l->s[k] == 's' && memcmp(l->s + k, "std::", 5) == 0)
                return 1;
        if (l->len > 9 && memcmp(l->s, "namespace", 9) == 0)
            return 1;
        if (l->len > 8 && memcmp(l->s, "template", 8) == 0)
            return 1;
    }
    return 0;
}

static int env_has_class(const Env *e, const char *name);

/* Adds the files included by e->v[k]. A class mentioned by name (PHP `use`, `new`) is looked
 * for in the project only when nothing loaded so far defines it. */
static void env_includes(SymCache *c, Env *e, size_t k)
{
    SymIndex *x = e->v[k];
    size_t j;

    resolve_includes(x);
    for (j = 0; j < x->ninc; j++) {
        if (x->inc[j][0] == ':' && !x->incp[j] &&
            !env_has_class(e, last_segment(x->inc[j] + 1))) {
            char *f = find_class_file(c, x->inc[j] + 1, ext_of(x->key));

            if (!f && !c->files_done)
                continue;               /* the walk is not over: ask again next time */
            x->incp[j] = f ? f : xstrdup("");
        }
        if (x->incp[j] && x->incp[j][0])
            env_add(e, index_of_path(c, x->incp[j]), e->depth[k] + 1);
    }
}

static void env_build(SymCache *c, Doc *const *docs, size_t ndocs, Doc *cur, Env *e)
{
    size_t i, k, others = 0;
    const char *lang = syn_name(cur->syn);

    memset(e, 0, sizeof *e);
    e->cur = cur;
    e->c = c;
    c->pin = c->tick + 1;
    if (strcmp(lang, "PHP") == 0 || strcmp(lang, "PHP/HTML") == 0) {
        e->lib[e->nlib++] = lib_get(c, LIB_PHP);
    } else if (strcmp(lang, "Python") == 0) {
        e->lib[e->nlib++] = lib_get(c, LIB_PY);
    } else if (strcmp(lang, "JavaScript") == 0 || strcmp(lang, "TypeScript") == 0) {
        e->lib[e->nlib++] = lib_get(c, LIB_JS);
    } else if (strcmp(lang, "C") == 0) {
        if (sym_is_cxx(cur))
            e->lib[e->nlib++] = lib_get(c, LIB_CPP);
        e->lib[e->nlib++] = lib_get(c, LIB_C);
    }
    env_add(e, index_of_doc(c, cur), 0);
    set_root(c, e->v[0]->key);
    for (i = 0; i < ndocs && others < OPEN_MAX; i++)
        if (docs[i] != cur && docs[i]->syn == cur->syn && !docs[i]->scratch) {
            env_add(e, index_of_doc(c, docs[i]), 1);
            others++;
        }
    /* breadth first: the files included by the file, by those, up to INC_DEPTH levels */
    for (k = 0; k < e->n; k++)
        if (e->depth[k] < INC_DEPTH)
            env_includes(c, e, k);
}

typedef struct {
    SymFn fn;
    void *ctx;
    int member;
} StdCtx;

static void std_cb(void *ctx, const char *name, size_t nl, const char *params, size_t pl,
                   const char *ret, size_t rl, int kind)
{
    StdCtx *s = ctx;
    char n[PM_NAME], p[PM_PARAMS], r[PM_NAME];
    int k;

    put(n, sizeof n, name, nl);
    put(p, sizeof p, params, pl);
    put(r, sizeof r, ret, rl);
    switch (kind) {
    case STD_FUNC:
        k = s->member ? RULE_METHOD : RULE_FUNC;
        break;
    case STD_CONST:
        k = RULE_CONST;
        break;
    case STD_CLASS:
        k = RULE_CLASS;
        break;
    default:
        k = s->member ? RULE_PROP : RULE_VAR;
        break;
    }
    s->fn(s->ctx, n, k, p, r, 2);
}

/* Members of class cls from every index and library, then those of its base classes. */
static int members_of(const Env *e, const char *cls, SymFn fn, void *ctx, int depth, int pub)
{
    size_t i, j, ci;
    int found = 0;
    StdCtx sc;

    if (depth > 8 || !cls[0])
        return 0;
    for (i = 0; i < e->n; i++) {
        const SymIndex *x = e->v[i];

        for (ci = 0; ci < x->n; ci++) {
            const Sym *c = &x->v[ci];

            if (c->kind != RULE_CLASS || strcmp(c->name, cls) != 0)
                continue;
            found = 1;
            for (j = ci + 1; j < x->n; j++) {
                if (x->v[j].owner != (long)ci)
                    continue;
                if (x->v[j].name[0]) {
                    if (pub && x->v[j].hidden)
                        continue;
                    fn(ctx, x->v[j].name, x->v[j].kind, x->v[j].sig, x->v[j].ret, i == 0 ? 0 : 1);
                } else if (strcmp(x->v[j].sig, "@see") == 0 && strcmp(x->v[j].ret, cls) != 0)
                    members_of(e, x->v[j].ret, fn, ctx, depth + 1, 1);
            }
            if (c->ret[0]) {
                char base[PM_NAME];

                norm_type(c->ret, base, sizeof base);
                if (strcmp(base, cls) != 0)
                    members_of(e, base, fn, ctx, depth + 1, pub);
            }
        }
    }
    sc.fn = fn;
    sc.ctx = ctx;
    sc.member = 1;
    for (i = 0; i < e->nlib; i++)
        if (e->lib[i] && std_class(e->lib[i], cls, strlen(cls))) {
            found = 1;
            std_members(e->lib[i], cls, strlen(cls), std_cb, &sc);
        }
    return found;
}

static int env_members(const Env *e, const char *cls, SymFn fn, void *ctx, int depth)
{
    return members_of(e, cls, fn, ctx, depth, 0);
}

typedef struct {
    const char *name;
    int kind;
    char sig[PM_PARAMS], ret[PM_NAME];
    int found;
} FindCtx;

static void find_cb(void *ctx, const char *name, int kind, const char *sig, const char *ret,
                    int rank)
{
    FindCtx *f = ctx;

    (void)rank;
    if (f->found || strcmp(name, f->name) != 0)
        return;
    f->found = 1;
    f->kind = kind;
    put(f->sig, sizeof f->sig, sig, strlen(sig));
    put(f->ret, sizeof f->ret, ret, strlen(ret));
}

/* A function (global) named f->name: its signature and return type. */
static int env_func_find(const Env *e, FindCtx *f)
{
    size_t i, j;
    StdCtx sc;

    for (i = 0; i < e->n; i++)
        for (j = 0; j < e->v[i]->n; j++) {
            const Sym *s = &e->v[i]->v[j];

            if (s->kind == RULE_FUNC && s->owner < 0 && strcmp(s->name, f->name) == 0) {
                f->found = 1;
                f->kind = RULE_FUNC;
                put(f->sig, sizeof f->sig, s->sig, strlen(s->sig));
                put(f->ret, sizeof f->ret, s->ret, strlen(s->ret));
                return 1;
            }
        }
    sc.fn = find_cb;
    sc.ctx = f;
    sc.member = 0;
    for (i = 0; i < e->nlib; i++)
        if (e->lib[i])
            std_globals(e->lib[i], std_cb, &sc);
    return f->found && f->kind == RULE_FUNC;
}

/* A function (global) named name: its return type. */
static int env_function(const Env *e, const char *name, char *ret, size_t cap)
{
    FindCtx f;

    memset(&f, 0, sizeof f);
    f.name = name;
    if (!env_func_find(e, &f))
        return 0;
    put(ret, cap, f.ret, strlen(f.ret));
    return 1;
}

/* A global variable (a module-level one, or a library object such as document): its type. */
static int env_variable(const Env *e, const char *name, char *ret, size_t cap)
{
    size_t i, j;
    StdCtx sc;
    FindCtx f;

    for (i = 0; i < e->n; i++)
        for (j = 0; j < e->v[i]->n; j++) {
            const Sym *s = &e->v[i]->v[j];

            if (s->kind == RULE_VAR && s->owner < 0 && s->ret[0] && strcmp(s->name, name) == 0) {
                put(ret, cap, s->ret, strlen(s->ret));
                return 1;
            }
        }
    memset(&f, 0, sizeof f);
    f.name = name;
    sc.fn = find_cb;
    sc.ctx = &f;
    sc.member = 0;
    for (i = 0; i < e->nlib; i++)
        if (e->lib[i])
            std_globals(e->lib[i], std_cb, &sc);
    if (f.found && f.kind == RULE_VAR && f.ret[0]) {
        put(ret, cap, f.ret, strlen(f.ret));
        return 1;
    }
    return 0;
}

static int env_has_class(const Env *e, const char *name)
{
    size_t i, j;

    for (i = 0; i < e->n; i++)
        for (j = 0; j < e->v[i]->n; j++)
            if (e->v[i]->v[j].kind == RULE_CLASS && strcmp(e->v[i]->v[j].name, name) == 0)
                return 1;
    for (i = 0; i < e->nlib; i++)
        if (e->lib[i] && std_class(e->lib[i], name, strlen(name)))
            return 1;
    return 0;
}

/* ---- the object before a member operator -------------------------------- */

typedef struct {
    char name[PM_NAME];
    int call;                   /* followed by (...) */
    int dollar;                 /* written with a leading $ */
    int isnew;                  /* "new Name" */
    int index;                  /* followed by [...] */
} Seg;

#define SEG_MAX 8

static size_t sep_before(const char *seps, const char *s, size_t end, size_t *tok_len)
{
    size_t best = 0, i = 0;

    while (seps[i]) {
        size_t e = i;

        while (seps[e] && seps[e] != ' ')
            e++;
        if (e > i && e - i <= end && e - i > best && memcmp(s + end - (e - i), seps + i, e - i) == 0)
            best = e - i;
        i = seps[e] ? e + 1 : e;
    }
    *tok_len = best;
    return best ? end - best : (size_t)-1;
}

/* Parses the expression that ends at `end` (right before an operator) into segments, last
 * first. Returns how many, 0 if it is not a chain we understand. */
static size_t chain_parse(const char *s, size_t end, const char *seps, Seg *out)
{
    size_t n = 0, pos = end;

    for (;;) {
        Seg *g;
        size_t e, tl, p2, op;

        if (n >= SEG_MAX)
            return 0;
        g = &out[n];
        memset(g, 0, sizeof *g);
        while (pos > 0 && is_blank(s[pos - 1]))
            pos--;
        while (pos > 0 && s[pos - 1] == ']') {
            int d = 0;

            while (pos > 0) {
                pos--;
                if (s[pos] == ']')
                    d++;
                else if (s[pos] == '[' && --d == 0)
                    break;
            }
            if (d != 0)
                return 0;
            g->index = 1;
            while (pos > 0 && is_blank(s[pos - 1]))
                pos--;
        }
        if (pos > 0 && s[pos - 1] == ')') {
            int d = 0;
            size_t q = pos, close = pos - 1;

            while (q > 0) {
                q--;
                if (s[q] == ')')
                    d++;
                else if (s[q] == '(' && --d == 0)
                    break;
            }
            if (d != 0)
                return 0;
            g->call = 1;
            pos = q;
            while (pos > 0 && is_blank(s[pos - 1]))
                pos--;
            if (pos == 0 || !is_idc((unsigned char)s[pos - 1])) {
                /* (new Foo) */
                PatMatch m;

                if (!pat_match("new %", s, close, q + 1, &m))
                    return 0;
                memcpy(g->name, m.type, strlen(m.type) + 1);
                g->isnew = 1;
                return n + 1;
            }
        }
        e = pos;
        while (pos > 0 && is_idc((unsigned char)s[pos - 1]))
            pos--;
        if (pos == e)
            return 0;
        put(g->name, sizeof g->name, s + pos, e - pos);
        if (pos > 0 && s[pos - 1] == '$') {
            g->dollar = 1;
            pos--;
        }
        n++;
        p2 = pos;
        while (p2 > 0 && is_blank(s[p2 - 1]))
            p2--;
        op = sep_before(seps, s, p2, &tl);
        if (op != (size_t)-1 && !(tl == 1 && s[op] == '.' && op > 0 && s[op - 1] == '.')) {
            pos = op;
            if (pos > 0 && s[pos - 1] == '?')
                pos--;
            continue;
        }
        /* "new Foo" */
        {
            size_t w = p2;

            while (w > 0 && is_idc((unsigned char)s[w - 1]))
                w--;
            if (p2 - w == 3 && memcmp(s + w, "new", 3) == 0)
                out[n - 1].isnew = 1;
        }
        return n;
    }
}

size_t sym_member_at(Syntax *syn, const Buffer *b, size_t y, size_t start)
{
    const char *seps = syn_member_seps(syn);
    const Line *l = &b->lines[y];
    size_t e = start, tl, op;
    const uint8_t *cl;

    if (!seps)
        return (size_t)-1;
    while (e > 0 && is_blank(l->s[e - 1]))
        e--;
    op = sep_before(seps, l->s, e, &tl);
    if (op == (size_t)-1 || op == 0)
        return (size_t)-1;
    if (tl == 1 && l->s[op] == '.' && l->s[op - 1] == '.')
        return (size_t)-1;
    {
        unsigned char p = (unsigned char)l->s[op - 1];

        if (p == '?' && op > 1)
            p = (unsigned char)l->s[op - 2];
        if (!is_idc(p) && p != ')' && p != ']')
            return (size_t)-1;
        if (tl == 1 && l->s[op] == '.' && isdigit(p)) {
            size_t k = op;

            while (k > 0 && isdigit((unsigned char)l->s[k - 1]))
                k--;
            if (k == 0 || !is_idc((unsigned char)l->s[k - 1]))
                return (size_t)-1;          /* a number like 3.5 */
        }
    }
    syn_update(syn, (Buffer *)b, y);
    cl = syn_classes(syn, b, y);
    if (cl && (cl[op] == SC_COMMENT || cl[op] == SC_STRING))
        return (size_t)-1;
    return op;
}

/* The innermost class whose block holds line y of the current document. */
static const Sym *enclosing_class(const Env *e, size_t y)
{
    const SymIndex *x = e->v[0];
    const Sym *best = NULL;
    size_t i;

    for (i = 0; i < x->n; i++)
        if (x->v[i].kind == RULE_CLASS && x->v[i].y0 <= y && y <= x->v[i].y1 &&
            x->v[i].name[0] && (!best || x->v[i].y0 >= best->y0))
            best = &x->v[i];
    return best;
}

static int is_ident(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

/* The type a name was last bound to above the cursor: "x = new Foo", "Foo $x", "x: Foo". */
static int find_bind(const Syntax *syn, const Buffer *b, size_t y, size_t xlimit,
                     const char *name, char *type, size_t cap)
{
    size_t nr, k, lines;
    const SynRule *r = syn_rules(syn, &nr);

    for (lines = 0; lines < 400; lines++) {
        const Line *l = &b->lines[y];
        size_t limit = lines == 0 ? xlimit : l->len, i;
        int found = 0;

        for (i = 0; i < limit; i++) {
            PatMatch m;

            if (!token_start(l->s, i))
                continue;
            for (k = 0; k < nr; k++)
                if (r[k].kind == RULE_BIND && pat_match(r[k].pat, l->s, limit, i, &m) &&
                    strcmp(m.name, name) == 0 && m.type[0]) {
                    /* "x = Foo::make(": remembered as "Foo::make", see settle */
                    size_t n = strlen(m.type), q;
                    int stat = 0;

                    for (q = i + 1; q + 1 < m.end && !stat; q++)
                        if (l->s[q] == ':' && l->s[q + 1] == ':' && is_ident(l->s[q - 1])) {
                            size_t c0 = q, m0 = q + 2;

                            while (c0 > i && is_ident(l->s[c0 - 1]))
                                c0--;
                            if (m.end - m0 >= n && memcmp(l->s + m0, m.type, n) == 0 &&
                                c0 < q && q - c0 + 2 + n < cap) {
                                memcpy(type, l->s + c0, q - c0);
                                memcpy(type + (q - c0), "::", 2);
                                memcpy(type + (q - c0) + 2, m.type, n);
                                type[q - c0 + 2 + n] = '\0';
                                stat = 1;
                            }
                        }
                    if (!stat)
                        put(type, cap, m.type, n);
                    found = 1;
                    break;
                }
        }
        if (found)
            return 1;
        if (y == 0)
            break;
        y--;
    }
    return 0;
}

/* Turns a type as written into a class that exists: a class name stays, a function name
 * gives its return type. */
/* The class an alias of the current file stands for ("use A\\Foo as Bar"); "" if none. */
static void alias_of(const Env *e, const char *name, char *out, size_t cap)
{
    size_t nr, k, y, n;
    const SynRule *r;

    out[0] = '\0';
    if (!e->cur || !e->cur->syn)
        return;
    r = syn_rules(e->cur->syn, &nr);
    n = e->cur->buf.nlines < 400 ? e->cur->buf.nlines : 400;      /* they sit at the top */
    for (y = 0; y < n; y++) {
        const Line *l = &e->cur->buf.lines[y];
        size_t i;

        for (i = 0; i < l->len; i++) {
            PatMatch m;

            if (!token_start(l->s, i))
                continue;
            for (k = 0; k < nr; k++)
                if (r[k].kind == RULE_ALIAS && pat_match(r[k].pat, l->s, l->len, i, &m) &&
                    strcmp(m.name, name) == 0 && m.type[0]) {
                    put(out, cap, m.type, strlen(m.type));
                    return;
                }
        }
    }
}

static int is_self_name(const char *n);

static void settle(const Env *e, const char *raw, char *out, size_t cap)
{
    char t[PM_NAME], r[PM_NAME];
    const char *colon = strstr(raw, "::");

    out[0] = '\0';
    if (colon) {
        /* "Foo::make": a static call; the declared return type, else the class itself
         * (the usual factory) */
        char cls[PM_NAME], c2[PM_NAME];
        FindCtx f;

        put(cls, sizeof cls, raw, (size_t)(colon - raw));
        settle(e, cls, c2, sizeof c2);
        if (!c2[0])
            return;
        memset(&f, 0, sizeof f);
        f.name = colon + 2;
        env_members(e, c2, find_cb, &f, 0);
        if (!f.found || !f.ret[0]) {
            put(out, cap, c2, strlen(c2));
            return;
        }
        norm_type(f.ret, r, sizeof r);
        if (is_self_name(r))
            put(out, cap, c2, strlen(c2));
        else
            settle(e, r, out, cap);
        return;
    }
    norm_type(raw, t, sizeof t);
    if (!t[0])
        return;
    if (!env_has_class(e, t)) {
        char a[PM_NAME];

        alias_of(e, t, a, sizeof a);
        if (a[0] && env_has_class(e, a)) {
            put(out, cap, a, strlen(a));
            return;
        }
    }
    if (env_has_class(e, t)) {
        put(out, cap, t, strlen(t));
        return;
    }
    if (t[0] >= 'a' && t[0] <= 'z') {
        char up[PM_NAME];                       /* TypeScript's string, number: String, Number */

        memcpy(up, t, strlen(t) + 1);
        up[0] = (char)(up[0] - 32);
        if (env_has_class(e, up)) {
            put(out, cap, up, strlen(up));
            return;
        }
    }
    if (env_function(e, t, r, sizeof r)) {
        norm_type(r, t, sizeof t);
        if (t[0] && env_has_class(e, t))
            put(out, cap, t, strlen(t));
    }
}

static int is_self_name(const char *n)
{
    return strcmp(n, "this") == 0 || strcmp(n, "self") == 0 || strcmp(n, "static") == 0 ||
           strcmp(n, "cls") == 0;
}

/* The class of the expression before the operator at (y, sep); "" if unknown. */
static void resolve_chain(const Env *e, Doc *cur, size_t y, size_t sep, char *out, size_t cap)
{
    Seg segs[SEG_MAX];
    size_t n, i;
    const Line *l = &cur->buf.lines[y];
    const char *seps = syn_member_seps(cur->syn);
    char t[PM_NAME];
    const Sym *ec;

    out[0] = '\0';
    if (!seps)
        return;
    {
        Seg rev[SEG_MAX];

        n = chain_parse(l->s, sep, seps, rev);
        if (n == 0)
            return;
        for (i = 0; i < n; i++)
            segs[i] = rev[n - 1 - i];
    }
    ec = enclosing_class(e, y);
    t[0] = '\0';
    if (segs[0].isnew) {
        settle(e, segs[0].name, t, sizeof t);
    } else if (is_self_name(segs[0].name) && !segs[0].call) {
        if (ec)
            put(t, sizeof t, ec->name, strlen(ec->name));
    } else if (strcmp(segs[0].name, "parent") == 0 && !segs[0].call) {
        if (ec && ec->ret[0])
            norm_type(ec->ret, t, sizeof t);
    } else if (segs[0].call) {
        char r[PM_NAME];

        if (env_function(e, segs[0].name, r, sizeof r))
            settle(e, r, t, sizeof t);
        else
            settle(e, segs[0].name, t, sizeof t);       /* Foo(...)->x: a constructor call */
    } else {
        char raw[PM_NAME];

        if (find_bind(cur->syn, &cur->buf, y, segs[0].dollar ? sep : sep, segs[0].name, raw,
                      sizeof raw))
            settle(e, raw, t, sizeof t);
        else if (!segs[0].dollar && env_has_class(e, segs[0].name))
            put(t, sizeof t, segs[0].name, strlen(segs[0].name));
        else if (!segs[0].dollar && env_variable(e, segs[0].name, raw, sizeof raw))
            settle(e, raw, t, sizeof t);
        if (segs[0].index)
            t[0] = '\0';
    }
    for (i = 1; i < n && t[0]; i++) {
        FindCtx f;
        char nt[PM_NAME];

        memset(&f, 0, sizeof f);
        f.name = segs[i].name;
        env_members(e, t, find_cb, &f, 0);
        if (!f.found || !f.ret[0]) {
            t[0] = '\0';
            break;
        }
        norm_type(f.ret, nt, sizeof nt);
        if (is_self_name(nt))
            continue;                   /* fluent: the class itself */
        if (segs[i].index)
            nt[0] = '\0';
        settle(e, nt, t, sizeof t);
    }
    put(out, cap, t, strlen(t));
}

/* The global class aliases of a Laravel project: 'URL' => Illuminate\\Support\\Facades\\URL::class
 * in config/app.php, read once. */
static void load_aliases(SymCache *c)
{
    FILE *f;
    char line[512];

    if (c->al_done)
        return;
    c->al_done = 1;
    rooted(c, line, sizeof line, "config/app.php");
    f = fopen(line, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *p = line, *q, *arrow, name[PM_NAME], qual[PM_PATH];
        size_t n;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != '\'' && *p != '"')
            continue;
        q = strchr(p + 1, *p);
        arrow = q ? strstr(q, "=>") : NULL;
        if (!arrow || (size_t)(q - p - 1) >= sizeof name || q == p + 1)
            continue;
        memcpy(name, p + 1, (size_t)(q - p - 1));
        name[q - p - 1] = '\0';
        p = arrow + 2;
        while (*p == ' ' || *p == '\t' || *p == '\\')
            p++;
        for (n = 0; is_idc((unsigned char)p[n]) || p[n] == '\\'; n++)
            ;
        if (n == 0 || n >= sizeof qual || strncmp(p + n, "::class", 7) != 0)
            continue;
        memcpy(qual, p, n);
        qual[n] = '\0';
        c->al_name = xrealloc(c->al_name, (c->nal + 1) * sizeof *c->al_name);
        c->al_qual = xrealloc(c->al_qual, (c->nal + 1) * sizeof *c->al_qual);
        c->al_name[c->nal] = xstrdup(name);
        c->al_qual[c->nal++] = xstrdup(qual);
    }
    fclose(f);
}

/* A global alias (Laravel facade: URL, Route, Session) written before the operator at (y, sep)
 * and not defined by any loaded file: its file joins the world. */
static void env_alias(SymCache *c, Env *e, Doc *cur, size_t y, size_t sep)
{
    Seg rev[SEG_MAX];
    const char *seps = syn_member_seps(cur->syn), *lang = syn_name(cur->syn);
    const Seg *s0;
    size_t n, i;

    if (!seps || (strcmp(lang, "PHP") != 0 && strcmp(lang, "PHP/HTML") != 0))
        return;
    n = chain_parse(cur->buf.lines[y].s, sep, seps, rev);
    if (n == 0)
        return;
    s0 = &rev[n - 1];
    if (s0->dollar || s0->call || s0->isnew || env_has_class(e, s0->name))
        return;
    load_aliases(c);
    for (i = 0; i < c->nal; i++)
        if (strcmp(c->al_name[i], s0->name) == 0) {
            char *f = find_class_file(c, c->al_qual[i], "php");

            if (f) {
                env_add(e, index_of_path(c, f), 1);
                free(f);
            }
            return;
        }
}

static void add_class(SymCache *c, const char *fq, size_t n)
{
    if (c->ncls >= 60000 || n == 0)
        return;
    if (c->ncls == c->clscap) {
        c->clscap = c->clscap ? c->clscap * 2 : 1024;
        c->cls = xrealloc(c->cls, c->clscap * sizeof *c->cls);
    }
    c->cls[c->ncls++] = xstrndup(fq, n);
}

typedef struct {
    SymCache *c;
    const char *prefix;         /* the namespace of the PSR-4 directory being walked */
} WalkCtx;

static void walk_add(const char *rel, void *ctx)
{
    WalkCtx *w = ctx;
    size_t n = strlen(rel), i, pl;
    char fq[PM_PATH];

    if (n < 5 || strcmp(rel + n - 4, ".php") != 0 || strlen(w->prefix) + n >= sizeof fq)
        return;
    pl = strlen(w->prefix);
    memcpy(fq, w->prefix, pl);
    memcpy(fq + pl, rel, n - 4);
    fq[pl + n - 4] = '\0';
    for (i = 0; fq[i]; i++)
        if (fq[i] == '/')
            fq[i] = '\\';
    /* a class the classmap already listed */
    for (i = 0; i < w->c->ncls; i++)
        if (strcmp(w->c->cls[i], fq) == 0)
            return;
    add_class(w->c, fq, strlen(fq));
}

/* config/app.php aliases, then vendor/composer/autoload_classmap.php, then the project's own
 * PSR-4 directories (the classmap may be old: a class added since is only found there). It may
 * take long on a big project or a network disk: each call works until the deadline and goes on
 * at the next; the classes found so far are already usable. */
static void load_classes(SymCache *c)
{
    char line[1024];
    size_t i;

    if (c->cls_done)
        return;
    if (c->cl_phase == 0) {
        load_aliases(c);
        for (i = 0; i < c->nal; i++)
            add_class(c, c->al_qual[i], strlen(c->al_qual[i]));
        rooted(c, line, sizeof line, "vendor/composer/autoload_classmap.php");
        c->cl_file = fopen(line, "r");
        c->cl_phase = 1;
    }
    if (c->cl_phase == 1) {
        size_t n = 0;

        while (c->cl_file && fgets(line, sizeof line, c->cl_file)) {
            char fq[PM_PATH], *p = line;
            size_t k = 0;

            if ((++n & 255) == 0 && scan_left(c) < 0) {
                c->busy = 1;
                return;
            }
            while (*p == ' ')
                p++;
            if (*p != '\'')
                continue;
            for (p++; *p && *p != '\'' && k + 1 < sizeof fq; p++) {
                if (*p == '\\' && p[1] == '\\')
                    p++;
                fq[k++] = *p;
            }
            if (*p != '\'' || strstr(fq, "Composer\\") == fq)
                continue;
            add_class(c, fq, k);
            c->cl_opt++;
        }
        if (c->cl_file)
            fclose(c->cl_file);
        c->cl_file = NULL;
        c->cl_phase = 2;
        load_psr4(c);
        c->cl_i = 0;
    }
    while (c->cl_i != (size_t)-1 && c->cl_i < c->nps) {
        static const char *const prune[] = { "vendor", NULL };
        long left = scan_left(c);
        WalkCtx w;

        if (c->ps_dir[c->cl_i][0] != '.') {
            c->cl_i++;                  /* the project's own directories, not vendor */
            continue;
        }
        if (left < 0) {
            c->busy = 1;
            return;
        }
        if (!c->cl_fs) {
            char top[PATH_MAX];

            rooted(c, top, sizeof top, c->ps_dir[c->cl_i]);
            c->cl_fs = files_scan_begin(top, prune);
        }
        w.c = c;
        w.prefix = c->ps_prefix[c->cl_i];
        if (!files_scan_step(c->cl_fs, walk_add, &w, left)) {
            c->busy = 1;
            return;
        }
        files_scan_free(c->cl_fs);
        c->cl_fs = NULL;
        c->cl_i++;
    }
    c->cls_done = 1;
}

static int prefix_ci(const char *s, const char *p, size_t pl)
{
    size_t i;

    for (i = 0; i < pl; i++)
        if (s[i] == '\0' || tolower((unsigned char)s[i]) != tolower((unsigned char)p[i]))
            return 0;
    return 1;
}

void sym_classes(SymCache *c, Doc *cur, const char *prefix, size_t plen, int full, ClassFn fn,
                 void *ctx)
{
    const char *lang = syn_name(cur->syn);
    size_t i, found = 0;

    if (strcmp(lang, "PHP") != 0 && strcmp(lang, "PHP/HTML") != 0)
        return;
    set_root(c, index_of_doc(c, cur)->key);
    load_classes(c);
    for (i = 0; i < c->ncls && found < 400; i++) {
        const char *fq = c->cls[i], *last = last_segment(fq);
        int rank;

        if (prefix_ci(last, prefix, plen))
            rank = strlen(last) == plen ? 0 : 1;
        else if (full && prefix_ci(fq, prefix, plen))
            rank = 2;
        else
            continue;
        fn(ctx, fq, rank);
        found++;
    }
}

int sym_class_fq(SymCache *c, Doc *cur, const char *name, char *out, size_t n)
{
    size_t i, nl = strlen(name), best_len = 0;
    const char *best = NULL;
    int best_rank = -1;
    long saved = c->deadline_us;

    if (!nl)
        return 0;
    set_root(c, index_of_doc(c, cur)->key);
    load_aliases(c);
    for (i = 0; i < c->nal; i++)
        if (strcmp(c->al_name[i], name) == 0) {
            snprintf(out, n, "%s", c->al_qual[i]);
            return 1;
        }
    /* the whole list: this is asked once, when a name is accepted */
    c->deadline_us = 0;
    for (i = 0; !c->cls_done && i < 100000; i++)
        load_classes(c);
    c->deadline_us = saved;
    for (i = 0; i < c->ncls; i++) {
        const char *fq = c->cls[i];
        int rank;

        if (strcmp(last_segment(fq), name) != 0)
            continue;
        /* the project's own first, then the facades, then the shortest name */
        rank = strncmp(fq, "App\\", 4) == 0 ? 2 : strstr(fq, "\\Facades\\") ? 1 : 0;
        if (rank > best_rank || (rank == best_rank && strlen(fq) < best_len)) {
            best = fq;
            best_rank = rank;
            best_len = strlen(fq);
        }
    }
    if (!best)
        return 0;
    snprintf(out, n, "%s", best);
    return 1;
}

typedef struct {
    SymFn fn;
    void *ctx;
} Pass;

int sym_members(SymCache *c, Doc *const *docs, size_t ndocs, Doc *cur, size_t y, size_t sep,
                SymFn fn, void *ctx)
{
    Env e;
    char t[PM_NAME];
    size_t i, j;

    env_build(c, docs, ndocs, cur, &e);
    env_alias(c, &e, cur, y, sep);
    resolve_chain(&e, cur, y, sep, t, sizeof t);
    if (t[0] && env_members(&e, t, fn, ctx, 0))
        return 1;
    /* unknown type: every member of every class we know */
    for (i = 0; i < e.n; i++)
        for (j = 0; j < e.v[i]->n; j++) {
            const Sym *s = &e.v[i]->v[j];

            if (s->owner >= 0 && s->name[0])
                fn(ctx, s->name, s->kind, s->sig, s->ret, i == 0 ? 0 : 1);
        }
    return 0;
}

int sym_type_before(SymCache *c, Doc *const *docs, size_t ndocs, Doc *cur, size_t y, size_t sep,
                    char *out, size_t n)
{
    Env e;

    env_build(c, docs, ndocs, cur, &e);
    env_alias(c, &e, cur, y, sep);
    resolve_chain(&e, cur, y, sep, out, n);
    return out[0] != '\0';
}

void sym_globals(SymCache *c, Doc *const *docs, size_t ndocs, Doc *cur, SymFn fn, void *ctx)
{
    Env e;
    size_t i, j;
    StdCtx sc;

    env_build(c, docs, ndocs, cur, &e);
    for (i = 0; i < e.n; i++)
        for (j = 0; j < e.v[i]->n; j++) {
            const Sym *s = &e.v[i]->v[j];

            if (s->owner < 0 && s->name[0])
                fn(ctx, s->name, s->kind, s->sig, s->ret, i == 0 ? 0 : 1);
        }
    sc.fn = fn;
    sc.ctx = ctx;
    sc.member = 0;
    for (i = 0; i < e.nlib; i++)
        if (e.lib[i]) {
            std_globals(e.lib[i], std_cb, &sc);
            /* with "using namespace std" the contents of std are global names */
            if (e.lib[i] == c->lib[LIB_CPP])
                std_members(e.lib[i], "std", 3, std_cb, &sc);
        }
}

/* ---- the headers of a C project ---------------------------------------- */

#define HDR_MAX 400                 /* headers of the project looked at */
#define HDR_SYMS 400                /* names kept from one header */
#define HDR_BYTES (256L * 1024)

static int is_header(const char *path)
{
    const char *x = ext_of(path);

    return strcmp(x, "h") == 0 || strcmp(x, "hpp") == 0 || strcmp(x, "hh") == 0 ||
           strcmp(x, "hxx") == 0;
}

static const char *strstr_len(const char *hay, size_t n, const char *needle)
{
    size_t nl = strlen(needle), i;

    for (i = 0; i + nl <= n; i++)
        if (memcmp(hay + i, needle, nl) == 0)
            return hay + i;
    return NULL;
}

static int is_js_module(const char *path)
{
    static const char *const ok[] = { "js", "jsx", "ts", "tsx", "mjs", "cjs", "mts", "cts", NULL };
    const char *x = ext_of(path);
    size_t i, l = strlen(path);

    if (l > 5 && strcmp(path + l - 5, ".d.ts") == 0)
        return 0;
    for (i = 0; ok[i]; i++)
        if (strcmp(x, ok[i]) == 0)
            return 1;
    return 0;
}

static void hsym_add(struct Hdr *h, const char *name, size_t nl, int kind, const char *sig, size_t sl,
                     int def)
{
    char *n;

    if (nl == 0 || nl >= PM_NAME || h->n >= 400)
        return;
    h->v = xrealloc(h->v, (h->n + 1) * sizeof *h->v);
    n = xmalloc(nl + 1);
    memcpy(n, name, nl);
    n[nl] = '\0';
    h->v[h->n].name = n;
    h->v[h->n].sig = xmalloc(sl + 1);
    memcpy(h->v[h->n].sig, sig, sl);
    h->v[h->n].sig[sl] = '\0';
    h->v[h->n].ret = xstrdup("");
    h->v[h->n].kind = kind;
    h->v[h->n].def = def;
    h->v[h->n].ns = NULL;
    h->n++;
}

/* Skips the word w (and the blanks after it) at s[*i]; 1 if it was there. */
static int js_word(const char *s, size_t len, size_t *i, const char *w)
{
    size_t n = strlen(w);

    if (*i + n > len || memcmp(s + *i, w, n) != 0 || (*i + n < len && is_idc((unsigned char)s[*i + n])))
        return 0;
    *i += n;
    while (*i < len && (s[*i] == ' ' || s[*i] == '\t'))
        (*i)++;
    return 1;
}

static size_t js_ident(const char *s, size_t len, size_t i)
{
    while (i < len && (is_idc((unsigned char)s[i]) || s[i] == '$'))
        i++;
    return i;
}

/* The names a JavaScript or TypeScript file exports, read from its lines: export function,
 * class, const, interface, type, enum, export default function/class Name, export { a, b as c },
 * and in CommonJS exports.x = and module.exports = { a, b }. */
static void js_exports(const Buffer *b, struct Hdr *h)
{
    size_t y;

    for (y = 0; y < b->nlines; y++) {
        const char *s = b->lines[y].s;
        size_t len = b->lines[y].len, i = 0, e, p0, p1;
        int def = 0;

        if (js_word(s, len, &i, "export")) {
            if (js_word(s, len, &i, "default"))
                def = 1;
            js_word(s, len, &i, "declare");
            js_word(s, len, &i, "abstract");
            js_word(s, len, &i, "async");
            if (js_word(s, len, &i, "function")) {
                if (i < len && s[i] == '*')
                    i++;
                e = js_ident(s, len, i);
                for (p0 = e; p0 < len && s[p0] != '('; p0++)
                    ;
                for (p1 = p0; p1 < len && s[p1] != ')'; p1++)
                    ;
                hsym_add(h, s + i, e - i, RULE_FUNC, s + p0 + (p0 < len), p1 > p0 ? p1 - p0 - 1 : 0, def);
            } else if (js_word(s, len, &i, "class") || js_word(s, len, &i, "interface") ||
                       js_word(s, len, &i, "type") || js_word(s, len, &i, "enum")) {
                e = js_ident(s, len, i);
                hsym_add(h, s + i, e - i, RULE_CLASS, "", 0, def);
            } else if (!def && (js_word(s, len, &i, "const") || js_word(s, len, &i, "let") ||
                                js_word(s, len, &i, "var"))) {
                size_t k;
                int fn = 0;

                e = js_ident(s, len, i);
                for (k = e; k < len && s[k] != '='; k++)
                    ;
                p0 = k + 1;
                while (p0 < len && s[p0] == ' ')
                    p0++;
                if (p0 + 5 < len && memcmp(s + p0, "async", 5) == 0)
                    p0 += 5;
                while (p0 < len && s[p0] == ' ')
                    p0++;
                if (p0 + 8 <= len && memcmp(s + p0, "function", 8) == 0) {
                    fn = 1;
                    while (p0 < len && s[p0] != '(')
                        p0++;
                }
                if (p0 < len && s[p0] == '(') {
                    for (p1 = p0; p1 < len && s[p1] != ')'; p1++)
                        ;
                    if (fn || (p1 + 4 <= len && strstr_len(s + p1, len - p1, "=>")))
                        fn = 1;
                    if (fn) {
                        hsym_add(h, s + i, e - i, RULE_FUNC, s + p0 + 1, p1 > p0 ? p1 - p0 - 1 : 0, 0);
                        continue;
                    }
                }
                hsym_add(h, s + i, e - i, RULE_VAR, "", 0, 0);
            } else if (!def && i < len && s[i] == '{') {
                /* export { a, b as c } (also re-exported from another module) */
                i++;
                while (i < len && s[i] != '}') {
                    size_t a0, a1;

                    while (i < len && (s[i] == ' ' || s[i] == ','))
                        i++;
                    a0 = i;
                    a1 = js_ident(s, len, i);
                    i = a1;
                    while (i < len && s[i] == ' ')
                        i++;
                    if (js_word(s, len, &i, "as")) {
                        a0 = i;
                        a1 = js_ident(s, len, i);
                        i = a1;
                    }
                    if (a1 > a0 && !(a1 - a0 == 7 && memcmp(s + a0, "default", 7) == 0))
                        hsym_add(h, s + a0, a1 - a0, RULE_VAR, "", 0, 0);
                    if (a1 == a0)
                        i++;
                }
            } else if (def) {
                /* export default Name; */
                e = js_ident(s, len, i);
                if (e > i)
                    hsym_add(h, s + i, e - i, RULE_VAR, "", 0, 1);
            }
            continue;
        }
        /* CommonJS */
        i = 0;
        if (len > 8 && memcmp(s, "exports.", 8) == 0) {
            i = 8;
            e = js_ident(s, len, i);
            hsym_add(h, s + i, e - i, RULE_VAR, "", 0, 0);
        } else if (len > 15 && memcmp(s, "module.exports.", 15) == 0) {
            i = 15;
            e = js_ident(s, len, i);
            hsym_add(h, s + i, e - i, RULE_VAR, "", 0, 0);
        } else if (len > 14 && memcmp(s, "module.exports", 14) == 0) {
            i = 14;
            while (i < len && (s[i] == ' ' || s[i] == '='))
                i++;
            if (i < len && s[i] == '{') {
                i++;
                while (i < len && s[i] != '}') {
                    size_t a0, a1;

                    while (i < len && (s[i] == ' ' || s[i] == ','))
                        i++;
                    a0 = i;
                    a1 = js_ident(s, len, i);
                    if (a1 > a0)
                        hsym_add(h, s + a0, a1 - a0, RULE_VAR, "", 0, 0);
                    i = a1;
                    /* a: value — skip to the next comma at this level */
                    while (i < len && s[i] != ',' && s[i] != '}')
                        i++;
                    if (a1 == a0 && i < len && s[i] != '}')
                        i++;
                }
            } else {
                e = js_ident(s, len, i);
                if (e > i)
                    hsym_add(h, s + i, e - i, RULE_VAR, "", 0, 1);
            }
        }
    }
}

/* A Python file of the project, not one of a virtual environment or an installed package. */
static int is_py_module(const char *rel)
{
    static const char *const skip[] = { "venv", "env", "site-packages", "__pycache__", "dist-packages", NULL };
    size_t i;

    if (strcmp(ext_of(rel), "py") != 0)
        return 0;
    for (i = 0; skip[i]; i++) {
        size_t n = strlen(skip[i]);
        const char *p = rel;

        while ((p = strstr(p, skip[i])) != NULL) {
            if ((p == rel || p[-1] == '/') && p[n] == '/')
                return 0;
            p += n;
        }
    }
    return 1;
}

/* The public names a Python module defines at its top level: def, async def, class, and the
 * names it assigns (NAME = ..., name: type = ...); those starting with "_" are left out. */
static void py_exports(const Buffer *b, struct Hdr *h)
{
    size_t y;

    for (y = 0; y < b->nlines; y++) {
        const char *s = b->lines[y].s;
        size_t len = b->lines[y].len, i = 0, e, p0, p1;

        if (len == 0 || s[0] == ' ' || s[0] == '\t' || s[0] == '#' || s[0] == '_')
            continue;
        js_word(s, len, &i, "async");
        if (js_word(s, len, &i, "def")) {
            e = js_ident(s, len, i);
            if (e == i || s[i] == '_')
                continue;
            for (p0 = e; p0 < len && s[p0] != '('; p0++)
                ;
            for (p1 = p0; p1 < len && s[p1] != ')'; p1++)
                ;
            hsym_add(h, s + i, e - i, RULE_FUNC, s + p0 + (p0 < len), p1 > p0 ? p1 - p0 - 1 : 0, 0);
        } else if (js_word(s, len, &i, "class")) {
            e = js_ident(s, len, i);
            if (e > i && s[i] != '_')
                hsym_add(h, s + i, e - i, RULE_CLASS, "", 0, 0);
        } else {
            size_t k;
            int upper = 1;

            e = js_ident(s, len, 0);
            if (e == 0)
                continue;
            for (k = e; k < len && s[k] == ' '; k++)
                ;
            if (k < len && s[k] == ':') {
                while (k < len && s[k] != '=')
                    k++;
            }
            if (k >= len || s[k] != '=' || (k + 1 < len && s[k + 1] == '='))
                continue;
            for (k = 0; k < e; k++)
                if (s[k] >= 'a' && s[k] <= 'z')
                    upper = 0;
            /* keywords that look like assignments at the top level are not names */
            if ((e == 2 && memcmp(s, "if", 2) == 0) || (e == 5 && memcmp(s, "while", 5) == 0))
                continue;
            hsym_add(h, s, e, upper ? RULE_CONST : RULE_VAR, "", 0, 0);
        }
    }
}

/* The functions, classes and constants a PHP file defines at its top level (function, class,
 * interface, trait, enum, const, define), each with the namespace of the file. */
static void php_exports(const Buffer *b, struct Hdr *h)
{
    size_t y;
    char ns[PM_PATH] = "";

    for (y = 0; y < b->nlines; y++) {
        const char *s = b->lines[y].s;
        size_t len = b->lines[y].len, i = 0, e, p0, p1, before = h->n;

        if (len == 0 || s[0] == ' ' || s[0] == '\t')
            continue;
        if (js_word(s, len, &i, "namespace")) {
            for (e = i; e < len && s[e] != ';' && s[e] != ' ' && s[e] != '{'; e++)
                ;
            put(ns, sizeof ns, s + i, e - i);
            continue;
        }
        js_word(s, len, &i, "abstract");
        js_word(s, len, &i, "final");
        js_word(s, len, &i, "readonly");
        if (js_word(s, len, &i, "function")) {
            if (i < len && s[i] == '&')
                i++;
            e = js_ident(s, len, i);
            for (p0 = e; p0 < len && s[p0] != '('; p0++)
                ;
            for (p1 = p0; p1 < len && s[p1] != ')'; p1++)
                ;
            hsym_add(h, s + i, e - i, RULE_FUNC, s + p0 + (p0 < len), p1 > p0 ? p1 - p0 - 1 : 0, 0);
        } else if (js_word(s, len, &i, "class") || js_word(s, len, &i, "interface") ||
                   js_word(s, len, &i, "trait") || js_word(s, len, &i, "enum")) {
            e = js_ident(s, len, i);
            hsym_add(h, s + i, e - i, RULE_CLASS, "", 0, 0);
        } else if (js_word(s, len, &i, "const")) {
            e = js_ident(s, len, i);
            hsym_add(h, s + i, e - i, RULE_CONST, "", 0, 0);
        } else if (len > 8 && memcmp(s, "define(", 7) == 0 && (s[7] == '\'' || s[7] == '"')) {
            char q = s[7];

            for (e = 8; e < len && s[e] != q; e++)
                ;
            hsym_add(h, s + 8, e - 8, RULE_CONST, "", 0, 0);
            continue;                       /* define() is always global */
        }
        if (h->n > before && ns[0])
            h->v[h->n - 1].ns = xstrdup(ns);
    }
}

/* The dotted name under which a Python file is imported: the folders above it that are packages
 * (they have __init__.py) and its name; a package's __init__.py is the package. */
static void py_module_name(const char *real, char *out, size_t n)
{
    char dir[PATH_MAX], probe_path[PATH_MAX + 16], parts[PATH_MAX];
    char *sl;
    struct stat st;
    size_t l;

    snprintf(dir, sizeof dir, "%s", real);
    sl = strrchr(dir, '/');
    if (!sl) {
        snprintf(out, n, "%s", real);
        return;
    }
    *sl = '\0';
    snprintf(parts, sizeof parts, "%s", sl + 1);
    l = strlen(parts);
    if (l > 3)
        parts[l - 3] = '\0';           /* .py */
    if (strcmp(parts, "__init__") == 0) {
        sl = strrchr(dir, '/');
        if (!sl) {
            snprintf(out, n, "%s", dir);
            return;
        }
        *sl = '\0';
        snprintf(parts, sizeof parts, "%s", sl + 1);
    }
    for (;;) {
        SBuf t = SBUF_INIT;

        snprintf(probe_path, sizeof probe_path, "%s/__init__.py", dir);
        if (stat(probe_path, &st) != 0)
            break;
        sl = strrchr(dir, '/');
        if (!sl)
            break;
        sbuf_adds(&t, sl + 1);
        sbuf_adds(&t, ".");
        sbuf_adds(&t, parts);
        sbuf_add(&t, "", 1);
        if (t.len <= sizeof parts)
            memcpy(parts, t.p, t.len);
        sbuf_free(&t);
        *sl = '\0';
    }
    snprintf(out, n, "%s", parts);
}

/* A macro that only guards the header against a second inclusion (XTED_BUFFER_H). */
static int guard_macro(const char *n)
{
    size_t l = strlen(n);

    if (l > 2 && strcmp(n + l - 2, "_H") == 0)
        return 1;
    return l > 3 && strcmp(n + l - 3, "_H_") == 0;
}

/* The names of a header, read again when it changed; NULL if it cannot be read, or if there is
 * no time left to read it now (c->busy is set and the next call goes on). */
static struct Hdr *hdr_get(SymCache *c, const char *real)
{
    struct Hdr *h = NULL;
    struct stat st;
    long now = (long)time(NULL);
    size_t i;
    Buffer b;
    char err[128];
    SymIndex *x = NULL;
    int js;

    for (i = 0; i < c->nhdr; i++)
        if (strcmp(c->hdr[i].path, real) == 0) {
            h = &c->hdr[i];
            break;
        }
    if (h && now - h->checked < 2)
        return h;
    if (stat(real, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > HDR_BYTES)
        return h;
    js = is_js_module(real) ? 1 : strcmp(ext_of(real), "py") == 0 ? 2 : strcmp(ext_of(real), "php") == 0 ? 3 : 0;
    if (h && h->mtime == (long)st.st_mtime && h->size == (long)st.st_size) {
        h->checked = now;
        return h;
    }
    if (c->deadline_us && scan_left(c) < 0) {
        c->busy = 1;
        return h;
    }
    buf_init(&b);
    if (buf_load(&b, real, err, sizeof err) != 0) {
        buf_free(&b);
        return h;
    }
    if (!js) {
        Syntax *syn = syn_detect(&b);

        x = sym_build(&b, syn);
        syn_free(syn);
    }
    if (!h) {
        c->hdr = xrealloc(c->hdr, (c->nhdr + 1) * sizeof *c->hdr);
        h = &c->hdr[c->nhdr++];
        memset(h, 0, sizeof *h);
        h->path = xstrdup(real);
    } else {
        for (i = 0; i < h->n; i++) {
            free(h->v[i].name);
            free(h->v[i].sig);
            free(h->v[i].ret);
            free(h->v[i].ns);
        }
        free(h->v);
        h->v = NULL;
        h->n = 0;
    }
    if (js == 1)
        js_exports(&b, h);
    else if (js == 2)
        py_exports(&b, h);
    else if (js == 3)
        php_exports(&b, h);
    buf_free(&b);
    for (i = 0; x && i < x->n && h->n < HDR_SYMS; i++) {
        const Sym *s = &x->v[i];

        if (s->owner >= 0 || !s->name[0] || s->hidden || (s->kind == RULE_CONST && guard_macro(s->name)))
            continue;
        if (s->kind != RULE_FUNC && s->kind != RULE_CLASS && s->kind != RULE_CONST && s->kind != RULE_VAR)
            continue;
        h->v = xrealloc(h->v, (h->n + 1) * sizeof *h->v);
        h->v[h->n].name = xstrdup(s->name);
        h->v[h->n].sig = xstrdup(s->sig ? s->sig : "");
        h->v[h->n].ret = xstrdup(s->ret ? s->ret : "");
        h->v[h->n].kind = s->kind;
        h->v[h->n].def = 0;
        h->v[h->n].ns = NULL;
        h->n++;
    }
    if (x)
        sym_index_free(x);
    h->mtime = (long)st.st_mtime;
    h->size = (long)st.st_size;
    h->checked = now;
    return h;
}

/* The path of file seen from the folder of from (a file too): "x.h", "../src/x.h". */
static void rel_path(const char *from, const char *file, char *out, size_t n)
{
    char dir[PATH_MAX];
    char *sl;
    size_t last = 0, up = 0, k;
    SBuf r = SBUF_INIT;

    snprintf(dir, sizeof dir, "%s", from);
    sl = strrchr(dir, '/');
    if (sl)
        *sl = '\0';
    for (k = 0; dir[k] && file[k] && dir[k] == file[k]; k++)
        if (dir[k] == '/')
            last = k;
    if (!dir[k] && file[k] == '/')
        last = k;                           /* the file is below the folder */
    for (k = last; dir[k]; k++)
        if (dir[k] == '/')
            up++;
    for (k = 0; k < up; k++)
        sbuf_adds(&r, "../");
    sbuf_adds(&r, file + last + 1);
    sbuf_add(&r, "", 1);
    snprintf(out, n, "%s", r.p);
    sbuf_free(&r);
}

/* How a JS/TS file cur would write the import of module file mod: relative ("./x", "../lib/x"),
 * without the extension and without a final /index, unless its relative imports keep the
 * extension (Node's ES modules need it). */
static void js_spec(const SymIndex *cur, const char *mod, char *out, size_t n)
{
    char rel[PATH_MAX];
    size_t i, l;
    int keep = 0;
    char *dot, *sl;

    for (i = 0; i < cur->ninc; i++) {
        const char *s = cur->inc[i];

        if (s[0] == '.' && (s[1] == '/' || s[1] == '.') && strrchr(s, '.') > strrchr(s, '/'))
            keep = 1;
    }
    rel_path(cur->key, mod, rel, sizeof rel);
    if (!keep) {
        dot = strrchr(rel, '.');
        sl = strrchr(rel, '/');
        if (dot && (!sl || dot > sl))
            *dot = '\0';
        l = strlen(rel);
        if (l >= 6 && strcmp(rel + l - 6, "/index") == 0)
            rel[l - 6] = '\0';
        else if (strcmp(rel, "index") == 0)
            snprintf(rel, sizeof rel, ".");
    }
    if (strncmp(rel, "../", 3) == 0 || strcmp(rel, ".") == 0)
        snprintf(out, n, "%s", rel);
    else
        snprintf(out, n, "./%.4000s", rel);
}

/* How the file cur would write the include of hdr: like the includes it already has (the part of
 * hdr below the folder one of them is found from), else relative to the folder of cur. */
static void hdr_spec(const SymIndex *cur, const char *hdr, char *out, size_t n)
{
    size_t i;
    const char *d = cur->key;

    for (i = 0; i < cur->ninc && cur->incp; i++) {
        const char *spec = cur->inc[i], *p = cur->incp[i];
        size_t sl, pl;

        if (!p || !p[0] || spec[0] == ':' || strstr(spec, ".."))
            continue;
        sl = strlen(spec);
        pl = strlen(p);
        if (pl > sl + 1 && strcmp(p + pl - sl, spec) == 0 && p[pl - sl - 1] == '/' &&
            strncmp(hdr, p, pl - sl) == 0 && strchr(hdr + (pl - sl), '/') == NULL) {
            snprintf(out, n, "%s", hdr + (pl - sl));
            return;
        }
    }
    for (i = 0; i < cur->ninc && cur->incp; i++) {
        const char *spec = cur->inc[i], *p = cur->incp[i];
        size_t sl, pl;

        if (!p || !p[0] || spec[0] == ':' || strstr(spec, ".."))
            continue;
        sl = strlen(spec);
        pl = strlen(p);
        if (pl > sl + 1 && strcmp(p + pl - sl, spec) == 0 && p[pl - sl - 1] == '/' &&
            strncmp(hdr, p, pl - sl) == 0) {
            snprintf(out, n, "%s", hdr + (pl - sl));
            return;
        }
    }
    rel_path(d, hdr, out, n);
}

/* PHP. With Composer: the functions and constants of the files it loads ("autoload": {"files"}),
 * with "function Ns\\f" / "const Ns\\C" for those in a namespace ("" for the global ones: no import).
 * Without Composer: the functions, classes and constants of the other files of the project that
 * have no namespace, with their path for a require_once. */
static void php_project(SymCache *c, Env *e, HdrFn fn, void *ctx)
{
    char path[PATH_MAX], spec[PATH_MAX + 32];
    struct stat st;
    size_t i, j, k, seen = 0;
    int composer;

    rooted(c, path, sizeof path, "composer.json");
    composer = stat(path, &st) == 0;
    if (composer) {
        FILE *f = fopen(path, "r");
        char *txt;
        size_t n;
        const char *p;

        if (!f)
            return;
        txt = xmalloc(256 * 1024 + 1);
        n = fread(txt, 1, 256 * 1024, f);
        fclose(f);
        txt[n] = '\0';
        for (p = strstr(txt, "\"files\""); p; p = strstr(p + 7, "\"files\"")) {
            const char *q = strchr(p, '['), *end = q ? strchr(q, ']') : NULL;

            while (q && end && q < end) {
                const char *a = strchr(q, '"'), *b;
                char rel[PATH_MAX], *real;
                struct Hdr *h;

                if (!a || a > end)
                    break;
                b = strchr(a + 1, '"');
                if (!b || b > end)
                    break;
                put(rel, sizeof rel, a + 1, (size_t)(b - a - 1));
                q = b + 1;
                rooted(c, path, sizeof path, rel);
                real = realpath(path, NULL);
                if (!real)
                    continue;
                h = hdr_get(c, real);
                for (j = 0; h && j < h->n; j++) {
                    const HSym *s = &h->v[j];

                    if (s->kind == RULE_CLASS)
                        continue;           /* classes come through Composer's maps and "use" */
                    if (s->ns)
                        snprintf(spec, sizeof spec, "%s %.200s\\%s", s->kind == RULE_FUNC ? "function" : "const",
                                 s->ns, s->name);
                    else
                        spec[0] = '\0';
                    fn(ctx, s->name, s->kind, s->sig, s->ret, spec);
                }
                free(real);
            }
        }
        free(txt);
        return;
    }
    if (!ensure_files(c))
        return;
    for (i = 0; i < c->nfiles && seen < HDR_MAX; i++) {
        const char *rel = c->files[i];
        size_t rl = strlen(rel);
        char *real;
        struct Hdr *h;
        int visible = 0, nsd = 0;

        if (strcmp(ext_of(rel), "php") != 0 || (rl > 10 && strcmp(rel + rl - 10, ".blade.php") == 0))
            continue;
        seen++;
        rooted(c, path, sizeof path, rel);
        real = realpath(path, NULL);
        if (!real)
            continue;
        for (k = 0; k < e->n; k++)
            if (e->v[k]->key && strcmp(e->v[k]->key, real) == 0)
                visible = 1;
        h = visible ? NULL : hdr_get(c, real);
        for (j = 0; h && j < h->n; j++)
            if (h->v[j].ns)
                nsd = 1;                    /* a namespace without an autoloader: not this way */
        if (h && h->n && !nsd) {
            rel_path(e->v[0]->key, real, spec, sizeof spec);
            for (j = 0; j < h->n; j++)
                fn(ctx, h->v[j].name, h->v[j].kind, h->v[j].sig, h->v[j].ret, spec);
        }
        free(real);
    }
}

void sym_project_symbols(SymCache *c, Doc *const *docs, size_t ndocs, Doc *cur, HdrFn fn, void *ctx)
{
    Env e;
    size_t i, j, k, seen = 0;
    const char *lang = syn_name(cur->syn);
    int cl = strcmp(lang, "C") == 0, js = strcmp(lang, "JavaScript") == 0 || strcmp(lang, "TypeScript") == 0;
    int py = strcmp(lang, "Python") == 0;
    int php = strcmp(lang, "PHP") == 0 || strcmp(lang, "PHP/HTML") == 0;

    if (!cur->buf.path || (!cl && !js && !py && !php))
        return;
    env_build(c, docs, ndocs, cur, &e);
    if (!e.v[0]->key)
        return;
    if (php) {
        php_project(c, &e, fn, ctx);
        return;
    }
    if (!ensure_files(c))
        return;
    for (i = 0; i < c->nfiles && seen < HDR_MAX; i++) {
        char path[PATH_MAX], *real, spec[PATH_MAX + 1];
        struct Hdr *h;
        int visible = 0;

        if (cl ? !is_header(c->files[i]) : py ? !is_py_module(c->files[i]) : !is_js_module(c->files[i]))
            continue;
        seen++;
        rooted(c, path, sizeof path, c->files[i]);
        real = realpath(path, NULL);
        if (!real)
            continue;
        /* C: a header the file reaches needs nothing; JS: a module it imports may still lack the
         * name (import { a } from './m' and b is wanted), so only the file itself is left out */
        for (k = 0; k < (cl ? e.n : 1); k++)
            if (e.v[k]->key && strcmp(e.v[k]->key, real) == 0)
                visible = 1;
        /* C: included under a name the editor cannot resolve ("buffer.h" with -Isrc) */
        for (k = 0; cl && k < e.v[0]->ninc && !visible; k++) {
            const char *sp = e.v[0]->inc[k], *sb = strrchr(sp, '/'), *rb = strrchr(real, '/');

            if (sp[0] != ':' && strcmp(sb ? sb + 1 : sp, rb ? rb + 1 : real) == 0)
                visible = 1;
        }
        h = visible ? NULL : hdr_get(c, real);
        if (h && h->n) {
            if (cl)
                hdr_spec(e.v[0], real, spec, sizeof spec);
            else if (py)
                py_module_name(real, spec, sizeof spec);
            else
                js_spec(e.v[0], real, spec + 1, sizeof spec - 1);
            if (js)
                spec[0] = '=';              /* a default export is told by a leading "=" */
            for (j = 0; j < h->n; j++)
                fn(ctx, h->v[j].name, h->v[j].kind, h->v[j].sig, h->v[j].ret,
                   !js || h->v[j].def ? spec : spec + 1);
        }
        free(real);
    }
}

/* ---- calls, parameters and local variables ------------------------------ */

#define CALL_LINES 40
#define CALL_NOT " if while for foreach switch catch elif return and or not in else with until unless match "
#define CALL_DECLKW " function def fn func fun sub "
#define CALL_NOT_DECL \
    " return else goto case break continue delete throw typedef do if while switch sizeof " \
    "using namespace new not and or in extends implements "
#define CALL_QUAL \
    " const struct enum union unsigned signed volatile static final readonly public private " \
    "protected var register extern inline ref out in mut "

static int in_words(const char *list, const char *w, size_t n)
{
    size_t i = 0;

    while (list[i]) {
        size_t j;

        while (list[i] == ' ')
            i++;
        for (j = i; list[j] && list[j] != ' '; j++)
            ;
        if (j > i && j - i == n && memcmp(list + i, w, n) == 0)
            return 1;
        i = j;
    }
    return 0;
}

static size_t ident_back(const char *s, size_t end, size_t *start)
{
    size_t b = end;

    while (b > 0 && is_idc((unsigned char)s[b - 1]))
        b--;
    *start = b;
    return end - b;
}

static int cl_skip(const uint8_t *cl, size_t i)
{
    return cl && (cl[i] == SC_COMMENT || cl[i] == SC_STRING);
}

/* C: the name at ns is preceded by a type at the start of a statement ("static int foo(") */
static int c_decl_before(const char *s, size_t ns)
{
    size_t p = ns, ids = 0;

    for (;;) {
        size_t b, n;

        while (p > 0 && (is_blank(s[p - 1]) || s[p - 1] == '*' || s[p - 1] == '&'))
            p--;
        if (p == 0 || !is_idc((unsigned char)s[p - 1]))
            break;
        n = ident_back(s, p, &b);
        if (in_words(CALL_NOT_DECL, s + b, n))
            return 0;
        ids++;
        p = b;
    }
    return ids > 0 && (p == 0 || strchr(";{}", s[p - 1]) != NULL);
}

/* Is the word that ends the segment s[from, to) of a parameter list where a type goes? */
static int type_pos(const char *lang, const char *s, size_t from, size_t to)
{
    size_t i = from;
    int ids = 0, colon = 0, eq = 0, dollar = 0;
    long d = 0;

    while (i < to) {
        char c = s[i];

        if (is_idc((unsigned char)c)) {
            size_t b = i;

            while (i < to && is_idc((unsigned char)s[i]))
                i++;
            if (!in_words(CALL_QUAL, s + b, i - b))
                ids++;
            continue;
        }
        if (c == '(' || c == '[' || c == '<' || c == '{')
            d++;
        else if (c == ')' || c == ']' || c == '>' || c == '}')
            d--;
        else if (c == '$')
            dollar = 1;
        else if (c == '=' && d == 0)
            eq = 1;
        else if (c == ':' && d == 0)
            colon = 1;
        else if (c == '|' && d == 0)
            ids = 0;
        i++;
    }
    if (strncmp(lang, "PHP", 3) == 0 || strcmp(lang, "C") == 0)
        return ids == 0 && !eq && !dollar;
    return colon && !eq;
}

int sym_call_at(Syntax *syn, const Buffer *b, size_t y, size_t x, SymCall *o)
{
    long depth = 0;
    int commas = 0;
    size_t ly = y, lines, i = 0, seg_y = (size_t)-1, seg_x = 0, ns, ne, n, q, pb, pn;
    const char *lang, *s;

    memset(o, 0, sizeof *o);
    if (!syn || y >= b->nlines)
        return 0;
    lang = syn_name(syn);
    for (lines = 0; lines < CALL_LINES; lines++) {
        const Line *l = &b->lines[ly];
        const uint8_t *cl;

        syn_update(syn, (Buffer *)b, ly);
        cl = syn_classes(syn, b, ly);
        for (i = lines == 0 && x < l->len ? x : l->len; i > 0; i--) {
            char c = l->s[i - 1];

            if (cl_skip(cl, i - 1))
                continue;
            if (c == ')' || c == ']' || c == '}') {
                depth++;
            } else if (c == '(' || c == '[' || c == '{') {
                if (depth > 0)
                    depth--;
                else if (c == '(')
                    goto found;
                else
                    return 0;
            } else if (depth == 0 && c == ',') {
                if (commas++ == 0) {
                    seg_y = ly;
                    seg_x = i;
                }
            } else if (depth == 0 && c == ';') {
                return 0;
            }
        }
        if (ly == 0)
            return 0;
        ly--;
    }
    return 0;
found:
    s = b->lines[ly].s;
    o->y = ly;
    o->x = i - 1;
    o->arg = commas;
    ne = i - 1;
    while (ne > 0 && is_blank(s[ne - 1]))
        ne--;
    n = ident_back(s, ne, &ns);
    if (n == 0 || n >= PM_NAME || isdigit((unsigned char)s[ns]))
        goto params;
    if (in_words(CALL_DECLKW, s + ns, n)) {
        o->decl = 1;                    /* an anonymous function */
        goto params;
    }
    if (in_words(CALL_NOT, s + ns, n))
        goto params;
    memcpy(o->name, s + ns, n);
    o->name[n] = '\0';
    o->name_x = ns;
    q = ns;
    while (q > 0 && is_blank(s[q - 1]))
        q--;
    pn = q > 0 && is_idc((unsigned char)s[q - 1]) ? ident_back(s, q, &pb) : 0;
    if (pn && in_words(CALL_DECLKW, s + pb, pn))
        o->decl = 1;
    else if (pn == 3 && memcmp(s + pb, "new", 3) == 0)
        o->isnew = 1;
    else if (pn == 5 && memcmp(s + pb, "class", 5) == 0)
        o->name[0] = '\0';
    else if (strcmp(lang, "C") == 0 && c_decl_before(s, ns))
        o->decl = 1;
params:
    if (o->decl) {
        size_t sy = seg_y == (size_t)-1 ? ly : seg_y, sx = seg_y == (size_t)-1 ? o->x + 1 : seg_x;

        if (sy != y)
            sx = 0;
        if (y < b->nlines && sx <= x && x <= b->lines[y].len)
            o->typepos = type_pos(lang, b->lines[y].s, sx, x);
    }
    return 1;
}

int sym_param_span(const char *sig, int arg, size_t *from, size_t *to)
{
    size_t i, n = strlen(sig), start = 0, last = 0;
    int idx = 0;
    long d = 0;

    if (n == 0)
        return 0;
    for (i = 0; i <= n; i++) {
        char c = i < n ? sig[i] : ',';

        if (i < n && (c == '(' || c == '[' || c == '<' || c == '{'))
            d++;
        else if (i < n && (c == ')' || c == ']' || c == '}' ||
                           (c == '>' && i > 0 && sig[i - 1] != '-' && sig[i - 1] != '=')))
            d--;
        else if (c == ',' && d <= 0) {
            last = start;
            if (idx == arg)
                goto got;
            idx++;
            start = i + 1;
        }
    }
    /* more arguments than parameters: fine after a variadic one */
    if (strstr(sig + last, "...") == NULL && sig[last] != '*' && strstr(sig + last, " *") != sig + last)
        return 0;
    start = last;
    i = n;
got:
    while (start < i && sig[start] == ' ')
        start++;
    while (i > start && sig[i - 1] == ' ')
        i--;
    *from = start;
    *to = i;
    return i > start;
}

int sym_call_hint(SymCache *c, Doc *const *docs, size_t ndocs, Doc *cur, size_t y, size_t x,
                  char *out, size_t cap, size_t *hl_from, size_t *hl_to)
{
    SymCall call;
    Env e;
    FindCtx f;
    char t[PM_NAME];
    size_t from, to, pre;

    *hl_from = *hl_to = 0;
    if (!cur->syn || syn_rule_count(cur->syn) == 0 ||
        !sym_call_at(cur->syn, &cur->buf, y, x, &call) || !call.name[0] || call.decl)
        return 0;
    memset(&f, 0, sizeof f);
    f.name = call.name;
    env_build(c, docs, ndocs, cur, &e);
    if (call.isnew) {
        static const char *const ctors[] = { "__construct", "constructor", "__init__" };
        size_t k;

        for (k = 0; k < 3 && !f.found; k++) {
            f.name = ctors[k];
            env_members(&e, call.name, find_cb, &f, 0);
        }
    } else {
        size_t sep = sym_member_at(cur->syn, &cur->buf, call.y, call.name_x);

        if (sep != (size_t)-1) {
            env_alias(c, &e, cur, call.y, sep);
            resolve_chain(&e, cur, call.y, sep, t, sizeof t);
            if (t[0])
                env_members(&e, t, find_cb, &f, 0);
        } else {
            env_func_find(&e, &f);
        }
    }
    if (!f.found || (f.kind != RULE_FUNC && f.kind != RULE_METHOD))
        return 0;
    snprintf(out, cap, "%s(%s)", call.name, f.sig);
    pre = strlen(call.name) + 1;
    if (sym_param_span(f.sig, call.arg, &from, &to) && pre + to < cap) {
        *hl_from = pre + from;
        *hl_to = pre + to;
    }
    return 1;
}

/* -- local variables -- */

static int lang_family(const char *n)
{
    if (strncmp(n, "PHP", 3) == 0)
        return 1;
    if (strcmp(n, "Python") == 0)
        return 2;
    if (strcmp(n, "JavaScript") == 0 || strcmp(n, "TypeScript") == 0)
        return 3;
    return strcmp(n, "C") == 0 ? 4 : 0;
}

static void local_emit(SymFn fn, void *ctx, const char *w, size_t n)
{
    char name[PM_NAME];

    if (n == 0 || n >= PM_NAME || isdigit((unsigned char)w[0]) || in_words(" self this cls void ", w, n))
        return;
    put(name, sizeof name, w, n);
    fn(ctx, name, RULE_VAR, "", "", 0);
}

/* The names in a parameter list ("int a, char *b", "$x = 1, Foo $y", "a, b: int = 3"). */
static void local_params(const char *sig, int fam, SymFn fn, void *ctx)
{
    size_t i = 0, n = strlen(sig);

    while (i <= n) {
        size_t j = i, e, b, k;
        long d = 0;
        int paren = 0;
        size_t eq = (size_t)-1, colon = (size_t)-1;

        while (j < n && !(sig[j] == ',' && d <= 0)) {
            char c = sig[j];

            if (c == '(' || c == '[' || c == '<' || c == '{')
                d++;
            else if (c == ')' || c == ']' || c == '>' || c == '}')
                d--;
            if (c == '(' && fam == 4)
                paren = 1;
            if (d == 0 && c == '=' && eq == (size_t)-1)
                eq = j;
            else if (d == 0 && c == ':' && colon == (size_t)-1 && eq == (size_t)-1)
                colon = j;
            j++;
        }
        e = eq != (size_t)-1 ? eq : colon != (size_t)-1 ? colon : j;
        for (k = i; k < e; k++)
            if (sig[k] == '[') {
                e = k;
                break;
            }
        while (e > i && (is_blank(sig[e - 1]) || sig[e - 1] == '?'))
            e--;
        if (!paren && ident_back(sig, e, &b) > 0) {
            size_t p = b;

            while (p > i && (is_blank(sig[p - 1]) || sig[p - 1] == '*' || sig[p - 1] == '&'))
                p--;
            /* C: a lone "int" is an unnamed parameter */
            if (fam != 4 || (p > i && is_idc((unsigned char)sig[p - 1])))
                local_emit(fn, ctx, sig + b, e - b);
        }
        i = j + 1;
    }
}

/* C: a declaration "type [*] name" at the start of a statement; returns where to go on. */
static size_t c_stmt(const char *s, size_t i, size_t lim, const uint8_t *cl, SymFn fn, void *ctx)
{
    size_t p = i, ids = 0, lb = 0, ln = 0;

    for (;;) {
        while (p < lim && is_blank(s[p]))
            p++;
        if (p >= lim)
            break;
        if (is_idc((unsigned char)s[p])) {
            size_t b = p;

            while (p < lim && is_idc((unsigned char)s[p]))
                p++;
            if (in_words(CALL_NOT_DECL, s + b, p - b))
                return p;
            ids++;
            lb = b;
            ln = p - b;
        } else if (ids > 0 && (s[p] == '*' || s[p] == '&')) {
            p++;
        } else if (ids > 0 && p + 1 < lim && s[p] == ':' && s[p + 1] == ':') {
            p += 2;
        } else if (ids > 0 && s[p] == '<') {
            int d = 0;

            for (; p < lim; p++) {
                d += s[p] == '<';
                d -= s[p] == '>';
                if (d == 0) {
                    p++;
                    break;
                }
            }
        } else {
            break;
        }
    }
    if (ids >= 2 && p < lim && strchr("=;,[)", s[p]) && !cl_skip(cl, p)) {
        local_emit(fn, ctx, s + lb, ln);
        /* int a, *b, c; */
        while (s[p] == ',' && p < lim) {
            size_t b;

            p++;
            while (p < lim && (is_blank(s[p]) || s[p] == '*'))
                p++;
            b = p;
            while (p < lim && is_idc((unsigned char)s[p]))
                p++;
            if (p == b || p >= lim || !strchr("=;,[", s[p]))
                break;
            local_emit(fn, ctx, s + b, p - b);
        }
    }
    return p > i ? p : i + 1;
}

static void local_line(int fam, const char *s, size_t lim, const uint8_t *cl, SymFn fn, void *ctx)
{
    size_t i = 0;
    int start = 1;              /* C: at the start of a statement */

    while (i < lim) {
        size_t b, j;

        if (cl_skip(cl, i) || is_blank(s[i])) {
            i++;
            continue;
        }
        if (fam == 1) {
            if (s[i] == '$' && i + 1 < lim && is_idc((unsigned char)s[i + 1])) {
                for (j = i + 1; j < lim && is_idc((unsigned char)s[j]); j++)
                    ;
                local_emit(fn, ctx, s + i + 1, j - i - 1);
                i = j;
            } else {
                i++;
            }
            continue;
        }
        if (!is_idc((unsigned char)s[i]) || (i > 0 && is_idc((unsigned char)s[i - 1]))) {
            if (fam == 4 && start == 0 && (s[i] == ';' || s[i] == '{' || s[i] == '}'))
                start = 1;
            i++;
            continue;
        }
        b = i;
        for (j = i; j < lim && is_idc((unsigned char)s[j]); j++)
            ;
        i = j;
        if (fam == 4) {
            if (j - b == 3 && memcmp(s + b, "for", 3) == 0) {
                while (i < lim && is_blank(s[i]))
                    i++;
                if (i < lim && s[i] == '(') {
                    i++;
                    start = 1;
                }
                continue;
            }
            if (start) {
                i = c_stmt(s, b, lim, cl, fn, ctx);
                start = 0;
            }
            continue;
        }
        if ((fam == 3 && (in_words(" let const var ", s + b, j - b))) ||
            ((fam == 2 || fam == 3) && j - b == 2 && memcmp(s + b, "as", 2) == 0)) {
            size_t p = j;

            while (p < lim && is_blank(s[p]))
                p++;
            for (j = p; j < lim && is_idc((unsigned char)s[j]); j++)
                ;
            local_emit(fn, ctx, s + p, j - p);
            i = j;
        } else if (fam == 2 && j - b == 3 && memcmp(s + b, "for", 3) == 0) {
            size_t p = j;

            for (;;) {
                while (p < lim && (is_blank(s[p]) || s[p] == ',' || s[p] == '('))
                    p++;
                for (j = p; j < lim && is_idc((unsigned char)s[j]); j++)
                    ;
                if (j == p || (j - p == 2 && memcmp(s + p, "in", 2) == 0))
                    break;
                local_emit(fn, ctx, s + p, j - p);
                p = j;
            }
            i = p;
        } else if (fam == 2) {
            /* a target at the start of the line: "a, b = ...", "x: int = ..." */
            size_t k = 0, p = b, lead = 0;
            size_t nb[4], nl[4];

            while (lead < b && is_blank(s[lead]))
                lead++;
            if (lead != b)
                continue;
            for (;;) {
                size_t e = p;

                while (e < lim && is_idc((unsigned char)s[e]))
                    e++;
                if (e == p || k >= 4)
                    break;
                nb[k] = p;
                nl[k++] = e - p;
                p = e;
                while (p < lim && is_blank(s[p]))
                    p++;
                if (p < lim && s[p] == ',') {
                    p++;
                    while (p < lim && is_blank(s[p]))
                        p++;
                    continue;
                }
                break;
            }
            if (k == 0 || k > 4)
                continue;
            if (p < lim && s[p] == ':' && k == 1) {
                size_t q;

                for (q = p; q < lim; q++)
                    if (s[q] == '=' && (q + 1 >= lim || s[q + 1] != '=') && s[q - 1] != '=')
                        break;
                if (q >= lim)
                    continue;
            } else if (!(p < lim && s[p] == '=' && (p + 1 >= lim || s[p + 1] != '='))) {
                continue;
            }
            for (j = 0; j < k; j++)
                local_emit(fn, ctx, s + nb[j], nl[j]);
        }
    }
}

static const Sym *enclosing_func(const SymIndex *x, size_t y)
{
    const Sym *best = NULL;
    size_t i;

    for (i = 0; i < x->n; i++)
        if ((x->v[i].kind == RULE_FUNC || x->v[i].kind == RULE_METHOD) && x->v[i].name[0] &&
            x->v[i].y0 <= y && (!best || x->v[i].y0 >= best->y0))
            best = &x->v[i];
    return best;
}

/* Has the function declared at line y0 already ended above line y? */
static int func_ended(Syntax *syn, const Buffer *b, size_t y0, size_t y)
{
    size_t k, i, d0 = 0;
    long depth = 0;
    int opened = 0;

    if (y - y0 > 3000)
        return 0;
    if (syn_indent_scope(syn)) {
        while (d0 < b->lines[y0].len && is_blank(b->lines[y0].s[d0]))
            d0++;
        for (k = y0 + 1; k <= y; k++) {
            const Line *l = &b->lines[k];
            size_t ind = 0;

            while (ind < l->len && is_blank(l->s[ind]))
                ind++;
            if (ind == l->len || l->s[ind] == ')' || l->s[ind] == ']')
                continue;
            if (ind <= d0)
                return 1;
        }
        return 0;
    }
    for (k = y0; k < y; k++) {
        const Line *l = &b->lines[k];
        const uint8_t *cl;

        syn_update(syn, (Buffer *)b, k);
        cl = syn_classes(syn, b, k);
        for (i = 0; i < l->len; i++) {
            if (cl_skip(cl, i))
                continue;
            if (l->s[i] == '{') {
                depth++;
                opened = 1;
            } else if (l->s[i] == '}') {
                depth--;
                if (opened && depth <= 0)
                    return 1;
            } else if (l->s[i] == ';' && !opened && depth == 0) {
                return 1;           /* a prototype: no body */
            }
        }
    }
    return 0;
}

void sym_locals(SymCache *c, Doc *cur, size_t y, size_t xlimit, SymFn fn, void *ctx)
{
    const SymIndex *x;
    const Sym *f;
    Syntax *syn = cur->syn;
    int fam;
    size_t k;

    if (!syn || y >= cur->buf.nlines)
        return;
    fam = lang_family(syn_name(syn));
    if (!fam)
        return;
    x = index_of_doc(c, cur);
    f = enclosing_func(x, y);
    if (!f || f->y0 >= cur->buf.nlines || func_ended(syn, &cur->buf, f->y0, y))
        return;
    local_params(f->sig, fam, fn, ctx);
    for (k = f->y0; k <= y; k++) {
        const Line *l = &cur->buf.lines[k];
        const uint8_t *cl;

        syn_update(syn, &cur->buf, k);
        cl = syn_classes(syn, &cur->buf, k);
        local_line(fam, l->s, k == y && xlimit < l->len ? xlimit : l->len, cl, fn, ctx);
    }
}

/* ---- outline of a file --------------------------------------------------- */

typedef struct {
    const Sym *s;
    size_t y, x;
} OutRow;

static int out_cmp(const void *pa, const void *pb)
{
    const OutRow *a = pa, *b = pb;

    if (a->y != b->y)
        return a->y < b->y ? -1 : 1;
    return a->x < b->x ? -1 : a->x > b->x;
}

/* Where the name of s starts on its line, as a whole word; (size_t)-1 if it is not there as
 * code (a member made up by a doc comment, or reached through an object: "$this->x = "). */
static size_t name_pos(Syntax *syn, const Buffer *b, const Sym *s)
{
    const Line *l;
    const uint8_t *cl;
    size_t n = strlen(s->name), i;

    if (s->y0 >= b->nlines || n == 0)
        return (size_t)-1;
    l = &b->lines[s->y0];
    syn_update(syn, (Buffer *)b, s->y0);
    cl = syn_classes(syn, b, s->y0);
    for (i = 0; i + n <= l->len; i++) {
        if (memcmp(l->s + i, s->name, n) != 0 || (i > 0 && is_idc((unsigned char)l->s[i - 1])) ||
            (i + n < l->len && is_idc((unsigned char)l->s[i + n])))
            continue;
        if (cl_skip(cl, i))
            continue;
        if (s->kind == RULE_PROP && i > 0 && (l->s[i - 1] == '>' || l->s[i - 1] == '.'))
            return (size_t)-1;
        return i;
    }
    return (size_t)-1;
}

int sym_outline(SymCache *c, Doc *cur, OutlineFn fn, void *ctx)
{
    const SymIndex *x;
    OutRow *rows;
    size_t i, n = 0;
    const char *sep;

    if (!cur->syn || syn_rule_count(cur->syn) == 0)
        return 0;
    x = index_of_doc(c, cur);
    rows = xmalloc((x->n + 1) * sizeof *rows);
    for (i = 0; i < x->n; i++) {
        const Sym *s = &x->v[i];
        size_t col;

        if (!s->name[0] || s->kind == RULE_VAR)
            continue;
        col = name_pos(cur->syn, &cur->buf, s);
        if (col == (size_t)-1)
            continue;
        rows[n].s = s;
        rows[n].y = s->y0;
        rows[n].x = col;
        n++;
    }
    qsort(rows, n, sizeof *rows, out_cmp);
    sep = strncmp(syn_name(cur->syn), "PHP", 3) == 0 ? "::" : ".";
    for (i = 0; i < n; i++) {
        const Sym *s = rows[i].s;
        char q[2 * PM_NAME + 4];

        if (s->owner >= 0 && (size_t)s->owner < x->n && x->v[s->owner].name[0])
            snprintf(q, sizeof q, "%s%s%s", x->v[s->owner].name, sep, s->name);
        else
            snprintf(q, sizeof q, "%s", s->name);
        fn(ctx, s->kind, q, s->sig, rows[i].y, rows[i].x);
    }
    free(rows);
    return (int)n;
}
