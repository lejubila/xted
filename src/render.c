#include "render.h"
#include "doc.h"
#include "edit.h"
#include "editor.h"
#include "term.h"
#include "theme.h"
#include "utf8.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Render {
    char **prev;
    size_t *prevlen;
    int nrows;
    int ncols;
    int cur_r, cur_c;
    int need_clear;
    SBuf frame, row;
};

Render *render_create(void)
{
    Render *r = xmalloc(sizeof *r);

    memset(r, 0, sizeof *r);
    r->need_clear = 1;
    return r;
}

static void drop_rows(Render *r)
{
    int i;

    for (i = 0; i < r->nrows; i++)
        free(r->prev[i]);
    free(r->prev);
    free(r->prevlen);
    r->prev = NULL;
    r->prevlen = NULL;
    r->nrows = 0;
}

void render_free(Render *r)
{
    drop_rows(r);
    sbuf_free(&r->frame);
    sbuf_free(&r->row);
    free(r);
}

void render_invalidate(Render *r)
{
    drop_rows(r);
    r->need_clear = 1;
    r->cur_r = r->cur_c = 0;
}

static void emit_cp(SBuf *o, uint32_t cp)
{
    char b[4];
    int n = utf8_encode(cp, b);

    sbuf_add(o, b, (size_t)n);
}

static void spaces(SBuf *o, size_t n)
{
    while (n-- > 0)
        sbuf_add(o, " ", 1);
}

/* Appends s limited to maxcols cells, replacing control characters with
 * '?' so that file names and messages cannot inject escape sequences.
 * Returns the cells used. */
static size_t fit_cols(SBuf *o, const char *s, size_t len, size_t maxcols)
{
    size_t i = 0, used = 0;

    while (i < len) {
        uint32_t cp;
        int n = utf8_decode(s + i, len - i, &cp);
        int w;

        if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
            cp = '?';
        w = cp_width(cp);
        if (used + (size_t)w > maxcols)
            break;
        emit_cp(o, cp);
        used += (size_t)w;
        i += (size_t)n;
    }
    return used;
}

/* Like fit_cols, but when s does not fit it keeps the end, after a "…":
 * in a path the file name matters more than the leading directories. */
static size_t fit_tail(SBuf *o, const char *s, size_t len, size_t maxcols)
{
    size_t i = 0, start = 0, width = 0;

    while (i < len) {
        uint32_t cp;

        i += (size_t)utf8_decode(s + i, len - i, &cp);
        width += (size_t)((cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) ? 1 : cp_width(cp));
    }
    if (width <= maxcols || maxcols == 0)
        return fit_cols(o, s, len, maxcols);

    while (width > maxcols - 1 && start < len) {
        uint32_t cp;

        start += (size_t)utf8_decode(s + start, len - start, &cp);
        width -= (size_t)((cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) ? 1 : cp_width(cp));
    }
    emit_cp(o, 0x2026);
    return 1 + fit_cols(o, s + start, len - start, maxcols - 1);
}

/* Turns reverse video on or off, only emitting it on a change. */
static void set_hl(SBuf *o, int *hl, int on)
{
    if (on == *hl)
        return;
    sbuf_adds(o, on ? "\x1b[7m" : "\x1b[27m");
    *hl = on;
}

/* Selects the foreground colour of a syntax class, only emitting it on a change. */
static void set_cls(SBuf *o, const Editor *e, int *cur, int cls)
{
    if (cls == *cur)
        return;
    theme_fg(o, e->color_depth, e->theme->cls[cls]);
    *cur = cls;
}

/* Bold + underline marks a bracket and its partner, without needing colours. */
static void set_br(SBuf *o, int *br, int on)
{
    if (on == *br)
        return;
    sbuf_adds(o, on ? "\x1b[1;4m" : "\x1b[22;24m");
    *br = on;
}

/* Search matches: the theme's background, or an underline without colours. */
static void set_mt(SBuf *o, const Editor *e, int *mt, int on)
{
    if (on == *mt)
        return;
    if (e->color_depth != COLOR_NONE) {
        if (on)
            theme_bg(o, e->color_depth, e->theme->match);
        else if (e->theme->has_bg)
            theme_bg(o, e->color_depth, e->theme->bg);
        else
            sbuf_adds(o, "\x1b[49m");
    } else {
        sbuf_adds(o, on ? "\x1b[4m" : "\x1b[24m");
    }
    *mt = on;
}

