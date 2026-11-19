#include "sys.h"
#include "buffer.h"
#include "i18n.h"
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
    b->stable = 0;
    b->gen++;
}

static void push_line(Buffer *b, const char *s, size_t n)
{
    Line *l;

    lines_reserve(b, b->nlines + 1);
    l = &b->lines[b->nlines++];
    l->len = l->cap = n;
    l->s = NULL;
    l->hl = 0;
    l->git = 0;
    l->fold = 0;
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
    undo_free(&b->undo);
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

/* Replaces the contents with the bytes read from fd, which stays open. */
int buf_load_fd(Buffer *b, int fd, char *err, size_t errlen)
{
    struct stat st;
    char *data;
    size_t len, nl_count = 0;
    const char *p, *end, *first_nl;

    if (fstat(fd, &st) < 0) {
        snprintf(err, errlen, "%s", strerror(errno));
        return -1;
    }
    if (read_all(fd, S_ISREG(st.st_mode) ? (size_t)st.st_size : 0, &data, &len) < 0) {
        snprintf(err, errlen, "%s", strerror(errno));
        return -1;
    }

    lines_clear(b);
    undo_free(&b->undo);
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
    b->path = NULL;
    b->modified = 0;
    b->is_new = 0;
    return 0;
}

int buf_load(Buffer *b, const char *path, char *err, size_t errlen)
{
    int fd, r;
    struct stat st;

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
        snprintf(err, errlen, "%s", S_ISDIR(st.st_mode) ? tr("is a directory")
                                                        : tr("is not a regular file"));
        close(fd);
        return -1;
    }
    r = buf_load_fd(b, fd, err, errlen);
    close(fd);
    if (r < 0)
        return -1;
    b->path = xstrdup(path);
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
        snprintf(err, errlen, tr("no file name"));
        return -1;
    }
    real = sys_realpath(want, NULL);
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
    strcpy(tmp + dlen, ".xted.XXXXXX");
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
    if (existed && sys_fchown(fd, st.st_uid, st.st_gid) < 0) {
        /* best effort: not fatal when we are not the owner */
    }
    if (sys_fchmod(fd, mode) < 0 || sys_fsync(fd) < 0) {
        snprintf(err, errlen, "%s", strerror(errno));
        goto fail;
    }
    if (close(fd) < 0) {
        fd = -1;
        snprintf(err, errlen, "%s", strerror(errno));
        goto fail;
    }
    fd = -1;
    if (sys_rename(tmp, target) < 0) {
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
    undo_close(&b->undo);
    b->undo.saved_at = b->undo.cur;
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

/* Raw mutations: no undo recording. */

static void touch(Buffer *b, size_t y)
{
    if (b->stable > y)
        b->stable = y;
    b->gen++;
    b->modified = 1;
}

static void raw_insert(Buffer *b, size_t y, size_t x, const char *s, size_t n)
{
    Line *l = &b->lines[y];

    line_reserve(l, l->len + n);
    memmove(l->s + x + n, l->s + x, l->len - x);
    memcpy(l->s + x, s, n);
    l->len += n;
    touch(b, y);
}

static void raw_delete(Buffer *b, size_t y, size_t x, size_t n)
{
    Line *l = &b->lines[y];

    memmove(l->s + x, l->s + x + n, l->len - x - n);
    l->len -= n;
    touch(b, y);
}

static void raw_split(Buffer *b, size_t y, size_t x)
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
    nl->hl = 0;
    nl->git = 0;
    nl->fold = 0;
    nl->len = nl->cap = tail;
    if (tail > 0) {
        nl->s = xmalloc(tail);
        memcpy(nl->s, l->s + x, tail);
    }
    l->len = x;
    touch(b, y);
}

static void raw_join(Buffer *b, size_t y)
{
    Line *l = &b->lines[y], *next = &b->lines[y + 1];

    if (next->len > 0) {
        line_reserve(l, l->len + next->len);
        memcpy(l->s + l->len, next->s, next->len);
        l->len += next->len;
    }
    if (l->fold == 0)
        l->fold = next->fold;
    free(next->s);
    memmove(next, next + 1, (b->nlines - y - 2) * sizeof(Line));
    b->nlines--;
    touch(b, y);
}

void buf_insert(Buffer *b, size_t y, size_t x, const char *s, size_t n)
{
    if (n == 0)
        return;
    raw_insert(b, y, x, s, n);
    undo_record(&b->undo, OP_INSERT, y, x, s, n);
}

void buf_delete(Buffer *b, size_t y, size_t x, size_t n)
{
    if (n == 0)
        return;
    undo_record(&b->undo, OP_DELETE, y, x, b->lines[y].s + x, n);
    raw_delete(b, y, x, n);
}

void buf_split(Buffer *b, size_t y, size_t x)
{
    raw_split(b, y, x);
    undo_record(&b->undo, OP_SPLIT, y, x, NULL, 0);
}

void buf_join(Buffer *b, size_t y)
{
    size_t x = b->lines[y].len;

    raw_join(b, y);
    undo_record(&b->undo, OP_JOIN, y, x, NULL, 0);
}

