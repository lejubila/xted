#include "i18n.h"
#include "sys.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef XTED_DATADIR
#define XTED_DATADIR "/usr/local/share/xted"
#endif

typedef struct {
    char *key, *val;
} Ent;

static Ent *ent;
static size_t nent;
static char *blob;
static char code[16] = "en", name[48] = "English";
static char *cfg_dir;
static char *cat_path;

static void clear(void)
{
    free(ent);
    free(blob);
    free(cat_path);
    cat_path = NULL;
    ent = NULL;
    blob = NULL;
    nent = 0;
    snprintf(code, sizeof code, "en");
    snprintf(name, sizeof name, "English");
}

static int cmp_ent(const void *a, const void *b)
{
    return strcmp(((const Ent *)a)->key, ((const Ent *)b)->key);
}

const char *tr(const char *msg)
{
    Ent k, *r;

    if (nent == 0)
        return msg;
    k.key = (char *)msg;
    k.val = NULL;
    r = bsearch(&k, ent, nent, sizeof *ent, cmp_ent);
    return r && r->val[0] ? r->val : msg;
}

/* The conversions of a printf format, each as "%flags.width.length conv;". */
static void specs(const char *s, char *out, size_t n)
{
    size_t o = 0;

    for (; *s; s++) {
        if (*s != '%')
            continue;
        s++;
        if (*s == '%')
            continue;
        if (o + 2 >= n)
            break;
        out[o++] = '%';
        while (*s && !strchr("diouxXeEfFgGaAcspn", *s) && o + 3 < n)
            out[o++] = *s++;
        if (!*s)
            break;
        out[o++] = *s;
        out[o++] = ';';
    }
    out[o] = '\0';
}

int i18n_same_formats(const char *a, const char *b)
{
    char x[160], y[160];

    specs(a, x, sizeof x);
    specs(b, y, sizeof y);
    return strcmp(x, y) == 0;
}

static void unescape(char *s)
{
    char *w = s;

    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            *w++ = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s;
        } else {
            *w++ = *s;
        }
    }
    *w = '\0';
}

static char *exe_dir(void)
{
    char buf[4096];
    long n = (long)sys_exe_path(buf, sizeof buf - 1);
    char *slash;

    if (n <= 0)
        return NULL;
    buf[n] = '\0';
    slash = strrchr(buf, '/');
    if (!slash)
        return NULL;
    *slash = '\0';
    return xstrdup(buf);
}

/* The folders that may hold catalogs, most specific first (malloc'ed strings, NULL ended). */
static char **lang_dirs(void)
{
    char **v = xmalloc(5 * sizeof *v);
    char *ed = exe_dir();
    size_t n = 0, l;

    if (cfg_dir) {
        l = strlen(cfg_dir) + 8;
        v[n] = xmalloc(l);
        snprintf(v[n++], l, "%s/lang", cfg_dir);
    }
    if (ed) {
        l = strlen(ed) + 20;
        v[n] = xmalloc(l);
        snprintf(v[n++], l, "%s/lang", ed);
        v[n] = xmalloc(l);
        snprintf(v[n++], l, "%s/../share/xted/lang", ed);
    }
    v[n++] = xstrdup(XTED_DATADIR "/lang");
    v[n] = NULL;
    free(ed);
    return v;
}

static void free_dirs(char **v)
{
    size_t i;

    for (i = 0; v[i]; i++)
        free(v[i]);
    free(v);
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *b = NULL;
    size_t n = 0, cap = 0, r;

    if (!f)
        return NULL;
    for (;;) {
        if (n + 4096 + 1 > cap) {
            cap = cap ? cap * 2 : 8192;
            b = xrealloc(b, cap);
        }
        r = fread(b + n, 1, 4096, f);
        n += r;
        if (r < 4096)
            break;
    }
    fclose(f);
    b[n] = '\0';
    return b;
}