/* One line of a pane's text, exactly w->w cells wide. */
static void text_row(SBuf *o, const Editor *e, const Win *w, size_t y, int active)
{
    const Doc *d = w->doc;
    const View *v = &w->view;
    const Line *l;
    const uint8_t *cls;
    size_t left = v->coloff, right = left + (size_t)w->w;
    size_t i = 0, col = 0, sel_from = 0, sel_to = 0;
    int colors, cur = SC_NORMAL, br = 0, prev_visible = 0, hl = 0, sel_nl = 0, has_sel;
    Matcher *hm = e->find.hl && e->find.m && e->find.m->valid ? e->find.m : NULL;
    size_t mr[32][2], nm = 0, mk = 0;
    int mt = 0;
    int bg_row = e->color_depth != COLOR_NONE && e->theme->has_bg;

    if (y >= d->buf.nlines) {
        if (bg_row)
            theme_bg(o, e->color_depth, e->theme->bg);
        sbuf_adds(o, "\x1b[2m~\x1b[22m");
        spaces(o, (size_t)w->w - 1);
        if (bg_row)
            sbuf_adds(o, "\x1b[0m");
        return;
    }
    l = &d->buf.lines[y];
    colors = e->color_depth != COLOR_NONE && (d->syn || e->theme->has_bg);
    if (colors) {
        theme_fg(o, e->color_depth, e->theme->fg);
        if (e->theme->has_bg)
            theme_bg(o, e->color_depth, e->theme->bg);
    }
    cls = colors ? syn_classes(d->syn, &d->buf, y) : NULL;
    has_sel = active && edit_view_sel_line(&d->buf, v, y, &sel_from, &sel_to, &sel_nl);
    if (hm)
        nm = sm_line_matches(hm, l->s, l->len, mr, 32);

    while (i < l->len && col < right) {
        uint32_t cp;
        int n = utf8_decode(l->s + i, l->len - i, &cp);
        size_t cw = (size_t)char_cols(cp, col, e->tabw);
        size_t start = col, stop = col + cw;
        int selected = has_sel && i >= sel_from && i < sel_to;
        int mark = active && e->match_on &&
                   ((y == e->by && i == e->bx) || (y == e->my && i == e->mx));
        int cc = cls ? cls[i] : SC_NORMAL;
        int inm;

        while (mk < nm && mr[mk][1] <= i)
            mk++;
        inm = mk < nm && i >= mr[mk][0];
        i += (size_t)n;
        if (cw == 0) {
            /* a combining mark belongs to the character before it */
            if (prev_visible)
                emit_cp(o, cp);
            continue;
        }
        col = stop;
        if (stop <= left) {
            prev_visible = 0;
            continue;
        }
        set_hl(o, &hl, selected);
        set_mt(o, e, &mt, inm && !selected);
        set_br(o, &br, mark);
        if (colors)
            set_cls(o, e, &cur, cc);
        if (start >= left && stop <= right) {
            if (cp == '\t') {
                spaces(o, cw);
            } else if (cw == 2 && (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))) {
                /* ^X is drawn in reverse video; inside a selection that is already on */
                sbuf_adds(o, hl ? "^" : "\x1b[7m^");
                emit_cp(o, cp < 0x20 ? cp + 64 : '?');
                if (!hl)
                    sbuf_adds(o, "\x1b[27m");
            } else {
                emit_cp(o, cp);
            }
            prev_visible = 1;
        } else {
            /* wide character or tab cut by the window edge */
            size_t vs = start > left ? start : left;
            size_t vt = stop < right ? stop : right;

            spaces(o, vt - vs);
            prev_visible = 0;
        }
    }
    set_hl(o, &hl, 0);
    set_mt(o, e, &mt, 0);
    set_br(o, &br, 0);
    /* a selection running past the end of the line shows the newline as one cell */
    if (has_sel && sel_nl && col >= left && col < right) {
        sbuf_adds(o, "\x1b[7m \x1b[27m");
        col++;
    }
    if (col < left)
        col = left;
    if (col < right)
        spaces(o, right - col);
    if (colors)
        sbuf_adds(o, "\x1b[0m");
}

