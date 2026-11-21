/* Commands on Windows: CreateProcess in a job object (stopping a command stops what it started,
 * as a process group does on POSIX), with a hidden console of their own, so that nothing they
 * write reaches the editor's screen. A command line goes to cmd.exe (or %COMSPEC%) where POSIX
 * has /bin/sh -c. The output comes as UTF-8 with LF, as on POSIX: what is not UTF-8 already
 * (Git, Python with PYTHONUTF8) is in the OEM code page of the console, as cmd.exe and its
 * tools write it, and is converted; CR LF is LF. (No "chcp 65001": with it sort.exe writes
 * nothing.) The output pipe is a descriptor of the C library, read without blocking through
 * PeekNamedPipe. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "shell.h"

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- command lines --------------------------------------------------------- */

/* The cmd.exe line of a POSIX "sh -c" command. */
static void shell_line(SBuf *b, const char *cmd)
{
    const char *sh = getenv("COMSPEC");

    sbuf_add(b, "\"", 1);
    sbuf_adds(b, sh && sh[0] ? sh : "cmd.exe");
    /* /s: what is between the outer quotes is the command, as it is */
    sbuf_adds(b, "\" /d /s /c \"");
    sbuf_adds(b, cmd);
    sbuf_add(b, "\"", 1);
}

/* An argument as the C runtime of the program splits it again (CommandLineToArgvW's rules). */
static void quote_arg(SBuf *b, const char *a)
{
    size_t bs = 0;

    if (a[0] && !strpbrk(a, " \t\"")) {
        sbuf_adds(b, a);
        return;
    }
    sbuf_add(b, "\"", 1);
    for (; *a; a++) {
        if (*a == '\\') {
            bs++;
            continue;
        }
        if (*a == '"')
            bs = bs * 2 + 1;            /* the backslashes before a quote, and the quote, escaped */
        while (bs) {
            sbuf_add(b, "\\", 1);
            bs--;
        }
        sbuf_add(b, a, 1);
    }
    for (bs *= 2; bs; bs--)             /* before the closing quote: doubled */
        sbuf_add(b, "\\", 1);
    sbuf_add(b, "\"", 1);
}

/* argv as a command line; the "/bin/sh -c cmd" of the callers as cmd.exe's. */
static char *command_line(const char *const *argv)
{
    SBuf b = SBUF_INIT;
    size_t i;

    if (argv[0] && strcmp(argv[0], "/bin/sh") == 0 && argv[1] && strcmp(argv[1], "-c") == 0 && argv[2]) {
        shell_line(&b, argv[2]);
    } else {
        for (i = 0; argv[i]; i++) {
            if (i)
                sbuf_add(&b, " ", 1);
            quote_arg(&b, argv[i]);
        }
    }
    sbuf_add(&b, "", 1);
    return sbuf_steal(&b);
}

/* ---- the encoding of the output --------------------------------------------- */

/* Whether s[0, n) is UTF-8; *ok is how much of it is whole characters (a cut one may end it). */
static int utf8_prefix(const unsigned char *s, size_t n, size_t *ok)
{
    size_t i = 0;

    while (i < n) {
        size_t need, k;

        if (s[i] < 0x80) {
            i++;
            continue;
        }
        if (s[i] >= 0xC2 && s[i] <= 0xDF)
            need = 1;
        else if (s[i] >= 0xE0 && s[i] <= 0xEF)
            need = 2;
        else if (s[i] >= 0xF0 && s[i] <= 0xF4)
            need = 3;
        else
            return 0;
        for (k = 1; k <= need; k++) {
            if (i + k >= n) {
                *ok = i;                /* cut at the end: the rest comes with the next read */
                return 1;
            }
            if ((s[i + k] & 0xC0) != 0x80)
                return 0;
        }
        i += need + 1;
    }
    *ok = n;
    return 1;
}

/* The OEM code page of the system (850 in Italy), the one of a console and of what cmd.exe
 * writes: not CP_OEMCP, which the manifest makes UTF-8 for xted too. */
static UINT oem_cp(void)
{
    static UINT cp;
    WCHAR v[8];

    if (!cp)
        cp = GetLocaleInfoW(LOCALE_SYSTEM_DEFAULT, LOCALE_IDEFAULTCODEPAGE, v, 8) ? (UINT)_wtoi(v) : 0;
    if (!cp || cp == CP_UTF8)
        cp = 437;
    return cp;
}

