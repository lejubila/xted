#ifndef KED_LAYOUT_H
#define KED_LAYOUT_H

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
    int pct;            /* share of the space given to a, 10..90 */
    int x, y, w, h;
} Node;

typedef struct {
    Node *root;
    Win *active;
} Layout;

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
/* Cycles the focus by delta (+1 / -1). */
void layout_cycle(Layout *l, int delta);
/* The pane showing cell (x, y), or NULL (separator or outside). */
Win *layout_at(const Layout *l, int x, int y);
/* How many panes show d. */
size_t layout_uses(const Layout *l, const Doc *d);

#endif
