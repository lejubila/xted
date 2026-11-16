#include "imports.h"
#include "editor.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* The first occurrence of the NUL-terminated needle in the n bytes of hay, or NULL. */
static const char *strstr_n(const char *hay, size_t n, const char *needle)
{
    size_t nl = strlen(needle), i;

    for (i = 0; i + nl <= n; i++)
        if (memcmp(hay + i, needle, nl) == 0)
            return hay + i;
    return NULL;
}

/* Where the first statement of a file is: past the comments at its top (line and block ones) and the
 * blank lines after them, and in C the include guard (#ifndef, #define, #pragma once), in
 * JavaScript a #! line and "use strict". */
static size_t head_end(const Buffer *b, size_t lim, int js)
{
    size_t y, top = 0, i, k;
    int in_comment = 0;

    for (y = 0; y < lim; y++) {
        const Line *l = &b->lines[y];
        const char *s;

        for (i = 0; !js && i < l->len && (l->s[i] == ' ' || l->s[i] == '\t'); i++)
            ;
        s = l->s + i;
        if (in_comment) {
            top = y + 1;
            for (k = 0; k + 1 < l->len; k++)
                if (l->s[k] == '*' && l->s[k + 1] == '/')
                    in_comment = 0;
        } else if (l->len - i >= 2 && s[0] == '/' && s[1] == '*') {
            in_comment = 1;
            top = y + 1;
            for (k = i + 2; k + 1 < l->len; k++)
                if (l->s[k] == '*' && l->s[k + 1] == '/')
                    in_comment = 0;
        } else if (l->len - i >= 2 && s[0] == '/' && s[1] == '/') {
            top = y + 1;
        } else if (l->len == i) {
            if (top == y)
                top = y + 1;
        } else if (js && (s[0] == '#' || strstr_n(l->s, l->len, "use strict") != NULL)) {
            top = y + 1;
        } else if (!js && ((l->len - i >= 7 && memcmp(s, "#ifndef", 7) == 0) ||
                           (l->len - i >= 7 && memcmp(s, "#define", 7) == 0 && top > 0) ||
                           (l->len - i >= 12 && memcmp(s, "#pragma once", 12) == 0))) {
            top = y + 1;
        } else {
            break;
        }
    }
    return top;
}

/* Inserts text (ending with a newline) before line at, with blank lines before/after if pre/post;
 * moves the cursor down when that is above it. */
static void insert_lines(Editor *e, size_t at, const char *text, int pre, int post)
{
    SBuf all = SBUF_INIT;
    size_t yy = at, xx = 0, added = 0, k;
    const Buffer *b = e->buf;

    if (pre)
        sbuf_adds(&all, "\n");
    sbuf_adds(&all, text);
    if (post)
        sbuf_adds(&all, "\n");
    if (at >= b->nlines) {
        SBuf t2 = SBUF_INIT;

        yy = b->nlines - 1;
        xx = b->lines[yy].len;
        if (xx > 0)
            sbuf_adds(&t2, "\n");
        sbuf_add(&t2, all.p, all.len);
        sbuf_free(&all);
        all = t2;
        at = b->nlines;
    }
    for (k = 0; k < all.len; k++)
        if (all.p[k] == '\n')
            added++;
    buf_insert_text(e->buf, &yy, &xx, all.p, all.len);
    sbuf_free(&all);
    if (at <= e->cy)
        e->cy += added;
}

static int is_word_ch(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

/* True if the line has the word name (identifier boundaries) at or after column from. */
static int has_word(const Line *l, size_t from, const char *name)
{
    size_t nl = strlen(name), i;

    for (i = from; i + nl <= l->len; i++)
        if (memcmp(l->s + i, name, nl) == 0 && (i == 0 || !is_word_ch(l->s[i - 1])) &&
            (i + nl == l->len || !is_word_ch(l->s[i + nl])))
            return 1;
    return 0;
}

static int starts_with(const Line *l, const char *w)
{
    size_t n = strlen(w);

    return l->len >= n && memcmp(l->s, w, n) == 0 && (l->len == n || !is_word_ch(l->s[n]));
}

/* The "use Class;" statement for a class of the project or of the library: nothing if it is
 * imported or defined in the file, or in the namespace of the file. Keeps the use block sorted. */
int imp_php_use(Editor *e, const char *fq)
{
    const Buffer *b = e->buf;
    const char *name = strrchr(fq, '\\');
    size_t nl, y, nsl = (size_t)-1, php = (size_t)-1, first_use = (size_t)-1, last_use = 0, at;
    size_t ins = (size_t)-1;
    int header = 1;
    char stmt[264], pre[2] = "", post[2] = "";
    size_t sl, lim = b->nlines < 600 ? b->nlines : 600;

    name = name ? name + 1 : fq;
    nl = strlen(name);
    for (y = 0; y < lim; y++) {
        const Line *l = &b->lines[y];
        size_t i = 0;

        while (i < l->len && (l->s[i] == ' ' || l->s[i] == '\t'))
            i++;
        {
            Line t = *l;

            t.s += i;
            t.len -= i;
            if (php == (size_t)-1 && t.len >= 5 && memcmp(t.s, "<?php", 5) == 0)
                php = y;
            if (header && starts_with(&t, "namespace") && nsl == (size_t)-1) {
                nsl = y;
            } else if (header && starts_with(&t, "use")) {
                if (has_word(&t, 0, name))
                    return 0;                         /* imported (maybe with as) */
                if (memchr(t.s, '{', t.len) == NULL && memchr(t.s, '(', t.len) == NULL &&
                    !has_word(&t, 0, "function") && !has_word(&t, 0, "const")) {
                    if (first_use == (size_t)-1)
                        first_use = y;
                    last_use = y;
                    /* the first use line that sorts after fq: the new one goes before it */
                    if (ins == (size_t)-1 && t.len > 4) {
                        char cur[256];
                        size_t cl = t.len - 4 < sizeof cur - 1 ? t.len - 4 : sizeof cur - 1;

                        memcpy(cur, t.s + 4, cl);
                        cur[cl] = '\0';
                        while (cl > 0 && (cur[cl - 1] == ';' || cur[cl - 1] == ' '))
                            cur[--cl] = '\0';
                        if (strcasecmp(cur, fq) > 0)
                            ins = y;
                    }
                }
            } else if (t.len > 0 && (starts_with(&t, "class") || starts_with(&t, "abstract") ||
                                     starts_with(&t, "final") || starts_with(&t, "interface") ||
                                     starts_with(&t, "trait") || starts_with(&t, "enum") ||
                                     starts_with(&t, "function"))) {
                header = 0;
            }
            if (t.len > 6 && (starts_with(&t, "class") || starts_with(&t, "interface") ||
                              starts_with(&t, "trait") || starts_with(&t, "enum") ||
                              starts_with(&t, "abstract") || starts_with(&t, "final"))) {
                /* defined in this very file */
                size_t k;

                for (k = 0; k + nl <= t.len; k++)
                    if (memcmp(t.s + k, name, nl) == 0 && k > 0 && t.s[k - 1] == ' ' &&
                        (k + nl == t.len || !is_word_ch(t.s[k + nl])))
                        return 0;
            }
        }
    }
    /* the namespace of the file: a class of the same one needs no use */
    if (nsl != (size_t)-1) {
        const Line *l = &b->lines[nsl];
        size_t i = 0, k;
        const char *sep = strrchr(fq, '\\');

        while (i < l->len && l->s[i] != ' ')
            i++;
        while (i < l->len && l->s[i] == ' ')
            i++;
        for (k = l->len; k > i && (l->s[k - 1] == ';' || l->s[k - 1] == ' '); k--)
            ;
        if (sep && (size_t)(sep - fq) == k - i && memcmp(fq, l->s + i, k - i) == 0)
            return 0;
    } else if (!strchr(fq, '\\')) {
        return 0;                                     /* a global class in a file without namespace */
    }
    sl = (size_t)snprintf(stmt, sizeof stmt, "use %s;\n", fq);
    if (sl >= sizeof stmt)
        return 0;
    if (first_use != (size_t)-1) {
        at = ins != (size_t)-1 ? ins : last_use + 1;
    } else {
        size_t base = nsl != (size_t)-1 ? nsl : php;

        if (base == (size_t)-1)
            return 0;
        at = base + 1;
        if (at < b->nlines && b->lines[at].len == 0)
            at++;                                   /* after the blank line */
        else
            pre[0] = '\n';
        if (at >= b->nlines || b->lines[at].len > 0)
            post[0] = '\n';
    }
    if (at >= b->nlines)
        return 0;
    insert_lines(e, at, stmt, pre[0] != 0, post[0] != 0);
    return 1;
}

/* 1 if two names are the same header: cstdio and stdio.h are. */
static int same_header(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);

    if (strcmp(a, b) == 0)
        return 1;
    if (la > 2 && a[0] == 'c' && lb > 2 && strcmp(b + lb - 2, ".h") == 0 && la - 1 == lb - 2 &&
        memcmp(a + 1, b, lb - 2) == 0)
        return 1;
    if (lb > 2 && b[0] == 'c' && la > 2 && strcmp(a + la - 2, ".h") == 0 && lb - 1 == la - 2 &&
        memcmp(b + 1, a, la - 2) == 0)
        return 1;
    return 0;
}