/* s[0, n) in the OEM code page as UTF-8 in out (room for 3 bytes each); its length. */
static size_t oem_to_utf8(const char *s, size_t n, char *out, size_t cap)
{
    WCHAR w[4096];
    int wl, r;

    if (n == 0)
        return 0;
    if (n > sizeof w / sizeof w[0])
        n = sizeof w / sizeof w[0];
    wl = MultiByteToWideChar(oem_cp(), 0, s, (int)n, w, (int)(sizeof w / sizeof w[0]));
    if (wl <= 0)
        return 0;
    r = WideCharToMultiByte(CP_UTF8, 0, w, wl, out, (int)cap, NULL, NULL);
    return r > 0 ? (size_t)r : 0;
}

/* The whole output of a command as UTF-8. */
static void to_utf8(SBuf *b)
{
    size_t ok, i, n;
    SBuf o = SBUF_INIT;
    char tmp[3 * 2048];

    if (!b || !b->p || (utf8_prefix((const unsigned char *)b->p, b->len, &ok) && ok == b->len))
        return;
    for (i = 0; i < b->len; i += 2048) {
        n = b->len - i < 2048 ? b->len - i : 2048;
        sbuf_add(&o, tmp, oem_to_utf8(b->p + i, n, tmp, sizeof tmp));
    }
    sbuf_free(b);
    *b = o;
}

/* ---- processes ------------------------------------------------------------- */

static HANDLE inheritable_null(void)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };

    return CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                       0, NULL);
}

/* A pipe whose write end the child inherits; the read end stays ours. */
static int out_pipe(HANDLE *rd, HANDLE *wr)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };

    if (!CreatePipe(rd, wr, &sa, 1 << 16))
        return -1;
    SetHandleInformation(*rd, HANDLE_FLAG_INHERIT, 0);
    return 0;
}

/* Starts line with the standard handles given, suspended in a new job object that ends with it;
 * 0, or -1 with GetLastError() set. */
static int spawn(char *line, const char *cwd, HANDLE in, HANDLE out, HANDLE err, HANDLE *proc,
                 HANDLE *group)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
    HANDLE g = CreateJobObjectA(NULL, NULL);
    DWORD e;

    /* Python writes UTF-8 to a pipe only when asked (else the ANSI code page, not the OEM one) */
    if (!getenv("PYTHONUTF8"))
        _putenv_s("PYTHONUTF8", "1");
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = out;
    si.hStdError = err;
    if (g) {
        memset(&lim, 0, sizeof lim);
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g, JobObjectExtendedLimitInformation, &lim, sizeof lim);
    }
    if (!CreateProcessA(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL,
                        cwd && cwd[0] ? cwd : NULL, &si, &pi)) {
        e = GetLastError();
        if (g)
            CloseHandle(g);
        SetLastError(e);
        return -1;
    }
    if (g && !AssignProcessToJobObject(g, pi.hProcess)) {
        CloseHandle(g);
        g = NULL;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    *proc = pi.hProcess;
    *group = g;
    return 0;
}

static void win_error(char *err, size_t errlen)
{
    DWORD e = GetLastError();
    char *msg = NULL;
    size_t n;

    if (FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, (char *)&msg, 0, NULL) && msg) {
        n = strlen(msg);
        while (n > 0 && (msg[n - 1] == '\r' || msg[n - 1] == '\n' || msg[n - 1] == '.'))
            msg[--n] = '\0';
        snprintf(err, errlen, "%s", msg);
        LocalFree(msg);
    } else {
        snprintf(err, errlen, "error %lu", (unsigned long)e);
    }
}

static void stop_group(Job *j)
{
    if (j->group)
        TerminateJobObject(j->group, 1);
    else if (j->proc)
        TerminateProcess(j->proc, 1);
}

int shell_start(Job *j, const char *cmd, const char *cwd, char *err, size_t errlen)
{
    const char *argv[] = { "/bin/sh", "-c", cmd, NULL };
    HANDLE rd, wr, nul;
    char *line;
    int r;

    memset(j, 0, sizeof *j);
    j->fd = -1;
    if (out_pipe(&rd, &wr) < 0) {
        win_error(err, errlen);
        return -1;
    }
    nul = inheritable_null();
    line = command_line(argv);
    r = spawn(line, cwd, nul, wr, wr, (HANDLE *)&j->proc, (HANDLE *)&j->group);
    if (r < 0)
        win_error(err, errlen);
    free(line);
    CloseHandle(wr);                    /* the child's now: its end closes the pipe at its exit */
    if (nul != INVALID_HANDLE_VALUE)
        CloseHandle(nul);
    if (r < 0) {
        CloseHandle(rd);
        return -1;
    }
    j->pid = (pid_t)GetProcessId(j->proc);
    j->fd = _open_osfhandle((intptr_t)rd, _O_RDONLY | _O_BINARY);
    if (j->fd < 0) {
        CloseHandle(rd);
        stop_group(j);
    }
    return 0;
}

