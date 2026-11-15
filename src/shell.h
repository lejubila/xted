#ifndef XTED_SHELL_H
#define XTED_SHELL_H

#include "util.h"

#include <stddef.h>
#include <sys/types.h>

/* A command running in the background, with stdout and stderr on one pipe. */
typedef struct {
    pid_t pid;              /* 0: none */
    int fd;                 /* read end of the output pipe, -1 once closed */
    int status;             /* wait status, valid when done */
    int done;
} Job;

/* Starts /bin/sh -c cmd in directory cwd (NULL: the current one), in a process group of its
 * own and with stdin from /dev/null. Returns 0, or -1 with err. */
int shell_start(Job *j, const char *cmd, const char *cwd, char *err, size_t errlen);
/* Reads what is available without blocking. Returns bytes read, 0 at the end of the
 * output, -1 if there is nothing yet. */
long shell_read(Job *j, char *buf, size_t n);
/* Asks the whole process group to end (SIGTERM). */
void shell_stop(Job *j);
/* Collects the exit status once the process is gone. Returns 1 when it has finished. */
int shell_reap(Job *j);
/* Stops the command if it still runs, waits for it and closes the pipe. */
void shell_close(Job *j);
/* Exit code of a finished job, or 128 + signal; -1 if it is not done. */
int shell_exit_code(const Job *j);

/* Runs argv to completion with in as its stdin, collecting stdout in out and stderr in err
 * (either may be NULL). Output beyond cap bytes is dropped. Returns 0 on completion (the wait
 * status is in *status), -1 if it could not start, -2 if it ran longer than timeout_ms and
 * was killed. */
int shell_capture(const char *const *argv, const char *cwd, const char *in, size_t inlen,
                  SBuf *out, SBuf *err, int timeout_ms, size_t cap, int *status);

#endif
