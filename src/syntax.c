#include "syntax.h"
#include "langs.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BRACKET_SCAN_LINES 5000

typedef struct {
    char *start, *end;      /* end == NULL: runs to the end of the line */
    size_t sl, el;
    uint8_t cls;
    char esc;               /* escape character, 0 for none */
    int ml, bol, ns;        /* multi-line, only at line start, start not followed by a space */
} Region;

typedef struct {
    const char *w;
    uint8_t len, cls;
} WordEnt;

struct Syntax {
    char *name;
    Region *reg;
    size_t nreg, regcap;
    WordEnt *tab;           /* open addressing, size is a power of two; w == NULL: free */
    size_t tabsize;
    char *pool;             /* storage for the words */
    int number, calls, sigil;
    char *cmt, *cmt_end;    /* comment markers for the toggle command; cmt_end may be NULL */
    uint8_t call_cls, sigil_cls;
    uint8_t opch[256], idstart[256], idch[256];
    uint8_t *scratch;
    size_t scratch_cap;
};

static const char *const class_names[SC_COUNT] = {
    "normal", "comment", "string", "keyword", "type", "constant", "number",
    "preproc", "function", "operator", "variable", "heading", "emphasis",
    "code", "link"
};

static int class_by_name(const char *s, size_t n)
{
    int i;

    for (i = 0; i < SC_COUNT; i++)
        if (strlen(class_names[i]) == n && memcmp(class_names[i], s, n) == 0)
            return i;
    return -1;
}

/* ---- parser ------------------------------------------------------------ */

typedef struct {
    const char *p;
    size_t n;
} Tok;

/* Splits a line on blanks; returns the number of tokens (up to max). */
static size_t tokenize(const char *s, size_t n, Tok *t, size_t max)
{
    size_t i = 0, k = 0;

    while (i < n && k < max) {
        size_t j;

        while (i < n && (s[i] == ' ' || s[i] == '\t'))
            i++;
        if (i >= n)
            break;
        j = i;
        while (j < n && s[j] != ' ' && s[j] != '\t')
            j++;
        t[k].p = s + i;
        t[k].n = j - i;
        k++;
        i = j;
    }
    return k;
}

static int tok_is(Tok t, const char *s)
{
    return strlen(s) == t.n && memcmp(t.p, s, t.n) == 0;
}

typedef struct {
    size_t off;
    uint8_t len, cls;
} PendWord;

static uint32_t hash_word(const char *w, size_t n)
{
    uint32_t h = 2166136261u;

    while (n-- > 0)
        h = (h ^ (uint8_t)*w++) * 16777619u;
    return h;
}

static void build_table(Syntax *s, const PendWord *pw, size_t npw)
{
    size_t i, size = 16;

    while (size < npw * 2)
        size *= 2;
    s->tab = xmalloc(size * sizeof *s->tab);
    memset(s->tab, 0, size * sizeof *s->tab);
    s->tabsize = size;
    for (i = 0; i < npw; i++) {
        const char *w = s->pool + pw[i].off;
        size_t h = hash_word(w, pw[i].len) & (size - 1);

        while (s->tab[h].w &&
               !(s->tab[h].len == pw[i].len && memcmp(s->tab[h].w, w, pw[i].len) == 0))
            h = (h + 1) & (size - 1);
        s->tab[h].w = w;
        s->tab[h].len = pw[i].len;
        s->tab[h].cls = pw[i].cls;
    }
}

static int lookup(const Syntax *s, const char *w, size_t n)
{
    size_t h;

    if (!s->tab || n > 255)
        return -1;
    h = hash_word(w, n) & (s->tabsize - 1);
    while (s->tab[h].w) {
        if (s->tab[h].len == n && memcmp(s->tab[h].w, w, n) == 0)
            return s->tab[h].cls;
        h = (h + 1) & (s->tabsize - 1);
    }
    return -1;
}

void syn_free(Syntax *s)
{
    size_t i;

    if (!s)
        return;
    for (i = 0; i < s->nreg; i++) {
        free(s->reg[i].start);
        free(s->reg[i].end);
    }
    free(s->reg);
    free(s->tab);
    free(s->pool);
    free(s->name);
    free(s->cmt);
    free(s->cmt_end);
    free(s->scratch);
    free(s);
}