/* Bytes waiting in the pipe of fd; -1 when it is closed (the command has ended). */
static long pipe_avail(int fd)
{
    DWORD n = 0;

    if (!PeekNamedPipe((HANDLE)_get_osfhandle(fd), NULL, 0, NULL, &n, NULL))
        return -1;
    return (long)n;
}

/* CR LF as LF in buf[0, n); a CR at the end is held back in *cr for the next piece. */
static size_t drop_cr(char *buf, size_t n, int *cr, size_t cap)
{
    size_t i, w = 0;
    char tmp[4096];

    if (n + 1 > sizeof tmp)
        n = sizeof tmp - 1;
    if (*cr) {
        tmp[w++] = '\r';
        *cr = 0;
    }
    memcpy(tmp + w, buf, n);
    n += w;
    w = 0;
    for (i = 0; i < n; i++) {
        if (tmp[i] == '\r' && i + 1 < n && tmp[i + 1] == '\n')
            continue;
        if (tmp[i] == '\r' && i + 1 == n) {
            *cr = 1;
            break;
        }
        if (w < cap)
            buf[w++] = tmp[i];
    }
    return w;
}

/* A piece of output as UTF-8 in out (room for 3 bytes each and 4 more): with the start of a
 * character the last piece cut, and holding back the start of one this piece cuts. */
static size_t decode(Job *j, const char *in, size_t n, char *out, size_t cap)
{
    char c[1408];
    size_t len = (size_t)j->nhold, ok;

    memcpy(c, j->hold, len);
    memcpy(c + len, in, n);
    len += n;
    j->nhold = 0;
    if (utf8_prefix((const unsigned char *)c, len, &ok)) {
        memcpy(out, c, ok);
        j->nhold = (int)(len - ok);
        memcpy(j->hold, c + ok, len - ok);
        return ok;
    }
    return oem_to_utf8(c, len, out, cap);
}

long shell_read(Job *j, char *buf, size_t n)
{
    char raw[1400];
    long avail;
    size_t want, got;
    int r;

    if (j->fd < 0)
        return 0;
    if (n > 4000)
        n = 4000;                       /* a piece grows to 3 times as UTF-8, and a held CR */
    for (;;) {
        avail = pipe_avail(j->fd);
        if (avail < 0) {
            /* the end: what was held back */
            got = 0;
            if (j->cr) {
                j->cr = 0;
                buf[got++] = '\r';
            }
            if (j->nhold) {
                got += oem_to_utf8(j->hold, (size_t)j->nhold, buf + got, n - got);
                j->nhold = 0;
            }
            return (long)got;
        }
        if (avail == 0)
            return -1;                  /* nothing yet */
        want = (n - 8) / 3;
        if (want > sizeof raw)
            want = sizeof raw;
        if ((size_t)avail < want)
            want = (size_t)avail;
        r = _read(j->fd, raw, (unsigned)want);
        if (r <= 0)
            return 0;
        got = decode(j, raw, (size_t)r, buf, n);
        got = drop_cr(buf, got, &j->cr, n);
        if (got > 0)
            return (long)got;
        /* only a held CR or a cut character: see what follows */
    }
}

void shell_stop(Job *j)
{
    if (j->proc && !j->done)
        stop_group(j);
}

int shell_reap(Job *j)
{
    DWORD code;

    if (!j->proc || j->done)
        return 1;
    if (WaitForSingleObject(j->proc, 0) != WAIT_OBJECT_0)
        return 0;
    j->status = GetExitCodeProcess(j->proc, &code) ? (int)code : 0;
    j->done = 1;
    return 1;
}

void shell_close(Job *j)
{
    if (j->proc && !j->done) {
        DWORD code = 1;

        stop_group(j);
        WaitForSingleObject(j->proc, INFINITE);
        GetExitCodeProcess(j->proc, &code);
        j->status = (int)code;
        j->done = 1;
    }
    if (j->proc)
        CloseHandle(j->proc);
    if (j->group)
        CloseHandle(j->group);
    j->proc = j->group = NULL;
    if (j->fd >= 0)
        _close(j->fd);
    j->fd = -1;
}

int shell_exit_code(const Job *j)
{
    return j->done ? j->status : -1;
}

/* ---- capture --------------------------------------------------------------- */

