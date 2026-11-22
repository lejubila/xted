#ifndef XTED_LAYOUT_H
#define XTED_LAYOUT_H

#include "doc.h"

/* A pane: one document seen through a view. The last row of its rectangle
 * is its status line. */
typedef struct Win {
    Doc *doc;
    View view;
    int x, y, w, h;
} Win;

/* Binary tree of splits. A leaf holds a Win; an inner node splits its
 * rectangle between a and b, side by side (vertical) or stacked. */
typedef struct Node {
    struct Node *parent, *a, *b;
    Win *win;
    int vertical;       /* a is left of b (a separator column sits between them) */
    int frac;           /* share of the space given to a, in thousandths (50..950) */
    int x, y, w, h;
} Node;

typedef struct {
    Node *root;
    Win *active;
} Layout;

/* The column of the line numbers: as many digits as the last line has (3 at least) and a space;
 * none when they are off, or when the pane is so narrow that the text would have less than
 * 20 columns. */
static inline int win_numw(const Win *w)
{
    size_t n;
    int d = 1;

    if (!w->doc || !w->doc->numbers)
        return 0;
    for (n = w->doc->buf.nlines; n >= 10; n /= 10)
        d++;
    d = (d < 3 ? 3 : d) + 1;
    return w->w - d - (w->doc->gutter ? 1 : 0) >= 20 ? d : 0;
}

/* The text area of a pane is its width minus the margin: the line numbers, the Git marks. */
static inline int win_gutter(const Win *w)
{
    return win_numw(w) + (w->doc && w->doc->gutter ? 1 : 0);
}

static inline int win_text_w(const Win *w)
{
    int g = win_gutter(w);

    return w->w > g ? w->w - g : 1;
}

enum { WIN_MIN_W = 12, WIN_MIN_H = 4 };

void layout_init(Layout *l, Doc *d);
void layout_free(Layout *l);
/* Assigns rectangles to every pane inside (x, y, w, h). */
void layout_compute(Layout *l, int x, int y, int w, int h);
/* Splits the active pane; the new pane shows the same document and view and
 * becomes active. Returns it, or NULL when there is not enough room. */
Win *layout_split(Layout *l, int vertical);
/* Same, giving the original pane pct percent of the space (10..90). */
Win *layout_split_at(Layout *l, int vertical, int pct);
/* Removes a pane, giving its space to its sibling. Returns 0, or -1 for the last one. */
int layout_close(Layout *l, Win *w);
/* Panes in reading order. Returns the count; fills at most max entries. */
size_t layout_wins(const Layout *l, Win **out, size_t max);
size_t layout_count(const Layout *l);
/* Recomputes every rectangle inside the root's current one (after a share changed). */
void layout_refresh(Layout *l);
/* The split whose border contains cell (x, y): the separator column of a side-by-side split,
 * or the last row (the status line) of the upper half of a stacked one. NULL if none. */
Node *layout_border_at(const Layout *l, int x, int y);
/* Gives the first half of n exactly `cells` rows/columns (before the minimum sizes apply). */
void layout_set_size(Node *n, int cells);
/* Shares 50/50 in every split. */
void layout_equalize(Layout *l);
enum { LAYOUT_LEFT, LAYOUT_RIGHT, LAYOUT_UP, LAYOUT_DOWN };
/* Moves a border of pane w by `cells` in the direction of the arrow: the pane's border on that
 * side if it has one, otherwise the one on the opposite side. Returns -1 when no split runs
 * that way. */
int layout_resize(Layout *l, Win *w, int dir, int cells);
/* Cycles the focus by delta (+1 / -1). */
void layout_cycle(Layout *l, int delta);
/* The pane showing cell (x, y), or NULL (separator or outside). */
Win *layout_at(const Layout *l, int x, int y);
/* How many panes show d. */
size_t layout_uses(const Layout *l, const Doc *d);

#endif
