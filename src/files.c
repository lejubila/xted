#include "sys.h"
#include "files.h"
#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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

/* A walk that can stop and go on later: one frame per directory being read (its entries and
 * the next one to visit), the innermost last, as the recursion would hold them. */
typedef struct {
    char *rel;
    int depth;
    char **names;
    size_t nn, ni;
} Dirf;

struct FScan {
    char *root;
    const char *const *prune;
    Dirf *st;
    size_t ns, cap;
    size_t count;
};

static void push_dir(FScan *s, char *rel, int depth)
{
    char *dir = rel[0] ? files_join(s->root, rel) : xstrdup(s->root);
    Dirf *f;

    if (s->ns == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->st = xrealloc(s->st, s->cap * sizeof *s->st);
    }
    f = &s->st[s->ns++];
    f->rel = rel;
    f->depth = depth;
    f->nn = list_dir(dir, &f->names, 0);
    f->ni = 0;
    free(dir);
}

static void pop_dir(FScan *s)
{
    Dirf *f = &s->st[--s->ns];
    size_t i;

    for (i = 0; i < f->nn; i++)
        free(f->names[i]);
    free(f->names);
    free(f->rel);
}

FScan *files_scan_begin(const char *root, const char *const *prune)
{
    FScan *s = xmalloc(sizeof *s);

    memset(s, 0, sizeof *s);
    s->root = xstrdup(root);
    s->prune = prune;
    push_dir(s, xstrdup(""), 0);
    return s;
}

void files_scan_free(FScan *s)
{
    if (!s)
        return;
    while (s->ns > 0)
        pop_dir(s);
    free(s->st);
    free(s->root);
    free(s);
}

static long mono_us(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000000L + t.tv_nsec / 1000;
}

int files_scan_step(FScan *s, void (*add)(const char *rel, void *ctx), void *ctx, long budget_us)
{
    long end = budget_us > 0 ? mono_us() + budget_us : 0;

    while (s->ns > 0) {
        Dirf *f = &s->st[s->ns - 1];
        const char *name;
        char *r, *full;
        struct stat st;

        if (f->ni >= f->nn || s->count >= SCAN_MAX_FILES) {
            pop_dir(s);
            continue;
        }
        name = f->names[f->ni++];
        r = f->rel[0] ? files_join(f->rel, name) : xstrdup(name);
        full = files_join(s->root, r);
        if (sys_lstat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                size_t pi;
                int skip = 0;

                for (pi = 0; s->prune && s->prune[pi]; pi++)
                    if (strcmp(s->prune[pi], name) == 0)
                        skip = 1;
                if (!skip && f->depth < SCAN_MAX_DEPTH) {
                    push_dir(s, r, f->depth + 1);       /* takes r; f may move */
                    r = NULL;
                }
            } else if (S_ISREG(st.st_mode) ||
                       (S_ISLNK(st.st_mode) && stat(full, &st) == 0 && S_ISREG(st.st_mode))) {
                add(r, ctx);
                s->count++;
            }
        }
        free(full);
        free(r);
        if (end && mono_us() >= end)
            return s->ns == 0;
    }
    return 1;
}

size_t files_scan_prune(const char *root, void (*add)(const char *rel, void *ctx), void *ctx,
                        const char *const *prune)
{
    FScan *s = files_scan_begin(root, prune);
    size_t n;

    files_scan_step(s, add, ctx, 0);
    n = s->count;
    files_scan_free(s);
    return n;
}

size_t files_scan(const char *root, void (*add)(const char *rel, void *ctx), void *ctx)
{
    return files_scan_prune(root, add, ctx, NULL);
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

        if (sys_getcwd(cwd, sizeof cwd)) {
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
