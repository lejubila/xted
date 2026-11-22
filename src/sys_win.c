/* The calls of sys.h on Windows. Paths are given back with '/' (Windows takes both), so the rest
 * of the editor, which splits them at '/', needs no change. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "sys.h"

#include <ctype.h>
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void slashes(char *p)
{
    for (; *p; p++)
        if (*p == '\\')
            *p = '/';
}

char *sys_realpath(const char *path, char *out)
{
    char buf[PATH_MAX];

    if (!_fullpath(buf, path, sizeof buf))
        return NULL;
    if (GetFileAttributesA(buf) == INVALID_FILE_ATTRIBUTES) {
        errno = ENOENT;                 /* as realpath: the file must be there */
        return NULL;
    }
    slashes(buf);
    if (!out)
        return _strdup(buf);
    memcpy(out, buf, strlen(buf) + 1);
    return out;
}

int sys_rename(const char *from, const char *to)
{
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return 0;
    errno = GetLastError() == ERROR_ACCESS_DENIED ? EACCES : EIO;
    return -1;
}

int sys_fsync(int fd)
{
    return _commit(fd);
}

int sys_mkdir(const char *path, int mode)
{
    (void)mode;
    return _mkdir(path);
}

int sys_setenv(const char *name, const char *value)
{
    return _putenv_s(name, value) == 0 ? 0 : -1;
}

long sys_exe_path(char *buf, size_t n)
{
    DWORD r = GetModuleFileNameA(NULL, buf, (DWORD)n);

    if (r == 0 || r >= n)
        return -1;
    buf[r] = '\0';
    slashes(buf);
    return (long)r;
}

struct timespec sys_timespec(time_t t)
{
    struct timespec ts;

    ts.tv_sec = t;
    ts.tv_nsec = 0;
    return ts;
}

/* name = the folder in the variable from, with '/', unless name is set already. */
static void env_dir(const char *name, const char *from)
{
    const char *v = getenv(from);
    char buf[PATH_MAX];

    if ((getenv(name) && getenv(name)[0]) || !v || !v[0] || strlen(v) >= sizeof buf)
        return;
    memcpy(buf, v, strlen(v) + 1);
    slashes(buf);
    _putenv_s(name, buf);
}

/* Files in binary mode, as on POSIX: the C library of Windows would otherwise write CR LF for
 * every LF and stop reading at Ctrl+Z. The line ends are the editor's business.
 * The folders of the user as the variables the editor reads on every system: the configuration
 * in %APPDATA%\xted, the session in %LOCALAPPDATA%\xted, HOME for ~ (Git for Windows reads it
 * the same way). */
void sys_init(void)
{
    _set_fmode(_O_BINARY);
    _setmode(0, _O_BINARY);
    _setmode(1, _O_BINARY);
    env_dir("XDG_CONFIG_HOME", "APPDATA");
    env_dir("XDG_STATE_HOME", "LOCALAPPDATA");
    env_dir("HOME", "USERPROFILE");
}

char *sys_getcwd(char *buf, size_t n)
{
    if (!_getcwd(buf, (int)n))
        return NULL;
    slashes(buf);
    return buf;
}

void sys_path_fix(char *path)
{
    slashes(path);
}

/* The language of Windows' interface, "it-IT" as "it". */
void sys_lang(char *out, size_t n)
{
    WCHAR name[LOCALE_NAME_MAX_LENGTH];
    size_t k;

    if (n < 3 || !LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), name,
                                     LOCALE_NAME_MAX_LENGTH, 0))
        return;
    for (k = 0; k + 1 < n && name[k] && name[k] != '-' && name[k] < 0x80; k++)
        out[k] = (char)tolower((unsigned char)name[k]);
    if (k > 0)
        out[k] = '\0';
}

/* The console window to the front: the classic console lets it (xtedw.exe gave the right);
 * Windows Terminal's window is not the console's, and stays where it is. */
void sys_attention(void)
{
    HWND w = GetConsoleWindow();

    if (!w)
        return;
    if (IsIconic(w))
        ShowWindow(w, SW_RESTORE);
    SetForegroundWindow(w);
}

int sys_is_abs(const char *p)
{
    return p[0] == '/' || p[0] == '\\' ||
           (isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '/' || p[2] == '\\'));
}
