#include "doc.h"
#include "i18n.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

Doc *doc_new(void)
{
    Doc *d = xmalloc(sizeof *d);

    memset(d, 0, sizeof *d);
    buf_init(&d->buf);
    d->tabw = 8;
    d->indent_w = 8;
    d->autosuggest = 1;
    d->autopair = 1;
    d->suggest_min = 2;
    return d;
}

void doc_free(Doc *d)
{
    if (!d)
        return;
    syn_free(d->syn);
    buf_free(&d->buf);
    free(d->title);
    free(d);
}

void doc_set_syntax(Doc *d, Syntax *syn)
{
    syn_free(d->syn);
    d->syn = syn;
    d->buf.stable = 0;
}

static void record_disk(Doc *d)
{
    struct stat st;

    d->disk_known = 0;
    d->readonly = 0;
    if (!d->buf.path || stat(d->buf.path, &st) != 0)
        return;
    d->disk_known = 1;
    d->mtime = st.st_mtim;
    d->size = st.st_size;
    d->ino = st.st_ino;
    d->readonly = access(d->buf.path, W_OK) != 0;
}

void doc_accept_disk(Doc *d)
{
    record_disk(d);
}

int doc_load(Doc *d, const char *path, char *err, size_t errlen)
{
    if (buf_load(&d->buf, path, err, errlen) < 0)
        return -1;
    record_disk(d);
    doc_set_syntax(d, syn_detect(&d->buf));
    return 0;
}

int doc_load_fd(Doc *d, int fd, char *err, size_t errlen)
{
    if (buf_load_fd(&d->buf, fd, err, errlen) < 0)
        return -1;
    d->disk_known = 0;
    doc_set_syntax(d, syn_detect(&d->buf));
    return 0;
}

long doc_save(Doc *d, const char *path, char *err, size_t errlen)
{
    int renamed = path && (!d->buf.path || strcmp(path, d->buf.path) != 0);
    long n = buf_save(&d->buf, path, err, errlen);

    if (n < 0)
        return -1;
    record_disk(d);
    if (renamed || !d->syn)
        doc_set_syntax(d, syn_detect(&d->buf));
    return n;
}

int doc_disk_changed(const Doc *d)
{
    struct stat st;

    if (!d->buf.path || !d->disk_known)
        return DISK_SAME;
    if (stat(d->buf.path, &st) != 0)
        return DISK_MISSING;
    if (st.st_mtim.tv_sec != d->mtime.tv_sec || st.st_mtim.tv_nsec != d->mtime.tv_nsec ||
        st.st_size != d->size || st.st_ino != d->ino)
        return DISK_CHANGED;
    return DISK_SAME;
}

int doc_reload(Doc *d, char *err, size_t errlen)
{
    char *path = d->buf.path ? xstrdup(d->buf.path) : NULL;
    int r;

    if (!path) {
        snprintf(err, errlen, tr("no file name"));
        return -1;
    }
    r = doc_load(d, path, err, errlen);
    free(path);
    return r;
}

const char *doc_base_name(const Doc *d)
{
    const char *p = d->buf.path, *s;

    if (d->title)
        return d->title;
    if (!p)
        return tr("[Untitled]");
    s = strrchr(p, '/');
    return s && s[1] ? s + 1 : p;
}