/* ---- undo -------------------------------------------------------------- */

void buf_undo_begin(Buffer *b, size_t cy, size_t cx)
{
    undo_begin(&b->undo, cy, cx);
}

void buf_undo_cursor(Buffer *b, size_t cy, size_t cx)
{
    undo_cursor(&b->undo, cy, cx);
}

void buf_undo_close(Buffer *b)
{
    undo_close(&b->undo);
}

static void apply_op(Buffer *b, const Op *op, int inverse)
{
    switch (op->kind) {
    case OP_INSERT:
        if (inverse)
            raw_delete(b, op->y, op->x, op->n);
        else
            raw_insert(b, op->y, op->x, op->text, op->n);
        break;
    case OP_DELETE:
        if (inverse)
            raw_insert(b, op->y, op->x, op->text, op->n);
        else
            raw_delete(b, op->y, op->x, op->n);
        break;
    case OP_SPLIT:
        if (inverse)
            raw_join(b, op->y);
        else
            raw_split(b, op->y, op->x);
        break;
    case OP_JOIN:
        if (inverse)
            raw_split(b, op->y, op->x);
        else
            raw_join(b, op->y);
        break;
    }
}

int buf_undo(Buffer *b, size_t *cy, size_t *cx)
{
    Undo *u = &b->undo;
    UndoGroup *g;
    size_t i;

    undo_close(u);
    if (u->cur == 0)
        return 0;
    g = &u->groups[--u->cur];
    for (i = g->nops; i-- > 0;)
        apply_op(b, &g->ops[i], 1);
    *cy = g->cy_before;
    *cx = g->cx_before;
    b->modified = u->cur != u->saved_at;
    return 1;
}

int buf_redo(Buffer *b, size_t *cy, size_t *cx)
{
    Undo *u = &b->undo;
    UndoGroup *g;
    size_t i;

    undo_close(u);
    if (u->cur >= u->ngroups)
        return 0;
    g = &u->groups[u->cur++];
    for (i = 0; i < g->nops; i++)
        apply_op(b, &g->ops[i], 0);
    *cy = g->cy_after;
    *cx = g->cx_after;
    b->modified = u->cur != u->saved_at;
    return 1;
}

/* ---- multi-line helpers, built on the primitives ----------------------- */

char *buf_get_range(const Buffer *b, size_t y0, size_t x0, size_t y1, size_t x1, size_t *n)
{
    SBuf out = SBUF_INIT;
    size_t y;

    for (y = y0; y <= y1; y++) {
        const Line *l = &b->lines[y];
        size_t from = y == y0 ? x0 : 0;
        size_t to = y == y1 ? x1 : l->len;

        if (to > from)
            sbuf_add(&out, l->s + from, to - from);
        if (y < y1)
            sbuf_add(&out, "\n", 1);
    }
    *n = out.len;
    return out.p ? out.p : xmalloc(1);
}

void buf_delete_range(Buffer *b, size_t y0, size_t x0, size_t y1, size_t x1)
{
    size_t y;

    if (y0 == y1) {
        buf_delete(b, y0, x0, x1 - x0);
        return;
    }
    /* empty the middle lines first, so that the joins below only splice
     * the head of the first line onto the tail of the last */
    buf_delete(b, y1, 0, x1);
    for (y = y0 + 1; y < y1; y++)
        buf_delete(b, y, 0, b->lines[y].len);
    buf_delete(b, y0, x0, b->lines[y0].len - x0);
    for (y = y0; y < y1; y++)
        buf_join(b, y0);
}

void buf_insert_text(Buffer *b, size_t *y, size_t *x, const char *s, size_t n)
{
    while (n > 0) {
        const char *nl = memchr(s, '\n', n);
        size_t seg = nl ? (size_t)(nl - s) : n;

        buf_insert(b, *y, *x, s, seg);
        *x += seg;
        s += seg;
        n -= seg;
        if (nl) {
            buf_split(b, *y, *x);
            (*y)++;
            *x = 0;
            s++;
            n--;
        }
    }
}

void buf_delete_lines(Buffer *b, size_t y0, size_t y1)
{
    size_t y, i, count = y1 - y0 + 1;

    /* empty every line, then join the empty ones away: each join removes one line */
    for (y = y0; y <= y1; y++)
        buf_delete(b, y, 0, b->lines[y].len);
    if (y1 + 1 < b->nlines) {
        for (i = 0; i < count; i++)
            buf_join(b, y0);
    } else if (y0 > 0) {
        for (i = 0; i < count; i++)
            buf_join(b, y0 - 1);
    } else {
        for (i = 1; i < count; i++)
            buf_join(b, 0);
    }
}

void buf_insert_lines(Buffer *b, size_t y, int after, const char *s, size_t n)
{
    size_t yy, xx = 0;

    if (after) {
        buf_split(b, y, b->lines[y].len);
        yy = y + 1;
    } else {
        buf_split(b, y, 0);
        yy = y;
    }
    buf_insert_text(b, &yy, &xx, s, n);
}