/* Display name of the tab of document i: the file name, with its folder when
 * two open files share it. */
static void tab_label(const Editor *e, size_t i, char *out, size_t outsz)
{
    const Doc *d = e->docs[i];
    const char *base = doc_base_name(d), *p = d->buf.path;
    size_t k;
    int dup = 0;

    for (k = 0; k < e->ndocs; k++)
        if (k != i && strcmp(doc_base_name(e->docs[k]), base) == 0)
            dup = 1;
    if (dup && p) {
        const char *slash = base - 1;

        if (slash > p) {
            const char *prev = slash - 1;

            while (prev > p && prev[-1] != '/')
                prev--;
            base = prev;
        }
    }
    snprintf(out, outsz, "%s%s", base, d->buf.modified ? "*" : "");
}

static void tab_row(SBuf *o, const Editor *e)
{
    size_t cols = (size_t)e->cols, used = 0, i, start = 0, act = 0;
    size_t *wd = xmalloc(e->ndocs * sizeof *wd);
    char label[300];

    for (i = 0; i < e->ndocs; i++) {
        size_t n;

        tab_label(e, i, label, sizeof label);
        n = utf8_cols(label, strlen(label), strlen(label), 8);
        wd[i] = (n > 24 ? 24 : n) + 2;
        if (e->docs[i] == e->doc)
            act = i;
    }
    /* scroll the bar so that the active tab is the last one that has to fit */
    for (;;) {
        size_t sum = 0;

        for (i = start; i <= act; i++)
            sum += wd[i];
        if (sum <= cols || start >= act)
            break;
        start++;
    }
    sbuf_adds(o, "\x1b[7m");
    for (i = start; i < e->ndocs && used < cols; i++) {
        int on = e->docs[i] == e->doc;

        tab_label(e, i, label, sizeof label);
        sbuf_adds(o, on ? "\x1b[27;1m" : "\x1b[22;7m");
        used += 1;
        sbuf_adds(o, " ");
        used += fit_cols(o, label, strlen(label), wd[i] - 2 < cols - used ? wd[i] - 2 : cols - used);
        if (used < cols) {
            sbuf_adds(o, " ");
            used++;
        }
    }
    sbuf_adds(o, "\x1b[22;7m");
    spaces(o, cols - used);
    sbuf_adds(o, "\x1b[0m");
    free(wd);
}

static void status_row(SBuf *o, const Editor *e, const Win *w, int active)
{
    const Doc *d = w->doc;
    SBuf right = SBUF_INIT;
    const char *name = d->buf.path ? d->buf.path : "[Senza nome]";
    char mark[16];
    size_t cols = (size_t)w->w, lmax, used = 0, marklen;

    snprintf(mark, sizeof mark, "%s%s", d->buf.modified ? " [+]" : "", d->readonly ? " [RO]" : "");
    marklen = strlen(mark);
    if (cols >= 48)
        sbuf_addf(&right, " %s  %s  UTF-8  Ln %zu, Col %zu ", syn_name(d->syn),
                  d->buf.crlf ? "CRLF" : "LF", w->view.cy + 1,
                  editor_view_col(&d->buf, &w->view, e->tabw) + 1);
    else if (cols >= 20)
        sbuf_addf(&right, " Ln %zu, Col %zu ", w->view.cy + 1,
                  editor_view_col(&d->buf, &w->view, e->tabw) + 1);

    /* left part is " name [+]"; the modified mark wins over the name when space is tight */
    lmax = cols > right.len ? cols - right.len : cols;
    sbuf_adds(o, active ? "\x1b[7m" : "\x1b[7;2m");
    if (lmax >= 1 + marklen) {
        sbuf_adds(o, " ");
        used = 1 + fit_tail(o, name, strlen(name), lmax - 1 - marklen);
        sbuf_adds(o, mark);
        used += marklen;
    }
    spaces(o, lmax - used);
    if (cols > right.len)
        sbuf_add(o, right.p, right.len);
    sbuf_adds(o, "\x1b[0m");
    sbuf_free(&right);
}

