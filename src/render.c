#include "render.h"
#include "doc.h"
#include "edit.h"
#include "editor.h"
#include "fold.h"
#include "git.h"
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

/* The suggestion popup as seen from one pane row: a "hole" in the line where
 * the popup's cells go instead of the text. */
typedef struct {
    int on;
    int row0, h;            /* screen rows of the popup */
    int cx, w;              /* first column inside the pane, width in cells */
} PopupGeom;

typedef struct {
    size_t c0, c1;          /* line columns [c0, c1) covered */
    const Cand *cand;
    int sel;
} Ovl;

/* Appends one popup row, exactly w cells wide, in its own colours. */
static void emit_popup(SBuf *o, const Editor *e, const Ovl *ov)
{
    size_t w = ov->c1 - ov->c0, used = 0;
    const char *name = ov->cand->sig ? ov->cand->sig : ov->cand->text;

    if (e->color_depth != COLOR_NONE) {
        sbuf_adds(o, "\x1b[0m");
        theme_fg(o, e->color_depth, e->theme->fg);
        theme_bg(o, e->color_depth, ov->sel ? e->theme->popup_sel : e->theme->popup);
    } else {
        sbuf_adds(o, ov->sel ? "\x1b[0;1m" : "\x1b[0;7m");
    }
    sbuf_adds(o, ov->sel ? ">" : " ");
    used = 1;
    used += fit_cols(o, name, strlen(name), w > used ? w - used : 0);
    if (ov->cand->kind == CAND_SNIPPET && w > used + 9) {
        spaces(o, w - used - 7 - 1);
        sbuf_adds(o, "snippet");
        used = w - 1;
    } else if (ov->cand->kind >= CAND_FUNC && w > used + 3) {
        static const char tag[] = { 'f', 'c', 'm', 'p', 'k' };

        spaces(o, w - used - 2);
        sbuf_adds(o, "\x1b[2m");
        sbuf_add(o, &tag[ov->cand->kind - CAND_FUNC], 1);
        sbuf_adds(o, "\x1b[22m");
        used = w - 1;
    }
    spaces(o, w > used ? w - used : 0);
    sbuf_adds(o, "\x1b[0m");
}

static void popup_geom(const Editor *e, PopupGeom *g)
{
    const Win *aw = e->layout.active;
    const Complete *c = &e->comp;
    const Line *l;
    size_t i, wmax = 0, h;
    int r, w, cx;

    memset(g, 0, sizeof *g);
    if (!c->on || c->n == 0 || e->picker.on || e->prompt_on || c->y >= e->buf->nlines)
        return;
    l = &e->buf->lines[c->y];
    h = c->n - c->top < COMPLETE_ROWS ? c->n - c->top : COMPLETE_ROWS;
    /* the width of the whole list, so that the popup does not jump while scrolling */
    for (i = 0; i < c->n; i++) {
        const char *t = c->v[i].sig ? c->v[i].sig : c->v[i].text;
        size_t n = utf8_cols(t, strlen(t), strlen(t), 8);

        if (c->v[i].kind == CAND_SNIPPET)
            n += 9;
        else if (c->v[i].kind >= CAND_FUNC)
            n += 3;
        if (n > wmax)
            wmax = n;
    }
    w = (int)wmax + 2;
    if (w > 100)
        w = 100;
    if (w < 8)
        w = 8;
    if (w > win_text_w(aw))
        w = win_text_w(aw);
    r = aw->y + (int)fold_count(e->buf, e->rowoff, e->cy);
    {
        /* as many rows as fit under the cursor line, or above it; at least one */
        int below = aw->y + aw->h - 2 - r, above = r - aw->y, room = below >= above ? below : above;
        size_t take = h;

        if (room < 1)
            return;
        if (take > (size_t)room) {
            h = (size_t)room;
            take = h;
            ((Complete *)c)->rows = h;  /* so the selection scrolls within what is shown */
        } else {
            ((Complete *)c)->rows = 0;
        }
        if ((int)take <= below)
            g->row0 = r + 1;
        else
            g->row0 = r - (int)take;
        h = take;
    }
    cx = (int)utf8_cols(l->s, l->len, c->x < l->len ? c->x : l->len, e->tabw) - (int)e->coloff;
    if (cx < 0)
        cx = 0;
    if (cx + w > win_text_w(aw))
        cx = win_text_w(aw) - w;
    g->on = 1;
    g->h = (int)h;
    g->cx = cx;
    g->w = w;
}

