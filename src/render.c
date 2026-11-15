#include "render.h"
#include "i18n.h"
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
    const Theme *cur_theme;     /* the theme the cursor colour was set for */
    int cur_depth;
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

/* The chrome (bars, lists) in the theme's own colours, so that it does not depend on the
 * terminal's defaults: reverse video on a terminal whose colours do not match the theme gives
 * bars that vanish into the text area. Without a theme background it stays reverse video. */
static int themed(const Editor *e)
{
    return e->color_depth != COLOR_NONE && e->theme->has_bg;
}

static Rgb mix_rgb(Rgb a, Rgb b, int pct)
{
    Rgb r;

    r.r = (unsigned char)((a.r * pct + b.r * (100 - pct)) / 100);
    r.g = (unsigned char)((a.g * pct + b.g * (100 - pct)) / 100);
    r.b = (unsigned char)((a.b * pct + b.b * (100 - pct)) / 100);
    return r;
}

enum { BAR_ON, BAR_DIM, BAR_LIFT };

/* A bar: the text colour as background with the background colour as text (the look of reverse
 * video), dimmer for an inactive one, and BAR_LIFT for the active tab: the text area's colours. */
static void bar(SBuf *o, const Editor *e, int mode)
{
    const Theme *t = e->theme;

    if (!themed(e)) {
        sbuf_adds(o, mode == BAR_ON ? "\x1b[7m" : mode == BAR_DIM ? "\x1b[7;2m" : "\x1b[27;1m");
        return;
    }
    sbuf_adds(o, "\x1b[22m");
    if (mode == BAR_LIFT) {
        theme_bg(o, e->color_depth, t->bg);
        theme_fg(o, e->color_depth, t->fg);
        sbuf_adds(o, "\x1b[1m");
    } else {
        theme_bg(o, e->color_depth, mode == BAR_ON ? t->fg : mix_rgb(t->fg, t->bg, 55));
        theme_fg(o, e->color_depth, t->bg);
    }
}

/* Back to the text colour after a coloured word. */
static void fg_back(SBuf *o, const Editor *e)
{
    if (themed(e))
        theme_fg(o, e->color_depth, e->theme->fg);
    else
        sbuf_adds(o, "\x1b[39m");
}

/* The background of a list row: the popup colours (the highlighted one for the chosen row);
 * without a theme background the chosen row is reverse video. */
static void row_on(SBuf *o, const Editor *e, int sel)
{
    if (themed(e)) {
        theme_bg(o, e->color_depth, sel ? e->theme->popup_sel : e->theme->popup);
        theme_fg(o, e->color_depth, e->theme->fg);
    } else if (sel) {
        sbuf_adds(o, "\x1b[7m");
    }
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
        static const char tag[] = { 'f', 'c', 'm', 'p', 'k', 'v' };

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
    bar(o, e, BAR_ON);
    for (i = start; i < e->ndocs && used < cols; i++) {
        int on = e->docs[i] == e->doc;

        tab_label(e, i, label, sizeof label);
        if (on)
            bar(o, e, BAR_LIFT);
        else
            bar(o, e, BAR_ON);
        used += 1;
        sbuf_adds(o, " ");
        used += fit_cols(o, label, strlen(label), wd[i] - 2 < cols - used ? wd[i] - 2 : cols - used);
        if (used < cols) {
            sbuf_adds(o, " ");
            used++;
        }
    }
    bar(o, e, BAR_ON);
    spaces(o, cols - used);
    sbuf_adds(o, "\x1b[0m");
    free(wd);
}

