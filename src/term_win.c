/* The terminal on Windows: the console of Windows 10 (1809 and later) and Windows Terminal, in
 * their VT mode. The output is the same escape sequences as on POSIX; the keys come as the same
 * sequences too (ENABLE_VIRTUAL_TERMINAL_INPUT), read as console records and put as UTF-8 in the
 * buffer of term_keys.c. What a console reports as records of its own (the mouse of the classic
 * console, the focus, the size) is turned into the sequences or the events of POSIX. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
/* winuser.h's hot key modifiers: the names are term.h's */
#undef MOD_ALT
#undef MOD_CONTROL
#undef MOD_SHIFT
#undef MOD_WIN

#include "term.h"
#include "term_in.h"
#include "utf8.h"

#include <io.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef DISABLE_NEWLINE_AUTO_RETURN
#define DISABLE_NEWLINE_AUTO_RETURN 0x0008
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif

/* as on POSIX (term.c), but for ?1l, which the console has no need of */
#define SEQ_ENTER "\x1b[?1049h\x1b[2J\x1b[H\x1b[?2004h\x1b[?1004h"
#define SEQ_LEAVE "\x1b]112\x07\x1b[0m\x1b[?25h\x1b[?1006l\x1b[?1002l\x1b[?1000l" \
                  "\x1b[?1004l\x1b[?2004l\x1b[?1049l"

/* the modes the editor runs in: keys as VT sequences, the size and the focus as records; no line
 * editing, no echo, Ctrl+C as a key; the quick edit of the console off, so the mouse is ours */
#define IN_MODE (ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS)
#define OUT_MODE (ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING)

static HANDLE h_in = INVALID_HANDLE_VALUE, h_out = INVALID_HANDLE_VALUE;
static DWORD orig_in, orig_out;
static UINT orig_cp, orig_out_cp;
static volatile LONG raw_active;
static int mouse_on;
static int resized;             /* a size record came while reading keys: reported by term_wait */
static WCHAR high_surrogate;    /* the first half of a character outside the BMP */
static DWORD mouse_buttons;     /* the buttons held at the last mouse record */
static int watch_fd = -1;
static HANDLE server_ev;        /* set when a client of ipc_win.c connects */

void term_restore(void)
{
    DWORD w;

    if (InterlockedExchange(&raw_active, 0) == 0)
        return;
    WriteConsoleA(h_out, SEQ_LEAVE, (DWORD)(sizeof SEQ_LEAVE - 1), &w, NULL);
    SetConsoleMode(h_in, orig_in);
    SetConsoleMode(h_out, orig_out);
    SetConsoleCP(orig_cp);
    SetConsoleOutputCP(orig_out_cp);
}

static void on_fatal(int sig)
{
    term_restore();
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Closing the window, logging off: the console as it was. Ctrl+Break is ignored (no ISIG). */
static BOOL WINAPI on_ctrl(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT)
        return TRUE;
    term_restore();
    return FALSE;
}

int term_init(void)
{
    static const int fatal[] = { SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGTERM };
    DWORD out_mode;
    size_t i;

    h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h_out == INVALID_HANDLE_VALUE || !GetConsoleMode(h_out, &orig_out))
        return -1;
    /* the keyboard of the console even when stdin is a pipe (xted - < file) */
    h_in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
    if (h_in == INVALID_HANDLE_VALUE || !GetConsoleMode(h_in, &orig_in))
        return -1;
    out_mode = orig_out | OUT_MODE;
    if (!SetConsoleMode(h_out, out_mode | DISABLE_NEWLINE_AUTO_RETURN) && !SetConsoleMode(h_out, out_mode))
        return -1;                      /* no VT: a console older than Windows 10 */
    if (!SetConsoleMode(h_in, IN_MODE)) {
        SetConsoleMode(h_out, orig_out);
        return -1;
    }
    orig_cp = GetConsoleCP();
    orig_out_cp = GetConsoleOutputCP();
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    raw_active = 1;

    for (i = 0; i < sizeof fatal / sizeof fatal[0]; i++)
        signal(fatal[i], on_fatal);
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    atexit(term_restore);
    term_write(SEQ_ENTER, sizeof SEQ_ENTER - 1);
    return 0;
}

void term_size(int *rows, int *cols)
{
    CONSOLE_SCREEN_BUFFER_INFO bi;

    if (!GetConsoleScreenBufferInfo(h_out, &bi) || bi.srWindow.Right < bi.srWindow.Left) {
        *rows = 24;
        *cols = 80;
        return;
    }
    *cols = bi.srWindow.Right - bi.srWindow.Left + 1;
    *rows = bi.srWindow.Bottom - bi.srWindow.Top + 1;
}

