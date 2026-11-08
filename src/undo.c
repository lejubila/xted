#include "undo.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

void undo_init(Undo *u)
{
    memset(u, 0, sizeof *u);
}

static void group_free(UndoGroup *g)
{
    size_t i;

    for (i = 0; i < g->nops; i++)
        free(g->ops[i].text);
    free(g->ops);
}

void undo_free(Undo *u)
{
    size_t i;

    for (i = 0; i < u->ngroups; i++)
        group_free(&u->groups[i]);
    free(u->groups);
    memset(u, 0, sizeof *u);
}

void undo_begin(Undo *u, size_t cy, size_t cx)
{
    if (u->open && u->has_group)
        return;
    u->open = 1;
    u->pcy = cy;
    u->pcx = cx;
}

void undo_close(Undo *u)
{
    u->open = 0;
    u->has_group = 0;
}

void undo_cursor(Undo *u, size_t cy, size_t cx)
{
    if (u->has_group) {
        UndoGroup *g = &u->groups[u->cur - 1];

        g->cy_after = cy;
        g->cx_after = cx;
    }
}

static UndoGroup *new_group(Undo *u)
{
    UndoGroup *g;
    size_t i;

    if (u->cur < u->ngroups) {
        /* a new edit after an undo: the redo list is gone for good */
        for (i = u->cur; i < u->ngroups; i++)
            group_free(&u->groups[i]);
        u->ngroups = u->cur;
        if (u->saved_at > u->cur)
            u->saved_at = SIZE_MAX;
    }
    if (u->ngroups == u->cap) {
        u->cap = u->cap ? u->cap * 2 : 64;
        u->groups = xrealloc(u->groups, u->cap * sizeof *u->groups);
    }
    g = &u->groups[u->ngroups++];
    memset(g, 0, sizeof *g);
    g->cy_before = g->cy_after = u->pcy;
    g->cx_before = g->cx_after = u->pcx;
    u->cur = u->ngroups;
    u->has_group = 1;
    return g;
}

static void push_op(UndoGroup *g, OpKind kind, size_t y, size_t x, const char *text, size_t n)
{
    Op *op;

    if (g->nops == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 8;
        g->ops = xrealloc(g->ops, g->cap * sizeof *g->ops);
    }
    op = &g->ops[g->nops++];
    op->kind = kind;
    op->y = y;
    op->x = x;
    op->n = n;
    op->text = NULL;
    if (n > 0) {
        op->text = xmalloc(n);
        memcpy(op->text, text, n);
    }
}

void undo_record(Undo *u, OpKind kind, size_t y, size_t x, const char *text, size_t n)
{
    UndoGroup *g;
    Op *last;

    if (!u->open)
        undo_begin(u, y, x);
    g = u->has_group ? &u->groups[u->cur - 1] : new_group(u);

    last = g->nops ? &g->ops[g->nops - 1] : NULL;
    if (last && last->y == y) {
        if (kind == OP_INSERT && last->kind == OP_INSERT && last->x + last->n == x) {
            last->text = xrealloc(last->text, last->n + n);
            memcpy(last->text + last->n, text, n);
            last->n += n;
            return;
        }
        if (kind == OP_DELETE && last->kind == OP_DELETE) {
            if (x == last->x) {                 /* Delete key: text after the cursor */
                last->text = xrealloc(last->text, last->n + n);
                memcpy(last->text + last->n, text, n);
                last->n += n;
                return;
            }
            if (x + n == last->x) {             /* Backspace: text before the cursor */
                last->text = xrealloc(last->text, last->n + n);
                memmove(last->text + n, last->text, last->n);
                memcpy(last->text, text, n);
                last->n += n;
                last->x = x;
                return;
            }
        }
    }
    push_op(g, kind, y, x, text, n);
}
