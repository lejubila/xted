#include "sys.h"
#include "session.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAGIC "xted-session 1"
#define MAGIC_OLD "ked-session 1"     /* written before the editor was renamed */

char *session_file(void)
{
    const char *state = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    char cwd[4096], *key, *out, *base, *dir;
    SBuf b = SBUF_INIT;
    size_t i;

    if (!sys_getcwd(cwd, sizeof cwd))
        return NULL;
    if (state && sys_is_abs(state)) {
        sbuf_adds(&b, state);
    } else if (home && home[0]) {
        sbuf_adds(&b, home);
        sbuf_adds(&b, "/.local/state");
    } else {
        return NULL;
    }
    sbuf_add(&b, "", 1);
    base = sbuf_steal(&b);
    dir = app_dir(base);
    free(base);
    key = xstrdup(cwd);
    for (i = 0; key[i]; i++)
#ifdef _WIN32
        /* C:/dir: a ':' in a name is a stream of NTFS, not a file */
        if (key[i] == '/' || key[i] == ':')
#else
        if (key[i] == '/')
#endif
            key[i] = '%';
    sbuf_adds(&b, dir);
    sbuf_adds(&b, "/sessions/");
    sbuf_adds(&b, key);
    sbuf_add(&b, "", 1);
    out = sbuf_steal(&b);
    free(key);
    free(dir);
    return out;
}

void session_add(Session *s, const char *path, size_t y, size_t x, size_t rowoff)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->e = xrealloc(s->e, s->cap * sizeof *s->e);
    }
    s->e[s->n].path = xstrdup(path);
    s->e[s->n].y = y;
    s->e[s->n].x = x;
    s->e[s->n].rowoff = rowoff;
    s->n++;
}

/* mkdir -p for the directory part of path. */
static int make_parents(const char *path)
{
    char *p = xstrdup(path);
    size_t i;
    int r = 0;

    for (i = 1; p[i]; i++) {
        if (p[i] != '/')
            continue;
        p[i] = '\0';
        if (sys_mkdir(p, 0700) < 0 && errno != EEXIST)
            r = -1;
        p[i] = '/';
    }
    free(p);
    return r;
}

int session_save(const char *file, const Session *s)
{
    FILE *f;
    size_t i;

    if (s->n == 0) {
        unlink(file);
        return 0;
    }
    if (make_parents(file) < 0)
        return -1;
    f = fopen(file, "w");
    if (!f)
        return -1;
    fprintf(f, "%s\nactive %zu\nsidebar %d\n", MAGIC, s->active, s->sidebar);
    if (s->layout)
        fprintf(f, "activewin %zu\nlayout %s\n", s->activewin, s->layout);
    for (i = 0; i < s->n; i++)
        fprintf(f, "buf\t%zu\t%zu\t%zu\t%s\n", s->e[i].y, s->e[i].x, s->e[i].rowoff,
                s->e[i].path);
    return fclose(f) == 0 ? 0 : -1;
}

int session_load(const char *file, Session *s)
{
    FILE *f = fopen(file, "r");
    char line[8192];

    memset(s, 0, sizeof *s);
    if (!f)
        return -1;
    if (!fgets(line, sizeof line, f) || (strncmp(line, MAGIC, strlen(MAGIC)) != 0 &&
                                         strncmp(line, MAGIC_OLD, strlen(MAGIC_OLD)) != 0)) {
        fclose(f);
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        size_t len = strlen(line);
        size_t y, x, ro;
        int off = 0;

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (strncmp(line, "active ", 7) == 0) {
            s->active = (size_t)strtoul(line + 7, NULL, 10);
        } else if (strncmp(line, "activewin ", 10) == 0) {
            s->activewin = (size_t)strtoul(line + 10, NULL, 10);
        } else if (strncmp(line, "sidebar ", 8) == 0) {
            s->sidebar = atoi(line + 8);
        } else if (strncmp(line, "layout ", 7) == 0) {
            free(s->layout);
            s->layout = xstrdup(line + 7);
        } else if (sscanf(line, "buf\t%zu\t%zu\t%zu\t%n", &y, &x, &ro, &off) >= 3 && off > 0 &&
                   line[off]) {
            session_add(s, line + off, y, x, ro);
        }
    }
    fclose(f);
    if (s->active >= s->n)
        s->active = 0;
    return 0;
}

void session_free(Session *s)
{
    size_t i;

    for (i = 0; i < s->n; i++)
        free(s->e[i].path);
    free(s->e);
    free(s->layout);
    memset(s, 0, sizeof *s);
}
