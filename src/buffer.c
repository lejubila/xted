#include "buffer.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FLUSH_AT 65536

static void line_reserve(Line *l, size_t need)
{
    size_t nc;

    if (need <= l->cap)
        return;
    nc = l->cap * 2;
    if (nc < need)
        nc = need;
    if (nc < 16)
        nc = 16;
    l->s = xrealloc(l->s, nc);
    l->cap = nc;
}

static void lines_reserve(Buffer *b, size_t need)
{
    size_t nc;

    if (need <= b->cap)
        return;
    nc = b->cap * 2;
    if (nc < need)
        nc = need;
    b->lines = xrealloc(b->lines, nc * sizeof(Line));
    b->cap = nc;
}

static void lines_clear(Buffer *b)
{
    size_t i;

    for (i = 0; i < b->nlines; i++)
        free(b->lines[i].s);
    b->nlines = 0;
}

static void push_line(Buffer *b, const char *s, size_t n)
{
    Line *l;

    lines_reserve(b, b->nlines + 1);
    l = &b->lines[b->nlines++];
    l->len = l->cap = n;
    l->s = NULL;
    if (n > 0) {
        l->s = xmalloc(n);
        memcpy(l->s, s, n);
    }
}

void buf_init(Buffer *b)
{
    memset(b, 0, sizeof *b);
    push_line(b, NULL, 0);
    b->final_newline = 1;
    b->empty_origin = 1;
}

void buf_free(Buffer *b)
{
    lines_clear(b);
    free(b->lines);
    free(b->path);
    memset(b, 0, sizeof *b);
}

static int read_all(int fd, size_t hint, char **out, size_t *outlen)
{
    size_t cap = hint + 1, len = 0;
    char *p = xmalloc(cap);

    for (;;) {
        ssize_t n;

        if (len == cap) {
            cap *= 2;
            p = xrealloc(p, cap);
        }
        n = read(fd, p + len, cap - len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            free(p);
            return -1;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }
    *out = p;
    *outlen = len;
    return 0;
}

int buf_load(Buffer *b, const char *path, char *err, size_t errlen)
{
    int fd;
    struct stat st;
    char *data;
    size_t len, nl_count = 0;
    const char *p, *end, *first_nl;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            free(b->path);
            b->path = xstrdup(path);
            b->is_new = 1;
            return 0;
        }
        snprintf(err, errlen, "%s", strerror(errno));
        return -1;
    }
    if (fstat(fd, &st) < 0) {
        snprintf(err, errlen, "%s", strerror(errno));
        close(fd);
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        snprintf(err, errlen, "%s", S_ISDIR(st.st_mode) ? "è una directory"
                                                        : "non è un file regolare");
        close(fd);
        return -1;
    }
    if (read_all(fd, (size_t)st.st_size, &data, &len) < 0) {
        snprintf(err, errlen, "%s", strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);

    lines_clear(b);
    b->has_nul = len > 0 && memchr(data, 0, len) != NULL;
    first_nl = memchr(data, '\n', len);
    b->crlf = first_nl && first_nl > data && first_nl[-1] == '\r';
    b->final_newline = 1;
    b->empty_origin = len == 0;

    for (p = data, end = data + len; (p = memchr(p, '\n', (size_t)(end - p))); p++)
        nl_count++;
    lines_reserve(b, nl_count + 1);

    p = data;
    end = data + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t n = (size_t)((nl ? nl : end) - p);

        if (nl && b->crlf && n > 0 && p[n - 1] == '\r')
            n--;
        push_line(b, p, n);
        if (!nl) {
            b->final_newline = 0;
            break;
        }
        p = nl + 1;
    }
    if (b->nlines == 0)
        push_line(b, NULL, 0);
    free(data);

    free(b->path);
    b->path = xstrdup(path);
    b->modified = 0;
    b->is_new = 0;
    return 0;
}

static int write_all(int fd, const char *s, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, s, n);

        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        s += w;
        n -= (size_t)w;
    }
    return 0;
}