int imp_include(Editor *e, const char *hdr, int local)
{
    const Buffer *b = e->buf;
    size_t lim = b->nlines < 400 ? b->nlines : 400, y, first_sys = (size_t)-1, last_sys = 0;
    size_t first_loc = (size_t)-1, last_loc = 0, ins = (size_t)-1, lins = (size_t)-1, at;
    int sorted = 1, lsorted = 1;
    char prev[256] = "", lprev[256] = "", pre[2] = "", post[2] = "";
    SBuf text = SBUF_INIT;

    for (y = 0; y < lim; y++) {
        const Line *l = &b->lines[y];
        size_t i = 0;
        const char *s;

        while (i < l->len && (l->s[i] == ' ' || l->s[i] == '\t'))
            i++;
        s = l->s + i;
        if (l->len - i >= 8 && memcmp(s, "#include", 8) == 0) {
            const char *q = s + 8, *end = l->s + l->len;
            char name[80];
            size_t nl = 0;
            int sys;

            while (q < end && (*q == ' ' || *q == '\t'))
                q++;
            sys = q < end && *q == '<';
            if (q < end && (*q == '<' || *q == '"')) {
                char close = *q == '<' ? '>' : '"';

                for (q++; q + nl < end && q[nl] != close && nl < sizeof name - 1; nl++)
                    name[nl] = q[nl];
                name[nl] = '\0';
                if (same_header(name, hdr) || strcmp(name, "bits/stdc++.h") == 0)
                    return 0;                       /* already there (either way) */
                if (local && !sys) {
                    /* "buffer.h" and "../src/buffer.h": the same header by another path */
                    const char *a = strrchr(name, '/'), *h2 = strrchr(hdr, '/');

                    if (strcmp(a ? a + 1 : name, h2 ? h2 + 1 : hdr) == 0)
                        return 0;
                }
                if (sys) {
                    if (first_sys == (size_t)-1)
                        first_sys = y;
                    last_sys = y;
                    if (strcmp(prev, name) > 0)
                        sorted = 0;
                    snprintf(prev, sizeof prev, "%s", name);
                    if (ins == (size_t)-1 && strcmp(name, hdr) > 0)
                        ins = y;
                } else {
                    if (first_loc == (size_t)-1)
                        first_loc = y;
                    last_loc = y;
                    if (strcmp(lprev, name) > 0)
                        lsorted = 0;
                    snprintf(lprev, sizeof lprev, "%s", name);
                    if (lins == (size_t)-1 && strcmp(name, hdr) > 0)
                        lins = y;
                }
            }
            continue;
        }
    }
    if (local && first_loc != (size_t)-1) {
        at = lsorted && lins != (size_t)-1 ? lins : last_loc + 1;
    } else if (local && first_sys != (size_t)-1) {
        /* the local includes go after the system ones, past a blank line */
        at = last_sys + 1;
        if (at < b->nlines && b->lines[at].len == 0)
            at++;
        else
            pre[0] = '\n';
        if (at < b->nlines && b->lines[at].len > 0)
            post[0] = '\n';
    } else if (local) {
        at = head_end(b, lim, 0);
        if (at < b->nlines && b->lines[at].len > 0)
            post[0] = '\n';
    } else if (first_sys != (size_t)-1) {
        at = sorted && ins != (size_t)-1 ? ins : last_sys + 1;
    } else if (first_loc != (size_t)-1) {
        at = first_loc;
        post[0] = '\n';                             /* system includes before the local ones */
    } else {
        at = head_end(b, lim, 0);
        if (at < b->nlines && b->lines[at].len > 0)
            post[0] = '\n';
    }
    sbuf_adds(&text, local ? "#include \"" : "#include <");
    sbuf_adds(&text, hdr);
    sbuf_adds(&text, local ? "\"\n" : ">\n");
    sbuf_add(&text, "", 1);
    insert_lines(e, at, text.p, pre[0] != 0, post[0] != 0);
    sbuf_free(&text);
    return 1;
}

/* The first dotted name of an import line, after "import " or "from ": up to a blank or comma. */
static size_t py_mod_name(const Line *l, size_t from, char *out, size_t n)
{
    size_t i = from, k = 0;

    while (i < l->len && (l->s[i] == ' ' || l->s[i] == '\t'))
        i++;
    while (i < l->len && (is_word_ch(l->s[i]) || l->s[i] == '.') && k + 1 < n)
        out[k++] = l->s[i++];
    out[k] = '\0';
    return i;
}


