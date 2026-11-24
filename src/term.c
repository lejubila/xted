#include "term.h"
#include "term_in.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* ?1l: normal cursor keys (arrows as ESC [ A), so that ESC O A can only be PuTTY's Ctrl+Up */
#define SEQ_ENTER "\x1b[?1049h\x1b[2J\x1b[H\x1b[?2004h\x1b[?1004h\x1b[?1l"
#define SEQ_LEAVE "\x1b]112\x07\x1b[0 q\x1b[0m\x1b[?25h\x1b[?1006l\x1b[?1002l\x1b[?1000l" \
                  "\x1b[?1004l\x1b[?2004l\x1b[?1049l"

static struct termios orig_tio;
static volatile sig_atomic_t raw_active;
static int wake_pipe[2] = { -1, -1 };
static int watch_fd = -1, server_fd = -1;

static int in_fd = STDIN_FILENO;    /* the terminal: /dev/tty when stdin is a pipe */

void term_restore(void)
{
    ssize_t r;

    if (!raw_active)
        return;
    raw_active = 0;
    r = write(STDOUT_FILENO, SEQ_LEAVE, sizeof SEQ_LEAVE - 1);
    (void)r;
    tcsetattr(in_fd, TCSAFLUSH, &orig_tio);
}

static void on_fatal(int sig)
{
    term_restore();
    raise(sig);
}

/* Wakes poll() through a self-pipe: no race between the signal and the wait. */
static void on_winch(int sig)
{
    int saved = errno;
    ssize_t r;

    (void)sig;
    r = write(wake_pipe[1], "w", 1);
    (void)r;
    errno = saved;
}

static void set_signals(void)
{
    static const int fatal[] = { SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL,
                                 SIGTERM, SIGHUP, SIGINT, SIGQUIT };
    struct sigaction sa;
    size_t i;

    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND | SA_NODEFER;
    sa.sa_handler = on_fatal;
    for (i = 0; i < sizeof fatal / sizeof fatal[0]; i++)
        sigaction(fatal[i], &sa, NULL);

    sa.sa_flags = 0;
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);
}

int term_init(void)
{
    struct termios raw;
    int i;

    if (!isatty(STDOUT_FILENO))
        return -1;
    if (!isatty(STDIN_FILENO)) {
        in_fd = open("/dev/tty", O_RDONLY | O_CLOEXEC);
        if (in_fd < 0) {
            in_fd = STDIN_FILENO;
            return -1;
        }
    }
    if (tcgetattr(in_fd, &orig_tio) < 0 || pipe(wake_pipe) < 0)
        return -1;
    for (i = 0; i < 2; i++) {
        fcntl(wake_pipe[i], F_SETFD, FD_CLOEXEC);
        fcntl(wake_pipe[i], F_SETFL, fcntl(wake_pipe[i], F_GETFL) | O_NONBLOCK);
    }

    raw = orig_tio;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON | INLCR | IGNCR);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;     /* reads happen only after poll(), so never block */
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(in_fd, TCSAFLUSH, &raw) < 0)
        return -1;
    raw_active = 1;

    set_signals();
    atexit(term_restore);
    term_write(SEQ_ENTER, sizeof SEQ_ENTER - 1);
    return 0;
}

void term_size(int *rows, int *cols)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0 || ws.ws_col == 0 || ws.ws_row == 0) {
        *rows = 24;
        *cols = 80;
    } else {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    }
}

void term_mouse(int on)
{
    static const char enable[] = "\x1b[?1000h\x1b[?1002h\x1b[?1006h";
    static const char disable[] = "\x1b[?1006l\x1b[?1002l\x1b[?1000l";

    if (!raw_active)
        return;
    if (on)
        term_write(enable, sizeof enable - 1);
    else
        term_write(disable, sizeof disable - 1);
}

void term_write(const char *s, size_t n)
{
    while (n > 0) {
        ssize_t w = write(STDOUT_FILENO, s, n);

        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) {
                struct pollfd p = { STDOUT_FILENO, POLLOUT, 0 };

                poll(&p, 1, -1);
                continue;
            }
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

/* Reads from stdin into the (empty) input buffer; the caller has
 * already seen the descriptor become readable. */
static void fill_input(void)
{
    ssize_t n;

    do
        n = read(in_fd, term_inbuf, sizeof term_inbuf);
    while (n < 0 && errno == EINTR);
    if (n < 0 && errno == EAGAIN)
        return;
    if (n <= 0) {
        term_in_eof = 1;
        return;
    }
    term_inpos = 0;
    term_inlen = (size_t)n;
}

int term_fill(int timeout_ms)
{
    struct pollfd p = { in_fd, POLLIN, 0 };
    int r;

    do
        r = poll(&p, 1, timeout_ms);
    while (r < 0 && errno == EINTR);
    if (r <= 0)
        return 0;
    fill_input();
    return 1;
}

void term_watch(int fd)
{
    watch_fd = fd;
}

void term_watch_server(intptr_t h)
{
    server_fd = (int)h;
}

int term_wait(int timeout_ms)
{
    struct pollfd fds[4];
    int r, ev = 0, nfds = 2, i_watch = -1, i_server = -1;

    if (term_inpos < term_inlen || term_in_eof)
        return TERM_EV_KEY;

    fds[0].fd = in_fd;
    fds[0].events = POLLIN;
    fds[1].fd = wake_pipe[0];
    fds[1].events = POLLIN;
    if (watch_fd >= 0) {
        i_watch = nfds++;
        fds[i_watch].fd = watch_fd;
        fds[i_watch].events = POLLIN;
        fds[i_watch].revents = 0;
    }
    if (server_fd >= 0) {
        i_server = nfds++;
        fds[i_server].fd = server_fd;
        fds[i_server].events = POLLIN;
        fds[i_server].revents = 0;
    }
    do
        r = poll(fds, (nfds_t)nfds, timeout_ms);
    while (r < 0 && errno == EINTR);
    if (r <= 0)
        return 0;

    if (fds[1].revents & POLLIN) {
        char tmp[16];

        while (read(wake_pipe[0], tmp, sizeof tmp) > 0) {
        }
        ev |= TERM_EV_RESIZE;
    }
    if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
        fill_input();
        ev |= TERM_EV_KEY;
    }
    if (i_watch >= 0 && (fds[i_watch].revents & (POLLIN | POLLHUP | POLLERR)))
        ev |= TERM_EV_FD;
    if (i_server >= 0 && (fds[i_server].revents & POLLIN))
        ev |= TERM_EV_SERVER;
    return ev;
}
