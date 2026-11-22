#ifndef XTED_MOUSE_H
#define XTED_MOUSE_H

#include "editor.h"
#include "term.h"

/* What a screen cell belongs to. */
typedef enum {
    MH_NONE,       /* message row, a separator, outside */
    MH_TAB,        /* index: the document */
    MH_SIDEBAR,    /* index: the entry of the file browser */
    MH_PICKER,     /* index: row of the picker, 0 is its query line */
    MH_TEXT,       /* win, y, x: line and byte offset under the cell */
    MH_STATUS,     /* win: the status line of a pane (node: also a border, when it is one) */
    MH_BORDER,     /* node: the separator column of a side-by-side split */
    MH_EDGE        /* the separator between the file browser and the panes */
} MouseHitKind;

typedef struct {
    MouseHitKind kind;
    size_t index;
    Win *win;
    Node *node;
    size_t y, x;
    int margin;         /* MH_TEXT: the cell is in the column of the line numbers */
} MouseHit;

/* Where screen cell (x, y), 0-based, falls. Brings the panes' views up to date first. */
void mouse_locate(Editor *e, int x, int y, MouseHit *h);

/* Clicks place the cursor (double: word, triple: line, Alt: one more cursor), a drag selects,
 * the wheel scrolls, and tabs, the browser and the pickers can be clicked. Ignored while a
 * prompt or the search bar is open. */
void mouse_event(Editor *e, const MouseEv *ev);

#endif
