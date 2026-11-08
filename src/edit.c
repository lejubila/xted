#include "edit.h"
#include "utf8.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

static Line *cur_line(Editor *e)
{
    return &e->buf.lines[e->cy];
}

static int is_blank(char c)
{
    return c == ' ' || c == '\t';
}

static int word_at(const Line *l, size_t i)
{
    uint32_t cp;

    utf8_decode(l->s + i, l->len - i, &cp);
    return cp_is_word(cp);
}

/* Undo grouping for a command: begin remembers the cursor to restore (the
 * start of the selection, if any), done records where the cursor ended up,
 * finish also closes the group. */
static void begin(Editor *e)
{
    size_t y = e->cy, x = e->cx, y1, x1;

    if (edit_sel_active(e))
        edit_sel_range(e, &y, &x, &y1, &x1);
    buf_undo_begin(&e->buf, y, x);
}

static void done(Editor *e)
{
    buf_undo_cursor(&e->buf, e->cy, e->cx);
}

static void finish(Editor *e)
{
    done(e);
    buf_undo_close(&e->buf);
}

/* ---- selection --------------------------------------------------------- */

int edit_sel_active(const Editor *e)
{
    return e->sel_on && (e->ay != e->cy || e->ax != e->cx);
}

void edit_sel_range(const Editor *e, size_t *y0, size_t *x0, size_t *y1, size_t *x1)
{
    if (e->ay < e->cy || (e->ay == e->cy && e->ax <= e->cx)) {
        *y0 = e->ay;
        *x0 = e->ax;
        *y1 = e->cy;
        *x1 = e->cx;
    } else {
        *y0 = e->cy;
        *x0 = e->cx;
        *y1 = e->ay;
        *x1 = e->ax;
    }
}

int edit_sel_line(const Editor *e, size_t y, size_t *from, size_t *to, int *nl)
{
    size_t y0, x0, y1, x1;

    if (!edit_sel_active(e))
        return 0;
    edit_sel_range(e, &y0, &x0, &y1, &x1);
    if (y < y0 || y > y1)
        return 0;
    *from = y == y0 ? x0 : 0;
    *to = y == y1 ? x1 : e->buf.lines[y].len;
    *nl = y < y1;
    return 1;
}

void edit_sel_collapse(Editor *e, int to_end)
{
    size_t y0, x0, y1, x1;

    if (edit_sel_active(e)) {
        edit_sel_range(e, &y0, &x0, &y1, &x1);
        e->cy = to_end ? y1 : y0;
        e->cx = to_end ? x1 : x0;
    }
    e->sel_on = 0;
}

void edit_select_all(Editor *e)
{
    e->sel_on = 1;
    e->ay = 0;
    e->ax = 0;
    e->cy = e->buf.nlines - 1;
    e->cx = cur_line(e)->len;
}

void edit_select_line(Editor *e)
{
    e->sel_on = 1;
    e->ay = e->cy;
    e->ax = 0;
    if (e->cy + 1 < e->buf.nlines) {
        e->cy++;
        e->cx = 0;
    } else {
        e->cx = cur_line(e)->len;
    }
}

void edit_select_word(Editor *e)
{
    const Line *l = cur_line(e);
    size_t start = e->cx, end = e->cx;

    if (e->cx < l->len && word_at(l, e->cx)) {
        /* on a word */
    } else if (e->cx > 0 && word_at(l, utf8_prev(l->s, e->cx))) {
        /* just after one */
    } else {
        return;
    }
    while (start > 0 && word_at(l, utf8_prev(l->s, start)))
        start = utf8_prev(l->s, start);
    while (end < l->len && word_at(l, end))
        end = utf8_next(l->s, l->len, end);
    e->sel_on = 1;
    e->ay = e->cy;
    e->ax = start;
    e->cx = end;
}

/* Deletes the selection, if any; returns whether there was one. */
static int delete_selection(Editor *e)
{
    size_t y0, x0, y1, x1;

    if (!edit_sel_active(e)) {
        e->sel_on = 0;
        return 0;
    }
    edit_sel_range(e, &y0, &x0, &y1, &x1);
    buf_delete_range(&e->buf, y0, x0, y1, x1);
    e->cy = y0;
    e->cx = x0;
    e->sel_on = 0;
    return 1;
}