int imp_python(Editor *e, const char *mod, const char *name, int local)
{
    const Buffer *b = e->buf;
    size_t lim = b->nlines < 300 ? b->nlines : 300, y;
    size_t blk_start = (size_t)-1, blk_end = 0, fut_end = (size_t)-1, ext = (size_t)-1;
    size_t last_imp = (size_t)-1, ins = (size_t)-1, at, grp = (size_t)-1;
    int blk_open = 1, depth = 0, gap = 0;
    char pre[2] = "", post[2] = "", cur[96];
    SBuf text = SBUF_INIT;

    for (y = 0; y < lim; y++) {
        const Line *l = &b->lines[y];
        int is_imp, is_from;

        if (depth > 0) {                    /* the rest of a parenthesized "from ... import (" */
            if (name && has_word(l, 0, name))
                return 0;
            if (memchr(l->s, ')', l->len))
                depth = 0;
            if (blk_open && blk_start != (size_t)-1)
                blk_end = y;
            continue;
        }
        if (l->len == 0)
            gap = 1;                        /* a blank line: the next imports are another group */
        if (l->len == 0 || l->s[0] == ' ' || l->s[0] == '\t')
            continue;
        is_imp = l->len > 7 && memcmp(l->s, "import ", 7) == 0;
        is_from = l->len > 5 && memcmp(l->s, "from ", 5) == 0;
        if (!is_imp && !is_from) {
            if (l->s[0] != '#' && blk_start != (size_t)-1)
                blk_open = 0;               /* code after the first block of imports */
            continue;
        }
        if (blk_start == (size_t)-1)
            blk_start = y;
        if (blk_open) {
            blk_end = y;
            if (grp == (size_t)-1 || gap)
                grp = y;
        }
        gap = 0;
        if (is_imp) {
            /* import a, b.c as d: a name that is imported (maybe by its first part) */
            size_t p = 7;

            while (p < l->len) {
                size_t q = py_mod_name(l, p, cur, sizeof cur);
                char *dot = strchr(cur, '.');

                if (dot)
                    *dot = '\0';
                if (!name && strcmp(cur, mod) == 0)
                    return 0;
                while (q < l->len && l->s[q] != ',')
                    q++;
                p = q + 1;
            }
            if (blk_open) {
                py_mod_name(l, 7, cur, sizeof cur);
                if (ins == (size_t)-1 && !name && strcasecmp(cur, mod) > 0)
                    ins = y;
                last_imp = y;
            }
        } else {
            size_t q = py_mod_name(l, 5, cur, sizeof cur);

            while (q < l->len && l->s[q] == ' ')
                q++;
            if (strcmp(cur, "__future__") == 0)
                fut_end = y;
            if (q + 6 <= l->len && memcmp(l->s + q, "import", 6) == 0) {
                const char *rest = l->s + q + 6;
                size_t rl = l->len - q - 6;
                int paren = memchr(rest, '(', rl) != NULL;

                if (memchr(rest, '*', rl) && name && strcmp(cur, mod) == 0)
                    return 0;                   /* from mod import * */
                if (name && strcmp(cur, mod) == 0) {
                    if (has_word(l, q + 6, name))
                        return 0;
                    if (!paren && l->s[l->len - 1] != '\\' && !memchr(rest, '#', rl))
                        ext = y;
                }
                if (paren && !memchr(rest, ')', rl))
                    depth = 1;
                if (blk_open && name && ins == (size_t)-1 && strcmp(cur, "__future__") != 0 &&
                    strcasecmp(cur, mod) > 0)
                    ins = y;
            }
        }
    }
    /* "from mod import a, b" gets ", name" */
    if (name && ext != (size_t)-1) {
        size_t yy = ext, xx = b->lines[ext].len;
        SBuf add = SBUF_INIT;

        while (xx > 0 && (b->lines[ext].s[xx - 1] == ' ' || b->lines[ext].s[xx - 1] == '\t'))
            xx--;
        sbuf_adds(&add, ", ");
        sbuf_adds(&add, name);
        buf_insert_text(e->buf, &yy, &xx, add.p, add.len);
        sbuf_free(&add);
        return 1;
    }
    if (blk_start != (size_t)-1) {
        if (!name) {
            /* "import x" lines come first in the block; sorted among them */
            at = ins != (size_t)-1 ? ins : last_imp != (size_t)-1 ? last_imp + 1 : blk_start;
            if (last_imp == (size_t)-1 && fut_end != (size_t)-1 && at <= fut_end)
                at = fut_end + 1;
        } else if (local && grp != (size_t)-1) {
            /* a module of the project: into the last group of imports, in order there */
            at = blk_end + 1;
            for (y = grp; y <= blk_end; y++) {
                const Line *l = &b->lines[y];

                if (l->len > 5 && memcmp(l->s, "from ", 5) == 0) {
                    py_mod_name(l, 5, cur, sizeof cur);
                    if (strcmp(cur, "__future__") != 0 && strcasecmp(cur, mod) > 0) {
                        at = y;
                        break;
                    }
                }
            }
        } else {
            at = ins != (size_t)-1 ? ins : blk_end + 1;
        }
    } else {
        /* no imports: after the shebang, comments and the docstring */
        size_t i;

        at = 0;
        for (y = 0; y < lim; y++) {
            const Line *l = &b->lines[y];

            for (i = 0; i < l->len && (l->s[i] == ' ' || l->s[i] == '\t'); i++)
                ;
            if (l->len == i) {
                if (at == y)
                    at = y + 1;
                continue;
            }
            if (l->s[i] == '#') {
                at = y + 1;
                continue;
            }
            if (l->s[i] == '"' || l->s[i] == '\'' || ((l->s[i] == 'r' || l->s[i] == 'u') && i + 1 < l->len &&
                                                        (l->s[i + 1] == '"' || l->s[i + 1] == '\''))) {
                size_t q = i + (l->s[i] == 'r' || l->s[i] == 'u');
                char quote = l->s[q];
                int triple = q + 2 < l->len && l->s[q + 1] == quote && l->s[q + 2] == quote;
                size_t from = triple ? q + 3 : q + 1, yy;

                for (yy = y; yy < lim; yy++, from = 0) {
                    const Line *m = &b->lines[yy];
                    size_t k;
                    int closed = 0;

                    for (k = from; k < m->len; k++) {
                        if (triple ? (k + 2 < m->len && m->s[k] == quote && m->s[k + 1] == quote &&
                                      m->s[k + 2] == quote)
                                   : (m->s[k] == quote && (k == 0 || m->s[k - 1] != '\\'))) {
                            closed = 1;
                            break;
                        }
                    }
                    if (closed || !triple) {
                        at = yy + 1;
                        break;
                    }
                }
                if (yy >= lim)
                    return 0;               /* a docstring that does not end in view: leave it */
            }
            break;
        }
        if (at > 0 && at <= b->nlines && b->lines[at - 1].len > 0)
            pre[0] = '\n';
        if (at < b->nlines && b->lines[at].len > 0)
            post[0] = '\n';
    }
    if (name)
        sbuf_addf(&text, "from %s import %s\n", mod, name);
    else
        sbuf_addf(&text, "import %s\n", mod);
    sbuf_add(&text, "", 1);
    insert_lines(e, at, text.p, pre[0] != 0, post[0] != 0);
    sbuf_free(&text);
    return 1;
}


/* True if the nearest package.json above the file says "type": "module". */
static int js_pkg_module(const char *path)
{
    char dir[4096], file[4200], buf[8192];
    size_t n;

    if (!path || strlen(path) >= sizeof dir)
        return 0;
    snprintf(dir, sizeof dir, "%s", path);
    for (;;) {
        char *slash = strrchr(dir, '/');
        FILE *f;

        if (!slash)
            return 0;
        *slash = '\0';
        snprintf(file, sizeof file, "%s/package.json", dir[0] ? dir : "");
        f = fopen(file, "r");
        if (f) {
            n = fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
            buf[n] = '\0';
            {
                const char *t = strstr(buf, "\"type\"");

                return t && strstr(t, "\"module\"") != NULL && strstr(t, "\"module\"") - t < 20;
            }
        }
        if (!dir[0])
            return 0;
    }
}

