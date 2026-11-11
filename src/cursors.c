#include "cursors.h"
#include "edit.h"
#include "utf8.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

void cur_clear(Editor *e)
{
    free(e->xc);
    e->xc = NULL;
    e->nxc = e->xc_cap = 0;
}

size_t cur_count(const Editor *e)
{
    return e->nxc + 1;
}

static void push(Editor *e, const Cursor *c)
{
    if (e->nxc == e->xc_cap) {
        e->xc_cap = e->xc_cap ? e->xc_cap * 2 : 8;
        e->xc = xrealloc(e->xc, e->xc_cap * sizeof *e->xc);
    }
    e->xc[e->nxc++] = *c;
}

static Cursor primary(const Editor *e)
{
    Cursor c;

    c.cy = e->cy;
    c.cx = e->cx;
    c.want_col = e->want_col;
    c.sel_on = e->sel_on;
    c.ay = e->ay;
    c.ax = e->ax;
    return c;
}

static void load(Editor *e, const Cursor *c)
{
    e->cy = c->cy;
    e->cx = c->cx;
    e->want_col = c->want_col;
    e->sel_on = c->sel_on;
    e->ay = c->ay;
    e->ax = c->ax;
}

static int cursor_cmp(const void *pa, const void *pb)
{
    const Cursor *a = pa, *b = pb;

    if (a->cy != b->cy)
        return a->cy < b->cy ? -1 : 1;
    if (a->cx != b->cx)
        return a->cx < b->cx ? -1 : 1;
    return 0;
}

/* ---- running a command on every cursor ---------------------------------- */

/* Positions are kept as distances from the end of the text while a command
 * runs: an edit before a cursor then leaves them valid. */
typedef struct {
    Cursor c;
    int prim;
} Slot;

static void to_end(const Buffer *b, size_t *y, size_t *x)
{
    size_t len = b->lines[*y].len;

    *x = len - (*x < len ? *x : len);
    *y = b->nlines - 1 - *y;
}

static void from_end(const Buffer *b, size_t *y, size_t *x)
{
    size_t yy = b->nlines - 1 - (*y < b->nlines ? *y : b->nlines - 1);
    size_t len = b->lines[yy].len;

    *x = len - (*x < len ? *x : len);
    *y = yy;
}

static int slot_cmp(const void *pa, const void *pb)
{
    return cursor_cmp(&((const Slot *)pa)->c, &((const Slot *)pb)->c);
}

void cur_each(Editor *e, void (*fn)(Editor *e, void *arg, size_t idx), void *arg)
{
    size_t n = e->nxc + 1, i, w;
    Slot *s = xmalloc(n * sizeof *s);
    Cursor *keep = e->xc;
    size_t keep_n = e->nxc, keep_cap = e->xc_cap;
    size_t py, px;

    s[0].c = primary(e);
    s[0].prim = 1;
    for (i = 0; i < e->nxc; i++) {
        s[i + 1].c = e->xc[i];
        s[i + 1].prim = 0;
    }
    qsort(s, n, sizeof *s, slot_cmp);

    /* the cursor to restore on undo is the primary's start */
    py = e->cy;
    px = e->cx;
    buf_undo_begin(e->buf, py, px);
    e->undo_hold++;
    e->xc = NULL;
    e->nxc = e->xc_cap = 0;
    for (i = n; i-- > 0;) {
        load(e, &s[i].c);
        fn(e, arg, i);
        s[i].c = primary(e);
        to_end(e->buf, &s[i].c.cy, &s[i].c.cx);
        if (s[i].c.sel_on) {
            size_t ay = s[i].c.ay, ax = s[i].c.ax;

            if (ay >= e->buf->nlines)
                ay = e->buf->nlines - 1;
            to_end(e->buf, &ay, &ax);
            s[i].c.ay = ay;
            s[i].c.ax = ax;
        }
    }
    e->undo_hold--;
    free(keep);
    (void)keep_n;
    (void)keep_cap;

    for (i = 0; i < n; i++) {
        from_end(e->buf, &s[i].c.cy, &s[i].c.cx);
        if (s[i].c.sel_on) {
            from_end(e->buf, &s[i].c.ay, &s[i].c.ax);
            if (s[i].c.ay == s[i].c.cy && s[i].c.ax == s[i].c.cx)
                s[i].c.sel_on = 0;
        }
    }
    qsort(s, n, sizeof *s, slot_cmp);
    /* identical cursors merge, the primary one surviving */
    for (i = 1, w = 1; i < n; i++) {
        if (cursor_cmp(&s[i].c, &s[w - 1].c) == 0) {
            if (s[i].prim)
                s[w - 1] = s[i];
            continue;
        }
        s[w++] = s[i];
    }
    n = w;
    for (i = 0; i < n; i++)
        if (s[i].prim)
            break;
    if (i == n)
        i = 0;
    load(e, &s[i].c);
    for (w = 0; w < n; w++)
        if (w != i)
            push(e, &s[w].c);
    buf_undo_cursor(e->buf, e->cy, e->cx);
    free(s);
}

/* ---- adding cursors ------------------------------------------------------ */

/* Is the single-line text sel[0..n) at (y,x) already covered by a cursor? */
static int taken(const Editor *e, size_t y, size_t x)
{
    size_t i;

    for (i = 0; i < e->nxc; i++) {
        const Cursor *c = &e->xc[i];

        if (c->sel_on && c->cy == y && (c->ax == x || c->cx == x))
            return 1;
    }
    return 0;
}