void term_mouse(int on)
{
    static const char enable[] = "\x1b[?1000h\x1b[?1002h\x1b[?1006h";
    static const char disable[] = "\x1b[?1006l\x1b[?1002l\x1b[?1000l";

    if (!raw_active)
        return;
    mouse_on = on;
    /* Windows Terminal sends the mouse as VT sequences; the classic console as records */
    SetConsoleMode(h_in, IN_MODE | (on ? ENABLE_MOUSE_INPUT : 0));
    if (on)
        term_write(enable, sizeof enable - 1);
    else
        term_write(disable, sizeof disable - 1);
}

/* UTF-8 to the console in its own encoding (UTF-16): WriteConsoleW does not care where a write
 * ends, as WriteFile of UTF-8 can on some consoles. A sequence is never cut between two chunks. */
void term_write(const char *s, size_t n)
{
    WCHAR w[4096];

    while (n > 0) {
        size_t take = n < 2048 ? n : 2048;
        int wl;
        DWORD done;

        while (take < n && take > 0 && ((unsigned char)s[take] & 0xC0) == 0x80)
            take--;
        if (take == 0)
            take = n < 2048 ? n : 2048;
        wl = MultiByteToWideChar(CP_UTF8, 0, s, (int)take, w, (int)(sizeof w / sizeof w[0]));
        if (wl > 0)
            WriteConsoleW(h_out, w, (DWORD)wl, &done, NULL);
        s += take;
        n -= take;
    }
}

void term_watch(int fd)
{
    watch_fd = fd;
}

void term_watch_server(intptr_t h)
{
    server_ev = h == -1 ? NULL : (HANDLE)h;
}

/* The watched pipe (the output of Alt+R) has something to read, or is closed. */
static int watch_ready(void)
{
    DWORD n = 0;

    if (watch_fd < 0)
        return 0;
    return !PeekNamedPipe((HANDLE)_get_osfhandle(watch_fd), NULL, 0, NULL, &n, NULL) || n > 0;
}

static void put(const char *s, size_t n)
{
    if (term_inlen + n <= sizeof term_inbuf) {
        memcpy(term_inbuf + term_inlen, s, n);
        term_inlen += n;
    }
}

/* A character of a key, UTF-16 from the console, as UTF-8. */
static void put_char(WCHAR c)
{
    char b[4];
    uint32_t cp;

    if (c >= 0xD800 && c <= 0xDBFF) {
        high_surrogate = c;
        return;
    }
    if (c >= 0xDC00 && c <= 0xDFFF) {
        if (!high_surrogate)
            return;
        cp = 0x10000 + (((uint32_t)high_surrogate - 0xD800) << 10) + ((uint32_t)c - 0xDC00);
        high_surrogate = 0;
    } else {
        high_surrogate = 0;
        cp = c;
    }
    put(b, (size_t)utf8_encode(cp, b));
}

/* A mouse record of the classic console as the SGR sequence a terminal sends. */
static void put_mouse(const MOUSE_EVENT_RECORD *m)
{
    static const DWORD bit[3] = { FROM_LEFT_1ST_BUTTON_PRESSED, FROM_LEFT_2ND_BUTTON_PRESSED,
                                  RIGHTMOST_BUTTON_PRESSED };
    CONSOLE_SCREEN_BUFFER_INFO bi;
    int x = m->dwMousePosition.X, y = m->dwMousePosition.Y, mods = 0, i;
    DWORD ks = m->dwControlKeyState, now = m->dwButtonState & 7;
    char seq[48];

    if (!mouse_on)
        return;
    if (GetConsoleScreenBufferInfo(h_out, &bi)) {
        x -= bi.srWindow.Left;
        y -= bi.srWindow.Top;
    }
    if (x < 0 || y < 0)
        return;
    if (ks & SHIFT_PRESSED)
        mods |= 4;
    if (ks & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED))
        mods |= 8;
    if (ks & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))
        mods |= 16;
    if (m->dwEventFlags & MOUSE_WHEELED) {
        int up = (SHORT)HIWORD(m->dwButtonState) > 0;

        put(seq, (size_t)snprintf(seq, sizeof seq, "\x1b[<%d;%d;%dM", (up ? 64 : 65) | mods, x + 1, y + 1));
        return;
    }
    if (m->dwEventFlags & MOUSE_HWHEELED)
        return;
    for (i = 0; i < 3; i++) {
        int was = (mouse_buttons & bit[i]) != 0, is = (now & bit[i]) != 0;

        if (is != was)
            put(seq, (size_t)snprintf(seq, sizeof seq, "\x1b[<%d;%d;%d%c", i | mods, x + 1, y + 1,
                                      is ? 'M' : 'm'));
        else if (is && (m->dwEventFlags & MOUSE_MOVED))
            put(seq, (size_t)snprintf(seq, sizeof seq, "\x1b[<%d;%d;%dM", i | 32 | mods, x + 1, y + 1));
    }
    mouse_buttons = now;
}

