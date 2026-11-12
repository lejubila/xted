#include "layout.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

static Node *leaf(Win *w)
{
    Node *n = xmalloc(sizeof *n);

    memset(n, 0, sizeof *n);
    n->win = w;
    return n;
}

void layout_init(Layout *l, Doc *d)
{
    Win *w = xmalloc(sizeof *w);

    memset(w, 0, sizeof *w);
    w->doc = d;
    l->root = leaf(w);
    l->active = w;
}

static void free_node(Node *n)
{
    if (!n)
        return;
    free_node(n->a);
    free_node(n->b);
    free(n->win);
    free(n);
}

void layout_free(Layout *l)
{
    free_node(l->root);
    l->root = NULL;
    l->active = NULL;
}

static void compute(Node *n, int x, int y, int w, int h)
{
    n->x = x;
    n->y = y;
    n->w = w;
    n->h = h;
    if (n->win) {
        n->win->x = x;
        n->win->y = y;
        n->win->w = w;
        n->win->h = h;
        return;
    }
    if (n->vertical) {
        int aw = (w - 1) * n->frac / 1000;

        if (aw < WIN_MIN_W)
            aw = WIN_MIN_W;
        if (aw > w - 1 - WIN_MIN_W)
            aw = w - 1 - WIN_MIN_W;
        if (aw < 1)
            aw = 1;
        compute(n->a, x, y, aw, h);
        compute(n->b, x + aw + 1, y, w - aw - 1 > 0 ? w - aw - 1 : 1, h);
    } else {
        int ah = h * n->frac / 1000;

        if (ah < WIN_MIN_H)
            ah = WIN_MIN_H;
        if (ah > h - WIN_MIN_H)
            ah = h - WIN_MIN_H;
        if (ah < 1)
            ah = 1;
        compute(n->a, x, y, w, ah);
        compute(n->b, x, y + ah, w, h - ah > 0 ? h - ah : 1);
    }
}

void layout_compute(Layout *l, int x, int y, int w, int h)
{
    compute(l->root, x, y, w, h);
}

void layout_refresh(Layout *l)
{
    compute(l->root, l->root->x, l->root->y, l->root->w, l->root->h);
}

static Node *border_in(Node *n, int x, int y, int vertical)
{
    Node *r;

    if (n->win || x < n->x || x >= n->x + n->w || y < n->y || y >= n->y + n->h)
        return NULL;
    if (n->vertical == vertical && (vertical ? x == n->a->x + n->a->w : y == n->a->y + n->a->h - 1))
        return n;
    r = border_in(n->a, x, y, vertical);
    return r ? r : border_in(n->b, x, y, vertical);
}

/* A separator column is no pane's cell, a status line is: the two kinds never clash, and
 * the separator wins where they meet. */
Node *layout_border_at(const Layout *l, int x, int y)
{
    Node *n = border_in(l->root, x, y, 1);

    return n ? n : border_in(l->root, x, y, 0);
}

void layout_set_size(Node *n, int cells)
{
    int dim = n->vertical ? n->w - 1 : n->h, f;

    if (n->win || dim < 1)
        return;
    f = (cells * 1000 + dim - 1) / dim;
    n->frac = f < 50 ? 50 : f > 950 ? 950 : f;
}

static void equalize(Node *n)
{
    if (n->win)
        return;
    n->frac = 500;
    equalize(n->a);
    equalize(n->b);
}

void layout_equalize(Layout *l)
{
    equalize(l->root);
    layout_refresh(l);
}

static Node *find_leaf(Node *n, const Win *w)
{
    Node *r;

    if (!n)
        return NULL;
    if (n->win)
        return n->win == w ? n : NULL;
    r = find_leaf(n->a, w);
    return r ? r : find_leaf(n->b, w);
}

Win *layout_split(Layout *l, int vertical)
{
    return layout_split_at(l, vertical, 50);
}