/* Selection of the primary cursor if it lies on one line; returns its bytes. */
static int one_line_sel(const Editor *e, size_t *y, size_t *x0, size_t *x1)
{
    if (!edit_sel_active(e) || e->ay != e->cy)
        return 0;
    *y = e->cy;
    *x0 = e->ax < e->cx ? e->ax : e->cx;
    *x1 = e->ax < e->cx ? e->cx : e->ax;
    return 1;
}

/* Finds the next occurrence after the primary selection, wrapping. Returns
 * 0 when there is none that is not already a cursor. */
static int add_one(Editor *e)
{
    size_t y, x0, x1, n, k, cur;
    const Buffer *b = e->buf;
    char *needle;

    if (!one_line_sel(e, &y, &x0, &x1)) {
        edit_select_word(e);
        if (!one_line_sel(e, &y, &x0, &x1)) {
            if (edit_sel_active(e))
                editor_set_msg(e, MSG_TTL_MS, "La selezione deve stare su una riga");
            else
                editor_set_msg(e, MSG_TTL_MS, "Nessuna parola sotto il cursore");
            return -1;
        }
        return 2;       /* the first press only selects the word */
    }
    n = x1 - x0;
    needle = xmalloc(n);
    memcpy(needle, b->lines[y].s + x0, n);
    for (k = 0, cur = y; k <= b->nlines; k++, cur = (cur + 1) % b->nlines) {
        const Line *l = &b->lines[cur];
        size_t from = (k == 0) ? x1 : 0, i;

        for (i = from; i + n <= l->len; i++) {
            if (memcmp(l->s + i, needle, n) != 0)
                continue;
            if (taken(e, cur, i) || (cur == y && i == x0))
                continue;
            {
                Cursor old = primary(e);

                push(e, &old);
                e->sel_on = 1;
                e->ay = cur;
                e->ax = i;
                e->cy = cur;
                e->cx = i + n;
                e->want_col = editor_cursor_col(e);
                free(needle);
                return 1;
            }
        }
    }
    free(needle);
    return 0;
}

void cur_add_next(Editor *e)
{
    int r = add_one(e);

    if (r == 0)
        editor_set_msg(e, MSG_TTL_MS, "Nessun'altra occorrenza");
    else if (r == 1)
        editor_set_msg(e, MSG_TTL_MS, "%zu cursori", cur_count(e));
}

void cur_add_all(Editor *e)
{
    size_t guard = 0;
    int r;

    while ((r = add_one(e)) >= 1 && guard++ < 100000)
        ;
    if (r < 0)
        return;
    editor_set_msg(e, MSG_TTL_MS, "%zu cursori", cur_count(e));
}

void cur_add_vertical(Editor *e, int down)
{
    Cursor old = primary(e), c;
    const Line *l;
    size_t y;

    if (down ? e->cy + 1 >= e->buf->nlines : e->cy == 0)
        return;
    y = down ? e->cy + 1 : e->cy - 1;
    l = &e->buf->lines[y];
    push(e, &old);
    c = old;
    c.cy = y;
    c.cx = utf8_byte_at_col(l->s, l->len, old.want_col, e->tabw);
    c.sel_on = 0;
    load(e, &c);
    e->want_col = old.want_col;
    /* a cursor that lands on an existing one is not added twice */
    {
        size_t i;

        for (i = 0; i + 1 < e->nxc; i++)
            if (e->xc[i].cy == e->cy && e->xc[i].cx == e->cx) {
                Cursor back = e->xc[e->nxc - 1];

                e->nxc--;
                load(e, &back);
                return;
            }
    }
}

void cur_add_at(Editor *e, size_t y, size_t x)
{
    Cursor old = primary(e), c;
    const Line *l;
    size_t i;

    if (y >= e->buf->nlines || x > e->buf->lines[y].len)
        return;
    if (y == e->cy && x == e->cx)
        return;
    for (i = 0; i < e->nxc; i++)
        if (e->xc[i].cy == y && e->xc[i].cx == x)
            return;
    l = &e->buf->lines[y];
    push(e, &old);
    c = old;
    c.cy = y;
    c.cx = x;
    c.sel_on = 0;
    c.want_col = utf8_cols(l->s, l->len, x, e->tabw);
    load(e, &c);
}

char *cur_selected_text(const Editor *e, size_t *n)
{
    Cursor *all = xmalloc((e->nxc + 1) * sizeof *all);
    size_t cnt = e->nxc + 1, i;
    SBuf out = SBUF_INIT;
    int any = 0;

    all[0] = primary(e);
    for (i = 0; i < e->nxc; i++)
        all[i + 1] = e->xc[i];
    qsort(all, cnt, sizeof *all, cursor_cmp);
    for (i = 0; i < cnt; i++) {
        size_t y0, x0, y1, x1, len;
        char *t;

        if (!all[i].sel_on || (all[i].ay == all[i].cy && all[i].ax == all[i].cx))
            continue;
        if (all[i].ay < all[i].cy || (all[i].ay == all[i].cy && all[i].ax <= all[i].cx)) {
            y0 = all[i].ay; x0 = all[i].ax; y1 = all[i].cy; x1 = all[i].cx;
        } else {
            y0 = all[i].cy; x0 = all[i].cx; y1 = all[i].ay; x1 = all[i].ax;
        }
        t = buf_get_range(e->buf, y0, x0, y1, x1, &len);
        if (any)
            sbuf_add(&out, "\n", 1);
        sbuf_add(&out, t, len);
        free(t);
        any = 1;
    }
    free(all);
    if (!any) {
        sbuf_free(&out);
        return NULL;
    }
    *n = out.len;
    return sbuf_steal(&out);
}
