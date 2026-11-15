#include "cfgsave.h"
#include "editorconfig.h"
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char **v;
    size_t n, cap;
} Lines;

static void lines_insert(Lines *L, size_t at, const char *s)
{
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 32;
        L->v = xrealloc(L->v, L->cap * sizeof *L->v);
    }
    memmove(L->v + at + 1, L->v + at, (L->n - at) * sizeof *L->v);
    L->v[at] = xstrdup(s);
    L->n++;
}

static void lines_free(Lines *L)
{
    size_t i;

    for (i = 0; i < L->n; i++)
        free(L->v[i]);
    free(L->v);
}

/* Returns 0, or -1 if the file exists and cannot be read. A missing file is no lines. */
static int read_lines(const char *file, Lines *L)
{
    FILE *f = fopen(file, "r");
    char buf[4096];

    memset(L, 0, sizeof *L);
    if (!f)
        return errno == ENOENT ? 0 : -1;
    while (fgets(buf, sizeof buf, f)) {
        size_t n = strlen(buf);

        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
            buf[--n] = '\0';
        lines_insert(L, L->n, buf);
    }
    fclose(f);
    return 0;
}

static void make_parents(const char *file)
{
    char *p = xstrdup(file);
    size_t i;

    for (i = 1; p[i]; i++)
        if (p[i] == '/') {
            p[i] = '\0';
            mkdir(p, 0755);
            p[i] = '/';
        }
    free(p);
}