const char *syn_name(const Syntax *s)
{
    return s ? s->name : "Testo";
}

int syn_comment(const Syntax *s, const char **open, const char **close)
{
    if (!s || !s->cmt)
        return 0;
    *open = s->cmt;
    *close = s->cmt_end;
    return 1;
}

void syn_each_word(const Syntax *s, void (*fn)(const char *w, size_t n, int cls, void *ctx),
                   void *ctx)
{
    size_t i;

    if (!s || !s->tab)
        return;
    for (i = 0; i < s->tabsize; i++)
        if (s->tab[i].w)
            fn(s->tab[i].w, s->tab[i].len, s->tab[i].cls, ctx);
}

static void set_chars(uint8_t *table, const char *p, size_t n)
{
    while (n-- > 0)
        table[(uint8_t)*p++] = 1;
}

#define PERR(...) do { snprintf(err, errlen, __VA_ARGS__); goto fail; } while (0)

Syntax *syn_parse(const char *text, size_t n, char *err, size_t errlen)
{
    Syntax *s = xmalloc(sizeof *s);
    PendWord *pw = NULL;
    size_t npw = 0, pwcap = 0, pos = 0, lineno = 0, c;
    SBuf pool = SBUF_INIT;

    memset(s, 0, sizeof *s);
    for (c = 0; c < 256; c++) {
        int w = isalnum((int)c) || c == '_' || c >= 0x80;

        s->idch[c] = (uint8_t)w;
        s->idstart[c] = (uint8_t)(w && !isdigit((int)c));
    }

    while (pos < n) {
        const char *ln = text + pos;
        size_t len = 0;
        Tok t[16];
        size_t nt, i;

        while (pos + len < n && ln[len] != '\n')
            len++;
        pos += len + 1;
        lineno++;
        nt = tokenize(ln, len, t, 16);
        if (nt == 0 || t[0].p[0] == '#')
            continue;

        if (tok_is(t[0], "name")) {
            if (nt < 2)
                PERR("riga %zu: manca il nome", lineno);
            free(s->name);
            s->name = xmalloc(len + 1);
            /* the name is the rest of the line, blanks included */
            memcpy(s->name, t[1].p, (size_t)(ln + len - t[1].p));
            s->name[ln + len - t[1].p] = '\0';
        } else if (tok_is(t[0], "ext") || tok_is(t[0], "file") || tok_is(t[0], "shebang") ||
                   tok_is(t[0], "alias")) {
            /* only used by syn_detect */
        } else if (tok_is(t[0], "words")) {
            int cl;

            if (nt < 3)
                PERR("riga %zu: words richiede classe e parole", lineno);
            cl = class_by_name(t[1].p, t[1].n);
            if (cl < 0)
                PERR("riga %zu: classe sconosciuta", lineno);
            /* a word list can span many tokens: re-split without the 16 limit */
            {
                const char *q = t[2].p, *end = ln + len;

                while (q < end) {
                    const char *e;

                    while (q < end && (*q == ' ' || *q == '\t'))
                        q++;
                    e = q;
                    while (e < end && *e != ' ' && *e != '\t')
                        e++;
                    if (e > q) {
                        if (e - q > 255)
                            PERR("riga %zu: parola troppo lunga", lineno);
                        if (npw == pwcap) {
                            pwcap = pwcap ? pwcap * 2 : 64;
                            pw = xrealloc(pw, pwcap * sizeof *pw);
                        }
                        pw[npw].off = pool.len;
                        pw[npw].len = (uint8_t)(e - q);
                        pw[npw].cls = (uint8_t)cl;
                        npw++;
                        sbuf_add(&pool, q, (size_t)(e - q));
                        sbuf_add(&pool, "", 1);
                    }
                    q = e;
                }
            }
        } else if (tok_is(t[0], "comment")) {
            if (nt < 2)
                PERR("riga %zu: comment richiede un simbolo", lineno);
            free(s->cmt);
            free(s->cmt_end);
            s->cmt = xmalloc(t[1].n + 1);
            memcpy(s->cmt, t[1].p, t[1].n);
            s->cmt[t[1].n] = '\0';
            s->cmt_end = NULL;
            if (nt > 2) {
                s->cmt_end = xmalloc(t[2].n + 1);
                memcpy(s->cmt_end, t[2].p, t[2].n);
                s->cmt_end[t[2].n] = '\0';
            }
        } else if (tok_is(t[0], "number")) {
            s->number = 1;
        } else if (tok_is(t[0], "calls")) {
            int cl = nt > 1 ? class_by_name(t[1].p, t[1].n) : -1;

            if (cl < 0)
                PERR("riga %zu: calls richiede una classe", lineno);
            s->calls = 1;
            s->call_cls = (uint8_t)cl;
        } else if (tok_is(t[0], "sigil")) {
            int cl = nt > 2 ? class_by_name(t[1].p, t[1].n) : -1;

            if (cl < 0 || t[2].n != 1)
                PERR("riga %zu: sigil richiede classe e un carattere", lineno);
            s->sigil = (uint8_t)t[2].p[0];
            s->sigil_cls = (uint8_t)cl;
        } else if (tok_is(t[0], "operators")) {
            if (nt < 2)
                PERR("riga %zu: operators richiede dei caratteri", lineno);
            set_chars(s->opch, t[1].p, t[1].n);
        } else if (tok_is(t[0], "idchars")) {
            if (nt < 2)
                PERR("riga %zu: idchars richiede dei caratteri", lineno);
            set_chars(s->idch, t[1].p, t[1].n);
            set_chars(s->idstart, t[1].p, t[1].n);
        } else if (tok_is(t[0], "region")) {
            Region *r;
            int cl;

            if (nt < 4)
                PERR("riga %zu: region richiede classe, inizio e fine", lineno);
            cl = class_by_name(t[1].p, t[1].n);
            if (cl < 0)
                PERR("riga %zu: classe sconosciuta", lineno);
            if (s->nreg == s->regcap) {
                s->regcap = s->regcap ? s->regcap * 2 : 8;
                s->reg = xrealloc(s->reg, s->regcap * sizeof *s->reg);
            }
            if (s->nreg >= 0xFFFE)
                PERR("riga %zu: troppe regioni", lineno);
            r = &s->reg[s->nreg];
            memset(r, 0, sizeof *r);
            r->cls = (uint8_t)cl;
            r->sl = t[2].n;
            r->start = xmalloc(r->sl + 1);
            memcpy(r->start, t[2].p, r->sl);
            r->start[r->sl] = '\0';
            if (!tok_is(t[3], "$")) {
                r->el = t[3].n;
                r->end = xmalloc(r->el + 1);
                memcpy(r->end, t[3].p, r->el);
                r->end[r->el] = '\0';
            }
            s->nreg++;
            for (i = 4; i < nt; i++) {
                if (tok_is(t[i], "ml"))
                    r->ml = 1;
                else if (tok_is(t[i], "bol"))
                    r->bol = 1;
                else if (tok_is(t[i], "ns"))
                    r->ns = 1;
                else if (t[i].n == 5 && memcmp(t[i].p, "esc=", 4) == 0)
                    r->esc = t[i].p[4];
                else
                    PERR("riga %zu: opzione sconosciuta", lineno);
            }
            if (r->ml && !r->end)
                PERR("riga %zu: una regione fino a fine riga non può essere ml", lineno);
        } else {
            PERR("riga %zu: direttiva sconosciuta", lineno);
        }
    }

    if (!s->name)
        PERR("manca la direttiva name");
    s->pool = sbuf_steal(&pool);
    build_table(s, pw, npw);
    free(pw);
    return s;

fail:
    sbuf_free(&pool);
    free(pw);
    syn_free(s);
    return NULL;
}

