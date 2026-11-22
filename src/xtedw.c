/* xtedw.exe: what the Start menu, "Open with" and "Open with xted" of Windows run. A program of
 * the windows subsystem, so no console appears when the files go to the xted already open (the
 * single instance, ipc.h); else it starts xted.exe, next to it, in a console of its own, in the
 * folder of the first file. Files chosen together make Explorer start one xtedw for each: the
 * first starts xted, the others wait for it and hand it theirs. It carries the icon. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include "ipc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAIT_FOR_XTED_MS 5000

/* ---- what ipc.c and ipc_win.c need of util.c (which brings the terminal and the catalogs) ---- */

static void fail(void)
{
    MessageBoxW(NULL, L"Out of memory", L"xted", MB_ICONERROR);
    ExitProcess(1);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);

    if (!p)
        fail();
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        fail();
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);

    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void sbuf_free(SBuf *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

void sbuf_add(SBuf *b, const char *s, size_t n)
{
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 + 64;
        b->p = xrealloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

void sbuf_adds(SBuf *b, const char *s)
{
    sbuf_add(b, s, strlen(s));
}

void sbuf_addf(SBuf *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0)
        sbuf_add(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

/* ---- the launcher --------------------------------------------------------- */

static char *utf8(const WCHAR *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = xmalloc(n > 0 ? (size_t)n : 1);

    if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL))
        s[0] = '\0';
    return s;
}

static void slashes(char *p)
{
    for (; *p; p++)
        if (*p == '\\')
            *p = '/';
}

/* Starts xted.exe (next to this program) with the files, in a new console. */
static int start_xted(const WCHAR *const *files, int n)
{
    WCHAR exe[MAX_PATH], dir[MAX_PATH], *slash, *line;
    size_t len = 0, cap;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD attr;
    int i;

    if (!GetModuleFileNameW(NULL, exe, MAX_PATH))
        return -1;
    slash = wcsrchr(exe, L'\\');
    if (!slash)
        return -1;
    wcscpy(slash + 1, L"xted.exe");
    cap = wcslen(exe) + 8;
    for (i = 0; i < n; i++)
        cap += wcslen(files[i]) + 4;
    line = xmalloc(cap * sizeof *line);
    len += (size_t)swprintf(line, cap, L"\"%ls\"", exe);
    for (i = 0; i < n; i++)
        len += (size_t)swprintf(line + len, cap - len, L" \"%ls\"", files[i]);
    /* the folder of the first file (that folder, for one), else the user's */
    dir[0] = L'\0';
    if (n > 0 && wcslen(files[0]) < MAX_PATH) {
        wcscpy(dir, files[0]);
        attr = GetFileAttributesW(dir);
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            slash = wcsrchr(dir, L'\\');
            if (slash)
                slash[slash > dir && slash[-1] == L':' ? 1 : 0] = L'\0';
            else
                dir[0] = L'\0';
        }
    }
    if (!dir[0] && !GetEnvironmentVariableW(L"USERPROFILE", dir, MAX_PATH))
        dir[0] = L'\0';
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(exe, line, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, dir[0] ? dir : NULL, &si, &pi)) {
        free(line);
        return -1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    free(line);
    return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    int argc, i, n = 0, waited;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const WCHAR **files;
    IpcFile *f;
    HANDLE mutex;

    (void)inst;
    (void)prev;
    (void)cmd;
    (void)show;
    if (!argv)
        return 1;
    files = xmalloc((size_t)argc * sizeof *files);
    f = xmalloc((size_t)argc * sizeof *f);
    for (i = 1; i < argc; i++) {
        WCHAR full[MAX_PATH];

        if (argv[i][0] == L'-')
            continue;
        if (!GetFullPathNameW(argv[i], MAX_PATH, full, NULL))
            continue;
        f[n].path = utf8(full);
        slashes(f[n].path);
        f[n].line = f[n].col = 0;
        files[n] = _wcsdup(full);
        n++;
    }
    /* the xted that gets the files may come to the front (Windows lets the one the user clicked
     * give it the right) */
    AllowSetForegroundWindow(ASFW_ANY);
    if (n > 0 && ipc_send(f, (size_t)n))
        return 0;
    /* another xtedw of the same click is starting xted: wait for it, then hand it the files */
    mutex = CreateMutexW(NULL, FALSE, L"Local\\xted-launch");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        for (waited = 0; waited < WAIT_FOR_XTED_MS; waited += 100) {
            Sleep(100);
            if (n > 0 && ipc_send(f, (size_t)n))
                return 0;
        }
    }
    if (start_xted(files, n) < 0) {
        MessageBoxW(NULL, L"Cannot start xted.exe: it must be in the folder of xtedw.exe.", L"xted",
                    MB_ICONERROR);
        return 1;
    }
    /* the others of the click wait while xted starts: hold the mutex until it listens */
    for (waited = 0; mutex && waited < WAIT_FOR_XTED_MS && !ipc_send(NULL, 0); waited += 100)
        Sleep(100);
    if (mutex)
        CloseHandle(mutex);
    LocalFree(argv);
    return 0;
}
