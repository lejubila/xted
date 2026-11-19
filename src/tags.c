#include "sys.h"
#include "tags.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TAGS_MAX_BYTES (64u << 20)

char *tags_find_file(const char *dir)
{
    char *d = xstrdup(dir && *dir ? dir : "."), *path;
    struct stat st;
    int depth;

    /* make "." concrete so that going up is possible */
    if (!sys_is_abs(d)) {
        char cwd[4096];

        if (getcwd(cwd, sizeof cwd)) {
            size_t pn = strlen(cwd) + strlen(d) + 2;

            path = xmalloc(pn);
            if (strcmp(d, ".") == 0)
                snprintf(path, pn, "%s", cwd);
            else
                snprintf(path, pn, "%s/%s", cwd, d);
            free(d);
            d = path;
        }
    }
    for (depth = 0; depth < 12; depth++) {
        char *slash;

        size_t pn = strlen(d) + 8;

        path = xmalloc(pn);
        snprintf(path, pn, "%s/tags", d);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            free(d);
            return path;
        }
        free(path);
        slash = strrchr(d, '/');
        if (!slash || slash == d)
            break;
        *slash = '\0';
    }
    free(d);
    return NULL;
}

void taghits_free(TagHits *h)
{
    size_t i;

    for (i = 0; i < h->n; i++) {
        free(h->v[i].file);
        free(h->v[i].addr);
    }
    free(h->v);
    memset(h, 0, sizeof *h);
}

/* Compares the name field of the line at p (up to the tab) with name. */
static int name_cmp(const char *p, const char *end, const char *name, size_t nl)
{
    size_t i = 0;

    while (p + i < end && p[i] != '\t' && i < nl && p[i] == name[i])
        i++;
    if (i == nl)
        return (p + i < end && p[i] != '\t') ? 1 : 0;
    if (p + i >= end || p[i] == '\t')
        return -1;
    return (unsigned char)p[i] < (unsigned char)name[i] ? -1 : 1;
}

static void add_hit(TagHits *h, const char *line, size_t len)
{
    const char *t1 = memchr(line, '\t', len), *t2, *ext;
    size_t addr_len, i;
    TagHit *hit;

    if (!t1)
        return;
    t2 = memchr(t1 + 1, '\t', (size_t)(line + len - t1 - 1));
    if (!t2)
        return;
    /* the address ends at ;" (or at the end of the line) */
    ext = NULL;
    addr_len = (size_t)(line + len - t2 - 1);
    for (i = 0; i + 1 < addr_len; i++)
        if (t2[1 + i] == ';' && t2[2 + i] == '"') {
            ext = t2 + 3 + i;
            addr_len = i;
            break;
        }
    if (h->n == h->cap) {
        h->cap = h->cap ? h->cap * 2 : 8;
        h->v = xrealloc(h->v, h->cap * sizeof *h->v);
    }
    hit = &h->v[h->n++];
    memset(hit, 0, sizeof *hit);
    hit->file = xmalloc((size_t)(t2 - t1));
    memcpy(hit->file, t1 + 1, (size_t)(t2 - t1 - 1));
    hit->file[t2 - t1 - 1] = '\0';
    hit->addr = xmalloc(addr_len + 1);
    memcpy(hit->addr, t2 + 1, addr_len);
    hit->addr[addr_len] = '\0';
    /* extended fields: a bare kind letter, "kind:function", "line:12" */
    while (ext && ext < line + len) {
        const char *f = ext, *e = ext;
        size_t fl;

        while (e < line + len && *e != '\t')
            e++;
        fl = (size_t)(e - f);
        if (fl > 0) {
            if (fl > 5 && memcmp(f, "line:", 5) == 0) {
                hit->line = (size_t)strtoul(f + 5, NULL, 10);
            } else if (fl > 5 && memcmp(f, "kind:", 5) == 0) {
                snprintf(hit->kind, sizeof hit->kind, "%.*s", (int)(fl - 5 > 23 ? 23 : fl - 5), f + 5);
            } else if (!memchr(f, ':', fl) && !hit->kind[0]) {
                snprintf(hit->kind, sizeof hit->kind, "%.*s", (int)(fl > 23 ? 23 : fl), f);
            }
        }
        ext = e + 1;
    }
}