/* ---- line classification ---------------------------------------------- */

static void fill(uint8_t *cls, size_t a, size_t b, uint8_t c)
{
    if (cls && b > a)
        memset(cls + a, c, b - a);
}

/* Returns the offset just past the end of the region, searching from `from`. */
static size_t find_end(const Region *r, const char *s, size_t len, size_t from, int *closed)
{
    size_t i = from;

    if (!r->end) {
        *closed = 1;
        return len;
    }
    while (i < len) {
        if (r->esc && s[i] == r->esc) {
            i += 2;
            continue;
        }
        if (s[i] == r->end[0] && i + r->el <= len && memcmp(s + i, r->end, r->el) == 0) {
            *closed = 1;
            return i + r->el;
        }
        i++;
    }
    *closed = 0;
    return len;
}

static int only_blanks_before(const char *s, size_t i)
{
    while (i > 0) {
        i--;
        if (s[i] != ' ' && s[i] != '\t')
            return 0;
    }
    return 1;
}

/* Scans a number starting at i; returns the offset past it. */
static size_t scan_number(const Syntax *s, const char *t, size_t len, size_t i)
{
    size_t j = i;

    if (t[j] == '0' && j + 1 < len && (t[j + 1] == 'x' || t[j + 1] == 'X')) {
        j += 2;
        while (j < len && (isxdigit((uint8_t)t[j]) || t[j] == '_'))
            j++;
    } else if (t[j] == '0' && j + 1 < len && (t[j + 1] == 'b' || t[j + 1] == 'B')) {
        j += 2;
        while (j < len && (t[j] == '0' || t[j] == '1' || t[j] == '_'))
            j++;
    } else {
        while (j < len && (isdigit((uint8_t)t[j]) || t[j] == '_'))
            j++;
        if (j < len && t[j] == '.') {
            j++;
            while (j < len && (isdigit((uint8_t)t[j]) || t[j] == '_'))
                j++;
        }
        if (j < len && (t[j] == 'e' || t[j] == 'E')) {
            size_t k = j + 1;

            if (k < len && (t[k] == '+' || t[k] == '-'))
                k++;
            if (k < len && isdigit((uint8_t)t[k])) {
                j = k;
                while (j < len && isdigit((uint8_t)t[j]))
                    j++;
            }
        }
    }
    while (j < len && s->idch[(uint8_t)t[j]])     /* suffixes: u, L, f, ... */
        j++;
    return j;
}

