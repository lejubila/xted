#ifndef XTED_PICKER_H
#define XTED_PICKER_H

#include <stddef.h>

#define PICKER_QMAX 256
#define PICKER_ROWS 10

/* An item that starts with this byte is a section heading: shown only while the query is empty,
 * never selected (the arrows skip it). */
#define PICKER_HEAD '\x01'

/* A list that is narrowed down while typing a fuzzy query. Items are
 * strings; the caller learns the chosen one by its index in the order they
 * were added. */
typedef struct {
    int on;
    char title[32];
    char **items;
    size_t nitems, cap;
    size_t *shown;          /* indexes into items, best match first */
    size_t nshown;
    char query[PICKER_QMAX];
    size_t qlen;
    size_t sel;             /* position in shown */
    size_t top;             /* first visible position in shown */
    int loading;            /* items are still being added: the renderer shows an indicator */
    unsigned spin;          /* steps of that loading so far */
} Picker;

/* Starts a new list: drops the previous items. */
void picker_open(Picker *p, const char *title);
void picker_add(Picker *p, const char *text);
/* Call after the last picker_add (and after the query changes). */
void picker_filter(Picker *p);
/* Filters again after items were added while the list is open: the highlighted item stays
 * highlighted if the user has moved to one. */
void picker_refilter(Picker *p);
void picker_type(Picker *p, unsigned cp);
void picker_backspace(Picker *p);
void picker_move(Picker *p, long delta);
/* 1 if the item shown at position pos is a heading. */
int picker_is_head(const Picker *p, size_t pos);
/* Index of the highlighted item in the order of picker_add, or -1. */
long picker_chosen(const Picker *p);
void picker_close(Picker *p);
void picker_free(Picker *p);
/* Preselects an item by index (before the list is shown). */
void picker_select_item(Picker *p, size_t item);

#endif
