#include "fold.h"
#include "i18n.h"
#include "utf8.h"

#include <string.h>

#define FOLD_UP_LINES 500

/* Hidden lines after y, clamped so a stale count cannot run past the end. */
static size_t hidden_after(const Buffer *b, size_t y)
{
    size_t n = b->lines[y].fold;

    if (y + 1 + n > b->nlines)
        n = b->nlines - 1 - y;
    return n;
}

size_t fold_next(const Buffer *b, size_t y)
{
    if (!b->folded)
        return y + 1;
    return y + 1 + hidden_after(b, y);
}

size_t fold_prev(const Buffer *b, size_t y)
{
    size_t i = 0, prev = 0;

    if (y == 0)
        return 0;
    if (!b->folded)
        return y - 1;
    while (i < y) {
        prev = i;
        i = fold_next(b, i);
    }
    return prev;
}

size_t fold_advance(const Buffer *b, size_t y, size_t n)
{
    while (n-- > 0) {
        size_t nx = fold_next(b, y);

        if (nx >= b->nlines)
            break;
        y = nx;
    }
    return y;
}

size_t fold_back(const Buffer *b, size_t y, size_t n)
{
    size_t ring[256], i = 0, cnt = 0, k;

    if (!b->folded)
        return y > n ? y - n : 0;
    if (n >= sizeof ring / sizeof ring[0])
        n = sizeof ring / sizeof ring[0] - 1;
    /* one walk from the top, remembering the last n+1 visible lines */
    while (i < y) {
        ring[cnt % (n + 1)] = i;
        cnt++;
        i = fold_next(b, i);
    }
    if (cnt == 0)
        return 0;
    k = cnt > n ? cnt - n : 0;
    return ring[k % (n + 1)];
}

size_t fold_nth(const Buffer *b, size_t from, size_t k)
{
    if (!b->folded)
        return from + k;
    while (k-- > 0 && from < b->nlines)
        from = fold_next(b, from);
    return from < b->nlines ? from : b->nlines;
}

size_t fold_count(const Buffer *b, size_t from, size_t to)
{
    size_t n = 0;

    if (!b->folded)
        return to > from ? to - from : 0;
    while (from < to && from < b->nlines) {
        n++;
        from = fold_next(b, from);
    }
    return n;
}

size_t fold_head(const Buffer *b, size_t y)
{
    size_t i = 0;

    if (!b->folded)
        return y;
    while (i < b->nlines) {
        size_t nx = fold_next(b, i);

        if (y < nx)
            return i;
        i = nx;
    }
    return y;
}

/* ---- finding blocks --------------------------------------------------- */

static size_t indent_of(const Line *l, int *blank)
{
    size_t i, w = 0;

    for (i = 0; i < l->len && (l->s[i] == ' ' || l->s[i] == '\t'); i++)
        w += l->s[i] == '\t' ? 8 : 1;
    *blank = i == l->len;
    return w;
}

static int range_by_bracket(Syntax *s, Buffer *b, size_t y, size_t *last)
{
    const Line *l = &b->lines[y];
    size_t end = l->len, py, px, my, mx;
    const uint8_t *cls;

    while (end > 0 && (l->s[end - 1] == ' ' || l->s[end - 1] == '\t'))
        end--;
    if (end == 0 || !strchr("{([", l->s[end - 1]))
        return 0;
    if (y > 0)
        syn_update(s, b, y - 1);
    cls = syn_classes(s, b, y);
    if (cls && (cls[end - 1] == SC_COMMENT || cls[end - 1] == SC_STRING))
        return 0;
    if (!syn_match_bracket(s, b, y, end, &py, &px, &my, &mx) || my <= y + 1 || py != y)
        return 0;
    *last = my - 1;
    return 1;
}

static int range_by_indent(const Buffer *b, size_t y, size_t *last)
{
    int blank;
    size_t base = indent_of(&b->lines[y], &blank), k, end = 0;

    if (blank)
        return 0;
    for (k = y + 1; k < b->nlines; k++) {
        size_t w = indent_of(&b->lines[k], &blank);

        if (blank)
            continue;
        if (w <= base)
            break;
        end = k;
    }
    if (end == 0)
        return 0;
    *last = end;
    return 1;
}

int fold_range(Syntax *s, Buffer *b, size_t y, size_t *last)
{
    if (y + 1 >= b->nlines)
        return 0;
    if (s && range_by_bracket(s, b, y, last))
        return 1;
    return range_by_indent(b, y, last);
}

/* ---- commands --------------------------------------------------------- */

static void put_cursor_on_head(Editor *e)
{
    size_t h = fold_head(e->buf, e->cy);

    if (h != e->cy) {
        e->cy = h;
        if (e->cx > e->buf->lines[h].len)
            e->cx = e->buf->lines[h].len;
    }
    e->sel_on = 0;
}

void fold_toggle(Editor *e)
{
    Buffer *b = e->buf;
    size_t last = 0, h = (size_t)-1, cand, lim;

    if (b->lines[e->cy].fold > 0) {
        b->lines[e->cy].fold = 0;
        return;
    }
    if (fold_range(e->doc->syn, b, e->cy, &last)) {
        h = e->cy;
    } else {
        /* the innermost block above that still contains the cursor line */
        lim = e->cy > FOLD_UP_LINES ? e->cy - FOLD_UP_LINES : 0;
        for (cand = e->cy; cand > lim;) {
            size_t l2;

            cand--;
            if (fold_range(e->doc->syn, b, cand, &l2) && l2 >= e->cy) {
                h = cand;
                last = l2;
                break;
            }
        }
    }
    if (h == (size_t)-1) {
        editor_set_msg(e, MSG_TTL_MS, tr("Nothing to fold"));
        return;
    }
    b->lines[h].fold = (uint32_t)(last - h);
    b->folded = 1;
    e->cy = h;
    if (e->cx > b->lines[h].len)
        e->cx = b->lines[h].len;
    e->sel_on = 0;
}

void fold_all(Editor *e)
{
    Buffer *b = e->buf;
    size_t y = 0, last, count = 0;

    while (y < b->nlines) {
        if (b->lines[y].fold == 0 && fold_range(e->doc->syn, b, y, &last)) {
            b->lines[y].fold = (uint32_t)(last - y);
            b->folded = 1;
            count++;
            y = last + 1;
        } else {
            y = fold_next(b, y);
        }
    }
    put_cursor_on_head(e);
    if (count == 0)
        editor_set_msg(e, MSG_TTL_MS, tr("Nothing to fold"));
}

void unfold_all(Editor *e)
{
    size_t y;

    for (y = 0; y < e->buf->nlines; y++)
        e->buf->lines[y].fold = 0;
    e->buf->folded = 0;
}

void fold_reveal(Editor *e)
{
    Buffer *b = e->buf;
    size_t h;

    if (!b->folded)
        return;
    while ((h = fold_head(b, e->cy)) != e->cy)
        b->lines[h].fold = 0;
    if (e->sel_on && e->ay < b->nlines)
        while ((h = fold_head(b, e->ay)) != e->ay)
            b->lines[h].fold = 0;
}