uint16_t syn_line(const Syntax *s, uint16_t state_in, const char *text, size_t len, uint8_t *cls)
{
    size_t i = 0;
    uint16_t st = state_in;

    if (st && (size_t)st > s->nreg)
        st = 0;
    if (st) {
        const Region *r = &s->reg[st - 1];
        int closed;
        size_t e = find_end(r, text, len, 0, &closed);

        fill(cls, 0, e, r->cls);
        i = e;
        if (closed)
            st = 0;
    }

    while (i < len) {
        uint8_t c = (uint8_t)text[i];
        size_t k, j;
        int matched = 0;

        for (k = 0; k < s->nreg; k++) {
            const Region *r = &s->reg[k];
            int closed;
            size_t e;

            if (c != (uint8_t)r->start[0] || i + r->sl > len ||
                memcmp(text + i, r->start, r->sl) != 0)
                continue;
            if (r->bol && !only_blanks_before(text, i))
                continue;
            if (r->ns && (i + r->sl >= len || text[i + r->sl] == ' ' || text[i + r->sl] == '\t'))
                continue;
            e = find_end(r, text, len, i + r->sl, &closed);
            fill(cls, i, e, r->cls);
            i = e;
            if (!closed && r->ml)
                st = (uint16_t)(k + 1);
            matched = 1;
            break;
        }
        if (matched)
            continue;

        if (s->idstart[c]) {
            int wc;

            j = i + 1;
            while (j < len && s->idch[(uint8_t)text[j]])
                j++;
            wc = lookup(s, text + i, j - i);
            if (wc < 0 && s->calls && j < len && text[j] == '(')
                wc = s->call_cls;
            fill(cls, i, j, wc < 0 ? SC_NORMAL : (uint8_t)wc);
            i = j;
        } else if (s->number && (isdigit(c) || (c == '.' && i + 1 < len &&
                                                isdigit((uint8_t)text[i + 1])))) {
            j = scan_number(s, text, len, c == '.' ? i + 1 : i);
            fill(cls, i, j, SC_NUMBER);
            i = j;
        } else if (s->sigil && c == (uint8_t)s->sigil && i + 1 < len &&
                   (s->idstart[(uint8_t)text[i + 1]] || isdigit((uint8_t)text[i + 1]) ||
                    strchr("?@#$!*", text[i + 1]))) {
            j = i + 1;
            if (s->idstart[(uint8_t)text[j]]) {
                while (j < len && s->idch[(uint8_t)text[j]])
                    j++;
            } else {
                j++;
            }
            fill(cls, i, j, s->sigil_cls);
            i = j;
        } else {
            fill(cls, i, i + 1, s->opch[c] ? SC_OPERATOR : SC_NORMAL);
            i++;
        }
    }
    return st;
}

