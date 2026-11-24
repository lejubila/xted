#ifndef XTED_UNSAVED_H
#define XTED_UNSAVED_H

#include <stddef.h>

/* The buffers with no file, kept between sessions (keep_unsaved, as Notepad++ does): one file a
 * buffer in $XDG_STATE_HOME/xted/unsaved/ (%LOCALAPPDATA%\xted\unsaved on Windows), named by
 * its number, and an index with their order and cursors. One xted at a time keeps them: the
 * one that holds the lock of the folder (released by the system when that xted ends). */

typedef struct {
    unsigned id;
    size_t y, x, rowoff;
} UnsavedEntry;

/* Takes the lock of the folder: 1 when this xted keeps the buffers, 0 when another one does
 * (or there is no folder). */
int unsaved_lock(void);
/* The text of buffer id, written so that a crash leaves the old one or the new one whole. */
int unsaved_write(unsigned id, const char *text, size_t n);
/* The path of the text of buffer id (malloc'd). */
char *unsaved_path(unsigned id);
/* The index: "xted-unsaved 1" and a line "id y x rowoff" a buffer; the files of the folder
 * not in it are removed (buffers closed, or saved with a name). */
int unsaved_write_index(const UnsavedEntry *v, size_t n);
/* The entries of the index (malloc'd, *n of them), NULL when there is none. */
UnsavedEntry *unsaved_read_index(size_t *n);

#endif
