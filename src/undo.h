#ifndef XTED_UNDO_H
#define XTED_UNDO_H

#include <stddef.h>
#include <stdint.h>

typedef enum { OP_INSERT, OP_DELETE, OP_SPLIT, OP_JOIN } OpKind;

/* An edit as it was performed. INSERT/DELETE carry the bytes; for JOIN,
 * x is the length of line y before the join, so that undo can split it again. */
typedef struct {
    OpKind kind;
    size_t y, x;
    char *text;
    size_t n;
} Op;

/* Operations undone and redone together, with the cursor to restore. */
typedef struct {
    Op *ops;
    size_t nops, cap;
    size_t cy_before, cx_before;
    size_t cy_after, cx_after;
} UndoGroup;

typedef struct {
    UndoGroup *groups;
    size_t ngroups, cap;
    size_t cur;         /* groups [0,cur) are applied, [cur,ngroups) can be redone */
    size_t saved_at;    /* value of cur when saved; SIZE_MAX once unreachable */
    int open;           /* new operations join the current group */
    int has_group;      /* the open group already exists (created by its first operation) */
    size_t pcy, pcx;    /* cursor to store if the group gets created */
} Undo;

void undo_init(Undo *u);
void undo_free(Undo *u);
/* Starts a group (a no-op while one is open). It is created lazily, so
 * commands that change nothing leave no empty group and keep the redo list. */
void undo_begin(Undo *u, size_t cy, size_t cx);
void undo_close(Undo *u);
void undo_cursor(Undo *u, size_t cy, size_t cx);
/* Records an operation that has just been applied; adjacent typing and
 * adjacent deletions are merged into one operation. */
void undo_record(Undo *u, OpKind kind, size_t y, size_t x, const char *text, size_t n);

#endif
