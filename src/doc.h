#ifndef KED_DOC_H
#define KED_DOC_H

#include "buffer.h"
#include "syntax.h"

#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Where a window is looking at a document: cursor, scroll and selection. */
typedef struct {
    size_t cy, cx;
    size_t want_col;
    size_t rowoff, coloff;
    int sel_on;
    size_t ay, ax;
} View;

/* An open file: its text, language and what we know about it on disk. */
typedef struct Doc {
    Buffer buf;
    Syntax *syn;            /* NULL: plain text */
    int readonly;           /* the file exists and is not writable */
    int disk_known;         /* mtime/size/ino describe the file as we read or wrote it */
    struct timespec mtime;
    off_t size;
    ino_t ino;
    View last;              /* where the cursor was when the document was last left */
} Doc;

Doc *doc_new(void);
void doc_free(Doc *d);
/* Replaces the language (takes ownership of syn, which may be NULL). */
void doc_set_syntax(Doc *d, Syntax *syn);

/* Loads a file into a new document (0 on success, -1 with err). A nonexistent
 * path gives an empty new document. */
int doc_load(Doc *d, const char *path, char *err, size_t errlen);
/* Loads from an open descriptor (stdin); the document has no name. */
int doc_load_fd(Doc *d, int fd, char *err, size_t errlen);
/* Saves (to path, or to the document's own name if NULL) and records the
 * new state on disk. Returns bytes written, or -1 with err. */
long doc_save(Doc *d, const char *path, char *err, size_t errlen);

enum { DISK_SAME = 0, DISK_CHANGED = 1, DISK_MISSING = 2 };
/* Has the file changed since we read or wrote it? */
int doc_disk_changed(const Doc *d);
/* Takes the file's current state on disk as the one we know. */
void doc_accept_disk(Doc *d);
/* Re-reads the file, dropping undo history. */
int doc_reload(Doc *d, char *err, size_t errlen);

/* Short name for a tab: the file name, or "[Senza nome]". */
const char *doc_base_name(const Doc *d);

#endif
