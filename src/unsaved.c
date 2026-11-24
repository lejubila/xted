/* The store of unsaved.h: the folder, the texts and the index. */

#include "unsaved.h"
#include "sys.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAGIC "xted-unsaved 1"

/* $XDG_STATE_HOME/xted/unsaved (made when missing), or NULL. */
static char *folder(void)
{
    const char *state = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    char *base, *app, *dir;

    if (state && sys_is_abs(state)) {
        base = xstrdup(state);
    } else if (home && home[0]) {
        SBuf b = SBUF_INIT;

        sbuf_adds(&b, home);
        sbuf_adds(&b, "/.local/state");
        sbuf_add(&b, "", 1);
        base = sbuf_steal(&b);
    } else {
        return NULL;
    }
    app = app_dir(base);
    {
        SBuf b = SBUF_INIT;

        sbuf_adds(&b, app);
        sbuf_adds(&b, "/unsaved");
        sbuf_add(&b, "", 1);
        dir = sbuf_steal(&b);
    }
    /* each level, as the state folder itself may be missing */
    sys_mkdir(base, 0700);
    sys_mkdir(app, 0700);
    sys_mkdir(dir, 0700);
    free(base);
    free(app);
    return dir;
}

int unsaved_lock(void)
{
    char *dir = folder(), *p;
    size_t n;
    int r;

    if (!dir)
        return 0;
    n = strlen(dir) + 8;
    p = xmalloc(n);
    snprintf(p, n, "%s/lock", dir);
    r = sys_lock_file(p) == 0;
    free(p);
    free(dir);
    return r;
}

char *unsaved_path(unsigned id)
{
    char *dir = folder(), *p;
    size_t n;

    if (!dir)
        return NULL;
    n = strlen(dir) + 24;
    p = xmalloc(n);
    snprintf(p, n, "%s/%u.txt", dir, id);
    free(dir);
    return p;
}

/* data to path through a new file and a rename. */
static int write_file(const char *path, const char *data, size_t n)
{
    size_t pl = strlen(path) + 8;
    char *tmp = xmalloc(pl);
    FILE *f;
    int ok;

    snprintf(tmp, pl, "%s.new", path);
    f = fopen(tmp, "wb");
    if (!f) {
        free(tmp);
        return -1;
    }
    ok = (n == 0 || fwrite(data, 1, n, f) == n) && fflush(f) == 0 && sys_fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || sys_rename(tmp, path) != 0) {
        remove(tmp);
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

int unsaved_write(unsigned id, const char *text, size_t n)
{
    char *p = unsaved_path(id);
    int r;

    if (!p)
        return -1;
    r = write_file(p, text, n);
    free(p);
    return r;
}

int unsaved_write_index(const UnsavedEntry *v, size_t n)
{
    char *dir = folder(), *p;
    SBuf b = SBUF_INIT;
    DIR *d;
    struct dirent *de;
    size_t i, pl;
    int r;

    if (!dir)
        return -1;
    sbuf_adds(&b, MAGIC "\n");
    for (i = 0; i < n; i++)
        sbuf_addf(&b, "%u %zu %zu %zu\n", v[i].id, v[i].y, v[i].x, v[i].rowoff);
    pl = strlen(dir) + 32;
    p = xmalloc(pl);
    snprintf(p, pl, "%s/index", dir);
    r = write_file(p, b.p, b.len);
    sbuf_free(&b);
    /* the texts of the buffers that are no more */
    if (r == 0 && (d = opendir(dir)) != NULL) {
        while ((de = readdir(d)) != NULL) {
            char *end;
            unsigned long id = strtoul(de->d_name, &end, 10);
            int keep = 0;

            if (end == de->d_name || strcmp(end, ".txt") != 0)
                continue;
            for (i = 0; i < n; i++)
                if (v[i].id == id)
                    keep = 1;
            if (!keep) {
                snprintf(p, pl, "%s/%s", dir, de->d_name);
                remove(p);
            }
        }
        closedir(d);
    }
    free(p);
    free(dir);
    return r;
}

UnsavedEntry *unsaved_read_index(size_t *n)
{
    char *dir = folder(), *p, line[128];
    UnsavedEntry *v = NULL;
    size_t cap = 0, pl;
    FILE *f;

    *n = 0;
    if (!dir)
        return NULL;
    pl = strlen(dir) + 8;
    p = xmalloc(pl);
    snprintf(p, pl, "%s/index", dir);
    f = fopen(p, "rb");
    free(p);
    free(dir);
    if (!f)
        return NULL;
    if (!fgets(line, sizeof line, f) || strncmp(line, MAGIC, strlen(MAGIC)) != 0) {
        fclose(f);
        return NULL;
    }
    while (fgets(line, sizeof line, f)) {
        UnsavedEntry en;
        unsigned long id, y, x, ro;

        if (sscanf(line, "%lu %lu %lu %lu", &id, &y, &x, &ro) != 4 || id == 0)
            continue;
        en.id = (unsigned)id;
        en.y = (size_t)y;
        en.x = (size_t)x;
        en.rowoff = (size_t)ro;
        if (*n == cap) {
            cap = cap ? cap * 2 : 8;
            v = xrealloc(v, cap * sizeof *v);
        }
        v[(*n)++] = en;
    }
    fclose(f);
    return v;
}
