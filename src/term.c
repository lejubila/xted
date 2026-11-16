#include "term.h"
#include "utf8.h"
#include "util.h"

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
#define SEQ_LEAVE "\x1b]112\x07\x1b[0m\x1b[?25h\x1b[?1006l\x1b[?1002l\x1b[?1000l" \
                  "\x1b[?1004l\x1b[?2004l\x1b[?1049l"
#define PASTE_END "\x1b[201~"

/* How long to wait for the rest of an escape sequence before deciding
 * that ESC was a keypress on its own. */
#define ESC_TIMEOUT_MS 50
/* A paste is data in flight, not a keypress: only give up if it stalls. */
#define PASTE_TIMEOUT_MS 2000

static struct termios orig_tio;
static volatile sig_atomic_t raw_active;
static int wake_pipe[2] = { -1, -1 };
static int watch_fd = -1;

static unsigned char inbuf[4096];
static int in_fd = STDIN_FILENO;    /* the terminal: /dev/tty when stdin is a pipe */
static size_t inpos, inlen;
static int in_eof;
static SBuf paste_buf;
static MouseEv last_mouse;

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

MouseEv term_mouse_event(void)
{
    return last_mouse;
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
        n = read(in_fd, inbuf, sizeof inbuf);
    while (n < 0 && errno == EINTR);
    if (n < 0 && errno == EAGAIN)
        return;
    if (n <= 0) {
        in_eof = 1;
        return;
    }
    inpos = 0;
    inlen = (size_t)n;
}

int term_has_input(void)
{
    return inpos < inlen;
}

void term_watch(int fd)
{
    watch_fd = fd;
}

int term_wait(int timeout_ms)
{
    struct pollfd fds[3];
    int r, ev = 0, nfds = 2;

    if (inpos < inlen || in_eof)
        return TERM_EV_KEY;

    fds[0].fd = in_fd;
    fds[0].events = POLLIN;
    fds[1].fd = wake_pipe[0];
    fds[1].events = POLLIN;
    if (watch_fd >= 0) {
        fds[2].fd = watch_fd;
        fds[2].events = POLLIN;
        fds[2].revents = 0;
        nfds = 3;
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
    if (nfds == 3 && (fds[2].revents & (POLLIN | POLLHUP | POLLERR)))
        ev |= TERM_EV_FD;
    return ev;
}

/* Next input byte, waiting up to timeout_ms for it; -1 if none. */
static int next_byte(int timeout_ms)
{
    if (inpos >= inlen && !in_eof) {
        struct pollfd p = { in_fd, POLLIN, 0 };
        int r;

        do
            r = poll(&p, 1, timeout_ms);
        while (r < 0 && errno == EINTR);
        if (r <= 0)
            return -1;
        fill_input();
    }
    if (inpos < inlen)
        return inbuf[inpos++];
    return -1;
}

static uint32_t read_utf8(int lead)
{
    int need, n = 1;
    char b[4];
    uint32_t cp;

    if (lead >= 0xF0 && lead <= 0xF4)
        need = 3;
    else if (lead >= 0xE0 && lead <= 0xEF)
        need = 2;
    else if (lead >= 0xC2 && lead <= 0xDF)
        need = 1;
    else
        return KEY_NONE;

    b[0] = (char)lead;
    while (n <= need) {
        int c = next_byte(ESC_TIMEOUT_MS);

        if (c < 0)
            return KEY_NONE;
        if ((c & 0xC0) != 0x80) {
            inpos--;    /* not ours: push it back */
            return KEY_NONE;
        }
        b[n++] = (char)c;
    }
    if (utf8_decode(b, (size_t)n, &cp) != n)
        return KEY_NONE;
    return cp;
}

static int xterm_mods(int m)
{
    int mods = 0;

    if (m > 1) {
        m -= 1;
        if (m & 1)
            mods |= MOD_SHIFT;
        if (m & 2)
            mods |= MOD_ALT;
        if (m & 4)
            mods |= MOD_CTRL;
    }
    return mods;
}

static uint32_t final_key(int c)
{
    switch (c) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case 'P': return KEY_F1;
    case 'Q': return KEY_F2;
    case 'R': return KEY_F3;
    case 'S': return KEY_F4;
    default: return KEY_NONE;
    }
}

/* The key of ESC O x. The cursor keys are in normal mode (SEQ_ENTER), where a terminal sends
 * ESC [ A for Up: ESC O A is then PuTTY's Ctrl+Up, and rxvt writes Ctrl+Up as ESC O a. */
static void ss3_key(Key *k, int c)
{
    if (c >= 'A' && c <= 'D') {
        k->code = final_key(c);
        k->mods |= MOD_CTRL;
    } else if (c >= 'a' && c <= 'd') {
        k->code = final_key(c - 32);
        k->mods |= MOD_CTRL;
    } else {
        k->code = final_key(c);
    }
}

/* Reads up to the closing ESC[201~, which is dropped from the text. */
static void read_paste(void)
{
    size_t endlen = sizeof PASTE_END - 1;

    paste_buf.len = 0;
    for (;;) {
        int c = next_byte(PASTE_TIMEOUT_MS);
        char ch;

        if (c < 0)
            break;
        ch = (char)c;
        sbuf_add(&paste_buf, &ch, 1);
        if (paste_buf.len >= endlen &&
            memcmp(paste_buf.p + paste_buf.len - endlen, PASTE_END, endlen) == 0) {
            paste_buf.len -= endlen;
            break;
        }
    }
}

