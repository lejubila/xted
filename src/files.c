#include "files.h"
#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int skipped(const char *name)
{
    return name[0] == '.' || strcmp(name, "node_modules") == 0 || strcmp(name, "build") == 0 ||
           strcmp(name, "target") == 0 || strcmp(name, "__pycache__") == 0;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Entry names of a directory, sorted. Returns the count; *out is malloc'ed. */
static size_t list_dir(const char *path, char ***out, int include_hidden)
{
    DIR *d = opendir(path);
    struct dirent *de;
    char **v = NULL;
    size_t n = 0, cap = 0;

    *out = NULL;
    if (!d)
        return 0;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (!include_hidden && skipped(de->d_name))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            v = xrealloc(v, cap * sizeof *v);
        }
        v[n++] = xstrdup(de->d_name);
    }
    closedir(d);
    if (n > 1)
        qsort(v, n, sizeof *v, cmp_names);
    *out = v;
    return n;
}

char *files_join(const char *dir, const char *name)
{
    size_t dl = strlen(dir), nl = strlen(name);
    char *p;

    if (strcmp(dir, ".") == 0)
        return xstrdup(name);
    p = xmalloc(dl + nl + 2);

    memcpy(p, dir, dl);
    if (dl > 0 && dir[dl - 1] != '/')
        p[dl++] = '/';
    memcpy(p + dl, name, nl + 1);
    return p;
}

typedef struct {
    void (*add)(const char *, void *);
    void *ctx;
    size_t count;
} Scan;

static void scan_dir(Scan *s, const char *root, const char *rel, int depth)
{
    char *dir = rel[0] ? files_join(root, rel) : xstrdup(root);
    char **names;
    size_t n = list_dir(dir, &names, 0), i;

    for (i = 0; i < n && s->count < SCAN_MAX_FILES; i++) {
        char *r = rel[0] ? files_join(rel, names[i]) : xstrdup(names[i]);
        char *full = files_join(root, r);
        struct stat st;

        if (lstat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                if (depth < SCAN_MAX_DEPTH)
                    scan_dir(s, root, r, depth + 1);
            } else if (S_ISREG(st.st_mode) ||
                       (S_ISLNK(st.st_mode) && stat(full, &st) == 0 && S_ISREG(st.st_mode))) {
                s->add(r, s->ctx);
                s->count++;
            }
        }
        free(full);
        free(r);
    }
    for (i = 0; i < n; i++)
        free(names[i]);
    free(names);
    free(dir);
}

size_t files_scan(const char *root, void (*add)(const char *rel, void *ctx), void *ctx)
{
    Scan s;

    s.add = add;
    s.ctx = ctx;
    s.count = 0;
    scan_dir(&s, root, "", 0);
    return s.count;
}

/* ---- tree -------------------------------------------------------------- */

static void entry_free(TreeEntry *e)
{
    free(e->path);
    free(e->name);
}

static void tree_insert(Tree *t, size_t at, TreeEntry ent)
{
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 32;
        t->e = xrealloc(t->e, t->cap * sizeof *t->e);
    }
    memmove(&t->e[at + 1], &t->e[at], (t->n - at) * sizeof *t->e);
    t->e[at] = ent;
    t->n++;
}

static TreeEntry make_entry(const char *path, const char *name, int depth)
{
    TreeEntry e;
    struct stat st;

    e.path = xstrdup(path);
    e.name = xstrdup(name);
    e.depth = depth;
    e.open = 0;
    e.is_dir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    return e;
}

static void expand(Tree *t, size_t i)
{
    char **names;
    size_t n = list_dir(t->e[i].path, &names, 1), k, at = i + 1;
    int pass;

    t->e[i].open = 1;
    /* folders first, then files; hidden entries are shown in the browser */
    for (pass = 0; pass < 2; pass++)
        for (k = 0; k < n; k++) {
            char *p = files_join(t->e[i].path, names[k]);
            TreeEntry ent = make_entry(p, names[k], t->e[i].depth + 1);

            free(p);
            if ((pass == 0) == (ent.is_dir != 0)) {
                tree_insert(t, at++, ent);
            } else {
                entry_free(&ent);
            }
        }
    for (k = 0; k < n; k++)
        free(names[k]);
    free(names);
}

static void collapse(Tree *t, size_t i)
{
    size_t j = i + 1;

    while (j < t->n && t->e[j].depth > t->e[i].depth)
        entry_free(&t->e[j++]);
    memmove(&t->e[i + 1], &t->e[j], (t->n - j) * sizeof *t->e);
    t->n -= j - i - 1;
    t->e[i].open = 0;
}

void tree_init(Tree *t, const char *root)
{
    const char *base = strrchr(root, '/');
    TreeEntry r;

    memset(t, 0, sizeof *t);
    t->root = xstrdup(root);
    base = base && base[1] ? base + 1 : root;
    r = make_entry(root, base, 0);
    if (strcmp(root, ".") == 0) {
        char cwd[4096];

        if (getcwd(cwd, sizeof cwd)) {
            const char *b = strrchr(cwd, '/');

            free(r.name);
            r.name = xstrdup(b && b[1] ? b + 1 : cwd);
        }
    }
    r.is_dir = 1;
    tree_insert(t, 0, r);
    expand(t, 0);
    t->sel = t->n > 1 ? 1 : 0;
}

void tree_free(Tree *t)
{
    size_t i;

    for (i = 0; i < t->n; i++)
        entry_free(&t->e[i]);
    free(t->e);
    free(t->root);
    memset(t, 0, sizeof *t);
}

void tree_toggle(Tree *t, size_t i)
{
    if (i >= t->n || !t->e[i].is_dir)
        return;
    if (t->e[i].open)
        collapse(t, i);
    else
        expand(t, i);
}

void tree_left(Tree *t)
{
    size_t i = t->sel;

    if (i >= t->n)
        return;
    if (t->e[i].is_dir && t->e[i].open && t->e[i].depth > 0) {
        collapse(t, i);
        return;
    }
    while (i > 0 && t->e[i - 1].depth >= t->e[t->sel].depth)
        i--;
    if (i > 0)
        t->sel = i - 1;
}

void tree_fit(Tree *t, size_t rows)
{
    if (rows == 0)
        return;
    if (t->sel < t->top)
        t->top = t->sel;
    if (t->sel >= t->top + rows)
        t->top = t->sel - rows + 1;
}