static void sidebar_row(SBuf *o, const Editor *e, int y)
{
    size_t w = (size_t)e->sidebar_w, used = 0;
    size_t i = e->tree.top + (size_t)y;

    if (e->tree_ready && i < e->tree.n) {
        const TreeEntry *en = &e->tree.e[i];
        int sel = i == e->tree.sel;
        size_t ind = (size_t)en->depth * 2;

        if (sel)
            sbuf_adds(o, e->sidebar_focus ? "\x1b[7m" : "\x1b[2;7m");
        else if (en->is_dir)
            sbuf_adds(o, "\x1b[1m");
        if (ind > w / 2)
            ind = w / 2;
        spaces(o, ind);
        used = ind;
        if (used + 2 <= w) {
            sbuf_adds(o, en->is_dir ? (en->open ? "\xe2\x96\xbe " : "\xe2\x96\xb8 ") : "  ");
            used += 2;
        }
        used += fit_cols(o, en->name, strlen(en->name), w - used);
        spaces(o, w - used);
        sbuf_adds(o, "\x1b[0m");
        return;
    }
    spaces(o, w);
}

/* *pcol receives the cursor column (0-based) when a prompt is active. */
static void message_row(SBuf *o, const Editor *e, int *pcol)
{
    size_t cols = (size_t)e->cols;

    *pcol = -1;
    if (e->prompt_on) {
        const char *t = e->prompt_text;
        size_t tl = e->prompt_len, start = 0;
        size_t lw = fit_cols(o, e->prompt_label, strlen(e->prompt_label), cols);
        size_t avail = cols > lw + 1 ? cols - lw - 1 : 0;
        size_t tw = utf8_cols(t, tl, tl, 8);

        /* too long for the row: show the tail, where the user is typing */
        while (tw > avail && start < tl) {
            uint32_t cp;

            start += (size_t)utf8_decode(t + start, tl - start, &cp);
            tw -= (size_t)cp_width(cp);
        }
        fit_cols(o, t + start, tl - start, avail);
        *pcol = (int)(lw + tw);
        if (e->prompt_info[0]) {
            size_t iw = utf8_cols(e->prompt_info, strlen(e->prompt_info), strlen(e->prompt_info), 8);
            size_t used = lw + (tw < avail ? tw : avail);

            if (cols > used + iw + 2) {
                spaces(o, cols - used - iw - 1);
                sbuf_adds(o, "\x1b[2m");
                sbuf_adds(o, e->prompt_info);
                sbuf_adds(o, "\x1b[22m");
                sbuf_adds(o, "\x1b[K");
                return;
            }
        }
    } else {
        fit_cols(o, e->msg, strlen(e->msg), cols);
    }
    sbuf_adds(o, "\x1b[K");
}

static const Win *win_at(Win *const *wins, size_t n, int x, int y)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (x >= wins[i]->x && x < wins[i]->x + wins[i]->w && y >= wins[i]->y &&
            y < wins[i]->y + wins[i]->h)
            return wins[i];
    return NULL;
}

static void picker_row(SBuf *o, const Editor *e, int k)
{
    const Picker *p = &e->picker;
    size_t cols = (size_t)e->cols, used = 0;

    if (k == 0) {
        sbuf_adds(o, "\x1b[7m ");
        used = 1;
        used += fit_cols(o, p->title, strlen(p->title), cols - used);
        sbuf_adds(o, " \xe2\x80\xba ");
        used += 3;
        used += fit_cols(o, p->query, p->qlen, cols > used ? cols - used : 0);
        spaces(o, cols > used ? cols - used : 0);
        sbuf_adds(o, "\x1b[0m");
    } else {
        size_t pos = p->top + (size_t)k - 1;

        if (pos < p->nshown) {
            sbuf_adds(o, pos == p->sel ? "\x1b[7m " : " ");
            used = 1;
            used += fit_cols(o, p->items[p->shown[pos]], strlen(p->items[p->shown[pos]]),
                             cols - used);
        }
        spaces(o, cols - used);
        if (pos == p->sel && pos < p->nshown)
            sbuf_adds(o, "\x1b[0m");
    }
}

/* Rows taken by the picker, query line included. */
static int picker_height(const Editor *e)
{
    int n = (int)e->picker.nshown;

    if (n > PICKER_ROWS)
        n = PICKER_ROWS;
    if (n > e->rows - 4)
        n = e->rows - 4;
    return (n < 1 ? 1 : n) + 1;
}