/* ---- incremental state ------------------------------------------------- */

void syn_update(Syntax *s, Buffer *b, size_t last)
{
    size_t y;
    uint16_t st;

    if (!s)
        return;
    if (last >= b->nlines)
        last = b->nlines - 1;
    if (b->stable > last)
        return;
    st = b->stable > 0 ? b->lines[b->stable - 1].hl : 0;
    for (y = b->stable; y <= last; y++) {
        Line *l = &b->lines[y];

        st = syn_line(s, st, l->s, l->len, NULL);
        l->hl = st;
    }
    b->stable = last + 1;
}

const uint8_t *syn_classes(Syntax *s, const Buffer *b, size_t y)
{
    const Line *l = &b->lines[y];
    uint16_t st = y > 0 && b->stable >= y ? b->lines[y - 1].hl : 0;

    if (!s || l->len == 0)
        return NULL;
    if (s->scratch_cap < l->len) {
        s->scratch_cap = l->len * 2;
        s->scratch = xrealloc(s->scratch, s->scratch_cap);
    }
    syn_line(s, st, l->s, l->len, s->scratch);
    return s->scratch;
}

/* ---- brackets ---------------------------------------------------------- */

static int bracket_kind(char c, char *partner, int *forward)
{
    switch (c) {
    case '(': *partner = ')'; *forward = 1; return 1;
    case '[': *partner = ']'; *forward = 1; return 1;
    case '{': *partner = '}'; *forward = 1; return 1;
    case ')': *partner = '('; *forward = 0; return 1;
    case ']': *partner = '['; *forward = 0; return 1;
    case '}': *partner = '{'; *forward = 0; return 1;
    default: return 0;
    }
}

/* Brackets in strings and comments only pair among themselves. */
static int same_zone(const uint8_t *cls, size_t i, int zone)
{
    int c = cls ? cls[i] : SC_NORMAL;
    int z = c == SC_STRING || c == SC_COMMENT ? c : SC_NORMAL;

    return z == zone;
}

int syn_match_bracket(Syntax *s, Buffer *b, size_t y, size_t x,
                      size_t *py, size_t *px, size_t *my, size_t *mx)
{
    const Line *l = &b->lines[y];
    char open, partner;
    int forward, zone, depth = 0;
    const uint8_t *cls;
    size_t ly, i, scanned = 0;

    if (!(x < l->len && bracket_kind(l->s[x], &partner, &forward))) {
        if (!(x > 0 && x <= l->len && bracket_kind(l->s[x - 1], &partner, &forward)))
            return 0;
        x--;
    }
    open = l->s[x];
    *py = y;
    *px = x;

    syn_update(s, b, y);
    cls = syn_classes(s, b, y);
    zone = cls && (cls[x] == SC_STRING || cls[x] == SC_COMMENT) ? cls[x] : SC_NORMAL;

    ly = y;
    i = x;
    for (;;) {
        l = &b->lines[ly];
        if (forward) {
            i++;
            while (i >= l->len) {
                if (ly + 1 >= b->nlines || ++scanned > BRACKET_SCAN_LINES)
                    return 0;
                ly++;
                l = &b->lines[ly];
                i = 0;
                syn_update(s, b, ly);
                cls = syn_classes(s, b, ly);
                if (l->len > 0)
                    break;
            }
        } else {
            while (i == 0) {
                if (ly == 0 || ++scanned > BRACKET_SCAN_LINES)
                    return 0;
                ly--;
                l = &b->lines[ly];
                i = l->len;
                syn_update(s, b, ly);
                cls = syn_classes(s, b, ly);
            }
            i--;
        }
        if (!same_zone(cls, i, zone))
            continue;
        if (l->s[i] == open) {
            depth++;
        } else if (l->s[i] == partner) {
            if (depth == 0) {
                *my = ly;
                *mx = i;
                return 1;
            }
            depth--;
        }
    }
}

/* ---- detection --------------------------------------------------------- */