static int write_lines(const char *file, const Lines *L, char *err, size_t errn)
{
    size_t n = strlen(file) + 32, i;
    char *tmp = xmalloc(n);
    struct stat st;
    mode_t mode = stat(file, &st) == 0 ? (st.st_mode & 0777) : 0644;
    FILE *f;
    int ok;

    make_parents(file);
    snprintf(tmp, n, "%s.xted-tmp-%ld", file, (long)getpid());
    f = fopen(tmp, "w");
    if (!f) {
        snprintf(err, errn, "non posso scrivere %s: %s", file, strerror(errno));
        free(tmp);
        return -1;
    }
    for (i = 0; i < L->n; i++) {
        fputs(L->v[i], f);
        fputc('\n', f);
    }
    ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (ok)
        chmod(tmp, mode);
    if (!ok || rename(tmp, file) != 0) {
        snprintf(err, errn, "non posso scrivere %s: %s", file, strerror(errno));
        unlink(tmp);
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

static const char *blanks(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

/* "[name]" on a line: the name, else NULL. */
static int header_of(const char *line, char *name, size_t cap)
{
    const char *s = blanks(line), *e;
    size_t n;

    if (*s != '[' || !(e = strrchr(s, ']')))
        return 0;
    n = (size_t)(e - s - 1);
    if (n >= cap)
        n = cap - 1;
    memcpy(name, s + 1, n);
    name[n] = '\0';
    return 1;
}

static int key_line(const char *line, const char *key)
{
    const char *s = blanks(line);
    size_t n = strlen(key);

    return strncasecmp(s, key, n) == 0 && *blanks(s + n) == '=';
}

/* The lines of a section, [from, to): after its header up to the next one. A NULL section is
 * the top of the file. Returns 0 if there is no such section. */
static int find_section(const Lines *L, const char *section, size_t *from, size_t *to)
{
    size_t i;
    char name[256];
    int in = section == NULL;

    *from = 0;
    *to = L->n;
    for (i = 0; i < L->n; i++) {
        if (!header_of(L->v[i], name, sizeof name))
            continue;
        if (in) {
            *to = i;
            return 1;
        }
        if (strcasecmp(name, section) == 0) {
            in = 1;
            *from = i + 1;
        }
    }
    return in;
}

/* key = value in [from, *to): replaced if it is there, else after the last line with text. */
static void set_in_range(Lines *L, size_t from, size_t *to, const char *key, const char *value)
{
    size_t i, at = from, n = strlen(key) + strlen(value) + 4;
    char *line = xmalloc(n);

    snprintf(line, n, "%s = %s", key, value);
    for (i = from; i < *to; i++)
        if (key_line(L->v[i], key)) {
            free(L->v[i]);
            L->v[i] = line;
            return;
        }
    for (i = from; i < *to; i++)
        if (*blanks(L->v[i]))
            at = i + 1;
    lines_insert(L, at, line);
    (*to)++;
    free(line);
}

int cfgsave_write(const char *file, const char *section, const CfgPair *p, size_t n, char *err,
                  size_t errn)
{
    Lines L;
    size_t from, to, i;
    int r;

    if (read_lines(file, &L) < 0) {
        snprintf(err, errn, "non posso leggere %s: %s", file, strerror(errno));
        return -1;
    }
    if (!find_section(&L, section, &from, &to)) {
        char head[300];

        if (L.n > 0 && *blanks(L.v[L.n - 1]))
            lines_insert(&L, L.n, "");
        snprintf(head, sizeof head, "[%s]", section);
        lines_insert(&L, L.n, head);
        from = to = L.n;
    }
    for (i = 0; i < n; i++)
        set_in_range(&L, from, &to, p[i].key, p[i].value);
    r = write_lines(file, &L, err, errn);
    lines_free(&L);
    return r;
}

/* ---- .editorconfig ------------------------------------------------------ */

/* The folder of path as an absolute path (malloc'ed), and the file name in *base. */
static char *abs_dir(const char *path, const char **base)
{
    const char *slash = strrchr(path, '/');
    char dir[PATH_MAX], *r;

    if (slash && slash != path) {
        char *d = xstrndup(path, (size_t)(slash - path));

        r = realpath(d, dir) ? xstrdup(dir) : NULL;
        free(d);
    } else if (slash) {
        r = xstrdup("/");
    } else {
        r = getcwd(dir, sizeof dir) ? xstrdup(dir) : NULL;
    }
    *base = slash ? slash + 1 : path;
    return r ? r : xstrdup(".");
}

static int is_file(const char *dir, const char *name)
{
    size_t n = strlen(dir) + strlen(name) + 2;
    char *p = xmalloc(n);
    struct stat st;
    int r;

    snprintf(p, n, "%s/%s", dir, name);
    r = stat(p, &st) == 0 && S_ISREG(st.st_mode);
    free(p);
    return r;
}

static int is_here(const char *dir, const char *name)
{
    size_t n = strlen(dir) + strlen(name) + 2;
    char *p = xmalloc(n);
    struct stat st;
    int r;

    snprintf(p, n, "%s/%s", dir, name);
    r = stat(p, &st) == 0;
    free(p);
    return r;
}

/* Climbs from dir: the first folder for which pred says yes (malloc'ed), else NULL. */
static char *climb(const char *dir, const char *const *names, int files_only)
{
    char *d = xstrdup(dir);

    for (;;) {
        char *slash;
        size_t i;

        for (i = 0; names[i]; i++)
            if (files_only ? is_file(d, names[i]) : is_here(d, names[i]))
                return d;
        slash = strrchr(d, '/');
        if (!slash)
            break;
        if (slash == d) {
            if (d[1] == '\0')
                break;
            d[1] = '\0';
        } else {
            *slash = '\0';
        }
    }
    free(d);
    return NULL;
}

char *cfgsave_find_project(const char *path)
{
    static const char *const names[] = { ".xted", NULL };
    const char *base;
    char *dir = abs_dir(path, &base), *r = climb(dir, names, 1), *out = NULL;

    if (r) {
        size_t n = strlen(r) + 8;

        out = xmalloc(n);
        snprintf(out, n, "%s/.xted", r);
    }
    free(r);
    free(dir);
    return out;
}

char *cfgsave_project_root(const char *path)
{
    static const char *const names[] = { ".xted", ".git", "composer.json", "package.json", NULL };
    const char *base;
    char *dir = abs_dir(path, &base), *r = climb(dir, names, 0);

    if (!r)
        return dir;
    free(dir);
    return r;
}

/* The EditorConfig name and value of one of our keys; 0 if .editorconfig has no such thing. */
static int ec_map(const CfgPair *p, const char **key, const char **val)
{
    if (strcmp(p->key, "expand_tab") == 0) {
        *key = "indent_style";
        *val = strcmp(p->value, "true") == 0 ? "space" : "tab";
    } else if (strcmp(p->key, "indent_width") == 0) {
        *key = "indent_size";
        *val = p->value;
    } else if (strcmp(p->key, "tab_width") == 0) {
        *key = "tab_width";
        *val = p->value;
    } else {
        return 0;
    }
    return 1;
}

/* The sections of L whose glob matches rel: their line ranges, in file order. */
static size_t ec_sections(const Lines *L, const char *rel, size_t from[], size_t to[], size_t max)
{
    size_t i, n = 0;
    int open = 0;
    char name[256];

    for (i = 0; i < L->n; i++) {
        if (!header_of(L->v[i], name, sizeof name))
            continue;
        if (open)
            to[n - 1] = i;
        open = 0;
        if (n < max && editorconfig_match(name, rel)) {
            from[n] = i + 1;
            to[n++] = L->n;
            open = 1;
        }
    }
    return n;
}

char *cfgsave_editorconfig_path(const char *docpath)
{
    static const char *const names[] = { ".editorconfig", NULL };
    const char *base;
    char *dir = abs_dir(docpath, &base), *where = climb(dir, names, 1), *out;
    size_t n;

    if (!where)
        where = cfgsave_project_root(docpath);
    n = strlen(where) + 16;
    out = xmalloc(n);
    snprintf(out, n, "%s/.editorconfig", where);
    free(where);
    free(dir);
    return out;
}

int cfgsave_editorconfig(const char *docpath, const CfgPair *p, size_t n, char *file, size_t filen,
                         char *err, size_t errn)
{
    static const char *const names[] = { ".editorconfig", NULL };
    const char *base;
    char *dir = abs_dir(docpath, &base), *found = climb(dir, names, 1), *where, *full;
    const char *rel;
    Lines L;
    size_t i, written = 0, fn;
    int r;

    where = found ? found : cfgsave_project_root(docpath);
    fn = strlen(where) + 16;
    full = xmalloc(fn);
    memcpy(full, where, fn - 16);
    memcpy(full + fn - 16, "/.editorconfig", 15);
    snprintf(file, filen, "%s", full);
    /* the file name relative to the folder that holds the .editorconfig */
    {
        size_t wl = strlen(where), dl = strlen(dir);
        char *rp = xmalloc(dl + strlen(base) + 2);

        snprintf(rp, dl + strlen(base) + 2, "%s/%s", dir, base);
        rel = strncmp(rp, where, wl) == 0 && rp[wl] == '/' ? rp + wl + 1 : base;
        rel = xstrdup(rel);
        free(rp);
    }
    if (read_lines(full, &L) < 0) {
        snprintf(err, errn, "non posso leggere %s: %s", full, strerror(errno));
        r = -1;
        goto done;
    }
    if (L.n == 0 && !found) {
        lines_insert(&L, 0, "root = true");
    }
    for (i = 0; i < n; i++) {
        const char *key, *val;
        size_t from[16], to[16], ns;

        if (!ec_map(&p[i], &key, &val))
            continue;
        ns = ec_sections(&L, rel, from, to, 16);
        if (ns == 0) {
            char head[300];
            const char *dot = strrchr(base, '.');

            if (L.n > 0 && *blanks(L.v[L.n - 1]))
                lines_insert(&L, L.n, "");
            if (dot && dot != base)
                snprintf(head, sizeof head, "[*%s]", dot);
            else
                snprintf(head, sizeof head, "[%s]", base);
            lines_insert(&L, L.n, head);
            ns = ec_sections(&L, rel, from, to, 16);
        }
        /* the last matching section is the most specific one: the key goes there, so that it
         * decides for this kind of file only and wins over the earlier sections */
        {
            size_t t = to[ns - 1];

            set_in_range(&L, from[ns - 1], &t, key, val);
        }
        written++;
    }
    r = written == 0 ? 0 : write_lines(full, &L, err, errn);
    if (r == 0)
        r = (int)written;
done:
    lines_free(&L);
    free((char *)rel);
    free(full);
    free(where);
    free(dir);
    return r;
}
