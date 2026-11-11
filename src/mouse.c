#include "mouse.h"
#include "complete.h"
#include "cursors.h"
#include "edit.h"
#include "fold.h"
#include "render.h"
#include "run.h"
#include "utf8.h"

#include <string.h>
#include <time.h>

#define MULTI_CLICK_MS 400
#define WHEEL_LINES 3

static long clock_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The line shown on row `row` of a pane; below the text, the last visible line. */
static size_t line_at_row(const Buffer *b, const View *v, size_t row)
{
    size_t y = fold_nth(b, v->rowoff, row);

    return y < b->nlines ? y : fold_head(b, b->nlines - 1);
}

/* Byte offset of display column col in line y of pane w. */
static size_t byte_at(const Win *w, size_t y, size_t col)
{
    const Line *l = &w->doc->buf.lines[y];

    return utf8_byte_at_col(l->s, l->len, col, w->doc->tabw);
}

void mouse_locate(Editor *e, int x, int y, MouseHit *h)
{
    Win *w;
    int row, rel;

    memset(h, 0, sizeof *h);
    h->kind = MH_NONE;
    editor_store_view(e);

    if (e->picker.on) {
        int top = e->rows - 1 - render_picker_height(e);

        if (y >= top && y < e->rows - 1) {
            h->kind = MH_PICKER;
            h->index = (size_t)(y - top);
        }
        return;
    }
    if (y < 0 || y >= e->rows - 1 || x < 0 || x >= e->cols)
        return;
    if (e->text_y == 1 && y == 0) {
        size_t t = render_tab_at(e, x);

        if (t != (size_t)-1) {
            h->kind = MH_TAB;
            h->index = t;
        }
        return;
    }
    if (e->sidebar_w > 0 && x < e->sidebar_w) {
        h->kind = MH_SIDEBAR;
        h->index = e->tree.top + (size_t)(y - e->text_y);
        return;
    }
    w = layout_at(&e->layout, x, y);
    if (!w)
        return;
    h->win = w;
    row = y - w->y;
    if (row >= w->h - 1) {
        h->kind = MH_STATUS;
        return;
    }
    h->kind = MH_TEXT;
    h->y = line_at_row(&w->doc->buf, &w->view, (size_t)row);
    rel = x - w->x - win_gutter(w);
    h->x = byte_at(w, h->y, w->view.coloff + (size_t)(rel > 0 ? rel : 0));
}

/* Scrolls pane w by n lines (negative: up) without moving the focus; the cursor is pulled
 * back inside the window when it would be left outside. */
static void wheel(Editor *e, Win *w, int n)
{
    Buffer *b = &w->doc->buf;
    View *v = &w->view;
    size_t th = (size_t)(w->h > 1 ? w->h - 1 : 1), last;
    size_t maxoff = fold_back(b, fold_head(b, b->nlines - 1), th - 1);
    int active = w == e->layout.active;

    if (b->folded && v->rowoff < b->nlines)
        v->rowoff = fold_head(b, v->rowoff);
    if (n > 0) {
        v->rowoff = fold_advance(b, v->rowoff, (size_t)n);
        if (v->rowoff > maxoff)
            v->rowoff = maxoff;
    } else {
        v->rowoff = fold_back(b, v->rowoff, (size_t)-n);
    }
    last = fold_nth(b, v->rowoff, th - 1);
    if (last >= b->nlines)
        last = fold_head(b, b->nlines - 1);
    if (v->cy < v->rowoff || v->cy > last) {
        const Line *l;

        v->cy = v->cy < v->rowoff ? v->rowoff : last;
        l = &b->lines[v->cy];
        v->cx = utf8_byte_at_col(l->s, l->len, v->want_col, w->doc->tabw);
    }
    if (active)
        editor_reload_view(e);
}

static void settle(Editor *e)
{
    e->want_col = editor_cursor_col(e);
    e->paste_valid = 0;
    e->snip.on = 0;
    complete_close(&e->comp);
}