/* Reads the records that are there (it never waits) into the buffer, which has room for them:
 * keys and the mouse as bytes, a new size in resized. */
static void read_records(void)
{
    INPUT_RECORD rec[64];
    DWORD n = 0, i, avail = 0;

    if (!GetNumberOfConsoleInputEvents(h_in, &avail) || avail == 0)
        return;
    /* each record makes at most ~40 bytes: read no more than fit */
    if (avail > (DWORD)((sizeof term_inbuf - term_inlen) / 48))
        avail = (DWORD)((sizeof term_inbuf - term_inlen) / 48);
    if (avail > 64)
        avail = 64;
    if (avail == 0 || !ReadConsoleInputW(h_in, rec, avail, &n))
        return;
    for (i = 0; i < n; i++) {
        switch (rec[i].EventType) {
        case KEY_EVENT: {
            const KEY_EVENT_RECORD *k = &rec[i].Event.KeyEvent;
            WORD r;

            if (!k->bKeyDown || k->uChar.UnicodeChar == 0)
                break;              /* in VT mode every key is in the characters */
            for (r = 0; r < (k->wRepeatCount ? k->wRepeatCount : 1) && r < 8; r++)
                put_char(k->uChar.UnicodeChar);
            break;
        }
        case MOUSE_EVENT:
            put_mouse(&rec[i].Event.MouseEvent);
            break;
        case WINDOW_BUFFER_SIZE_EVENT:
            resized = 1;
            break;
        case FOCUS_EVENT:
            put(rec[i].Event.FocusEvent.bSetFocus ? "\x1b[I" : "\x1b[O", 3);
            break;
        default:
            break;
        }
    }
}

/* Waits up to ms (< 0: no limit) for a record; 1 when there is one. */
static int wait_records(int ms)
{
    return WaitForSingleObject(h_in, ms < 0 ? INFINITE : (DWORD)ms) == WAIT_OBJECT_0;
}

/* As wait_records, also for a client of ipc_win.c: 2 when one has come. */
static int wait_any(int ms)
{
    HANDLE h[2];
    DWORD r;

    h[0] = h_in;
    h[1] = server_ev;
    r = WaitForMultipleObjects(server_ev ? 2 : 1, h, FALSE, ms < 0 ? INFINITE : (DWORD)ms);
    return r == WAIT_OBJECT_0 ? 1 : r == WAIT_OBJECT_0 + 1 ? 2 : 0;
}

int term_fill(int timeout_ms)
{
    ULONGLONG end = GetTickCount64() + (ULONGLONG)(timeout_ms > 0 ? timeout_ms : 0);

    term_inpos = term_inlen = 0;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        int left = now >= end ? 0 : (int)(end - now);

        if (!wait_records(left))
            return 0;
        read_records();
        if (term_inlen > 0)
            return 1;
        if (left == 0)
            return 0;
    }
}

int term_wait(int timeout_ms)
{
    ULONGLONG end = GetTickCount64() + (ULONGLONG)(timeout_ms > 0 ? timeout_ms : 0);
    int ev = 0, w;

    if (term_inpos < term_inlen || term_in_eof)
        return TERM_EV_KEY;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        int left = timeout_ms < 0 ? -1 : now >= end ? 0 : (int)(end - now);

        if (resized) {
            resized = 0;
            return TERM_EV_RESIZE;
        }
        if (watch_ready())
            return ev | TERM_EV_FD;
        /* a watched command (Alt+R) is looked at every 20 ms: an anonymous pipe cannot be
         * waited on together with the console */
        if (watch_fd >= 0 && (left < 0 || left > 20))
            left = 20;
        w = wait_any(left);
        if (w == 2)
            return ev | TERM_EV_SERVER;
        if (w == 1) {
            term_inpos = term_inlen = 0;
            read_records();
            if (term_inlen > 0)
                ev |= TERM_EV_KEY;
            if (resized) {
                resized = 0;
                ev |= TERM_EV_RESIZE;
            }
        }
        if (watch_ready())
            ev |= TERM_EV_FD;
        if (ev || (timeout_ms >= 0 && GetTickCount64() >= end))
            return ev;
    }
}