Win *layout_split_at(Layout *l, int vertical, int pct)
{
    Node *n = find_leaf(l->root, l->active), *inner, *nl;
    Win *w;

    if (!n)
        return NULL;
    if (vertical ? n->w < 2 * WIN_MIN_W + 1 : n->h < 2 * WIN_MIN_H)
        return NULL;
    w = xmalloc(sizeof *w);
    *w = *l->active;
    nl = leaf(w);
    inner = xmalloc(sizeof *inner);
    memset(inner, 0, sizeof *inner);
    inner->vertical = vertical;
    inner->frac = (pct < 10 ? 10 : pct > 90 ? 90 : pct) * 10;
    inner->parent = n->parent;
    inner->x = n->x;
    inner->y = n->y;
    inner->w = n->w;
    inner->h = n->h;
    if (n->parent) {
        if (n->parent->a == n)
            n->parent->a = inner;
        else
            n->parent->b = inner;
    } else {
        l->root = inner;
    }
    inner->a = n;
    inner->b = nl;
    n->parent = inner;
    nl->parent = inner;
    l->active = w;
    compute(inner, inner->x, inner->y, inner->w, inner->h);
    return w;
}

static Win *first_win(Node *n)
{
    while (!n->win)
        n = n->a;
    return n->win;
}

int layout_close(Layout *l, Win *w)
{
    Node *n = find_leaf(l->root, w), *p, *sib;

    if (!n || !n->parent)
        return -1;
    p = n->parent;
    sib = p->a == n ? p->b : p->a;
    sib->parent = p->parent;
    if (p->parent) {
        if (p->parent->a == p)
            p->parent->a = sib;
        else
            p->parent->b = sib;
    } else {
        l->root = sib;
    }
    if (l->active == w)
        l->active = first_win(sib);
    compute(l->root, l->root->x, l->root->y, l->root->w, l->root->h);
    free(n->win);
    free(n);
    free(p);
    return 0;
}

/* The nearest ancestor of n that splits in the wanted direction with n on side `in_a`. */
static Node *ancestor(const Node *n, int vertical, int in_a)
{
    const Node *c = n;
    Node *p;

    for (p = n->parent; p; c = p, p = p->parent)
        if (p->vertical == vertical && (p->a == c) == in_a)
            return p;
    return NULL;
}

int layout_resize(Layout *l, Win *w, int dir, int cells)
{
    int vertical = dir == LAYOUT_LEFT || dir == LAYOUT_RIGHT;
    int positive = dir == LAYOUT_RIGHT || dir == LAYOUT_DOWN;
    Node *leaf_n = find_leaf(l->root, w), *p;

    if (!leaf_n)
        return -1;
    /* the arrow pushes the border on its side: below/right of the pane when it points that way */
    p = ancestor(leaf_n, vertical, positive);
    if (!p)
        p = ancestor(leaf_n, vertical, !positive);
    if (!p)
        return -1;
    layout_set_size(p, (vertical ? p->a->w : p->a->h) + (positive ? cells : -cells));
    layout_refresh(l);
    return 0;
}

static size_t collect(const Node *n, Win **out, size_t max, size_t k)
{
    if (n->win) {
        if (k < max)
            out[k] = n->win;
        return k + 1;
    }
    k = collect(n->a, out, max, k);
    return collect(n->b, out, max, k);
}

size_t layout_wins(const Layout *l, Win **out, size_t max)
{
    return collect(l->root, out, max, 0);
}

size_t layout_count(const Layout *l)
{
    return collect(l->root, NULL, 0, 0);
}

void layout_cycle(Layout *l, int delta)
{
    size_t n = layout_count(l), i;
    Win **v = xmalloc(n * sizeof *v);

    layout_wins(l, v, n);
    for (i = 0; i < n && v[i] != l->active; i++)
        ;
    if (i < n)
        l->active = v[(i + n + (size_t)(delta < 0 ? n - 1 : 1)) % n];
    free(v);
}

Win *layout_at(const Layout *l, int x, int y)
{
    size_t n = layout_count(l), i;
    Win **v = xmalloc(n * sizeof *v), *r = NULL;

    layout_wins(l, v, n);
    for (i = 0; i < n; i++)
        if (x >= v[i]->x && x < v[i]->x + v[i]->w && y >= v[i]->y && y < v[i]->y + v[i]->h)
            r = v[i];
    free(v);
    return r;
}

size_t layout_uses(const Layout *l, const Doc *d)
{
    size_t n = layout_count(l), i, c = 0;
    Win **v = xmalloc(n * sizeof *v);

    layout_wins(l, v, n);
    for (i = 0; i < n; i++)
        if (v[i]->doc == d)
            c++;
    free(v);
    return c;
}