/* The Git margin cell: a coloured mark, or a blank. */
static void emit_gutter(SBuf *o, const Editor *e, int mark, int colors)
{
    Rgb c;

    if (mark == GIT_NONE || mark > GIT_DEL) {
        sbuf_adds(o, " ");
        return;
    }
    if (e->color_depth != COLOR_NONE) {
        c = mark == GIT_ADD ? e->theme->git_add : mark == GIT_MOD ? e->theme->git_mod
                                                                    : e->theme->git_del;
        theme_fg(o, e->color_depth, c);
    }
    sbuf_adds(o, mark == GIT_ADD ? "+" : mark == GIT_MOD ? "~" : "_");
    if (e->color_depth != COLOR_NONE) {
        if (colors)
            theme_fg(o, e->color_depth, e->theme->fg);
        else
            sbuf_adds(o, "\x1b[39m");
    }
}

/* One line of a pane's text, exactly w->w cells wide (the Git margin included). */
static void text_row(SBuf *o, const Editor *e, const Win *w, size_t y, int active,
                     const Ovl *ov)
{
    const Doc *d = w->doc;
    const View *v = &w->view;
    const Line *l;
    const uint8_t *cls;
    size_t tw = (size_t)win_text_w(w), left = v->coloff, right = left + tw;
    int gut = win_gutter(w);
    size_t i = 0, col = 0, sel_from = 0, sel_to = 0;
    int colors, cur = SC_NORMAL, br = 0, prev_visible = 0, hl = 0, sel_nl = 0, has_sel;
    Matcher *hm = e->find.hl && e->find.m && e->find.m->valid ? e->find.m : NULL;
    size_t mr[32][2], nm = 0, mk = 0;
    int mt = 0, ov_done = 0, xeol = 0;
    size_t xr[64][2], nxr = 0, xi;
    int bg_row = e->color_depth != COLOR_NONE && e->theme->has_bg;

    if (y >= d->buf.nlines) {
        if (bg_row)
            theme_bg(o, e->color_depth, e->theme->bg);
        if (gut)
            sbuf_adds(o, " ");
        if (ov) {
            size_t pc = ov->c0 - v->coloff;

            if (pc >= 1) {
                sbuf_adds(o, "\x1b[2m~\x1b[22m");
                spaces(o, pc - 1);
            }
            emit_popup(o, e, ov);
            if (bg_row)
                theme_bg(o, e->color_depth, e->theme->bg);
            spaces(o, tw - (ov->c1 - v->coloff));
        } else {
            sbuf_adds(o, "\x1b[2m~\x1b[22m");
            spaces(o, tw - 1);
        }
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
    if (gut)
        emit_gutter(o, e, l->git, colors);
    cls = colors ? syn_classes(d->syn, &d->buf, y) : NULL;
    has_sel = active && edit_view_sel_line(&d->buf, v, y, &sel_from, &sel_to, &sel_nl);
    if (hm)
        nm = sm_line_matches(hm, l->s, l->len, mr, 32);
    /* the other cursors of multi-cursor mode: their selections and a cell at each cursor */
    if (active)
        for (xi = 0; xi < e->nxc && nxr < 64; xi++) {
            const Cursor *c = &e->xc[xi];
            View cv;
            size_t f, t;
            int nl;

            cv.cy = c->cy;
            cv.cx = c->cx;
            cv.sel_on = c->sel_on;
            cv.ay = c->ay;
            cv.ax = c->ax;
            if (edit_view_sel_line(&d->buf, &cv, y, &f, &t, &nl)) {
                xr[nxr][0] = f;
                xr[nxr][1] = t;
                nxr++;
                if (nl)
                    xeol = 1;
            }
            if (c->cy == y && nxr < 64) {
                if (c->cx < l->len) {
                    xr[nxr][0] = c->cx;
                    xr[nxr][1] = utf8_next(l->s, l->len, c->cx);
                    nxr++;
                } else {
                    xeol = 1;
                }
            }
        }

    while (i < l->len && col < right) {
        uint32_t cp;
        int n = utf8_decode(l->s + i, l->len - i, &cp);
        size_t cw = (size_t)char_cols(cp, col, d->tabw);
        size_t start = col, stop = col + cw;
        int selected = has_sel && i >= sel_from && i < sel_to;
        size_t xk;

        int mark = active && e->match_on &&
                   ((y == e->by && i == e->bx) || (y == e->my && i == e->mx));
        int cc = cls ? cls[i] : SC_NORMAL;
        int inm;

        for (xk = 0; xk < nxr; xk++)
            if (i >= xr[xk][0] && i < xr[xk][1])
                selected = 1;

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
        if (ov && (ov_done || stop > ov->c0)) {
            /* the popup replaces the columns [c0, c1) of the line */
            if (!ov_done) {
                if (start < ov->c0 && start >= left)
                    spaces(o, ov->c0 - start);
                set_hl(o, &hl, 0);
                set_mt(o, e, &mt, 0);
                set_br(o, &br, 0);
                emit_popup(o, e, ov);
                if (colors) {
                    theme_fg(o, e->color_depth, e->theme->fg);
                    if (e->theme->has_bg)
                        theme_bg(o, e->color_depth, e->theme->bg);
                }
                cur = SC_NORMAL;
                ov_done = 1;
            }
            if (start < ov->c1) {
                col = stop > ov->c1 ? stop : ov->c1;    /* the popup fills up to c1 */
                if (stop > ov->c1)      /* a tab or wide character cut by the popup's edge */
                    spaces(o, (stop < right ? stop : right) - ov->c1);
                continue;
            }
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
    if (ov && !ov_done) {
        /* the line ends before the popup */
        if (col < ov->c0)
            spaces(o, ov->c0 - (col > left ? col : left));
        emit_popup(o, e, ov);
        if (colors) {
            theme_fg(o, e->color_depth, e->theme->fg);
            if (e->theme->has_bg)
                theme_bg(o, e->color_depth, e->theme->bg);
        }
        col = ov->c1;
        ov_done = 1;
    }
    /* a selection running past the end of the line shows the newline as one cell */
    if (((has_sel && sel_nl) || xeol) && !ov && col >= left && col < right) {
        sbuf_adds(o, "\x1b[7m \x1b[27m");
        col++;
    }
    if (col < left)
        col = left;
    if (l->fold > 0 && !ov && col + 2 < right) {
        char lab[40];
        size_t hid = l->fold;

        if (y + 1 + hid > d->buf.nlines)
            hid = d->buf.nlines - 1 - y;
        snprintf(lab, sizeof lab, " \xe2\x8b\xaf %zu righe", hid);
        sbuf_adds(o, "\x1b[2m");
        col += fit_cols(o, lab, strlen(lab), right - col);
        sbuf_adds(o, "\x1b[22m");
    }
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

/* Width of every tab (wd, one entry per document) and the first one shown: the bar scrolls so
 * that the active tab is the last one that has to fit. */
static size_t tab_widths(const Editor *e, size_t *wd)
{
    size_t cols = (size_t)e->cols, i, start = 0, act = 0;
    char label[300];

    for (i = 0; i < e->ndocs; i++) {
        size_t n;

        tab_label(e, i, label, sizeof label);
        n = utf8_cols(label, strlen(label), strlen(label), 8);
        wd[i] = (n > 24 ? 24 : n) + 2;
        if (e->docs[i] == e->doc)
            act = i;
    }
    for (;;) {
        size_t sum = 0;

        for (i = start; i <= act; i++)
            sum += wd[i];
        if (sum <= cols || start >= act)
            break;
        start++;
    }
    return start;
}

size_t render_tab_at(const Editor *e, int x)
{
    size_t *wd, i, start, pos = 0, found = (size_t)-1;

    if (e->ndocs == 0 || x < 0 || x >= e->cols)
        return found;
    wd = xmalloc(e->ndocs * sizeof *wd);
    start = tab_widths(e, wd);
    for (i = start; i < e->ndocs && pos < (size_t)e->cols; i++) {
        if ((size_t)x < pos + wd[i]) {
            found = i;
            break;
        }
        pos += wd[i];
    }
    free(wd);
    return found;
}

static void tab_row(SBuf *o, const Editor *e)
{
    size_t cols = (size_t)e->cols, used = 0, i, start;
    size_t *wd = xmalloc(e->ndocs * sizeof *wd);
    char label[300];

    start = tab_widths(e, wd);
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

static void status_row(SBuf *o, const Win *w, int active, int scan)
{
    const Doc *d = w->doc;
    SBuf right = SBUF_INIT;
    const char *name = d->title ? d->title : d->buf.path ? d->buf.path : "[Senza nome]";
    char mark[16];
    size_t cols = (size_t)w->w, lmax, used = 0, marklen;

    snprintf(mark, sizeof mark, "%s%s", d->buf.modified ? " [+]" : "", d->readonly && !d->scratch ? " [RO]" : "");
    marklen = strlen(mark);
    if (scan >= 0 && cols >= 48)
        sbuf_addf(&right, " Scansione %c ", "|/-\\"[scan & 3]);
    else if (scan >= 0)
        sbuf_addf(&right, " %c ", "|/-\\"[scan & 3]);
    if (cols >= 48) {
        char style[24] = "";

        if (cols >= 62)
            snprintf(style, sizeof style, d->expand_tab ? "  Spazi %d" : "  Tab %d",
                     d->expand_tab ? d->indent_w : d->tabw);
        sbuf_addf(&right, " %s  %s  UTF-8%s  Ln %zu, Col %zu ", syn_name(d->syn),
                  d->buf.crlf ? "CRLF" : "LF", style, w->view.cy + 1,
                  editor_view_col(&d->buf, &w->view, d->tabw) + 1);
    }
    else if (cols >= 20)
        sbuf_addf(&right, " Ln %zu, Col %zu ", w->view.cy + 1,
                  editor_view_col(&d->buf, &w->view, d->tabw) + 1);

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
int render_picker_height(const Editor *e)
{
    int n = (int)e->picker.nshown;

    if (n > PICKER_ROWS)
        n = PICKER_ROWS;
    if (n > e->rows - 4)
        n = e->rows - 4;
    return (n < 1 ? 1 : n) + 1;
}

/* A screen row inside the pane area: sidebar, panes and the separators between them. */
static void pane_row(SBuf *o, const Editor *e, Win *const *wins, size_t nwins, int y,
                     const PopupGeom *pg)
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
        if (y - w->y < w->h - 1) {
            Ovl ov;
            int use = pg->on && w == e->layout.active && y >= pg->row0 && y < pg->row0 + pg->h;

            if (use) {
                size_t k = (size_t)(y - pg->row0);

                ov.c0 = w->view.coloff + (size_t)pg->cx;
                ov.c1 = ov.c0 + (size_t)pg->w;
                ov.cand = &e->comp.v[e->comp.top + k];
                ov.sel = e->comp.top + k == e->comp.sel;
            }
            text_row(o, e, w, fold_nth(&w->doc->buf, w->view.rowoff, (size_t)(y - w->y)),
                     w == e->layout.active,
                     use ? &ov : NULL);
        }
        else
            status_row(o, w, w == e->layout.active,
                       w == e->layout.active && e->comp.scanning ? (int)(e->comp.spin & 3) : -1);
        x += w->w;
    }
}

void render_frame(Render *r, const Editor *e)
{
    int y, changed = 0, cr, cc, pcol;
    PopupGeom pg;
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

    popup_geom(e, &pg);
    pcol = -1;
    for (y = 0; y < e->rows; y++) {
        SBuf *row = &r->row;

        row->len = 0;
        if (y == e->rows - 1)
            message_row(row, e, &pcol);
        else if (e->picker.on && y >= e->rows - 1 - render_picker_height(e))
            picker_row(row, e, y - (e->rows - 1 - render_picker_height(e)));
        else if (e->text_y == 1 && y == 0)
            tab_row(row, e);
        else
            pane_row(row, e, wins, nwins, y, &pg);

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
        cr = e->rows - render_picker_height(e);
        cc = 1 + 1 + (int)utf8_cols(e->picker.title, strlen(e->picker.title),
                                     strlen(e->picker.title), 8) + 3 +
             (int)utf8_cols(e->picker.query, e->picker.qlen, e->picker.qlen, 8);
        if (cc > e->cols)
            cc = e->cols;
    } else if (e->prompt_on && pcol >= 0) {
        cr = e->rows;
        cc = pcol + 1;
    } else {
        cr = aw->y + (int)fold_count(e->buf, e->rowoff, e->cy) + 1;
        cc = aw->x + win_gutter(aw) + (int)(editor_cursor_col(e) - e->coloff) + 1;
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
