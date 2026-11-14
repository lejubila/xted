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
            size_t st, ls;

            if (i < len && s[i] == '?')
                i++;
            if (i < len && s[i] == '\\')
                i++;
            for (;;) {
                st = i;
                while (i < len && is_idc((unsigned char)s[i]))
                    i++;
                if (i == st)
                    return 0;
                ls = st;
                if (i < len && s[i] == '\\')
                    i++;
                else if (i + 1 < len && s[i] == ':' && s[i + 1] == ':' && i + 2 < len &&
                         is_idc((unsigned char)s[i + 2]))
                    i += 2;
                else
                    break;
            }
            if (is_reject(s + ls, i - ls))
                return 0;
            put(m->type, sizeof m->type, s + ls, i - ls);
            if (i < len && s[i] == '<')
                i = skip_angle(s, len, i);
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
            B->pend_owner = ix_add(B->x, RULE_CLASS, m.name, "", m.type, -1, y);
            B->pend = F_CLASS;
            return kind;
        }
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
            ix_add(B->x, kind, m.name, sg, ret, owner, y);
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
            if (m.path[0]) {
                ix_inc(B->x, m.path);
            } else if (m.type[0]) {
                char spec[PM_NAME + 1];

                spec[0] = ':';
                memcpy(spec + 1, m.type, strlen(m.type) + 1);
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
#define PROJECT_FILES_MAX 20000

struct SymCache {
    SymIndex **v;
    size_t n;
    unsigned long tick;
    char **files;
    size_t nfiles;
    int files_done;
    StdLib *lib[LIB_N];
    int lib_tried[LIB_N];
};

SymCache *sym_cache_new(void)
{
    SymCache *c = xmalloc(sizeof *c);

    memset(c, 0, sizeof *c);
    return c;
}

void sym_cache_free(SymCache *c)
{
    size_t i;

    if (!c)
        return;
    for (i = 0; i < c->n; i++)
        sym_index_free(c->v[i]);
    for (i = 0; i < c->nfiles; i++)
        free(c->files[i]);
    for (i = 0; i < LIB_N; i++)
        std_close(c->lib[i]);
    free(c->v);
    free(c->files);
    free(c);
}

static long now_us(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000000L + t.tv_nsec / 1000;
}

static void cache_put(SymCache *c, SymIndex *x)
{
    size_t i, victim = 0;

    if (c->n >= CACHE_MAX) {
        unsigned long best = (unsigned long)-1;

        for (i = 0; i < c->n; i++)
            if (!c->v[i]->buf && c->v[i]->used < best) {
                best = c->v[i]->used;
                victim = i;
            }
        sym_index_free(c->v[victim]);
        c->v[victim] = x;
        return;
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

/* A project file called Name.<ext>, for a class mentioned by name (PHP `use`). */
static char *find_class_file(SymCache *c, const char *name, const char *ext)
{
    size_t i, nl = strlen(name), el = strlen(ext);

    if (!c->files_done) {
        files_scan(".", collect_file, c);
        c->files_done = 1;
    }
    for (i = 0; i < c->nfiles; i++) {
        const char *f = c->files[i], *base = strrchr(f, '/');
        size_t bl;

        base = base ? base + 1 : f;
        bl = strlen(base);
        if (bl == nl + 1 + el && memcmp(base, name, nl) == 0 && base[nl] == '.' &&
            memcmp(base + nl + 1, ext, el) == 0)
            return realpath(f, NULL);
    }
    return NULL;
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
} Env;

static void env_add(Env *e, SymIndex *x, int depth)
{
    size_t i;

    if (!x || e->n >= SET_MAX)
        return;
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

static int looks_cxx(const Doc *d)
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
        if (x->inc[j][0] == ':' && !x->incp[j] && !env_has_class(e, x->inc[j] + 1)) {
            char *f = find_class_file(c, x->inc[j] + 1, ext_of(x->key));

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
    if (strcmp(lang, "PHP") == 0 || strcmp(lang, "PHP/HTML") == 0) {
        e->lib[e->nlib++] = lib_get(c, LIB_PHP);
    } else if (strcmp(lang, "Python") == 0) {
        e->lib[e->nlib++] = lib_get(c, LIB_PY);
    } else if (strcmp(lang, "JavaScript") == 0 || strcmp(lang, "TypeScript") == 0) {
        e->lib[e->nlib++] = lib_get(c, LIB_JS);
    } else if (strcmp(lang, "C") == 0) {
        if (looks_cxx(cur))
            e->lib[e->nlib++] = lib_get(c, LIB_CPP);
        e->lib[e->nlib++] = lib_get(c, LIB_C);
    }
    env_add(e, index_of_doc(c, cur), 0);
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
static int env_members(const Env *e, const char *cls, SymFn fn, void *ctx, int depth)
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
            for (j = ci + 1; j < x->n; j++)
                if (x->v[j].owner == (long)ci && x->v[j].name[0])
                    fn(ctx, x->v[j].name, x->v[j].kind, x->v[j].sig, x->v[j].ret, i == 0 ? 0 : 1);
            if (c->ret[0]) {
                char base[PM_NAME];

                norm_type(c->ret, base, sizeof base);
                if (strcmp(base, cls) != 0)
                    env_members(e, base, fn, ctx, depth + 1);
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

/* A function (global) named name: its return type. */
static int env_function(const Env *e, const char *name, char *ret, size_t cap)
{
    size_t i, j;
    StdCtx sc;
    FindCtx f;

    for (i = 0; i < e->n; i++)
        for (j = 0; j < e->v[i]->n; j++) {
            const Sym *s = &e->v[i]->v[j];

            if (s->kind == RULE_FUNC && s->owner < 0 && strcmp(s->name, name) == 0) {
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
    if (f.found && f.kind == RULE_FUNC) {
        put(ret, cap, f.ret, strlen(f.ret));
        return 1;
    }
    return 0;
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
                    put(type, cap, m.type, strlen(m.type));
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
static void settle(const Env *e, const char *raw, char *out, size_t cap)
{
    char t[PM_NAME], r[PM_NAME];

    norm_type(raw, t, sizeof t);
    out[0] = '\0';
    if (!t[0])
        return;
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
