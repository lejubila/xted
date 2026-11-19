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

/* Files in binary mode, as on POSIX: the C library of Windows would otherwise write CR LF for
 * every LF and stop reading at Ctrl+Z. The line ends are the editor's business. */
void sys_init(void)
{
    _set_fmode(_O_BINARY);
    _setmode(0, _O_BINARY);
    _setmode(1, _O_BINARY);
}

int sys_is_abs(const char *p)
{
    return p[0] == '/' || p[0] == '\\' ||
           (isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '/' || p[2] == '\\'));
}