/* First and last line touched by the selection. A selection that ends at
 * column 0 of a later line does not include that line. */
static void sel_lines(const Editor *e, size_t *a, size_t *b)
{
    size_t y0, x0, y1, x1;

    if (!edit_sel_active(e)) {
        *a = *b = e->cy;
        return;
    }
    edit_sel_range(e, &y0, &x0, &y1, &x1);
    *a = y0;
    *b = (y1 > y0 && x1 == 0) ? y1 - 1 : y1;
}

/* ---- typing ------------------------------------------------------------ */

void edit_insert_cp(Editor *e, uint32_t cp)
{
    char b[4];
    int n = utf8_encode(cp, b);

    begin(e);
    delete_selection(e);
    buf_insert(&e->buf, e->cy, e->cx, b, (size_t)n);
    e->cx += (size_t)n;
    done(e);
}

void edit_newline(Editor *e)
{
    size_t n = 0;
    const Line *l;

    begin(e);
    delete_selection(e);
    l = cur_line(e);
    while (n < l->len && n < e->cx && is_blank(l->s[n]))
        n++;
    buf_split(&e->buf, e->cy, e->cx);
    e->cy++;
    e->cx = 0;
    if (n > 0) {
        /* the head of the old line keeps the indentation we copy from */
        const Line *prev = &e->buf.lines[e->cy - 1];

        buf_insert(&e->buf, e->cy, 0, prev->s, n);
        e->cx = n;
    }
    finish(e);
}

void edit_backspace(Editor *e)
{
    begin(e);
    if (!delete_selection(e)) {
        if (e->cx > 0) {
            const Line *l = cur_line(e);
            size_t from = grapheme_prev(l->s, l->len, e->cx);

            buf_delete(&e->buf, e->cy, from, e->cx - from);
            e->cx = from;
        } else if (e->cy > 0) {
            e->cy--;
            e->cx = cur_line(e)->len;
            buf_join(&e->buf, e->cy);
        }
    }
    done(e);
}

void edit_delete(Editor *e)
{
    begin(e);
    if (!delete_selection(e)) {
        const Line *l = cur_line(e);

        if (e->cx < l->len) {
            size_t to = grapheme_next(l->s, l->len, e->cx);

            buf_delete(&e->buf, e->cy, e->cx, to - e->cx);
        } else if (e->cy + 1 < e->buf.nlines) {
            buf_join(&e->buf, e->cy);
        }
    }
    done(e);
}

void edit_tab(Editor *e)
{
    size_t a, b, k;

    if (!edit_sel_active(e)) {
        edit_insert_cp(e, '\t');
        return;
    }
    sel_lines(e, &a, &b);
    begin(e);
    for (k = a; k <= b; k++)
        buf_insert(&e->buf, k, 0, "\t", 1);
    /* keep the selection on the same text; an end at column 0 stays there */
    if (e->ay >= a && e->ay <= b && e->ax > 0)
        e->ax++;
    if (e->cy >= a && e->cy <= b && e->cx > 0)
        e->cx++;
    finish(e);
}

void edit_dedent(Editor *e)
{
    size_t a, b, k;

    sel_lines(e, &a, &b);
    begin(e);
    for (k = a; k <= b; k++) {
        const Line *l = &e->buf.lines[k];
        size_t r = 0;

        if (l->len > 0 && l->s[0] == '\t')
            r = 1;
        else
            while (r < l->len && r < (size_t)e->tabw && l->s[r] == ' ')
                r++;
        if (r == 0)
            continue;
        buf_delete(&e->buf, k, 0, r);
        if (e->cy == k)
            e->cx = e->cx > r ? e->cx - r : 0;
        if (e->sel_on && e->ay == k)
            e->ax = e->ax > r ? e->ax - r : 0;
    }
    finish(e);
}

/* ---- clipboard --------------------------------------------------------- */