static void press_text(Editor *e, const MouseEv *ev, const MouseHit *h)
{
    MouseState *m = &e->mouse;
    long now = clock_ms();
    int same = now - m->t <= MULTI_CLICK_MS && ev->x == m->x && ev->y == m->y;

    m->n = same ? m->n % 3 + 1 : 1;
    m->t = now;
    m->x = ev->x;
    m->y = ev->y;
    m->drag = 0;

    if (h->win != e->layout.active)
        editor_focus(e, h->win);
    e->sidebar_focus = 0;

    if (ev->mods & MOD_ALT) {
        cur_add_at(e, h->y, h->x);
        settle(e);
        return;
    }
    cur_clear(e);
    buf_undo_close(e->buf);
    if (ev->mods & MOD_SHIFT) {
        if (!e->sel_on) {
            e->ay = e->cy;
            e->ax = e->cx;
        }
        e->sel_on = 1;
    } else {
        e->sel_on = 0;
    }
    e->cy = h->y;
    e->cx = h->x;
    if (!(ev->mods & MOD_SHIFT)) {
        e->ay = e->cy;
        e->ax = e->cx;
    }
    if (m->n == 2 && e->doc == e->run.out && run_enter(e)) {
        m->n = 0;
        return;
    }
    if (m->n == 2)
        edit_select_word(e);
    else if (m->n == 3)
        edit_select_line(e);
    else
        m->drag = 1;
    settle(e);
}

/* Extends the selection of the active pane to the cell under the pointer; a pointer outside
 * the pane moves one line (or column) past its edge, so the view follows. */
static void drag_to(Editor *e, const MouseEv *ev)
{
    Win *w = e->layout.active;
    const Buffer *b = e->buf;
    size_t th = (size_t)(w->h > 1 ? w->h - 1 : 1), y, col;
    int row = ev->y - w->y, rel = ev->x - w->x - win_gutter(w);

    if (row < 0) {
        y = fold_prev(b, e->rowoff);
    } else if ((size_t)row >= th) {
        y = fold_nth(b, e->rowoff, th - 1);
        y = y < b->nlines ? fold_next(b, y) : fold_head(b, b->nlines - 1);
    } else {
        y = line_at_row(b, &w->view, (size_t)row);
    }
    if (rel < 0)
        col = e->coloff > 0 ? e->coloff - 1 : 0;
    else
        col = e->coloff + (size_t)rel;
    e->sel_on = 1;
    e->cy = y;
    e->cx = utf8_byte_at_col(b->lines[y].s, b->lines[y].len, col, e->tabw);
    e->want_col = editor_cursor_col(e);
}

static void press_left(Editor *e, const MouseEv *ev, const MouseHit *h)
{
    Key enter = { KEY_ENTER, 0 };

    switch (h->kind) {
    case MH_PICKER: {
        Picker *p = &e->picker;
        size_t pos = p->top + h->index - 1;

        if (h->index > 0 && pos < p->nshown) {
            p->sel = pos;
            editor_key(e, enter);
        }
        break;
    }
    case MH_TAB:
        if (h->index < e->ndocs) {
            e->sidebar_focus = 0;
            editor_activate(e, e->docs[h->index]);
        }
        break;
    case MH_SIDEBAR:
        if (e->tree_ready && h->index < e->tree.n) {
            e->tree.sel = h->index;
            e->sidebar_focus = 1;
            editor_key(e, enter);
        }
        break;
    case MH_STATUS:
        if (h->win != e->layout.active)
            editor_focus(e, h->win);
        e->sidebar_focus = 0;
        break;
    case MH_TEXT:
        press_text(e, ev, h);
        break;
    case MH_NONE:
        break;
    }
}

void mouse_event(Editor *e, const MouseEv *ev)
{
    MouseHit h;

    if (!e->mouse_on || e->prompt_on || e->find.mode != FIND_OFF)
        return;
    if (ev->kind == MOUSE_RELEASE) {
        e->mouse.drag = 0;
        return;
    }
    if (ev->kind == MOUSE_DRAG) {
        if (e->mouse.drag && !e->picker.on)
            drag_to(e, ev);
        return;
    }
    mouse_locate(e, ev->x, ev->y, &h);

    if (ev->kind == MOUSE_WHEEL_UP || ev->kind == MOUSE_WHEEL_DOWN) {
        int n = ev->kind == MOUSE_WHEEL_UP ? -WHEEL_LINES : WHEEL_LINES;

        if (h.kind == MH_PICKER || e->picker.on) {
            picker_move(&e->picker, n);
        } else if (h.kind == MH_SIDEBAR && e->tree_ready && e->tree.n > 0) {
            Tree *t = &e->tree;

            if (n < 0)
                t->sel = t->sel > (size_t)-n ? t->sel + (size_t)n : 0;
            else
                t->sel = t->sel + (size_t)n < t->n ? t->sel + (size_t)n : t->n - 1;
        } else if (h.kind == MH_TEXT || h.kind == MH_STATUS) {
            wheel(e, h.win, n);
        }
        return;
    }
    if (ev->button == 0) {
        press_left(e, ev, &h);
    } else if (ev->button == 1 && h.kind == MH_TAB && h.index < e->ndocs) {
        editor_activate(e, e->docs[h.index]);
        editor_cmd_close_buffer(e);
    }
}