/* True if the file declares or imports name at its top level: an import, or const, let, var,
 * function, class, type, interface, enum with name before the "=" or the "(". */
static int js_declared(const Buffer *b, const char *name)
{
    static const char *const kw[] = { "const ", "let ", "var ", "function ", "async function ", "class ",
                                      "type ", "interface ", "enum ", "export const ", "export let ",
                                      "export var ", "export function ", "export class ", "export default class ",
                                      "export type ", "export interface ", "export enum ", NULL };
    size_t y, lim = b->nlines < 2000 ? b->nlines : 2000, k, i;
    int in_import = 0;

    for (y = 0; y < lim; y++) {
        const Line *l = &b->lines[y];
        Line t = *l;

        for (i = 0; i < l->len && (l->s[i] == ' ' || l->s[i] == '\t'); i++)
            ;
        if (in_import || (l->len - i > 7 && memcmp(l->s + i, "import ", 7) == 0 && i == 0)) {
            size_t end = l->len;
            const char *from = strstr_n(l->s + i, l->len - i, " from ");

            if (from)
                end = (size_t)(from - l->s);
            t.len = end;
            if (has_word(&t, i, name))
                return 1;
            in_import = end == l->len && !memchr(l->s, '\'', l->len) && !memchr(l->s, '"', l->len);
            continue;
        }
        for (k = 0; kw[k]; k++) {
            size_t kl = strlen(kw[k]);

            if (l->len - i > kl && memcmp(l->s + i, kw[k], kl) == 0) {
                size_t j;

                for (j = i + kl; j < l->len && l->s[j] != '=' && l->s[j] != '('; j++)
                    ;
                t.len = j;
                if (has_word(&t, i + kl, name))
                    return 1;
                break;
            }
        }
    }
    return 0;
}

/* The text of the last quoted string of a line, and its quote; 0 if none. */
static int js_last_string(const Line *l, char *out, size_t n, char *quote)
{
    size_t i = l->len, j;

    while (i > 0 && l->s[i - 1] != '\'' && l->s[i - 1] != '"')
        i--;
    if (i < 2)
        return 0;
    *quote = l->s[i - 1];
    for (j = i - 1; j > 0 && l->s[j - 1] != *quote; j--)
        ;
    if (j == 0 || i - 1 - j + 1 > n)
        return 0;
    memcpy(out, l->s + j, i - 1 - j);
    out[i - 1 - j] = '\0';
    return 1;
}

/* The order of modules in a block of imports: packages ("fs", "react") before the files of the
 * project ("./util"), each in alphabetical order. */
static int js_mod_cmp(const char *a, const char *b)
{
    int ra = a[0] == '.', rb = b[0] == '.';

    return ra != rb ? ra - rb : strcmp(a, b);
}

int imp_js(Editor *e, const char *mod, const char *name, int def, char *stmt, size_t sn)
{
    const Buffer *b = e->buf;
    size_t lim = b->nlines < 400 ? b->nlines : 400, y, blk_start = (size_t)-1, blk_end = 0, ins = (size_t)-1;
    size_t ext = (size_t)-1, at;
    int esm = 0, cjs = 0, semi = -1, star = 0, blk_open = 1, sorted = 1;
    char quote = 0, cur[200], prev[200] = "", q;
    const char *path = e->doc->buf.path, *ext_name;
    SBuf text = SBUF_INIT;
    char pre[2] = "", post[2] = "";

    if (js_declared(b, name))
        return 0;
    for (y = 0; y < lim; y++) {
        const Line *l = &b->lines[y];
        int is_import = l->len > 7 && memcmp(l->s, "import ", 7) == 0 && l->s[7] != '(';
        int is_req = (l->len > 6 && (memcmp(l->s, "const ", 6) == 0 || memcmp(l->s, "let ", 4) == 0 ||
                                     memcmp(l->s, "var ", 4) == 0)) &&
                     l->len > 10 && strstr_n(l->s, l->len, "require(") != NULL;

        if (is_import || is_req) {
            size_t last = y;
            const Line *fin = l;

            if (is_import) {
                esm = 1;
                /* a statement may take several lines: up to the one with the module string */
                while (last + 1 < lim && !(strstr_n(b->lines[last].s, b->lines[last].len, " from ") ||
                                           strstr_n(b->lines[last].s, b->lines[last].len, "from ") ||
                                           memchr(b->lines[last].s, '\'', b->lines[last].len) ||
                                           memchr(b->lines[last].s, '"', b->lines[last].len)))
                    last++;
                fin = &b->lines[last];
                if (strstr_n(l->s, l->len, "* as "))
                    star = 1;
            } else {
                cjs = 1;
            }
            if (js_last_string(fin, cur, sizeof cur, &q)) {
                if (!quote)
                    quote = q;
                if (blk_open) {
                    if (prev[0] && js_mod_cmp(prev, cur) > 0)
                        sorted = 0;
                    if (ins == (size_t)-1 && js_mod_cmp(cur, mod) > 0)
                        ins = y;
                    snprintf(prev, sizeof prev, "%s", cur);
                }
                if (!def && strcmp(cur, mod) == 0 && last == y && memchr(l->s, '{', l->len) &&
                    memchr(l->s, '}', l->len) && !strstr_n(l->s, l->len, "import type") &&
                    !(is_import && strstr_n(l->s, l->len, "* as ")))
                    ext = y;
            }
            if (semi < 0 && fin->len > 0)
                semi = fin->s[fin->len - 1] == ';';
            if (blk_start == (size_t)-1)
                blk_start = y;
            if (blk_open)
                blk_end = last;
            y = last;
            continue;
        }
        if (l->len == 0) {
            if (blk_start != (size_t)-1)
                blk_open = 0;
            continue;
        }
        if (blk_start != (size_t)-1 && l->s[0] != '/' && l->s[0] != '*')
            blk_open = 0;
    }
    ext_name = path ? strrchr(path, '.') : NULL;
    if (!esm && !cjs) {
        if (ext_name && (strcmp(ext_name, ".ts") == 0 || strcmp(ext_name, ".tsx") == 0 ||
                         strcmp(ext_name, ".mts") == 0 || strcmp(ext_name, ".jsx") == 0 ||
                         strcmp(ext_name, ".mjs") == 0))
            esm = 1;
        else if (!(ext_name && strcmp(ext_name, ".cjs") == 0))
            esm = js_pkg_module(path);
    }
    if (!quote)
        quote = '\'';
    if (semi < 0)
        semi = 1;
    /* "import { a } from 'mod'" gets ", name" */
    if (!def && ext != (size_t)-1) {
        const Line *l = &b->lines[ext];
        size_t xx = l->len, yy = ext;
        SBuf add = SBUF_INIT;

        while (xx > 0 && l->s[xx - 1] != '}')
            xx--;
        if (xx > 0) {
            xx--;
            while (xx > 0 && (l->s[xx - 1] == ' ' || l->s[xx - 1] == '\t'))
                xx--;
            sbuf_adds(&add, ", ");
            sbuf_adds(&add, name);
            buf_insert_text(e->buf, &yy, &xx, add.p, add.len);
            snprintf(stmt, sn, "%.*s", (int)add.len, add.p);
            sbuf_free(&add);
            return 1;
        }
        sbuf_free(&add);
    }
    {
        char line[600];

        if (esm && def)
            snprintf(line, sizeof line, star ? "import * as %s from %c%s%c%s" : "import %s from %c%s%c%s", name, quote, mod,
                     quote, semi ? ";" : "");
        else if (esm)
            snprintf(line, sizeof line, "import { %s } from %c%s%c%s", name, quote, mod, quote, semi ? ";" : "");
        else if (def)
            snprintf(line, sizeof line, "const %s = require(%c%s%c)%s", name, quote, mod, quote, semi ? ";" : "");
        else
            snprintf(line, sizeof line, "const { %s } = require(%c%s%c)%s", name, quote, mod, quote, semi ? ";" : "");
        snprintf(stmt, sn, "%s", line);
        sbuf_adds(&text, line);
        sbuf_adds(&text, "\n");
    }
    if (blk_start != (size_t)-1) {
        at = ins != (size_t)-1 && sorted ? ins : blk_end + 1;
    } else {
        at = head_end(b, lim, 1);
        if (at > 0 && at <= b->nlines && b->lines[at - 1].len > 0)
            pre[0] = '\n';
        if (at < b->nlines && b->lines[at].len > 0)
            post[0] = '\n';
    }
    sbuf_add(&text, "", 1);
    insert_lines(e, at, text.p, pre[0] != 0, post[0] != 0);
    sbuf_free(&text);
    return 1;
}


