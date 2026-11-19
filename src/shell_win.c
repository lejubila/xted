/* Commands on Windows: not yet (Windows port, phase 4, with CreateProcess). Until then running a
 * command (Alt+R, a filter, Git) says it is not available, and Git's markers are not shown. */

#include "shell.h"

#include <stdio.h>

int shell_start(Job *j, const char *cmd, const char *cwd, char *err, size_t errlen)
{
    (void)cmd;
    (void)cwd;
    j->pid = 0;
    j->fd = -1;
    snprintf(err, errlen, "%s", "commands are not available in this version for Windows");
    return -1;
}

long shell_read(Job *j, char *buf, size_t n)
{
    (void)j;
    (void)buf;
    (void)n;
    return 0;
}

void shell_stop(Job *j)
{
    (void)j;
}

int shell_reap(Job *j)
{
    (void)j;
    return 1;
}

void shell_close(Job *j)
{
    j->fd = -1;
}

int shell_exit_code(const Job *j)
{
    (void)j;
    return 127;
}

int shell_capture(const char *const *argv, const char *cwd, const char *in, size_t inlen,
                  SBuf *out, SBuf *err, int timeout_ms, size_t cap, int *status)
{
    (void)argv;
    (void)cwd;
    (void)in;
    (void)inlen;
    (void)out;
    (void)err;
    (void)timeout_ms;
    (void)cap;
    *status = 127;                  /* as a shell says of a command it cannot find */
    return -1;
}
