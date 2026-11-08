#ifndef KED_FILES_H
#define KED_FILES_H

#include <stddef.h>

#define SCAN_MAX_FILES 50000
#define SCAN_MAX_DEPTH 12

/* Calls add(path, ctx) for every regular file under root, with the path
 * relative to root, directories in alphabetical order. Hidden entries and
 * the usual build/dependency folders are skipped. Stops after
 * SCAN_MAX_FILES; returns the number of files reported. */
size_t files_scan(const char *root, void (*add)(const char *rel, void *ctx), void *ctx);

/* Visible rows of the file browser: a flat list in which an open folder is
 * followed by its (deeper) children. */
typedef struct {
    char *path;         /* full path */
    char *name;         /* last component, for display */
    int depth;
    int is_dir;
    int open;
} TreeEntry;

typedef struct {
    char *root;
    TreeEntry *e;
    size_t n, cap;
    size_t sel, top;
} Tree;

void tree_init(Tree *t, const char *root);      /* the root entry is open */
void tree_free(Tree *t);
/* Opens or closes folder i. */
void tree_toggle(Tree *t, size_t i);
/* Closes folder i if open, else selects its parent folder. */
void tree_left(Tree *t);
/* Keeps top such that sel is within rows visible rows. */
void tree_fit(Tree *t, size_t rows);

#endif