static void status_row(SBuf *o, const Win *w, int active, int scan, const Editor *e)
{
    const Doc *d = w->doc;
    SBuf right = SBUF_INIT;
    const char *name = d->title ? d->title : d->buf.path ? d->buf.path : "[Senza nome]";
    char mark[16];
    size_t cols = (size_t)w->w, lmax, used = 0, marklen;
    int hint_on;

    snprintf(mark, sizeof mark, "%s%s", d->buf.modified ? " [+]" : "", d->readonly && !d->scratch ? " [RO]" : "");
    marklen = strlen(mark);
    if (scan >= 0 && cols >= 48)
        sbuf_addf(&right, " Scansione %c ", "|/-\\"[scan & 3]);
    else if (scan >= 0)
        sbuf_addf(&right, " %c ", "|/-\\"[scan & 3]);
    hint_on = active && e->hint[0] && cols >= 48;
    if (cols >= 48 && !hint_on) {
        char style[24] = "";

        if (cols >= 62)
            snprintf(style, sizeof style, d->expand_tab ? "  Spazi %d" : "  Tab %d",
                     d->expand_tab ? d->indent_w : d->tabw);
        sbuf_addf(&right, " %s  %s  UTF-8%s  Ln %zu, Col %zu ", syn_name(d->syn),
                  d->buf.crlf ? "CRLF" : "LF", style, w->view.cy + 1,
                  editor_view_col(&d->buf, &w->view, d->tabw) + 1);
    }
    else if (cols >= 20)    /* also while a call is shown: the room goes to its signature */
        sbuf_addf(&right, " Ln %zu, Col %zu ", w->view.cy + 1,
                  editor_view_col(&d->buf, &w->view, d->tabw) + 1);

    /* left part is " name [+]"; the modified mark wins over the name when space is tight */
    lmax = cols > right.len ? cols - right.len : cols;
    bar(o, e, active ? BAR_ON : BAR_DIM);
    if (lmax >= 1 + marklen) {
        size_t room = lmax - 1 - marklen, nmax = room;
        int hint = hint_on && room >= 30;

        /* with a call to show, the name keeps a third of the room (at least 16 columns) */
        if (hint && nmax > (room / 3 >= 16 ? room / 3 : 16))
            nmax = room / 3 >= 16 ? room / 3 : 16;
        sbuf_adds(o, " ");
        used = 1 + fit_tail(o, name, strlen(name), hint ? nmax : room);
        sbuf_adds(o, mark);
        used += marklen;
        if (hint && lmax > used + 12) {
            size_t n = strlen(e->hint), a = e->hint_a, b = e->hint_b;

            if (b > n || a >= b)
                a = b = 0;
            spaces(o, 2);
            used += 2;
            used += fit_cols(o, e->hint, a ? a : n, lmax - used - 1);
            if (a < b) {
                size_t cap = lmax > used + 3 ? lmax - used - 3 : 0, m;

                if (cap > 0) {
                    sbuf_adds(o, "[\x1b[1m");
                    m = fit_cols(o, e->hint + a, b - a, cap);
                    sbuf_adds(o, "\x1b[22m]");
                    used += m + 2;
                    if (m == utf8_cols(e->hint + a, b - a, b - a, 8) && lmax > used)
                        used += fit_cols(o, e->hint + b, n - b, lmax - used - 1);
                }
            }
        }
    }
    spaces(o, lmax > used ? lmax - used : 0);
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
            bar(o, e, e->sidebar_focus ? BAR_ON : BAR_DIM);
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
/* The keys that work in the search bar, shown at the right of its row: the options with the state
 * they are in (bold when on), then Invio and Esc. Longer wordings first; the first that fits is used. */
typedef struct {
    const char *txt;
    int on;                 /* 1: the option is on, 0: off, -1: not an option */
} HintSeg;

#define HINT_MAXSEG 6

static size_t hint_cols(const HintSeg *g, size_t n)
{
    size_t i, w = 0;

    for (i = 0; i < n; i++)
        w += utf8_cols(g[i].txt, strlen(g[i].txt), strlen(g[i].txt), 8) + (i ? 2 : 0);
    return w;
}

/* Picks the longest wording that fits in room cells; returns its width (0: none fits) and the
 * segments in g and *n. */
static size_t find_hint(const Editor *e, size_t room, HintSeg *g, size_t *n)
{
    const Find *f = &e->find;
    int mode = f->mode, cs = f->cs, wd = f->word, rx = f->regex;
    const char *go = tr(mode == FIND_FILES ? "Enter searches" : f->replace ? "Enter next" : "Enter closes");
    HintSeg t[3][HINT_MAXSEG];
    size_t nt[3], k, i;

    if (mode == FIND_WITH) {
        HintSeg a[] = { { tr("Enter starts"), -1 }, { tr("Esc cancels"), -1 } };
        HintSeg b[] = { { tr("Enter"), -1 }, { "Esc", -1 } };

        memcpy(t[0], a, sizeof a);
        nt[0] = 2;
        memcpy(t[1], b, sizeof b);
        nt[1] = nt[2] = 2;
        memcpy(t[2], b, sizeof b);
    } else if (mode == FIND_SEARCH || mode == FIND_FILES) {
        HintSeg a[] = { { tr("Alt+C Case"), cs }, { tr("Alt+W Word"), wd }, { tr("Alt+R Regex"), rx },
                        { go, -1 }, { tr("Esc cancels"), -1 } };
        HintSeg b[] = { { "Alt+C Aa", cs }, { tr("Alt+W Word"), wd }, { tr("Alt+R Regex"), rx },
                        { "Esc", -1 } };
        HintSeg c[] = { { "Alt+C Aa", cs }, { "Alt+W W", wd }, { "Alt+R .*", rx } };

        memcpy(t[0], a, sizeof a);
        nt[0] = 5;
        memcpy(t[1], b, sizeof b);
        nt[1] = 4;
        memcpy(t[2], c, sizeof c);
        nt[2] = 3;
    } else {
        return 0;
    }
    for (k = 0; k < 3; k++)
        if (hint_cols(t[k], nt[k]) <= room) {
            for (i = 0; i < nt[k]; i++)
                g[i] = t[k][i];
            *n = nt[k];
            return hint_cols(t[k], nt[k]);
        }
    return 0;
}

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
        {
            size_t iw = utf8_cols(e->prompt_info, strlen(e->prompt_info), strlen(e->prompt_info), 8);
            size_t used = lw + (tw < avail ? tw : avail), room, hw, i, n = 0;
            HintSeg g[HINT_MAXSEG];
            int info = e->prompt_info[0] != '\0';

            room = cols > used + (info ? iw + 2 : 0) + 3 ? cols - used - (info ? iw + 2 : 0) - 3 : 0;
            hw = find_hint(e, room, g, &n);
            if (hw > 0 || (info && cols > used + iw + 2)) {
                size_t block = hw + (hw && info ? 2 : 0) + (info ? iw : 0);

                spaces(o, cols - used - block - 1);
                sbuf_adds(o, "\x1b[2m");
                for (i = 0; i < n; i++) {
                    if (i)
                        sbuf_adds(o, "  ");
                    if (g[i].on == 1)
                        sbuf_adds(o, "\x1b[22m\x1b[1m");
                    sbuf_adds(o, g[i].txt);
                    if (g[i].on == 1)
                        sbuf_adds(o, "\x1b[22m\x1b[2m");
                }
                if (info) {
                    sbuf_adds(o, hw ? "  " : "");
                    sbuf_adds(o, e->prompt_info);
                }
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

/* A palette row: marker, name in bold, the value of a setting (highlighted while it is on) and
 * the shortcut in the colour of comments, the last two in columns shared by all rows. */
static void palette_row(SBuf *o, const Editor *e, const char *it, int sel, size_t *used)
{
    size_t cols = (size_t)e->cols, w1 = (size_t)e->pal_w1, w2 = (size_t)e->pal_w2;
    size_t w3 = (size_t)e->pal_w3, base;
    const char *name = it + 1, *v = strchr(name, '\x1f'), *key, *vend;
    int color = e->color_depth != COLOR_NONE && !sel;
    size_t u;

    if (!v)
        return;
    v++;
    vend = strchr(v, '\x1f');
    key = vend ? vend + 1 : v + strlen(v);
    if (!vend)
        vend = key;
    /* the marker: a filled circle for a setting that is on, an empty one for one that is off */
    if (it[0] == '1') {
        if (color)
            theme_fg(o, e->color_depth, e->theme->cls[SC_KEYWORD]);
        sbuf_adds(o, "\xe2\x97\x8f ");
    } else if (it[0] == '0') {
        sbuf_adds(o, "\xe2\x97\x8b ");
    } else {
        sbuf_adds(o, "  ");
    }
    if (color)
        fg_back(o, e);
    *used += 2;
    base = *used;
    /* the shortcuts are kept whole: a long name gives way */
    if (*used + w1 + 2 + w2 + 2 + w3 > cols && cols > *used + 2 + w2 + 2 + w3 + 12)
        w1 = cols - (*used + 2 + w2 + 2 + w3);
    sbuf_adds(o, "\x1b[1m");
    u = fit_cols(o, name, (size_t)(v - 1 - name), *used + w1 + 2 + w2 > cols ? cols - *used : w1);
    sbuf_adds(o, "\x1b[22m");
    *used += u;
    if (base + w1 + 2 + w2 > cols)
        return;                 /* too narrow for the columns: the name alone */
    spaces(o, w1 - u + 2);
    *used += w1 - u + 2;
    if (it[0] == '1' || it[0] == ' ') {
        sbuf_adds(o, "\x1b[1m");
        if (color)
            theme_fg(o, e->color_depth, e->theme->cls[SC_KEYWORD]);
    }
    u = fit_cols(o, v, (size_t)(vend - v), w2);
    if (color)
        fg_back(o, e);
    sbuf_adds(o, "\x1b[22m");
    spaces(o, w2 - u + 2);
    *used += w2 + 2;
    sbuf_adds(o, sel || e->color_depth == COLOR_NONE ? "\x1b[2m" : "");
    if (color)
        theme_fg(o, e->color_depth, e->theme->cls[SC_COMMENT]);
    *used += fit_cols(o, key, strlen(key), cols - *used);
    if (color)
        fg_back(o, e);
    else
        sbuf_adds(o, "\x1b[22m");
}

/* A row of the symbol list: a letter for the kind in the colour of its class, the name in bold,
 * the parameters in the colour of strings and the line number in that of comments, each in a
 * column as wide as the widest of all rows (the parameters give way on a narrow screen). */
static void symbol_row(SBuf *o, const Editor *e, const char *it, int sel, size_t *used)
{
    static const char kinds[] = "cfmpk";
    static const int cls[] = { SC_TYPE, SC_KEYWORD, SC_KEYWORD, SC_NUMBER, SC_CONSTANT };
    size_t cols = (size_t)e->cols, w1 = (size_t)e->pal_w1, w2 = (size_t)e->pal_w2;
    size_t w3 = (size_t)e->pal_w3, u;
    const char *name = it + 1, *a = strchr(name, '\x1f'), *l = a ? strchr(a + 1, '\x1f') : NULL;
    const char *k = strchr(kinds, it[0]);
    int color = e->color_depth != COLOR_NONE && !sel;

    if (!l) {
        *used += fit_cols(o, it, strlen(it), cols - *used);
        return;
    }
    if (color && k && it[0])
        theme_fg(o, e->color_depth, e->theme->cls[cls[k - kinds]]);
    sbuf_add(o, it, 1);
    if (color)
        fg_back(o, e);
    sbuf_adds(o, " ");
    *used += 2;
    if (*used + w1 + 2 + w3 + 2 > cols)
        w1 = cols > *used + 2 + w3 + 2 + 8 ? cols - *used - 2 - w3 - 2 : 8;
    if (*used + w1 + 2 + w2 + 2 + w3 > cols)
        w2 = cols > *used + w1 + 2 + 2 + w3 ? cols - *used - w1 - 2 - 2 - w3 : 0;
    sbuf_adds(o, "\x1b[1m");
    u = fit_cols(o, name, (size_t)(a - name), w1);
    sbuf_adds(o, "\x1b[22m");
    spaces(o, w1 - u + 2);
    *used += w1 + 2;
    if (color)
        theme_fg(o, e->color_depth, e->theme->cls[SC_STRING]);
    u = fit_cols(o, a + 1, (size_t)(l - a - 1), w2);
    if (color)
        fg_back(o, e);
    spaces(o, w2 - u + 2);
    *used += w2 + 2;
    sbuf_adds(o, sel || e->color_depth == COLOR_NONE ? "\x1b[2m" : "");
    if (color)
        theme_fg(o, e->color_depth, e->theme->cls[SC_COMMENT]);
    spaces(o, w3 > strlen(l + 1) ? w3 - strlen(l + 1) : 0);
    *used += (w3 > strlen(l + 1) ? w3 - strlen(l + 1) : 0);
    *used += fit_cols(o, l + 1, strlen(l + 1), cols > *used ? cols - *used : 0);
    if (color)
        fg_back(o, e);
    else
        sbuf_adds(o, "\x1b[22m");
}

/* A row of the "save where" list: the kind of file in bold, the scope in the keyword colour, the
 * note in that of strings and the path in that of comments, in columns shared by all rows. */
static void savecfg_row(SBuf *o, const Editor *e, const char *it, int sel, size_t *used)
{
    size_t cols = (size_t)e->cols, w[3], i, u;
    const char *f[4], *c = it;
    static const int cls[3] = { SC_KEYWORD, SC_STRING, SC_COMMENT };
    int color = e->color_depth != COLOR_NONE && !sel;

    w[0] = (size_t)e->pal_w1;
    w[1] = (size_t)e->pal_w2;
    w[2] = (size_t)e->pal_w3;
    for (i = 0; i < 4; i++) {
        f[i] = c;
        c = i < 3 ? strchr(c, '\x1f') : NULL;
        if (i < 3 && !c) {
            *used += fit_cols(o, it, strlen(it), cols - *used);
            return;
        }
        if (c)
            c++;
    }
    /* on a narrow screen the note, then the scope, give way to the path */
    while (*used + w[0] + 2 + w[1] + 2 + w[2] + 2 + 6 > cols && (w[1] || w[2])) {
        if (w[2])
            w[2] = w[2] > 8 ? w[2] - 8 : 0;
        else
            w[1] = w[1] > 8 ? w[1] - 8 : 0;
    }
    for (i = 0; i < 3; i++) {
        const char *end = strchr(f[i], '\x1f');

        if (i == 0)
            sbuf_adds(o, "\x1b[1m");
        else if (color)
            theme_fg(o, e->color_depth, e->theme->cls[cls[i - 1]]);
        u = fit_cols(o, f[i], (size_t)(end - f[i]), w[i]);
        if (i == 0)
            sbuf_adds(o, "\x1b[22m");
        else if (color)
            fg_back(o, e);
        if (w[i]) {
            spaces(o, w[i] - u + 2);
            *used += w[i] + 2;
        }
    }
    sbuf_adds(o, sel || e->color_depth == COLOR_NONE ? "\x1b[2m" : "");
    if (color)
        theme_fg(o, e->color_depth, e->theme->cls[cls[2]]);
    *used += fit_cols(o, f[3], strlen(f[3]), cols > *used ? cols - *used : 0);
    if (color)
        fg_back(o, e);
    else
        sbuf_adds(o, "\x1b[22m");
}

/* A row of the buffer list: the name in bold, the path in the colour of comments, in a column as
 * wide as the longest name. */
static void buffer_row(SBuf *o, const Editor *e, const char *it, int rev, size_t *used)
{
    size_t cols = (size_t)e->cols, w1 = (size_t)e->pal_w1, u;
    const char *sep = strchr(it, '\x1f');
    int color = e->color_depth != COLOR_NONE && !rev;

    if (!sep) {
        *used += fit_cols(o, it, strlen(it), cols - *used);
        return;
    }
    if (*used + w1 + 2 + 10 > cols)
        w1 = cols > *used + 2 + 10 ? cols - *used - 2 - 10 : 0;
    sbuf_adds(o, "\x1b[1m");
    u = fit_cols(o, it, (size_t)(sep - it), w1);
    sbuf_adds(o, "\x1b[22m");
    spaces(o, w1 - u + 2);
    *used += w1 + 2;
    sbuf_adds(o, rev || e->color_depth == COLOR_NONE ? "\x1b[2m" : "");
    if (color)
        theme_fg(o, e->color_depth, e->theme->cls[SC_COMMENT]);
    *used += fit_cols(o, sep + 1, strlen(sep + 1), cols > *used ? cols - *used : 0);
    if (color)
        fg_back(o, e);
    else
        sbuf_adds(o, "\x1b[22m");
}

static void picker_row(SBuf *o, const Editor *e, int k)
{
    const Picker *p = &e->picker;
    size_t cols = (size_t)e->cols, used = 0;

    if (k == 0) {
        bar(o, e, BAR_ON);
        sbuf_adds(o, " ");
        used = 1;
        used += fit_cols(o, p->title, strlen(p->title), cols - used);
        sbuf_adds(o, " \xe2\x80\xba ");
        used += 3;
        used += fit_cols(o, p->query, p->qlen, cols > used ? cols - used : 0);
        if (p->loading) {
            char ind[48];
            size_t n = (size_t)snprintf(ind, sizeof ind, " Caricamento %c %zu ",
                                        "|/-\\"[p->spin & 3], p->nitems);

            if (cols > used + n) {
                spaces(o, cols - used - n);
                sbuf_adds(o, ind);
                used = cols;
            }
        }
        spaces(o, cols > used ? cols - used : 0);
        sbuf_adds(o, "\x1b[0m");
    } else {
        size_t pos = p->top + (size_t)k - 1;
        int sel = pos == p->sel && pos < p->nshown;
        int rev = sel && !themed(e);    /* the chosen row is reverse video: no extra colours */

        row_on(o, e, sel);
        sbuf_adds(o, " ");
        used = 1;
        if (pos < p->nshown) {
            const char *it = p->items[p->shown[pos]];

            if (it[0] == PICKER_HEAD) {
                /* a section of the palette */
                sbuf_adds(o, "\x1b[1m");
                used += fit_cols(o, it + 1, strlen(it + 1), cols - used);
                sbuf_adds(o, "\x1b[22m");
            } else if (e->picker_kind == PK_COMMANDS) {
                palette_row(o, e, it, rev, &used);
            } else if (e->picker_kind == PK_SAVECFG) {
                savecfg_row(o, e, it, rev, &used);
            } else if (e->picker_kind == PK_SYMBOLS) {
                symbol_row(o, e, it, rev, &used);
            } else if (e->picker_kind == PK_BUFFERS) {
                buffer_row(o, e, it, rev, &used);
            } else {
                used += fit_cols(o, it, strlen(it), cols - used);
            }
        }
        spaces(o, cols - used);
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
                       w == e->layout.active && e->comp.scanning ? (int)(e->comp.spin & 3) : -1,
                       e);
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
    /* the cursor is drawn by the terminal in its own colour, which may be the colour of a light
     * theme's background: set it to the text colour of the theme (OSC 12), reset on exit */
    if (r->cur_theme != e->theme || r->cur_depth != e->color_depth) {
        if (themed(e))
            sbuf_addf(&r->frame, "\x1b]12;#%02x%02x%02x\x07", e->theme->fg.r, e->theme->fg.g,
                      e->theme->fg.b);
        else
            sbuf_adds(&r->frame, "\x1b]112\x07");
        r->cur_theme = e->theme;
        r->cur_depth = e->color_depth;
    }
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
