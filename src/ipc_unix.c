/* The channel of ipc.h on POSIX: a Unix socket in $XDG_RUNTIME_DIR (a folder of the user's
 * only), else in /tmp/xted-<uid>, made 0700 and checked to be ours. */

#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SEND_TIMEOUT_MS 2000        /* an xted that does not answer is as none */
#define SERVE_TIMEOUT_MS 1000       /* a client that does not finish is dropped */
#define MAX_REQUEST (256u << 10)

static int listen_fd = -1;
static char listen_path[sizeof ((struct sockaddr_un *)0)->sun_path];

/* The socket's address; 0 if there is no safe place for it. */
static int address(struct sockaddr_un *a)
{
    const char *x = getenv("XTED_IPC"), *rt = getenv("XDG_RUNTIME_DIR");
    char dir[sizeof a->sun_path];
    struct stat st;
    int n;

    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    if (x && x[0]) {
        n = snprintf(a->sun_path, sizeof a->sun_path, "%s", x);
    } else if (rt && rt[0] == '/' && stat(rt, &st) == 0 && S_ISDIR(st.st_mode)) {
        n = snprintf(a->sun_path, sizeof a->sun_path, "%s/xted.sock", rt);
    } else {
        snprintf(dir, sizeof dir, "/tmp/xted-%lu", (unsigned long)getuid());
        if (mkdir(dir, 0700) < 0 && errno != EEXIST)
            return 0;
        /* someone else's folder, or one that others can write, is not a place for it */
        if (lstat(dir, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
            (st.st_mode & 077))
            return 0;
        n = snprintf(a->sun_path, sizeof a->sun_path, "%s/xted.sock", dir);
    }
    return n > 0 && (size_t)n < sizeof a->sun_path;
}

static long now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* Waits up to ms for fd to be ready for events; 1 when it is. */
static int wait_fd(int fd, short events, long ms)
{
    struct pollfd p;
    int r;

    p.fd = fd;
    p.events = events;
    do
        r = poll(&p, 1, ms < 0 ? 0 : (int)ms);
    while (r < 0 && errno == EINTR);
    return r > 0;
}

static int write_all(int fd, const char *s, size_t n, long deadline)
{
    while (n > 0) {
        ssize_t w;

        if (!wait_fd(fd, POLLOUT, deadline - now_ms()))
            return -1;
        w = write(fd, s, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        s += w;
        n -= (size_t)w;
    }
    return 0;
}

static int connect_to(const struct sockaddr_un *a)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (connect(fd, (const struct sockaddr *)a, sizeof *a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int ipc_send(const IpcFile *f, size_t n)
{
    struct sockaddr_un a;
    SBuf req = SBUF_INIT;
    long deadline = now_ms() + SEND_TIMEOUT_MS;
    char ans[8];
    size_t got = 0;
    int fd, ok = 0;

    if (!address(&a) || (fd = connect_to(&a)) < 0)
        return 0;
    ipc_request(&req, f, n);
    if (write_all(fd, req.p, req.len, deadline) == 0) {
        while (got < 3 && wait_fd(fd, POLLIN, deadline - now_ms())) {
            ssize_t r = read(fd, ans + got, sizeof ans - 1 - got);

            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0)
                break;
            got += (size_t)r;
        }
        ok = got >= 3 && memcmp(ans, "ok\n", 3) == 0;
    }
    sbuf_free(&req);
    close(fd);
    return ok;
}

intptr_t ipc_listen(void)
{
    struct sockaddr_un a;
    int fd;

    if (listen_fd >= 0)
        return listen_fd;
    if (!address(&a))
        return -1;
    /* one that answers: it is the one that listens */
    fd = connect_to(&a);
    if (fd >= 0) {
        close(fd);
        return -1;
    }
    {
        struct stat st;

        if (lstat(a.sun_path, &st) == 0 && S_ISSOCK(st.st_mode))
            unlink(a.sun_path);             /* left by an xted that ended badly */
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 8) < 0) {
        close(fd);                          /* another one started at the same moment */
        return -1;
    }
    chmod(a.sun_path, 0600);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    listen_fd = fd;
    memcpy(listen_path, a.sun_path, sizeof listen_path);
    return fd;
}

/* One client: its request into the queue, the answer. */
static void serve_one(int c)
{
    SBuf b = SBUF_INIT;
    long deadline = now_ms() + SERVE_TIMEOUT_MS;
    char buf[4096];

    while (b.len < MAX_REQUEST && !ipc_complete(b.p, b.len) && wait_fd(c, POLLIN, deadline - now_ms())) {
        ssize_t r = read(c, buf, sizeof buf);

        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        sbuf_add(&b, buf, (size_t)r);
    }
    if (b.p && ipc_complete(b.p, b.len)) {
        ipc_queue(b.p, b.len);
        write_all(c, "ok\n", 3, now_ms() + SERVE_TIMEOUT_MS);
    }
    sbuf_free(&b);
}

void ipc_serve(void)
{
    int c;

    if (listen_fd < 0)
        return;
    while ((c = accept(listen_fd, NULL, NULL)) >= 0) {
        fcntl(c, F_SETFD, FD_CLOEXEC);
        fcntl(c, F_SETFL, fcntl(c, F_GETFL) & ~O_NONBLOCK);
        serve_one(c);
        close(c);
    }
}

void ipc_close(void)
{
    if (listen_fd < 0)
        return;
    close(listen_fd);
    listen_fd = -1;
    unlink(listen_path);
}
