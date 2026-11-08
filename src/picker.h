#ifndef KED_PICKER_H
#define KED_PICKER_H

#include <stddef.h>

#define PICKER_QMAX 256
#define PICKER_ROWS 10

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
} Picker;

/* Starts a new list: drops the previous items. */
void picker_open(Picker *p, const char *title);
void picker_add(Picker *p, const char *text);
/* Call after the last picker_add (and after the query changes). */
void picker_filter(Picker *p);
void picker_type(Picker *p, unsigned cp);
void picker_backspace(Picker *p);
void picker_move(Picker *p, long delta);
/* Index of the highlighted item in the order of picker_add, or -1. */
long picker_chosen(const Picker *p);
void picker_close(Picker *p);
void picker_free(Picker *p);
/* Preselects an item by index (before the list is shown). */
void picker_select_item(Picker *p, size_t item);

#endif