/* Inserts text as a group of its own at line at: past one blank line that is there, with blank
 * lines around it. */
static void insert_block(Editor *e, size_t at, const char *text)
{
    const Buffer *b = e->buf;

    if (at < b->nlines && b->lines[at].len == 0)
        insert_lines(e, at + 1, text, 0, at + 1 < b->nlines && b->lines[at + 1].len > 0);
    else
        insert_lines(e, at, text, at > 0, at < b->nlines && b->lines[at].len > 0);
}

/* The line just after "<?php" (and after a declare(strict_types=1); that follows it), or 0. */
static size_t php_after_open(const Buffer *b, size_t lim)
{
    size_t y;

    for (y = 0; y < lim; y++) {
        const Line *l = &b->lines[y];

        if (strstr_n(l->s, l->len, "<?php")) {
            size_t z = y + 1;

            while (z < lim && b->lines[z].len == 0)
                z++;
            if (z < lim && strstr_n(b->lines[z].s, b->lines[z].len, "declare(") &&
                b->lines[z].len > 0 && b->lines[z].s[0] == 'd')
                return z + 1;
            return y + 1;
        }
    }
    return 0;
}

int imp_php_require(Editor *e, const char *spec)
{
    const Buffer *b = e->buf;
    size_t lim = b->nlines < 300 ? b->nlines : 300, y, last = (size_t)-1, at;
    const char *base = strrchr(spec, '/');
    char line[4200];

    base = base ? base + 1 : spec;
    for (y = 0; y < lim; y++) {
        const Line *l = &b->lines[y];
        size_t i = 0;

        while (i < l->len && (l->s[i] == ' ' || l->s[i] == '\t'))
            i++;
        if ((l->len - i >= 7 && memcmp(l->s + i, "require", 7) == 0) ||
            (l->len - i >= 7 && memcmp(l->s + i, "include", 7) == 0)) {
            const char *f = strstr_n(l->s, l->len, base);

            /* the same file: its name after a / or a quote */
            if (f && (f == l->s || f[-1] == '/' || f[-1] == '\'' || f[-1] == '"'))
                return 0;
            if (i == 0)
                last = y;
        }
    }
    snprintf(line, sizeof line, "require_once __DIR__ . '/%s';\n", spec);
    if (last != (size_t)-1) {
        insert_lines(e, last + 1, line, 0, 0);
        return 1;
    }
    at = php_after_open(b, lim);
    insert_block(e, at, line);
    return 1;
}

int imp_php_use_fn(Editor *e, const char *what, const char *fq)
{
    const Buffer *b = e->buf;
    size_t lim = b->nlines < 600 ? b->nlines : 600, y, wl = strlen(what), last_fn = (size_t)-1;
    size_t last_use = (size_t)-1, nsl = (size_t)-1, ins = (size_t)-1, at, last_func = (size_t)-1;
    const char *name = strrchr(fq, '\\');
    char line[600], prefix[40];

    name = name ? name + 1 : fq;
    snprintf(prefix, sizeof prefix, "use %s ", what);
    for (y = 0; y < lim; y++) {
        const Line *l = &b->lines[y];

        if (l->len > 10 && memcmp(l->s, "namespace ", 10) == 0) {
            const char *sep = strrchr(fq, '\\');
            size_t k = 10, end = l->len;

            while (end > k && (l->s[end - 1] == ';' || l->s[end - 1] == ' '))
                end--;
            if (sep && (size_t)(sep - fq) == end - k && memcmp(fq, l->s + k, end - k) == 0)
                return 0;               /* the file's own namespace */
            nsl = y;
        } else if (l->len > 4 + wl + 1 && memcmp(l->s, prefix, 4 + wl + 1) == 0) {
            Line t = *l;

            if (has_word(&t, 4 + wl + 1, name))
                return 0;
            if (ins == (size_t)-1 && l->len - (4 + wl + 1) > 0) {
                char cur[512];
                size_t cl = l->len - (4 + wl + 1) < sizeof cur - 1 ? l->len - (4 + wl + 1) : sizeof cur - 1;

                memcpy(cur, l->s + 4 + wl + 1, cl);
                cur[cl] = '\0';
                if (strcasecmp(cur, fq) > 0)
                    ins = y;
            }
            last_fn = y;
        } else if (l->len > 4 && memcmp(l->s, "use ", 4) == 0 && memchr(l->s, '(', l->len) == NULL) {
            if (l->len > 13 && memcmp(l->s, "use function ", 13) == 0)
                last_func = y;
            else if (!(l->len > 10 && memcmp(l->s, "use const ", 10) == 0))
                last_use = y;
        } else if (l->len > 0 && l->s[0] != ' ' && l->s[0] != '\t' && l->s[0] != '/' && l->s[0] != '*' &&
                   l->s[0] != '<' && l->s[0] != '#' && last_use != (size_t)-1 && y > last_use + 1) {
            break;                      /* the code after the uses */
        }
    }
    snprintf(line, sizeof line, "use %s %s;\n", what, fq);
    if (last_fn != (size_t)-1) {
        insert_lines(e, ins != (size_t)-1 ? ins : last_fn + 1, line, 0, 0);
        return 1;
    }
    if (strcmp(what, "const") == 0 && last_func != (size_t)-1) {
        insert_lines(e, last_func + 1, line, 1, 0);     /* constants after the functions */
        return 1;
    }
    if (last_use != (size_t)-1) {
        /* PSR-12: a group of its own after the classes */
        at = last_use + 1;
        insert_lines(e, at, line, 1, 0);
        return 1;
    }
    at = nsl != (size_t)-1 ? nsl + 1 : php_after_open(b, lim);
    insert_block(e, at, line);
    return 1;
}


/* ---- Go and Rust --------------------------------------------------------- */

/* The import path of a Go import line ("fmt", f "fmt", _ "x"), or "" when it has none. */
static void go_spec(const Line *l, char *out, size_t n)
{
    char q;

    if (!js_last_string(l, out, n, &q) || q != '"')
        out[0] = '\0';
}

