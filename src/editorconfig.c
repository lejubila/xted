#include "editorconfig.h"
#include "util.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EC_MAX_FILES 16
#define EC_MAX_BYTES (1u << 18)

/* ---- globs ------------------------------------------------------------- */

/* Matches pattern p (no braces) against s. '*' is any run of characters but '/', "**" any run,
 * '?' one character but '/', [abc] [!a-z] a class. */
static int glob_plain(const char *p, const char *s)
{
    while (*p) {
        switch (*p) {
        case '*': {
            int deep = p[1] == '*';
            const char *rest = p + (deep ? 2 : 1);
            const char *t = s;

            for (;;) {
                if (glob_plain(rest, t))
                    return 1;
                if (!*t || (!deep && *t == '/'))
                    return 0;
                t++;
            }
        }
        case '?':
            if (!*s || *s == '/')
                return 0;
            p++;
            s++;
            break;
        case '[': {
            const char *q = p + 1;
            int neg = 0, hit = 0;

            if (*q == '!') {
                neg = 1;
                q++;
            }
            if (!*s || *s == '/')
                return 0;
            for (; *q && (*q != ']' || q == p + 1 + neg); q++) {
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    if (*s >= *q && *s <= q[2])
                        hit = 1;
                    q += 2;
                } else if (*q == *s) {
                    hit = 1;
                }
            }
            if (*q != ']')
                return 0;
            if (hit == neg)
                return 0;
            p = q + 1;
            s++;
            break;
        }
        case '\\':
            if (p[1])
                p++;
            /* fall through */
        default:
            if (*p != *s)
                return 0;
            p++;
            s++;
            break;
        }
    }
    return *s == '\0';
}

/* Expands the first {a,b,c} or {n1..n2} of pattern into alternatives and tries each. */
static int glob_braces(const char *p, const char *s, int depth)
{
    const char *open = strchr(p, '{'), *q, *close = NULL;
    int nest = 0;

    if (!open || depth > 4)
        return glob_plain(p, s);
    for (q = open; *q; q++) {
        if (*q == '{')
            nest++;
        else if (*q == '}' && --nest == 0) {
            close = q;
            break;
        }
    }
    if (!close)
        return glob_plain(p, s);
    {
        size_t pre = (size_t)(open - p), post = strlen(close + 1);
        const char *a = open + 1, *b;
        char alt[PATH_MAX], pat[PATH_MAX];
        long lo, hi;
        char *end;

        /* {n1..n2} */
        lo = strtol(a, &end, 10);
        if (end > a && end[0] == '.' && end[1] == '.') {
            hi = strtol(end + 2, &end, 10);
            if (end == close) {
                long v;

                for (v = lo; v <= hi && v - lo < 1000; v++) {
                    snprintf(pat, sizeof pat, "%.*s%ld%s", (int)pre, p, v, close + 1);
                    if (glob_braces(pat, s, depth + 1))
                        return 1;
                }
                return 0;
            }
        }
        nest = 0;
        for (b = a;; b++) {
            if (*b == '{')
                nest++;
            else if (*b == '}' && b < close)
                nest--;
            if ((*b == ',' && nest == 0) || b == close) {
                size_t al = (size_t)(b - a);

                if (pre + al + post + 1 < sizeof pat && al < sizeof alt) {
                    memcpy(pat, p, pre);
                    memcpy(pat + pre, a, al);
                    memcpy(pat + pre + al, close + 1, post + 1);
                    if (glob_braces(pat, s, depth + 1))
                        return 1;
                }
                if (b == close)
                    break;
                a = b + 1;
            }
        }
    }
    return 0;
}

int editorconfig_match(const char *glob, const char *name)
{
    char pat[PATH_MAX];

    if (strchr(glob, '/') == NULL) {
        snprintf(pat, sizeof pat, "**/%s", glob);
    } else {
        snprintf(pat, sizeof pat, "%s", glob[0] == '/' ? glob + 1 : glob);
    }
    /* a leading "any directory" also matches no directory at all */
    if (glob_braces(pat, name, 0))
        return 1;
    if (strncmp(pat, "**/", 3) == 0)
        return glob_braces(pat + 3, name, 0);
    return 0;
}

/* ---- files ------------------------------------------------------------- */

static void lower(char *s)
{
    for (; *s; s++)
        *s = (char)tolower((unsigned char)*s);
}

static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

static int to_bool(const char *v)
{
    return strcmp(v, "true") == 0 ? 1 : strcmp(v, "false") == 0 ? 0 : -1;
}