/* A screen row inside the pane area: sidebar, panes and the separators between them. */
static void pane_row(SBuf *o, const Editor *e, Win *const *wins, size_t nwins, int y)
{
    int x = 0;

    if (e->sidebar_w > 0) {
        sidebar_row(o, e, y - e->text_y);
        x = e->sidebar_w;
        if (x < e->cols) {
            sbuf_adds(o, "\x1b[2m\xe2\x94\x82\x1b[22m");
            x++;
        }
    }
    while (x < e->cols) {
        const Win *w = win_at(wins, nwins, x, y);

        if (!w) {
            sbuf_adds(o, "\x1b[2m\xe2\x94\x82\x1b[22m");
            x++;
            continue;
        }
        if (y - w->y < w->h - 1)
            text_row(o, e, w, w->view.rowoff + (size_t)(y - w->y), w == e->layout.active);
        else
            status_row(o, e, w, w == e->layout.active);
        x += w->w;
    }
}

void render_frame(Render *r, const Editor *e)
{
    int y, changed = 0, cr, cc, pcol;
    Win *wins[64];
    size_t nwins;
    const Win *aw = e->layout.active;

    if (e->rows < 3 || e->cols < 1)
        return;
    if (e->rows != r->nrows || e->cols != r->ncols) {
        drop_rows(r);
        r->prev = xmalloc((size_t)e->rows * sizeof *r->prev);
        r->prevlen = xmalloc((size_t)e->rows * sizeof *r->prevlen);
        memset(r->prev, 0, (size_t)e->rows * sizeof *r->prev);
        memset(r->prevlen, 0, (size_t)e->rows * sizeof *r->prevlen);
        r->nrows = e->rows;
        r->ncols = e->cols;
        r->need_clear = 1;
        r->cur_r = r->cur_c = 0;
    }
    nwins = layout_wins(&e->layout, wins, 64);
    if (nwins > 64)
        nwins = 64;

    r->frame.len = 0;
    if (r->need_clear) {
        sbuf_adds(&r->frame, "\x1b[?25l\x1b[2J");
        changed = 1;
        r->need_clear = 0;
    }

    pcol = -1;
    for (y = 0; y < e->rows; y++) {
        SBuf *row = &r->row;

        row->len = 0;
        if (y == e->rows - 1)
            message_row(row, e, &pcol);
        else if (e->picker.on && y >= e->rows - 1 - picker_height(e))
            picker_row(row, e, y - (e->rows - 1 - picker_height(e)));
        else if (e->text_y == 1 && y == 0)
            tab_row(row, e);
        else
            pane_row(row, e, wins, nwins, y);

        if (r->prev[y] && r->prevlen[y] == row->len &&
            memcmp(r->prev[y], row->p, row->len) == 0)
            continue;
        if (!changed) {
            sbuf_adds(&r->frame, "\x1b[?25l");
            changed = 1;
        }
        sbuf_addf(&r->frame, "\x1b[%d;1H", y + 1);
        sbuf_add(&r->frame, row->p, row->len);
        r->prev[y] = xrealloc(r->prev[y], row->len);
        memcpy(r->prev[y], row->p, row->len);
        r->prevlen[y] = row->len;
    }

    if (e->picker.on) {
        cr = e->rows - picker_height(e);
        cc = 1 + 1 + (int)utf8_cols(e->picker.title, strlen(e->picker.title),
                                     strlen(e->picker.title), 8) + 3 +
             (int)utf8_cols(e->picker.query, e->picker.qlen, e->picker.qlen, 8);
        if (cc > e->cols)
            cc = e->cols;
    } else if (e->prompt_on && pcol >= 0) {
        cr = e->rows;
        cc = pcol + 1;
    } else {
        cr = aw->y + (int)(e->cy - e->rowoff) + 1;
        cc = aw->x + (int)(editor_cursor_col(e) - e->coloff) + 1;
    }
    if (changed || cr != r->cur_r || cc != r->cur_c) {
        sbuf_addf(&r->frame, "\x1b[%d;%dH", cr, cc);
        r->cur_r = cr;
        r->cur_c = cc;
    }
    if (changed)
        sbuf_adds(&r->frame, "\x1b[?25h");
    if (r->frame.len > 0)
        term_write(r->frame.p, r->frame.len);
}