/* True if the definition has a line "key w1 w2 ..." with word among the w's. */
static int def_has(const char *const *def, const char *key, const char *word, size_t wn,
                   int fold)
{
    size_t kl = strlen(key);

    for (; *def; def++) {
        const char *p = *def;
        const char *e;

        if (strncmp(p, key, kl) != 0 || p[kl] != ' ')
            continue;
        p += kl;
        while (*p) {
            while (*p == ' ')
                p++;
            e = p;
            while (*e && *e != ' ')
                e++;
            if ((size_t)(e - p) == wn) {
                size_t k;

                for (k = 0; k < wn; k++) {
                    int a = (uint8_t)p[k], c = (uint8_t)word[k];

                    if (fold) {
                        a = tolower(a);
                        c = tolower(c);
                    }
                    if (a != c)
                        break;
                }
                if (k == wn)
                    return 1;
            }
            p = e;
        }
    }
    return 0;
}

/* User languages come before the built-in ones in one numbering. */
typedef struct {
    LangDef def;
    char *name;
    char **lines;               /* NULL-terminated */
} UserLang;

static UserLang *user;
static size_t nuser, usercap;

static size_t lang_count(void)
{
    return nuser + lang_ndefs;
}

static const LangDef *lang_at(size_t i)
{
    return i < nuser ? &user[i].def : &lang_defs[i - nuser];
}

static int find_lang_word(const char *key, const char *w, size_t n, int fold)
{
    size_t i;

    for (i = 0; i < lang_count(); i++)
        if (def_has(lang_at(i)->def, key, w, n, fold))
            return (int)i;
    return -1;
}

static int by_modeline_text(const char *s, size_t n)
{
    size_t i;

    for (i = 0; i + 3 < n; i++) {
        size_t j, k;
        int idx;

        if (!(strncmp(s + i, "ft=", 3) == 0 && (i == 0 || s[i - 1] == ' ' || s[i - 1] == ':')) &&
            !(i + 9 <= n && strncmp(s + i, "filetype=", 9) == 0))
            continue;
        j = i + (s[i] == 'f' && s[i + 1] == 't' ? 3 : 9);
        k = j;
        while (k < n && (isalnum((uint8_t)s[k]) || s[k] == '+' || s[k] == '_'))
            k++;
        if (k == j)
            continue;
        idx = -1;
        {
            size_t li;

            for (li = 0; li < lang_count() && idx < 0; li++) {
                const char *nm = lang_at(li)->name;

                if (strlen(nm) == k - j) {
                    size_t q;

                    for (q = 0; q < k - j && tolower((uint8_t)nm[q]) == tolower((uint8_t)s[j + q]);
                         q++)
                        ;
                    if (q == k - j)
                        idx = (int)li;
                }
            }
        }
        if (idx < 0)
            idx = find_lang_word("alias", s + j, k - j, 1);
        if (idx < 0)
            idx = find_lang_word("ext", s + j, k - j, 1);
        if (idx >= 0)
            return idx;
    }
    return -1;
}

static int by_modeline(const Buffer *b)
{
    size_t i, n = b->nlines;

    for (i = 0; i < n; i++) {
        const Line *l;
        int idx;

        if (i >= 5 && i + 5 < n)
            i = n - 5;
        l = &b->lines[i];
        if (l->len == 0 || l->len > 200)
            continue;
        if (!memchr(l->s, ':', l->len))
            continue;
        idx = by_modeline_text(l->s, l->len);
        if (idx >= 0)
            return idx;
    }
    return -1;
}

static int by_shebang(const Buffer *b)
{
    const Line *l = &b->lines[0];
    size_t i = 2;

    if (l->len < 3 || l->s[0] != '#' || l->s[1] != '!')
        return -1;
    while (i < l->len) {
        size_t j;
        int idx;

        while (i < l->len && (l->s[i] == ' ' || l->s[i] == '/'))
            i++;
        j = i;
        while (j < l->len && l->s[j] != ' ' && l->s[j] != '/')
            j++;
        if (j > i && l->s[i] != '-') {
            size_t e = j;

            while (e > i && (isdigit((uint8_t)l->s[e - 1]) || l->s[e - 1] == '.'))
                e--;
            idx = find_lang_word("shebang", l->s + i, e - i, 0);
            if (idx >= 0)
                return idx;
        }
        i = j;
    }
    return -1;
}

