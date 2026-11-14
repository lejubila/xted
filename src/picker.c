#include "picker.h"
#include "fuzzy.h"
#include "utf8.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void drop_items(Picker *p)
{
    size_t i;

    for (i = 0; i < p->nitems; i++)
        free(p->items[i]);
    p->nitems = 0;
}

void picker_open(Picker *p, const char *title)
{
    drop_items(p);
    p->on = 1;
    snprintf(p->title, sizeof p->title, "%s", title);
    p->query[0] = '\0';
    p->qlen = 0;
    p->nshown = 0;
    p->sel = p->top = 0;
    p->loading = 0;
    p->spin = 0;
}

void picker_add(Picker *p, const char *text)
{
    if (p->nitems == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 64;
        p->items = xrealloc(p->items, p->cap * sizeof *p->items);
        p->shown = xrealloc(p->shown, p->cap * sizeof *p->shown);
    }
    p->items[p->nitems++] = xstrdup(text);
}

/* Scores live in a parallel array only while sorting. */
static const int *g_scores;

static int cmp(const void *a, const void *b)
{
    size_t x = *(const size_t *)a, y = *(const size_t *)b;

    if (g_scores[x] != g_scores[y])
        return g_scores[y] - g_scores[x];
    return x < y ? -1 : x > y;      /* stable: keep the original order */
}

void picker_filter(Picker *p)
{
    size_t i, n = 0;
    int *scores = xmalloc((p->nitems ? p->nitems : 1) * sizeof *scores);

    for (i = 0; i < p->nitems; i++) {
        scores[i] = fuzzy_score(p->query, p->items[i]);
        if (scores[i] >= 0)
            p->shown[n++] = i;
    }
    p->nshown = n;
    if (p->query[0]) {
        g_scores = scores;
        qsort(p->shown, n, sizeof *p->shown, cmp);
        g_scores = NULL;
    }
    free(scores);
    p->sel = p->top = 0;
}

void picker_refilter(Picker *p)
{
    long keep = p->sel > 0 ? picker_chosen(p) : -1;

    picker_filter(p);
    if (keep >= 0)
        picker_select_item(p, (size_t)keep);
}

void picker_type(Picker *p, unsigned cp)
{
    char b[4];
    int n = utf8_encode(cp, b);

    if (p->qlen + (size_t)n >= sizeof p->query)
        return;
    memcpy(p->query + p->qlen, b, (size_t)n);
    p->qlen += (size_t)n;
    p->query[p->qlen] = '\0';
    picker_filter(p);
}

void picker_backspace(Picker *p)
{
    if (p->qlen == 0)
        return;
    p->qlen = utf8_prev(p->query, p->qlen);
    p->query[p->qlen] = '\0';
    picker_filter(p);
}

void picker_move(Picker *p, long delta)
{
    long s = (long)p->sel + delta;

    if (p->nshown == 0)
        return;
    if (s < 0)
        s = 0;
    if (s >= (long)p->nshown)
        s = (long)p->nshown - 1;
    p->sel = (size_t)s;
    if (p->sel < p->top)
        p->top = p->sel;
    if (p->sel >= p->top + PICKER_ROWS)
        p->top = p->sel - PICKER_ROWS + 1;
}

long picker_chosen(const Picker *p)
{
    return p->sel < p->nshown ? (long)p->shown[p->sel] : -1;
}

void picker_select_item(Picker *p, size_t item)
{
    size_t i;

    for (i = 0; i < p->nshown; i++)
        if (p->shown[i] == item) {
            p->sel = 0;
            p->top = 0;
            picker_move(p, (long)i);
            return;
        }
}

void picker_close(Picker *p)
{
    p->on = 0;
}

void picker_free(Picker *p)
{
    drop_items(p);
    free(p->items);
    free(p->shown);
    memset(p, 0, sizeof *p);
}