static void go_spec_text(const char *text, char *out, size_t n)
{
    Line l;

    l.s = (char *)text;
    l.len = strlen(text);
    go_spec(&l, out, n);
}

/* A path outside the standard library: its first element is a domain (github.com/x/y). */
static int go_external(const char *path)
{
    size_t n = strcspn(path, "/");

    return memchr(path, '.', n) != NULL;
}

int imp_go(Editor *e, const char *path)
{
    const Buffer *b = e->buf;
    size_t lim = b->nlines < 400 ? b->nlines : 400, y, pk = (size_t)-1, blk = (size_t)-1, ins = (size_t)-1;
    size_t grp_end = (size_t)-1, s0 = (size_t)-1, s1 = 0, first_spec = (size_t)-1, last_spec = 0;
    char spec[256], line[300];
    int ext = go_external(path);

    for (y = 0; y < lim; y++) {
        const Line *l = &b->lines[y];

        if (pk == (size_t)-1 && l->len > 8 && memcmp(l->s, "package ", 8) == 0)
            pk = y;
        if (l->len >= 8 && memcmp(l->s, "import (", 8) == 0) {
            int grp = 0, kind = -1, target = -1;

            blk = y;
            /* gofmt's groups, split by blank lines: the standard library, then the modules; the
             * path joins the first group of its own kind */
            for (y++; y < b->nlines && !(b->lines[y].len > 0 && b->lines[y].s[0] == ')'); y++) {
                const Line *m = &b->lines[y];

                if (m->len == 0) {
                    if (kind != -1)
                        grp++;
                    kind = -1;
                    continue;
                }
                go_spec(m, spec, sizeof spec);
                if (!spec[0])
                    continue;
                if (strcmp(spec, path) == 0)
                    return 0;
                if (first_spec == (size_t)-1)
                    first_spec = y;
                last_spec = y;
                if (kind == -1)
                    kind = go_external(spec);
                if (kind == ext && (target == -1 || target == grp)) {
                    target = grp;
                    grp_end = y;
                    if (ins == (size_t)-1 && strcmp(spec, path) > 0)
                        ins = y;
                }
            }
            continue;
        }
        if (l->len > 7 && memcmp(l->s, "import ", 7) == 0) {
            go_spec(l, spec, sizeof spec);
            if (strcmp(spec, path) == 0)
                return 0;
            if (s0 == (size_t)-1)
                s0 = y;
            if (y == s1 + 1 || s0 == y)
                s1 = y;
        }
    }
    if (blk != (size_t)-1) {
        snprintf(line, sizeof line, "\t\"%s\"\n", path);
        if (grp_end != (size_t)-1)
            insert_lines(e, ins != (size_t)-1 ? ins : grp_end + 1, line, 0, 0);
        else if (first_spec == (size_t)-1)
            insert_lines(e, blk + 1, line, 0, 0);
        else if (ext)
            insert_lines(e, last_spec + 1, line, 1, 0);     /* the first module: a group of its own */
        else
            insert_lines(e, first_spec, line, 0, 1);        /* the library goes before the modules */
        return 1;
    }
    if (s0 != (size_t)-1) {
        /* import "os" on its own line: the imports become a block, in order */
        SBuf t = SBUF_INIT;
        char (*v)[300] = xmalloc((s1 - s0 + 2) * sizeof *v);
        size_t n = 0, i, yy = s0, xx = 0, added;
        int put = 0, pass;

        for (y = s0; y <= s1; y++) {
            const Line *l = &b->lines[y];
            size_t k = 7;

            while (k < l->len && l->s[k] == ' ')
                k++;
            snprintf(v[n++], sizeof v[0], "%.*s", (int)(l->len - k), l->s + k);
        }
        sbuf_adds(&t, "import (\n");
        /* the standard library first, then the modules after a blank line, each in order */
        for (pass = 0; pass < 2; pass++) {
            int any = 0;

            for (i = 0; i <= n; i++) {
                int k = i < n ? (go_spec_text(v[i], spec, sizeof spec), go_external(spec)) : pass;

                if (k != pass || (i == n && (put || ext != pass)))
                    continue;
                if (!any && pass == 1 && t.len > 9)
                    sbuf_adds(&t, "\n");
                /* the new path before the first that sorts after it */
                if (!put && ext == pass && (i == n || strcmp(spec, path) > 0)) {
                    sbuf_addf(&t, "\t\"%s\"\n", path);
                    put = 1;
                    any = 1;
                }
                if (i < n) {
                    sbuf_addf(&t, "\t%s\n", v[i]);
                    any = 1;
                }
            }
        }
        sbuf_adds(&t, ")");
        free(v);
        buf_delete_range(e->buf, s0, 0, s1, b->lines[s1].len);
        buf_insert_text(e->buf, &yy, &xx, t.p, t.len);
        for (added = 0, i = 0; i < t.len; i++)
            added += t.p[i] == '\n';
        added = added + 1 - (s1 - s0 + 1);
        sbuf_free(&t);
        if (e->cy > s1)
            e->cy += added;
        return 1;
    }
    snprintf(line, sizeof line, "import \"%s\"\n", path);
    insert_block(e, pk != (size_t)-1 ? pk + 1 : 0, line);
    return 1;
}

/* Rust's groups of uses: 0 std, core, alloc; 1 other crates; 2 crate, self, super. */
static int rust_kind(const char *p, size_t n)
{
    static const char *const k0[] = { "std", "core", "alloc" }, *const k2[] = { "crate", "self", "super" };
    size_t s = 0, i;

    while (s < n && !(p[s] == ':' && s + 1 < n && p[s + 1] == ':'))
        s++;
    for (i = 0; i < 3; i++) {
        if (s == strlen(k0[i]) && memcmp(p, k0[i], s) == 0)
            return 0;
        if (s == strlen(k2[i]) && memcmp(p, k2[i], s) == 0)
            return 2;
    }
    return 1;
}

/* rustfmt's order of two paths: segment by segment, a lowercase one (a module) before an
 * uppercase one (a type), else by bytes. */
static int rust_cmp(const char *a, size_t an, const char *b, size_t bn)
{
    size_t i = 0, j = 0;

    while (i < an && j < bn) {
        size_t ie = i, je = j;
        int ua, ub, c;

        while (ie < an && a[ie] != ':' && a[ie] != '{' && a[ie] != ';')
            ie++;
        while (je < bn && b[je] != ':' && b[je] != '{' && b[je] != ';')
            je++;
        ua = isupper((unsigned char)a[i]) != 0;
        ub = isupper((unsigned char)b[j]) != 0;
        if (ua != ub)
            return ua - ub;
        c = memcmp(a + i, b + j, ie - i < je - j ? ie - i : je - j);
        if (c || ie - i != je - j)
            return c ? c : (ie - i < je - j ? -1 : 1);
        i = ie;
        j = je;
        while (i < an && a[i] == ':')
            i++;
        while (j < bn && b[j] == ':')
            j++;
        if (i < an && a[i] == '{')
            return 1;
        if (j < bn && b[j] == '{')
            return -1;
    }
    return (i < an) - (j < bn);
}

#define RUST_USES 200