static void apply_prop(EcProps *o, const char *key, const char *val)
{
    char *end;
    long n;

    if (strcmp(key, "indent_style") == 0) {
        o->expand = strcmp(val, "space") == 0 ? 1 : strcmp(val, "tab") == 0 ? 0 : o->expand;
    } else if (strcmp(key, "indent_size") == 0) {
        if (strcmp(val, "tab") == 0) {
            o->size = 0;
        } else {
            n = strtol(val, &end, 10);
            if (end != val && *end == '\0' && n >= 1 && n <= 16)
                o->size = (int)n;
        }
    } else if (strcmp(key, "tab_width") == 0) {
        n = strtol(val, &end, 10);
        if (end != val && *end == '\0' && n >= 1 && n <= 16)
            o->tab_width = (int)n;
    } else if (strcmp(key, "trim_trailing_whitespace") == 0) {
        if (to_bool(val) >= 0)
            o->trim = to_bool(val);
    } else if (strcmp(key, "insert_final_newline") == 0) {
        if (to_bool(val) >= 0)
            o->final_nl = to_bool(val);
    }
}

/* Applies one file's sections to o. rel is the path of the target relative to the file's
 * directory. Returns 1 if the file has "root = true" in its preamble. */
static int apply_file(const char *text, size_t len, const char *rel, EcProps *o)
{
    char *copy = xmalloc(len + 1), *line, *save = NULL;
    int in_match = 0, in_preamble = 1, root = 0;

    memcpy(copy, text, len);
    copy[len] = '\0';
    for (line = copy; line && *line; line = save) {
        char *nl = strchr(line, '\n'), *t, *eq;

        if (nl) {
            *nl = '\0';
            save = nl + 1;
        } else {
            save = NULL;
        }
        t = trim(line);
        if (*t == '\0' || *t == '#' || *t == ';')
            continue;
        if (*t == '[') {
            char *close = strrchr(t, ']');

            in_preamble = 0;
            if (!close) {
                in_match = 0;
                continue;
            }
            *close = '\0';
            in_match = editorconfig_match(t + 1, rel);
            continue;
        }
        eq = strchr(t, '=');
        if (!eq)
            continue;
        *eq = '\0';
        {
            char *key = trim(t), *val = trim(eq + 1);

            lower(key);
            lower(val);
            if (in_preamble) {
                if (strcmp(key, "root") == 0 && strcmp(val, "true") == 0)
                    root = 1;
            } else if (in_match) {
                apply_prop(o, key, val);
            }
        }
    }
    free(copy);
    return root;
}

int editorconfig_lookup(const char *path, EcProps *out)
{
    char abs[PATH_MAX], dir[PATH_MAX], *sl;
    char *texts[EC_MAX_FILES];
    char *rels[EC_MAX_FILES];
    size_t lens[EC_MAX_FILES];
    int n = 0, i;

    out->expand = out->size = out->tab_width = out->trim = out->final_nl = -1;
    if (!path || !path[0])
        return 0;
    if (!realpath(path, abs)) {
        /* a file that does not exist yet: its directory */
        char tmp[PATH_MAX];
        const char *base = strrchr(path, '/');

        snprintf(tmp, sizeof tmp, "%.*s", base ? (int)(base - path) : 1, base ? path : ".");
        if (!realpath(tmp, dir))
            return 0;
        {
            const char *name = base ? base + 1 : path;
            size_t dl = strcmp(dir, "/") == 0 ? 0 : strlen(dir);

            if (dl + 1 + strlen(name) >= sizeof abs)
                return 0;
            memcpy(abs, dir, dl);
            abs[dl] = '/';
            memcpy(abs + dl + 1, name, strlen(name) + 1);
        }
    }
    snprintf(dir, sizeof dir, "%s", abs);
    /* from the nearest directory up: the files are applied in the opposite order */
    while (n < EC_MAX_FILES && (sl = strrchr(dir, '/')) != NULL) {
        char file[PATH_MAX + 16];
        FILE *f;

        *sl = '\0';
        snprintf(file, sizeof file, "%s/.editorconfig", dir);
        f = fopen(file, "r");
        if (f) {
            SBuf b = SBUF_INIT;
            char buf[4096];
            size_t k;

            while ((k = fread(buf, 1, sizeof buf, f)) > 0 && b.len < EC_MAX_BYTES)
                sbuf_add(&b, buf, k);
            fclose(f);
            texts[n] = xmalloc(b.len + 1);
            if (b.len)
                memcpy(texts[n], b.p, b.len);
            texts[n][b.len] = '\0';
            lens[n] = b.len;
            rels[n] = xstrdup(abs + strlen(dir) + 1);
            n++;
            sbuf_free(&b);
            /* root = true stops the climb */
            {
                EcProps scratch = *out;

                if (apply_file(texts[n - 1], lens[n - 1], rels[n - 1], &scratch))
                    break;
            }
        }
        if (dir[0] == '\0')
            break;
    }
    for (i = n - 1; i >= 0; i--) {
        apply_file(texts[i], lens[i], rels[i], out);
        free(texts[i]);
        free(rels[i]);
    }
    return n;
}