const char *term_paste_text(size_t *n)
{
    *n = paste_buf.len;
    return paste_buf.p ? paste_buf.p : "";
}

static void parse_csi(Key *k)
{
    int params[3] = { 0, 0, 0 }, idx = 0, c, sgr = 0;

    for (;;) {
        c = next_byte(ESC_TIMEOUT_MS);
        if (c < 0)
            return;
        if (c >= '0' && c <= '9') {
            if (params[idx] < 10000)
                params[idx] = params[idx] * 10 + (c - '0');
        } else if (c == ';') {
            if (idx < 2)
                idx++;
        } else if (c >= 0x20 && c <= 0x3F) {
            if (c == '<')
                sgr = 1;
            continue;
        } else if (c >= 0x40 && c <= 0x7E) {
            break;
        } else {
            return;
        }
    }

    if (sgr) {
        /* mouse, "CSI < button ; x ; y M" (press, motion) or "m" (release) */
        int b = params[0];

        if ((c != 'M' && c != 'm') || idx != 2 || params[1] < 1 || params[2] < 1)
            return;
        k->mods = ((b & 4) ? MOD_SHIFT : 0) | ((b & 8) ? MOD_ALT : 0) | ((b & 16) ? MOD_CTRL : 0);
        last_mouse.x = params[1] - 1;
        last_mouse.y = params[2] - 1;
        last_mouse.mods = k->mods;
        last_mouse.button = b & 3;
        if (b & 64) {
            if ((b & 3) > 1)
                return;                 /* horizontal wheel */
            last_mouse.kind = (b & 3) == 0 ? MOUSE_WHEEL_UP : MOUSE_WHEEL_DOWN;
        } else if (b & 32) {
            if ((b & 3) == 3)
                return;                 /* motion without a button */
            last_mouse.kind = MOUSE_DRAG;
        } else {
            last_mouse.kind = c == 'm' ? MOUSE_RELEASE : MOUSE_PRESS;
        }
        k->code = KEY_MOUSE;
        return;
    }
    k->mods = xterm_mods(params[1]);
    if (c == 'u' && params[0] == '\t') {
        /* Tab with modifiers, in the "CSI u" encoding (kitty, foot, WezTerm, ...) */
        k->code = KEY_TAB;
    } else if (c == '~' && params[0] == 27 && params[2] == '\t') {
        /* the same in xterm's modifyOtherKeys encoding */
        k->code = KEY_TAB;
    } else if (c == '~') {
        switch (params[0]) {
        case 1: case 7: k->code = KEY_HOME; break;
        case 2: k->code = KEY_INS; break;
        case 3: k->code = KEY_DEL; break;
        case 4: case 8: k->code = KEY_END; break;
        case 5: k->code = KEY_PGUP; break;
        case 6: k->code = KEY_PGDN; break;
        case 11: case 12: case 13: case 14: k->code = KEY_F1 + (uint32_t)(params[0] - 11); break;
        case 15: k->code = KEY_F5; break;
        case 17: case 18: case 19: case 20: case 21:
            k->code = KEY_F6 + (uint32_t)(params[0] - 17);
            break;
        case 23: case 24: k->code = KEY_F11 + (uint32_t)(params[0] - 23); break;
        case 200:
            read_paste();
            k->code = KEY_PASTE;
            break;
        default: break;
        }
    } else if (c == 'I' && idx == 0 && params[0] == 0) {
        k->code = KEY_FOCUS_IN;
    } else if (c == 'O' && idx == 0 && params[0] == 0) {
        k->code = KEY_FOCUS_OUT;
    } else if (c == 'Z') {
        k->code = KEY_TAB;
        k->mods |= MOD_SHIFT;
    } else if (c >= 'a' && c <= 'd' && idx == 0 && params[0] == 0) {
        k->code = final_key(c - 32);    /* rxvt's Shift+arrow */
        k->mods |= MOD_SHIFT;
    } else {
        k->code = final_key(c);
    }
}

static void parse_escape(Key *k)
{
    int c = next_byte(ESC_TIMEOUT_MS);

    if (c < 0) {
        k->code = KEY_ESC;
        return;
    }
    if (c == '[') {
        parse_csi(k);
    } else if (c == 'O') {
        c = next_byte(ESC_TIMEOUT_MS);
        if (c >= 0)
            ss3_key(k, c);
    } else if (c == 27) {
        /* PuTTY's Alt: ESC before the key's own sequence (ESC ESC [ C for Alt+Right, ESC ESC O C
         * in application mode); two ESC alone are an Esc */
        c = next_byte(ESC_TIMEOUT_MS);
        if (c == '[') {
            parse_csi(k);
        } else if (c == 'O') {
            c = next_byte(ESC_TIMEOUT_MS);
            if (c >= 0)
                ss3_key(k, c);
        } else {
            if (c >= 0)
                inpos--;                /* not a sequence: leave it for the next key */
            k->code = KEY_ESC;
            return;
        }
        if (k->code != KEY_NONE && k->code != KEY_MOUSE && k->code != KEY_PASTE)
            k->mods |= MOD_ALT;
    } else {
        k->mods = MOD_ALT;
        k->code = c < 0x80 ? (uint32_t)c : read_utf8(c);
    }
}

void term_read_key(Key *k)
{
    int c;

    k->code = KEY_NONE;
    k->mods = 0;
    c = next_byte(0);
    if (c < 0) {
        if (in_eof)
            k->code = KEY_EOF;
        return;
    }
    if (c == 27)
        parse_escape(k);
    else if (c < 0x80)
        k->code = (uint32_t)c;
    else
        k->code = read_utf8(c);
}