long buf_save(Buffer *b, const char *path, char *err, size_t errlen)
{
    const char *want = path ? path : b->path;
    char *real, *tmp = NULL;
    const char *target, *slash;
    size_t dlen, i;
    struct stat st;
    mode_t mode;
    int fd = -1, existed;
    SBuf out = SBUF_INIT;
    const char *eol = b->crlf ? "\r\n" : "\n";
    size_t eollen = b->crlf ? 2 : 1;
    long total = 0;

    if (!want) {
        snprintf(err, errlen, "nessun nome file");
        return -1;
    }
    real = realpath(want, NULL);
    target = real ? real : want;

    existed = stat(target, &st) == 0;
    if (existed && access(target, W_OK) != 0) {
        snprintf(err, errlen, "%s", strerror(errno));
        goto fail;
    }
    if (existed) {
        mode = st.st_mode & 07777;
    } else {
        mode_t um = umask(0);

        umask(um);
        mode = 0666 & ~um;
    }

    /* temp file next to the target so that rename() stays on one filesystem */
    slash = strrchr(target, '/');
    dlen = slash ? (size_t)(slash - target) + 1 : 0;
    tmp = xmalloc(dlen + 16);
    memcpy(tmp, target, dlen);
    strcpy(tmp + dlen, ".ked.XXXXXX");
    fd = mkstemp(tmp);
    if (fd < 0) {
        snprintf(err, errlen, "%s", strerror(errno));
        free(tmp);
        tmp = NULL;
        goto fail;
    }

    for (i = 0; i < b->nlines; i++) {
        int last = i + 1 == b->nlines;

        sbuf_add(&out, b->lines[i].s, b->lines[i].len);
        /* a lone empty line is "\n" in a file that had one, nothing in an empty file */
        if (!last || (b->final_newline && !(b->empty_origin && b->nlines == 1 &&
                                            b->lines[0].len == 0)))
            sbuf_add(&out, eol, eollen);
        if (out.len >= FLUSH_AT || last) {
            if (write_all(fd, out.p, out.len) < 0) {
                snprintf(err, errlen, "%s", strerror(errno));
                goto fail;
            }
            total += (long)out.len;
            out.len = 0;
        }
    }
    if (existed && fchown(fd, st.st_uid, st.st_gid) < 0) {
        /* best effort: not fatal when we are not the owner */
    }
    if (fchmod(fd, mode) < 0 || fsync(fd) < 0) {
        snprintf(err, errlen, "%s", strerror(errno));
        goto fail;
    }
    if (close(fd) < 0) {
        fd = -1;
        snprintf(err, errlen, "%s", strerror(errno));
        goto fail;
    }
    fd = -1;
    if (rename(tmp, target) < 0) {
        snprintf(err, errlen, "%s", strerror(errno));
        goto fail;
    }

    if (path) {
        char *np = xstrdup(path);

        free(b->path);
        b->path = np;
    }
    b->modified = 0;
    b->is_new = 0;
    sbuf_free(&out);
    free(tmp);
    free(real);
    return total;

fail:
    if (fd >= 0)
        close(fd);
    if (tmp) {
        unlink(tmp);
        free(tmp);
    }
    sbuf_free(&out);
    free(real);
    return -1;
}

void buf_insert(Buffer *b, size_t y, size_t x, const char *s, size_t n)
{
    Line *l = &b->lines[y];

    if (n == 0)
        return;
    line_reserve(l, l->len + n);
    memmove(l->s + x + n, l->s + x, l->len - x);
    memcpy(l->s + x, s, n);
    l->len += n;
    b->modified = 1;
}

void buf_delete(Buffer *b, size_t y, size_t x, size_t n)
{
    Line *l = &b->lines[y];

    if (n == 0)
        return;
    memmove(l->s + x, l->s + x + n, l->len - x - n);
    l->len -= n;
    b->modified = 1;
}

void buf_split(Buffer *b, size_t y, size_t x)
{
    Line *l, *nl;
    size_t tail;

    lines_reserve(b, b->nlines + 1);
    memmove(&b->lines[y + 2], &b->lines[y + 1], (b->nlines - y - 1) * sizeof(Line));
    b->nlines++;
    l = &b->lines[y];
    nl = &b->lines[y + 1];
    tail = l->len - x;
    nl->s = NULL;
    nl->len = nl->cap = tail;
    if (tail > 0) {
        nl->s = xmalloc(tail);
        memcpy(nl->s, l->s + x, tail);
    }
    l->len = x;
    b->modified = 1;
}

void buf_join(Buffer *b, size_t y)
{
    Line *l = &b->lines[y], *next = &b->lines[y + 1];

    if (next->len > 0) {
        line_reserve(l, l->len + next->len);
        memcpy(l->s + l->len, next->s, next->len);
        l->len += next->len;
    }
    free(next->s);
    memmove(next, next + 1, (b->nlines - y - 2) * sizeof(Line));
    b->nlines--;
    b->modified = 1;
}