/* Takes ownership of text. */
static void store_clip(Editor *e, char *text, size_t n, int whole, const char *verb)
{
    int too_big = 0;

    if (e->clipboard_osc52) {
        SBuf osc = SBUF_INIT;

        if (clip_osc52(&osc, text, n) == 0)
            term_write(osc.p, osc.len);
        else
            too_big = 1;
        sbuf_free(&osc);
    }
    clip_push(&e->clip, text, n, whole);
    editor_set_msg(e, MSG_TTL_MS, "%s: %zu byte%s", verb, n,
                   too_big ? " (troppo grande per gli appunti di sistema)" : "");
}

static void copy_to_clip(Editor *e, const char *verb)
{
    char *text;
    size_t n;

    if (edit_sel_active(e)) {
        size_t y0, x0, y1, x1;

        edit_sel_range(e, &y0, &x0, &y1, &x1);
        text = buf_get_range(&e->buf, y0, x0, y1, x1, &n);
        store_clip(e, text, n, 0, verb);
    } else {
        const Line *l = cur_line(e);

        n = l->len + 1;
        text = xmalloc(n);
        if (l->len > 0)
            memcpy(text, l->s, l->len);
        text[l->len] = '\n';
        store_clip(e, text, n, 1, verb);
    }
}

/* Puts the cursor on line y, clamped, at the column the user was aiming for. */
static void settle_cursor(Editor *e, size_t y)
{
    const Line *l;

    if (y >= e->buf.nlines)
        y = e->buf.nlines - 1;
    e->cy = y;
    l = cur_line(e);
    e->cx = utf8_byte_at_col(l->s, l->len, e->want_col, e->tabw);
}

void edit_copy(Editor *e)
{
    copy_to_clip(e, "Copiato");
}

void edit_cut(Editor *e)
{
    int had_sel = edit_sel_active(e);

    copy_to_clip(e, "Tagliato");
    begin(e);
    if (had_sel) {
        delete_selection(e);
    } else {
        buf_delete_lines(&e->buf, e->cy, e->cy);
        e->sel_on = 0;
        settle_cursor(e, e->cy);
    }
    finish(e);
}

/* Inserts clip entry c at the cursor and remembers where it went. */
static void insert_entry(Editor *e, const ClipEntry *c, size_t idx)
{
    size_t y = e->cy, x = e->cx;

    e->paste_y0 = y;
    e->paste_x0 = x;
    buf_insert_text(&e->buf, &y, &x, c->text, c->n);
    e->cy = e->paste_y1 = y;
    e->cx = e->paste_x1 = x;
    e->paste_idx = idx;
    e->paste_valid = 1;
}

void edit_paste(Editor *e)
{
    const ClipEntry *c = clip_get(&e->clip, 0);
    int had_sel;

    if (!c) {
        editor_set_msg(e, MSG_TTL_MS, "Appunti vuoti");
        return;
    }
    begin(e);
    had_sel = delete_selection(e);
    if (c->whole_line && !had_sel)
        e->cx = 0;
    insert_entry(e, c, 0);
    finish(e);
}

void edit_paste_older(Editor *e)
{
    size_t idx;
    const ClipEntry *c;

    if (!e->paste_valid) {
        editor_set_msg(e, MSG_TTL_MS, "Alt+V sostituisce l'ultimo incolla");
        return;
    }
    if (e->clip.count < 2) {
        editor_set_msg(e, MSG_TTL_MS, "Nessuna voce precedente negli appunti");
        e->paste_valid = 1;
        return;
    }
    idx = e->paste_idx + 1 < e->clip.count ? e->paste_idx + 1 : 0;
    c = clip_get(&e->clip, idx);

    begin(e);
    buf_delete_range(&e->buf, e->paste_y0, e->paste_x0, e->paste_y1, e->paste_x1);
    e->cy = e->paste_y0;
    e->cx = e->paste_x0;
    insert_entry(e, c, idx);
    finish(e);
}

