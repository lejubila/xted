#include "shell.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void set_cloexec(int fd)
{
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

static void set_nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

/* Runs in the child between fork and exec: only async-signal-safe calls. */
static void child_exec(const char *const *argv, const char *cwd, int in_fd, int out_fd,
                       int err_fd)
{
    int null;

    setpgid(0, 0);
    signal(SIGPIPE, SIG_DFL);
    if (cwd && chdir(cwd) < 0)
        _exit(126);
    if (in_fd < 0) {
        null = open("/dev/null", O_RDONLY);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            if (null != STDIN_FILENO)
                close(null);
        }
    } else {
        dup2(in_fd, STDIN_FILENO);
    }
    dup2(out_fd, STDOUT_FILENO);
    dup2(err_fd, STDERR_FILENO);
    /* every other descriptor of ours is close-on-exec */
    execvp(argv[0], (char *const *)argv);
    _exit(127);
}

int shell_start(Job *j, const char *cmd, const char *cwd, char *err, size_t errlen)
{
    int p[2];
    pid_t pid;
    const char *argv[4];

    memset(j, 0, sizeof *j);
    j->fd = -1;
    if (pipe(p) < 0) {
        snprintf(err, errlen, "%s", strerror(errno));
        return -1;
    }
    set_cloexec(p[0]);
    set_cloexec(p[1]);
    argv[0] = "/bin/sh";
    argv[1] = "-c";
    argv[2] = cmd;
    argv[3] = NULL;
    pid = fork();
    if (pid < 0) {
        snprintf(err, errlen, "%s", strerror(errno));
        close(p[0]);
        close(p[1]);
        return -1;
    }
    if (pid == 0)
        child_exec(argv, cwd, -1, p[1], p[1]);
    setpgid(pid, pid);              /* also here, so that stop cannot race the child */
    close(p[1]);
    set_nonblock(p[0]);
    j->pid = pid;
    j->fd = p[0];
    return 0;
}

long shell_read(Job *j, char *buf, size_t n)
{
    ssize_t r;

    if (j->fd < 0)
        return 0;
    do
        r = read(j->fd, buf, n);
    while (r < 0 && errno == EINTR);
    if (r < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : 0;
    return (long)r;
}

void shell_stop(Job *j)
{
    if (j->pid > 0 && !j->done)
        kill(-j->pid, SIGTERM);
}

int shell_reap(Job *j)
{
    int st;
    pid_t r;

    if (j->pid <= 0)
        return 1;
    if (j->done)
        return 1;
    do
        r = waitpid(j->pid, &st, WNOHANG);
    while (r < 0 && errno == EINTR);
    if (r == j->pid) {
        j->status = st;
        j->done = 1;
        return 1;
    }
    if (r < 0) {
        j->status = 0;
        j->done = 1;
        return 1;
    }
    return 0;
}

void shell_close(Job *j)
{
    if (j->pid > 0 && !j->done) {
        int st;

        kill(-j->pid, SIGKILL);
        while (waitpid(j->pid, &st, 0) < 0 && errno == EINTR)
            ;
        j->done = 1;
        j->status = st;
    }
    if (j->fd >= 0)
        close(j->fd);
    j->fd = -1;
}

int shell_exit_code(const Job *j)
{
    if (!j->done)
        return -1;
    if (WIFEXITED(j->status))
        return WEXITSTATUS(j->status);
    if (WIFSIGNALED(j->status))
        return 128 + WTERMSIG(j->status);
    return -1;
}

/* ---- capture --------------------------------------------------------------- */

static long now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void drain(int fd, SBuf *to, size_t cap, int *open_flag)
{
    char buf[4096];
    ssize_t r = read(fd, buf, sizeof buf);

    if (r > 0) {
        if (to && to->len < cap)
            sbuf_add(to, buf, to->len + (size_t)r > cap ? cap - to->len : (size_t)r);
    } else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
        *open_flag = 0;
    }
}

int shell_capture(const char *const *argv, const char *cwd, const char *in, size_t inlen,
                  SBuf *out, SBuf *err, int timeout_ms, size_t cap, int *status)
{
    int pin[2], pout[2], perr[2];
    pid_t pid;
    size_t sent = 0;
    int out_open = 1, err_open = 1, in_open = 1, timed_out = 0, st = 0;
    long deadline = now_ms() + timeout_ms;
    struct sigaction ign, old;

    if (pipe(pin) < 0)
        return -1;
    if (pipe(pout) < 0) {
        close(pin[0]);
        close(pin[1]);
        return -1;
    }
    if (pipe(perr) < 0) {
        close(pin[0]); close(pin[1]); close(pout[0]); close(pout[1]);
        return -1;
    }
    set_cloexec(pin[0]); set_cloexec(pin[1]);
    set_cloexec(pout[0]); set_cloexec(pout[1]);
    set_cloexec(perr[0]); set_cloexec(perr[1]);
    pid = fork();
    if (pid < 0) {
        close(pin[0]); close(pin[1]); close(pout[0]); close(pout[1]);
        close(perr[0]); close(perr[1]);
        return -1;
    }
    if (pid == 0)
        child_exec(argv, cwd, pin[0], pout[1], perr[1]);
    setpgid(pid, pid);
    close(pin[0]);
    close(pout[1]);
    close(perr[1]);
    set_nonblock(pin[1]);
    set_nonblock(pout[0]);
    set_nonblock(perr[0]);
    /* a child that stops reading must not kill us with SIGPIPE */
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ign, &old);
    if (inlen == 0 || !in) {
        close(pin[1]);
        in_open = 0;
    }

    while (out_open || err_open || in_open) {
        struct pollfd f[3];
        int n = 0, i_in = -1, i_out = -1, i_err = -1;
        long left = deadline - now_ms();

        if (left <= 0) {
            timed_out = 1;
            break;
        }
        if (in_open) {
            i_in = n;
            f[n].fd = pin[1];
            f[n].events = POLLOUT;
            n++;
        }
        if (out_open) {
            i_out = n;
            f[n].fd = pout[0];
            f[n].events = POLLIN;
            n++;
        }
        if (err_open) {
            i_err = n;
            f[n].fd = perr[0];
            f[n].events = POLLIN;
            n++;
        }
        if (poll(f, (nfds_t)n, left > 1000 ? 1000 : (int)left) < 0 && errno != EINTR)
            break;
        if (i_in >= 0 && (f[i_in].revents & (POLLOUT | POLLERR | POLLHUP))) {
            ssize_t w = write(pin[1], in + sent, inlen - sent > 65536 ? 65536 : inlen - sent);

            if (w > 0)
                sent += (size_t)w;
            if (sent >= inlen || (w < 0 && errno != EAGAIN && errno != EINTR)) {
                close(pin[1]);
                in_open = 0;
            }
        }
        if (i_out >= 0 && (f[i_out].revents & (POLLIN | POLLHUP | POLLERR)))
            drain(pout[0], out, cap, &out_open);
        if (i_err >= 0 && (f[i_err].revents & (POLLIN | POLLHUP | POLLERR)))
            drain(perr[0], err, cap, &err_open);
    }
    if (in_open)
        close(pin[1]);
    sigaction(SIGPIPE, &old, NULL);
    if (timed_out)
        kill(-pid, SIGKILL);
    close(pout[0]);
    close(perr[0]);
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    if (status)
        *status = st;
    return timed_out ? -2 : 0;
}
