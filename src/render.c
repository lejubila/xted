#include "render.h"
#include "edit.h"
#include "editor.h"
#include "term.h"
#include "theme.h"
#include "utf8.h"
#include "util.h"

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

/* Starts a row with the theme's default colours, if it asks for any. */
static int row_colors(SBuf *o, const Editor *e)
{
    if (e->color_depth == COLOR_NONE || (!e->syn && !e->theme->has_bg))
        return 0;
    theme_fg(o, e->color_depth, e->theme->fg);
    if (e->theme->has_bg)
        theme_bg(o, e->color_depth, e->theme->bg);
    return 1;
}

static void text_row(SBuf *o, const Editor *e, size_t y)
{
    const Line *l;
    const uint8_t *cls;
    int colors, cur = SC_NORMAL, br = 0;
    size_t left = e->coloff, right = left + (size_t)e->cols;
    size_t i = 0, col = 0, sel_from = 0, sel_to = 0;
    int prev_visible = 0, hl = 0, sel_nl = 0, has_sel;

    if (y >= e->buf.nlines) {
        if (e->color_depth != COLOR_NONE && e->theme->has_bg) {
            theme_bg(o, e->color_depth, e->theme->bg);
            sbuf_adds(o, "\x1b[2m~\x1b[K\x1b[0m");
        } else {
            sbuf_adds(o, "\x1b[2m~\x1b[0m\x1b[K");
        }
        return;
    }
    l = &e->buf.lines[y];
    colors = row_colors(o, e);
    cls = colors ? syn_classes(e->syn, &e->buf, y) : NULL;
    has_sel = edit_sel_line(e, y, &sel_from, &sel_to, &sel_nl);

    while (i < l->len && col < right) {
        uint32_t cp;
        int n = utf8_decode(l->s + i, l->len - i, &cp);
        size_t w = (size_t)char_cols(cp, col, e->tabw);
        size_t start = col, stop = col + w;
        int selected = has_sel && i >= sel_from && i < sel_to;
        int mark = e->match_on && ((y == e->by && i == e->bx) || (y == e->my && i == e->mx));
        int cc = cls ? cls[i] : SC_NORMAL;

        i += (size_t)n;
        if (w == 0) {
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
        set_br(o, &br, mark);
        if (colors)
            set_cls(o, e, &cur, cc);
        if (start >= left && stop <= right) {
            if (cp == '\t') {
                spaces(o, w);
            } else if (w == 2 && (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))) {
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
    set_br(o, &br, 0);
    /* a selection running past the end of the line shows the newline as one cell */
    if (has_sel && sel_nl && col >= left && col < right)
        sbuf_adds(o, "\x1b[7m \x1b[27m");
    sbuf_adds(o, "\x1b[K");
    if (colors)
        sbuf_adds(o, "\x1b[0m");
}

static void status_row(SBuf *o, const Editor *e)
{
    SBuf right = SBUF_INIT;
    const char *name = e->buf.path ? e->buf.path : "[Senza nome]";
    const char *mark = e->buf.modified ? " [+]" : "";
    size_t cols = (size_t)e->cols, lmax, used = 0, marklen = strlen(mark);

    sbuf_addf(&right, " %s  %s  UTF-8  Ln %zu, Col %zu ", syn_name(e->syn),
              e->buf.crlf ? "CRLF" : "LF", e->cy + 1, editor_cursor_col(e) + 1);

    /* left part is " name [+]"; the modified mark wins over the name when space is tight */
    lmax = cols > right.len ? cols - right.len : cols;
    sbuf_adds(o, "\x1b[7m");
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
    } else {
        fit_cols(o, e->msg, strlen(e->msg), cols);
    }
    sbuf_adds(o, "\x1b[K");
}

void render_frame(Render *r, const Editor *e)
{
    int y, text_rows, changed = 0, cr, cc, pcol;

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

    r->frame.len = 0;
    if (r->need_clear) {
        sbuf_adds(&r->frame, "\x1b[?25l\x1b[2J");
        changed = 1;
        r->need_clear = 0;
    }

    text_rows = e->rows - 2;
    pcol = -1;
    for (y = 0; y < e->rows; y++) {
        SBuf *row = &r->row;

        row->len = 0;
        if (y < text_rows)
            text_row(row, e, e->rowoff + (size_t)y);
        else if (y == text_rows)
            status_row(row, e);
        else
            message_row(row, e, &pcol);

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

    if (e->prompt_on && pcol >= 0) {
        cr = e->rows;
        cc = pcol + 1;
    } else {
        cr = (int)(e->cy - e->rowoff) + 1;
        cc = (int)(editor_cursor_col(e) - e->coloff) + 1;
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