int imp_rust(Editor *e, const char *path)
{
    const Buffer *b = e->buf;
    size_t lim = b->nlines < 600 ? b->nlines : 600, y, at = 0, pl = strlen(path), pre, k, n = 0;
    const char *name = path;
    char line[300];
    int body = 0, grp = 0, kind = rust_kind(path, pl), gap = 0;
    size_t attr = (size_t)-1;
    struct { size_t y0, y1, u, ul; int kind, grp; } *v = xmalloc(RUST_USES * sizeof *v);

    for (k = 0; k + 2 <= pl; k++)
        if (path[k] == ':' && path[k + 1] == ':')
            name = path + k + 2;
    pre = (size_t)(name - path);            /* "crate::net::" */
    for (y = 0; y < lim && n < RUST_USES; y++) {
        const Line *l = &b->lines[y];
        size_t i = 0;

        if (l->len > 4 && memcmp(l->s, "pub", 3) == 0 && (l->s[3] == ' ' || l->s[3] == '(')) {
            i = 3;
            if (l->s[i] == '(')
                while (i < l->len && l->s[i] != ')')
                    i++;
            while (i < l->len && (l->s[i] == ')' || l->s[i] == ' '))
                i++;
        }
        if (l->len - i > 4 && memcmp(l->s + i, "use ", 4) == 0) {
            const char *u = l->s + i + 4;
            size_t ul = l->len - i - 4, y1 = y, j;

            while (ul > 0 && (u[ul - 1] == ';' || u[ul - 1] == ' '))
                ul--;
            if (ul == pl && memcmp(u, path, pl) == 0) {
                free(v);
                return 0;
            }
            /* a use that goes on below: use crate::x::{\n a,\n b,\n}; */
            while (y1 + 1 < b->nlines && memchr(b->lines[y1].s, ';', b->lines[y1].len) == NULL)
                y1++;
            /* use crate::x::{A, B}, use crate::{x::A, y}, use crate::x::*: the name may be there */
            for (k = 0; k + 2 < pre + 1; k++) {
                size_t bp = k + 2;
                int has;

                if (!(path[k] == ':' && path[k + 1] == ':') || ul <= bp || memcmp(u, path, bp) != 0 ||
                    (u[bp] != '{' && !(bp == pre && u[bp] == '*')))
                    continue;
                has = u[bp] == '*';
                for (j = y; j <= y1 && !has; j++)
                    has = has_word(&b->lines[j], j == y ? i + 4 + bp : 0, name);
                if (has) {
                    free(v);
                    return 0;
                }
            }
            if (gap && n > 0)
                grp++;
            gap = 0;
            v[n].y0 = attr != (size_t)-1 ? attr : y;
            v[n].y1 = y1;
            v[n].u = i + 4;
            v[n].ul = ul;
            v[n].kind = rust_kind(u, ul);
            v[n++].grp = grp;
            attr = (size_t)-1;
            y = y1;
            continue;
        }
        if (l->len == 0) {
            gap = 1;
            attr = (size_t)-1;
            continue;
        }
        if (l->len >= 2 && l->s[0] == '#' && l->s[1] == '[') {
            if (attr == (size_t)-1)
                attr = y;                   /* #[cfg(...)] goes with the use below it */
            continue;
        }
        if (l->len >= 2 && l->s[0] == '/' && l->s[1] == '/' && !(l->len >= 3 && l->s[2] == '!'))
            continue;
        if (n > 0)
            break;                          /* the code after the uses */
        attr = (size_t)-1;
        /* where the uses go in a file without any: past //! comments, #![...] attributes and the
         * mod declarations that open the file (mod net;) */
        if (!body) {
            size_t m = l->len > 4 && memcmp(l->s, "pub ", 4) == 0 ? 4 : 0;

            if ((l->len >= 3 && memcmp(l->s, "//!", 3) == 0) || (l->len >= 3 && memcmp(l->s, "#![", 3) == 0) ||
                (l->len > m + 4 && memcmp(l->s + m, "mod ", 4) == 0 && l->s[l->len - 1] == ';'))
                at = y + 1;
            else
                body = 1;
        }
    }
    if (n == 0) {
        free(v);
        snprintf(line, sizeof line, "use %s;\n", path);
        insert_block(e, at, line);
        return 1;
    }
    /* the braces of a use of the module (use crate::net::{A, B};) or of one above it (use bat::{
     * config::Config, ... };): the rest of the path joins them, in rustfmt's order */
    {
        size_t best = (size_t)-1, bbp = 0, uy = 0, ux = 0;

        for (k = 0; k < n; k++) {
            size_t yy = v[k].y0, i = 0, ulen, c2;
            const Line *l;
            const char *u;

            while (yy < v[k].y1 && b->lines[yy].len > 0 && b->lines[yy].s[0] == '#')
                yy++;                       /* past #[cfg(...)] */
            l = &b->lines[yy];
            while (i + 4 <= l->len && memcmp(l->s + i, "use ", 4) != 0)
                i++;
            if (i + 4 > l->len)
                continue;
            u = l->s + i + 4;
            ulen = l->len - i - 4;
            for (c2 = 0; c2 + 2 <= pre; c2++) {
                size_t bp = c2 + 2;

                if (path[c2] == ':' && path[c2 + 1] == ':' && ulen > bp && memcmp(u, path, bp) == 0 &&
                    u[bp] == '{' && bp > bbp) {
                    best = k;
                    bbp = bp;
                    uy = yy;
                    ux = (size_t)(u - l->s) + bp;
                }
            }
        }
        if (best != (size_t)-1) {
            const char *item = path + bbp;
            size_t il = strlen(item), y = uy, x = ux + 1, ins_y = (size_t)-1, ins_x = 0;
            size_t close_y = (size_t)-1, close_x = 0, indent = 4;
            int depth = 0, ins_bol = 0, got_indent = 0;

            while (y <= v[best].y1 && close_y == (size_t)-1) {
                const Line *l = &b->lines[y];

                while (x < l->len) {
                    char ch = l->s[x];

                    if (depth == 0 && ch != ' ' && ch != '\t' && ch != ',' && ch != '}') {
                        size_t ie = x, bol = x;
                        int d2 = 0;

                        while (bol > 0 && (l->s[bol - 1] == ' ' || l->s[bol - 1] == '\t'))
                            bol--;
                        if (y != uy && !got_indent && bol == 0) {
                            indent = x;
                            got_indent = 1;
                        }
                        /* the item: up to a comma or the closing brace at its level */
                        while (ie < l->len && !(d2 == 0 && (l->s[ie] == ',' || l->s[ie] == '}'))) {
                            if (l->s[ie] == '{')
                                d2++;
                            else if (l->s[ie] == '}')
                                d2--;
                            ie++;
                        }
                        if (ins_y == (size_t)-1 && !(ie - x == 4 && memcmp(l->s + x, "self", 4) == 0) &&
                            rust_cmp(l->s + x, ie - x, item, il) > 0) {
                            ins_y = y;
                            ins_x = x;
                            ins_bol = bol == 0 && y != uy;
                        }
                        if (d2 > 0)
                            depth = d2;     /* a nested list that goes on below */
                        x = ie;
                        continue;
                    }
                    if (ch == '{')
                        depth++;
                    else if (ch == '}') {
                        if (depth == 0) {
                            close_y = y;
                            close_x = x;
                            break;
                        }
                        depth--;
                    }
                    x++;
                }
                y++;
                x = 0;
            }
            if (close_y != (size_t)-1) {
                int multi = close_y != uy;
                char ln[400];

                if (ins_y != (size_t)-1 && multi && ins_bol) {
                    /* one item a line: a line of its own */
                    snprintf(ln, sizeof ln, "%*s%s,\n", (int)indent, "", item);
                    insert_lines(e, ins_y, ln, 0, 0);
                } else if (ins_y != (size_t)-1) {
                    snprintf(ln, sizeof ln, "%s, ", item);
                    buf_insert(e->buf, ins_y, ins_x, ln, strlen(ln));
                    if (e->cy == ins_y && e->cx >= ins_x)
                        e->cx += strlen(ln);
                } else if (multi) {
                    const Line *cl = &b->lines[close_y];
                    size_t q = 0;

                    while (q < close_x && (cl->s[q] == ' ' || cl->s[q] == '\t'))
                        q++;
                    if (q == close_x) {
                        snprintf(ln, sizeof ln, "%*s%s,\n", (int)indent, "", item);
                        insert_lines(e, close_y, ln, 0, 0);
                    } else {
                        snprintf(ln, sizeof ln, "%s%s", cl->s[close_x - 1] == ',' ? " " : ", ", item);
                        buf_insert(e->buf, close_y, close_x, ln, strlen(ln));
                    }
                } else {
                    const Line *cl = &b->lines[close_y];
                    size_t q = close_x;

                    while (q > 0 && cl->s[q - 1] == ' ')
                        q--;
                    snprintf(ln, sizeof ln, "%s%s", q > 0 && cl->s[q - 1] == ',' ? " " : ", ", item);
                    buf_insert(e->buf, close_y, q, ln, strlen(ln));
                    if (e->cy == close_y && e->cx >= q)
                        e->cx += strlen(ln);
                }
                free(v);
                return 1;
            }
        }
    }
    snprintf(line, sizeof line, "use %s;\n", path);
    {
        /* the group of the same kind; of those of other crates, the one of the same crate */
        int g = -1;
        size_t ins = (size_t)-1, last = 0, seg = strcspn(path, ":");

        for (k = 0; k < n; k++)
            if (v[k].kind == kind) {
                const char *u = b->lines[v[k].y1].s + (v[k].y0 == v[k].y1 ? v[k].u : 0);

                if (v[k].y0 != v[k].y1)
                    u = strstr_n(b->lines[v[k].y1].s, b->lines[v[k].y1].len, "use ") ?
                        strstr_n(b->lines[v[k].y1].s, b->lines[v[k].y1].len, "use ") + 4 : u;
                if (g == -1 || kind != 1 || (strncmp(u, path, seg) == 0 && u[seg] == ':'))
                    g = v[k].grp;
                if (kind != 1 && g != -1)
                    break;
            }
        if (g != -1) {
            for (k = 0; k < n; k++) {
                const Line *l = &b->lines[v[k].y0 == v[k].y1 ? v[k].y0 : v[k].y1];
                const char *u = l->s;
                size_t ul;

                if (v[k].grp != g)
                    continue;
                if (v[k].y0 == v[k].y1) {
                    u = l->s + v[k].u;
                    ul = v[k].ul;
                } else {
                    const char *q = strstr_n(l->s, l->len, "use ");

                    if (q) {
                        u = q + 4;
                        ul = l->len - (size_t)(u - l->s);
                    } else {
                        u = b->lines[v[k].y0].s + v[k].u;
                        ul = v[k].ul;
                    }
                }
                last = v[k].y1;
                if (ins == (size_t)-1 && rust_cmp(u, ul, path, pl) > 0)
                    ins = v[k].y0;
            }
            insert_lines(e, ins != (size_t)-1 ? ins : last + 1, line, 0, 0);
        } else if (kind == 0) {
            insert_lines(e, v[0].y0, line, 0, 1);       /* the library comes first */
        } else {
            /* other crates before the crate's own, which come last */
            for (k = 0; k < n && !(kind == 1 && v[k].kind == 2); k++)
                ;
            if (k < n)
                insert_lines(e, v[k].y0, line, 0, 1);
            else
                insert_lines(e, v[n - 1].y1 + 1, line, 1, 0);
        }
    }
    free(v);
    return 1;
}