static int valid_code(const char *s)
{
    size_t i, n = strlen(s);

    if (n == 0 || n >= sizeof code)
        return 0;
    for (i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '_' && s[i] != '-')
            return 0;
    return 1;
}

void i18n_init(const char *cfgdir)
{
    free(cfg_dir);
    cfg_dir = cfgdir ? xstrdup(cfgdir) : NULL;
}

int i18n_set(const char *lang)
{
    char want[16], **dirs, *text = NULL, *p;
    size_t i, cap = 0;

    if (!lang || !lang[0] || strcmp(lang, "auto") == 0)
        i18n_detect(want, sizeof want);
    else
        snprintf(want, sizeof want, "%s", lang);
    clear();
    if (strcmp(want, "en") == 0)
        return 0;
    if (!valid_code(want))
        return -1;
    dirs = lang_dirs();
    for (i = 0; dirs[i] && !text; i++) {
        size_t l = strlen(dirs[i]) + strlen(want) + 2;
        char *path = xmalloc(l);

        snprintf(path, l, "%s/%s", dirs[i], want);
        text = slurp(path);
        if (text)
            cat_path = path;
        else
            free(path);
    }
    free_dirs(dirs);
    if (!text)
        return -1;
    blob = text;
    for (p = text; *p;) {
        char *eol = strchr(p, '\n'), *tab;

        if (eol)
            *eol = '\0';
        if (p[0] && p[0] != '#' && (tab = strchr(p, '\t')) != NULL) {
            *tab = '\0';
            unescape(p);
            unescape(tab + 1);
            if (strcmp(p, "@name") == 0) {
                snprintf(name, sizeof name, "%s", tab + 1);
            } else if (i18n_same_formats(p, tab + 1)) {
                if (nent == cap) {
                    cap = cap ? cap * 2 : 128;
                    ent = xrealloc(ent, cap * sizeof *ent);
                }
                ent[nent].key = p;
                ent[nent++].val = tab + 1;
            }
        }
        p = eol ? eol + 1 : p + strlen(p);
    }
    qsort(ent, nent, sizeof *ent, cmp_ent);
    snprintf(code, sizeof code, "%s", want);
    return 0;
}

const char *i18n_path(void)
{
    return cat_path;
}

const char *i18n_code(void)
{
    return code;
}

const char *i18n_name(void)
{
    return name;
}

void i18n_detect(char *out, size_t n)
{
    static const char *const vars[] = { "LC_ALL", "LC_MESSAGES", "LANG" };
    size_t i, k;

    snprintf(out, n, "en");
    for (i = 0; i < sizeof vars / sizeof vars[0]; i++) {
        const char *v = getenv(vars[i]);

        if (!v || !v[0])
            continue;
        if (strcmp(v, "C") == 0 || strcmp(v, "POSIX") == 0)
            return;
        for (k = 0; k + 1 < n && v[k] && v[k] != '_' && v[k] != '.' && v[k] != '@'; k++)
            out[k] = (char)tolower((unsigned char)v[k]);
        out[k] = '\0';
        return;
    }
    sys_lang(out, n);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

size_t i18n_available(char (*codes)[16], size_t max)
{
    char **dirs = lang_dirs();
    size_t n = 0, i, k;

    if (max == 0)
        return 0;
    snprintf(codes[n++], 16, "en");
    for (i = 0; dirs[i]; i++) {
        DIR *d = opendir(dirs[i]);
        struct dirent *de;

        if (!d)
            continue;
        while ((de = readdir(d)) != NULL && n < max) {
            int dup = 0;

            if (de->d_name[0] == '.' || !valid_code(de->d_name))
                continue;
            for (k = 0; k < n; k++)
                if (strcmp(codes[k], de->d_name) == 0)
                    dup = 1;
            if (!dup)
                snprintf(codes[n++], 16, "%.15s", de->d_name);
        }
        closedir(d);
    }
    free_dirs(dirs);
    if (n > 2)
        qsort(codes + 1, n - 1, 16, cmp_str);
    return n;
}