void edit_paste_text(Editor *e, const char *s, size_t n)
{
    SBuf clean = SBUF_INIT;
    size_t i, y, x;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == '\r') {
            sbuf_add(&clean, "\n", 1);
            if (i + 1 < n && s[i + 1] == '\n')
                i++;
        } else if (c == '\n' || c == '\t' || (c >= 0x20 && c != 0x7F)) {
            sbuf_add(&clean, s + i, 1);
        }
    }
    if (clean.len == 0) {
        sbuf_free(&clean);
        return;
    }
    begin(e);
    delete_selection(e);
    y = e->cy;
    x = e->cx;
    buf_insert_text(&e->buf, &y, &x, clean.p, clean.len);
    e->cy = y;
    e->cx = x;
    finish(e);
    sbuf_free(&clean);
}

/* ---- undo -------------------------------------------------------------- */

void edit_undo(Editor *e)
{
    size_t y, x;

    e->sel_on = 0;
    if (!buf_undo(&e->buf, &y, &x)) {
        editor_set_msg(e, MSG_TTL_MS, "Niente da annullare");
        return;
    }
    e->cy = y;
    e->cx = x;
}

void edit_redo(Editor *e)
{
    size_t y, x;

    e->sel_on = 0;
    if (!buf_redo(&e->buf, &y, &x)) {
        editor_set_msg(e, MSG_TTL_MS, "Niente da ripetere");
        return;
    }
    e->cy = y;
    e->cx = x;
}

/* ---- lines ------------------------------------------------------------- */

void edit_move_lines(Editor *e, int down)
{
    size_t a, b, ny, n;
    char *text;
    const Line *l;

    sel_lines(e, &a, &b);
    if (down ? b + 1 >= e->buf.nlines : a == 0)
        return;

    /* the line the block jumps over moves to the other side of it */
    begin(e);
    ny = down ? b + 1 : a - 1;
    l = &e->buf.lines[ny];
    n = l->len;
    text = xmalloc(n + 1);
    if (n > 0)
        memcpy(text, l->s, n);
    buf_delete_lines(&e->buf, ny, ny);
    if (down)
        buf_insert_lines(&e->buf, a, 0, text, n);
    else
        buf_insert_lines(&e->buf, b - 1, 1, text, n);
    free(text);

    if (down) {
        e->cy++;
        e->ay++;
    } else {
        e->cy--;
        e->ay--;
    }
    finish(e);
}

void edit_duplicate_lines(Editor *e, int down)
{
    size_t a, b, n;
    char *text;

    sel_lines(e, &a, &b);
    text = buf_get_range(&e->buf, a, 0, b, e->buf.lines[b].len, &n);
    begin(e);
    if (down) {
        buf_insert_lines(&e->buf, b, 1, text, n);
        e->cy += b - a + 1;
        e->ay += b - a + 1;
    } else {
        buf_insert_lines(&e->buf, a, 0, text, n);
    }
    free(text);
    finish(e);
}

void edit_delete_lines(Editor *e)
{
    size_t a, b;

    sel_lines(e, &a, &b);
    begin(e);
    buf_delete_lines(&e->buf, a, b);
    e->sel_on = 0;
    settle_cursor(e, a);
    finish(e);
}

void edit_join_lines(Editor *e)
{
    size_t y0, x0, y1, x1, count, k, y;

    if (edit_sel_active(e)) {
        edit_sel_range(e, &y0, &x0, &y1, &x1);
    } else {
        y0 = y1 = e->cy;
    }
    count = y1 > y0 ? y1 - y0 : 1;
    if (y0 + count >= e->buf.nlines)
        count = e->buf.nlines - 1 - y0;
    if (count == 0)
        return;

    begin(e);
    y = y0;
    for (k = 0; k < count; k++) {
        const Line *l, *next = &e->buf.lines[y + 1];
        size_t lead = 0;

        while (lead < next->len && is_blank(next->s[lead]))
            lead++;
        buf_delete(&e->buf, y + 1, 0, lead);
        l = &e->buf.lines[y];
        next = &e->buf.lines[y + 1];
        e->cx = l->len;
        if (l->len > 0 && next->len > 0 && !is_blank(l->s[l->len - 1]))
            buf_insert(&e->buf, y, l->len, " ", 1);
        buf_join(&e->buf, y);
    }
    e->cy = y;
    e->sel_on = 0;
    finish(e);
}