Syntax *syn_detect(const Buffer *b)
{
    int idx = by_modeline(b);
    const char *base, *dot;
    SBuf text = SBUF_INIT;
    char err[128];
    Syntax *s;
    const char *const *d;

    if (idx < 0)
        idx = by_shebang(b);
    if (idx < 0 && b->path) {
        base = strrchr(b->path, '/');
        base = base ? base + 1 : b->path;
        idx = find_lang_word("file", base, strlen(base), 0);
        dot = strrchr(base, '.');
        if (idx < 0 && dot && dot[1])
            idx = find_lang_word("ext", dot + 1, strlen(dot + 1), 1);
    }
    if (idx < 0)
        return NULL;

    for (d = lang_at((size_t)idx)->def; *d; d++) {
        sbuf_adds(&text, *d);
        sbuf_add(&text, "\n", 1);
    }
    s = syn_parse(text.p ? text.p : "", text.len, err, sizeof err);
    sbuf_free(&text);
    return s;
}

/* ---- user languages --------------------------------------------------------- */

static void free_user(UserLang *u)
{
    char **l;

    for (l = u->lines; l && *l; l++)
        free(*l);
    free(u->lines);
    free(u->name);
}

void syn_clear_user(void)
{
    size_t i;

    for (i = 0; i < nuser; i++)
        free_user(&user[i]);
    free(user);
    user = NULL;
    nuser = usercap = 0;
}

int syn_add_user(const char *text, size_t n, char *err, size_t errlen)
{
    Syntax *s = syn_parse(text, n, err, errlen);
    UserLang u;
    size_t pos = 0, cnt = 0, cap = 16, k;

    if (!s)
        return -1;
    memset(&u, 0, sizeof u);
    u.name = xstrdup(syn_name(s));
    syn_free(s);
    u.lines = xmalloc(cap * sizeof *u.lines);
    while (pos < n) {
        size_t len = 0;

        while (pos + len < n && text[pos + len] != '\n')
            len++;
        if (cnt + 2 > cap) {
            cap *= 2;
            u.lines = xrealloc(u.lines, cap * sizeof *u.lines);
        }
        u.lines[cnt] = xmalloc(len + 1);
        memcpy(u.lines[cnt], text + pos, len);
        u.lines[cnt][len] = '\0';
        cnt++;
        pos += len + 1;
    }
    u.lines[cnt] = NULL;
    u.def.name = u.name;
    u.def.def = (const char *const *)u.lines;
    /* a later file with the same name replaces the earlier one */
    for (k = 0; k < nuser; k++)
        if (strcmp(user[k].name, u.name) == 0) {
            free_user(&user[k]);
            user[k] = u;
            user[k].def.name = user[k].name;
            user[k].def.def = (const char *const *)user[k].lines;
            return 0;
        }
    if (nuser == usercap) {
        usercap = usercap ? usercap * 2 : 8;
        user = xrealloc(user, usercap * sizeof *user);
    }
    user[nuser] = u;
    user[nuser].def.name = user[nuser].name;
    user[nuser].def.def = (const char *const *)user[nuser].lines;
    nuser++;
    return 0;
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

size_t syn_load_dir(const char *dir, void (*report)(const char *msg, void *ctx), void *ctx)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    char **names = NULL;
    size_t n = 0, cap = 0, i, added = 0;

    if (!d)
        return 0;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            names = xrealloc(names, cap * sizeof *names);
        }
        names[n++] = xstrdup(de->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, name_cmp);
    for (i = 0; i < n; i++) {
        size_t pn = strlen(dir) + strlen(names[i]) + 2;
        char *path = xmalloc(pn), err[160], msg[300];
        FILE *f;
        SBuf text = SBUF_INIT;
        char buf[4096];
        size_t got;

        snprintf(path, pn, "%s/%s", dir, names[i]);
        f = fopen(path, "r");
        if (f) {
            while ((got = fread(buf, 1, sizeof buf, f)) > 0 && text.len < (256u << 10))
                sbuf_add(&text, buf, got);
            fclose(f);
            if (syn_add_user(text.p ? text.p : "", text.len, err, sizeof err) == 0) {
                added++;
            } else if (report) {
                snprintf(msg, sizeof msg, "%s: %s", names[i], err);
                report(msg, ctx);
            }
        }
        sbuf_free(&text);
        free(path);
        free(names[i]);
    }
    free(names);
    return added;
}