/* The input of a command in a temporary file the child reads (it is gone when closed): writing
 * a pipe while reading two others would need a thread. */
static HANDLE input_file(const char *in, size_t inlen)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    char dir[MAX_PATH], name[MAX_PATH];
    HANDLE h;
    DWORD w;

    if (!GetTempPathA(sizeof dir, dir) || !GetTempFileNameA(dir, "xtd", 0, name))
        return INVALID_HANDLE_VALUE;
    h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, &sa,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return h;
    while (inlen > 0) {
        DWORD part = inlen > (1u << 20) ? (1u << 20) : (DWORD)inlen;

        if (!WriteFile(h, in, part, &w, NULL) || w == 0) {
            CloseHandle(h);
            return INVALID_HANDLE_VALUE;
        }
        in += w;
        inlen -= w;
    }
    SetFilePointer(h, 0, NULL, FILE_BEGIN);
    return h;
}

/* Moves what the pipe has into to (up to cap); 0 once it is closed. */
static int drain(HANDLE rd, SBuf *to, size_t cap)
{
    char buf[4096];
    DWORD n = 0, got;

    if (!PeekNamedPipe(rd, NULL, 0, NULL, &n, NULL))
        return 0;
    while (n > 0) {
        if (!ReadFile(rd, buf, n < sizeof buf ? n : (DWORD)sizeof buf, &got, NULL) || got == 0)
            return 0;
        if (to && to->len < cap)
            sbuf_add(to, buf, to->len + got > cap ? cap - to->len : got);
        n -= got;
    }
    return 1;
}

static void lf_only(SBuf *b)
{
    size_t i, w = 0;

    if (!b || !b->p)
        return;
    for (i = 0; i < b->len; i++)
        if (!(b->p[i] == '\r' && i + 1 < b->len && b->p[i + 1] == '\n'))
            b->p[w++] = b->p[i];
    b->len = w;
}

int shell_capture(const char *const *argv, const char *cwd, const char *in, size_t inlen,
                  SBuf *out, SBuf *err, int timeout_ms, size_t cap, int *status)
{
    HANDLE in_h, out_rd, out_wr, err_rd, err_wr, proc, group;
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)(timeout_ms > 0 ? timeout_ms : 0);
    char *line;
    int out_open = 1, err_open = 1, timed_out = 0, r;
    size_t raw_cap;
    DWORD code = 0;

    if (out_pipe(&out_rd, &out_wr) < 0)
        return -1;
    if (out_pipe(&err_rd, &err_wr) < 0) {
        CloseHandle(out_rd);
        CloseHandle(out_wr);
        return -1;
    }
    in_h = in && inlen ? input_file(in, inlen) : inheritable_null();
    line = command_line(argv);
    r = spawn(line, cwd, in_h, out_wr, err_wr, &proc, &group);
    free(line);
    CloseHandle(out_wr);
    CloseHandle(err_wr);
    if (in_h != INVALID_HANDLE_VALUE)
        CloseHandle(in_h);
    if (r < 0) {
        DWORD e = GetLastError();

        CloseHandle(out_rd);
        CloseHandle(err_rd);
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            *status = 127;              /* as a shell says of a program it cannot find */
            return 0;
        }
        return -1;
    }
    /* the cap is for the text as it is given back (UTF-8, LF): twice as much is read */
    raw_cap = cap < ((size_t)-1 - 16) / 2 ? cap * 2 + 16 : cap;
    while (out_open || err_open) {
        size_t before = (out ? out->len : 0) + (err ? err->len : 0);

        if (out_open)
            out_open = drain(out_rd, out, raw_cap);
        if (err_open)
            err_open = drain(err_rd, err, raw_cap);
        if (GetTickCount64() >= deadline) {
            timed_out = 1;
            break;
        }
        if ((out ? out->len : 0) + (err ? err->len : 0) == before && (out_open || err_open))
            Sleep(2);
    }
    if (timed_out) {
        if (group)
            TerminateJobObject(group, 1);
        else
            TerminateProcess(proc, 1);
    }
    WaitForSingleObject(proc, INFINITE);
    GetExitCodeProcess(proc, &code);
    CloseHandle(proc);
    if (group)
        CloseHandle(group);
    CloseHandle(out_rd);
    CloseHandle(err_rd);
    to_utf8(out);
    to_utf8(err);
    lf_only(out);
    lf_only(err);
    if (out && out->len > cap)
        out->len = cap;
    if (err && err->len > cap)
        err->len = cap;
    if (status)
        *status = (int)code;
    return timed_out ? -2 : 0;
}