long tags_lookup(const char *tagsfile, const char *name, TagHits *out)
{
    FILE *f = fopen(tagsfile, "rb");
    SBuf text = SBUF_INIT;
    char buf[8192];
    size_t got, nl = strlen(name), i, nlines = 0, cap = 0, *start = NULL, lo, hi;
    int sorted = 0;
    long found = 0;

    if (!f)
        return -1;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0 && text.len < TAGS_MAX_BYTES)
        sbuf_add(&text, buf, got);
    fclose(f);
    if (!text.p)
        return 0;
    /* line starts */
    for (i = 0; i < text.len;) {
        const char *nlp = memchr(text.p + i, '\n', text.len - i);
        size_t e = nlp ? (size_t)(nlp - text.p) : text.len;

        if (nlines == cap) {
            cap = cap ? cap * 2 : 1024;
            start = xrealloc(start, (cap + 1) * sizeof *start);
        }
        start[nlines++] = i;
        i = e + 1;
    }
    start[nlines] = i;                  /* one past the newline that ends the last line */
    for (i = 0; i < nlines && text.p[start[i]] == '!'; i++) {
        const char *l = text.p + start[i];

        if (strncmp(l, "!_TAG_FILE_SORTED\t1", 19) == 0)
            sorted = 1;
    }
    {
        size_t first = i;

        if (sorted) {
            /* first line whose name is not below the wanted one */
            lo = first;
            hi = nlines;
            while (lo < hi) {
                size_t mid = lo + (hi - lo) / 2;
                const char *l = text.p + start[mid];
                const char *end = text.p + start[mid + 1] - 1;

                if (name_cmp(l, end, name, nl) < 0)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            for (i = lo; i < nlines; i++) {
                const char *l = text.p + start[i];
                size_t len = start[i + 1] - start[i] - 1;

                if (name_cmp(l, l + len, name, nl) != 0)
                    break;
                add_hit(out, l, len);
                found++;
            }
        } else {
            for (i = first; i < nlines; i++) {
                const char *l = text.p + start[i];
                size_t len = start[i + 1] - start[i] - 1;

                if (len > 0 && l[len - 1] == '\r')
                    len--;
                if (name_cmp(l, l + len, name, nl) == 0) {
                    add_hit(out, l, len);
                    found++;
                }
            }
        }
    }
    free(start);
    sbuf_free(&text);
    return found;
}

/* The text of a /^...$/ search address, unescaped; *anchored tells whether ^ and $ were there. */
static char *pattern_of(const char *addr, int *anchored)
{
    size_t n = strlen(addr), i, k = 0;
    char *out;
    char delim;

    if (n < 3 || (addr[0] != '/' && addr[0] != '?'))
        return NULL;
    delim = addr[0];
    if (addr[n - 1] != delim)
        return NULL;
    out = xmalloc(n);
    i = 1;
    *anchored = 0;
    if (addr[i] == '^') {
        *anchored = 1;
        i++;
    }
    for (; i + 1 < n; i++) {
        if (addr[i] == '\\' && i + 2 < n && (addr[i + 1] == '\\' || addr[i + 1] == delim)) {
            out[k++] = addr[i + 1];
            i++;
        } else if (addr[i] == '$' && i + 2 == n) {
            if (*anchored)
                *anchored = 2;
        } else {
            out[k++] = addr[i];
        }
    }
    out[k] = '\0';
    return out;
}

static int contains(const Line *l, const char *pat, size_t pl)
{
    size_t i;

    if (pl == 0 || l->len < pl)
        return 0;
    for (i = 0; i + pl <= l->len; i++)
        if (memcmp(l->s + i, pat, pl) == 0)
            return 1;
    return 0;
}

size_t tag_resolve(const TagHit *h, const char *name, const Buffer *b)
{
    char *end;
    unsigned long v = strtoul(h->addr, &end, 10);
    size_t y, pl;
    int anchored = 0;
    char *pat;

    if (end != h->addr && *end == '\0')
        return v > 0 ? (size_t)v : 0;
    if (h->line > 0)
        return h->line;
    pat = pattern_of(h->addr, &anchored);
    if (!pat)
        return 0;
    pl = strlen(pat);
    for (y = 0; y < b->nlines; y++) {
        const Line *l = &b->lines[y];

        if (anchored == 2 ? (l->len == pl && memcmp(l->s, pat, pl) == 0)
                          : anchored == 1 ? (l->len >= pl && memcmp(l->s, pat, pl) == 0)
                                          : contains(l, pat, pl)) {
            free(pat);
            return y + 1;
        }
    }
    free(pat);
    (void)name;
    return 0;
}