/* ---- Java ----------------------------------------------------------------- */

int imp_java(Editor *e, const char *fq)
{
    const Buffer *b = e->buf;
    size_t lim = b->nlines < 400 ? b->nlines : 400, y, pk = (size_t)-1, last_static = (size_t)-1;
    size_t fql = strlen(fq), pl, seg = strcspn(fq, ".");
    const char *dot = strrchr(fq, '.');
    char line[300];
    int grp = 0, gap = 0, any = 0, target = -1, last_grp = -1;
    size_t ins = (size_t)-1, grp_end = (size_t)-1, last_end = (size_t)-1;

    if (!dot)
        return 0;
    pl = (size_t)(dot - fq);                /* java.util */
    for (y = 0; y < lim; y++) {
        const Line *l = &b->lines[y];
        const char *u;
        size_t ul;

        if (l->len == 0) {
            gap = 1;
            continue;
        }
        if (pk == (size_t)-1 && l->len > 8 && memcmp(l->s, "package ", 8) == 0) {
            pk = y;
            continue;
        }
        if (l->len > 14 && memcmp(l->s, "import static ", 14) == 0) {
            last_static = y;
            continue;
        }
        if (!(l->len > 7 && memcmp(l->s, "import ", 7) == 0)) {
            if ((l->s[0] == '/' || l->s[0] == '*' || (l->len > 1 && l->s[0] == ' ' && l->s[1] == '*')))
                continue;               /* comments before the code */
            break;                      /* the class */
        }
        u = l->s + 7;
        for (ul = l->len - 7; ul > 0 && (u[ul - 1] == ';' || u[ul - 1] == ' '); ul--)
            ;
        if ((ul == fql && memcmp(u, fq, fql) == 0) ||
            (ul == pl + 2 && memcmp(u, fq, pl) == 0 && u[pl] == '.' && u[pl + 1] == '*'))
            return 0;
        /* a class of that name imported from another package: it is the one meant */
        if (ul > fql - pl && u[ul - (fql - pl)] == '.' && memcmp(u + ul - (fql - pl) + 1, dot + 1, fql - pl - 1) == 0)
            return 0;
        if (gap && any)
            grp++;
        gap = 0;
        any = 1;
        last_grp = grp;
        last_end = y;
        /* the group whose imports start like fq (java., com.acme.) */
        if ((target == -1 || target == grp) && strcspn(u, ".") == seg && memcmp(u, fq, seg) == 0) {
            target = grp;
            grp_end = y;
            int c = memcmp(u, fq, ul < fql ? ul : fql);

            if (ins == (size_t)-1 && (c > 0 || (c == 0 && ul > fql)))
                ins = y;
        }
    }
    snprintf(line, sizeof line, "import %s;\n", fq);
    if (target != -1) {
        insert_lines(e, ins != (size_t)-1 ? ins : grp_end + 1, line, 0, 0);
        return 1;
    }
    if (last_grp != -1) {
        /* no import like it: a group of its own after the others */
        insert_lines(e, last_end + 1, line, 1, 0);
        return 1;
    }
    if (last_static != (size_t)-1) {
        insert_lines(e, last_static + 1, line, 1, 0);
        return 1;
    }
    insert_block(e, pk != (size_t)-1 ? pk + 1 : 0, line);
    return 1;
}
