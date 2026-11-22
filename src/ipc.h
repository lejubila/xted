#ifndef XTED_IPC_H
#define XTED_IPC_H

#include "util.h"

#include <stddef.h>
#include <stdint.h>

/* One xted for the files opened from the desktop: the first one started listens (a Unix socket
 * in $XDG_RUNTIME_DIR, a named pipe on Windows), the others (xted --remote) hand it their files
 * and end. $XTED_IPC, when set, is the address instead (the tests: never a real xted's).
 *
 * A request: "xted 1\n", then a line "path\tline\tcol\n" for each file (absolute path, 0 for no
 * line), then an empty line; the answer is "ok\n". */

typedef struct {
    char *path;
    size_t line, col;
} IpcFile;

/* ---- the system's channel: ipc_unix.c, ipc_win.c ---- */

/* Hands the files to an xted that listens; 1 when it took them, 0 when there is none (or it
 * did not answer in time). */
int ipc_send(const IpcFile *f, size_t n);
/* Listens for the others, unless one listens already; what term_watch_server waits on (a
 * descriptor, a HANDLE on Windows), or -1. */
intptr_t ipc_listen(void);
/* Takes the requests that came (term_wait said TERM_EV_SERVER) into the queue. */
void ipc_serve(void);
/* Stops listening (at exit). */
void ipc_close(void);

/* ---- common: ipc.c ---- */

/* The request of the files, in b. */
void ipc_request(SBuf *b, const IpcFile *f, size_t n);
/* Whether s[0, n) is a whole request (it ends with the empty line). */
int ipc_complete(const char *s, size_t n);
/* The files of a whole request into the queue; how many. */
size_t ipc_queue(const char *s, size_t n);
/* The files come and not opened yet (the caller frees each path and the array); NULL if none. */
IpcFile *ipc_take(size_t *n);

#endif
