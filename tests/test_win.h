#ifndef XTED_TEST_WIN_H
#define XTED_TEST_WIN_H

/* test_core.c on Windows (make WIN=1 test): the POSIX calls the tests use, in the names of
 * Windows. Not for the editor, which has sys.h. */
#ifdef _WIN32
/* Sleep; not winuser.h, whose mouse_event is a name of the tests */
#define WIN32_LEAN_AND_MEAN
#define NOUSER
#include <windows.h>
/* the hot key modifiers of imm.h (and winuser.h): the names are term.h's */
#undef MOD_ALT
#undef MOD_CONTROL
#undef MOD_SHIFT
#undef MOD_WIN
#include <direct.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define mkdir(path, mode) _mkdir(path)
#define setenv(name, value, over) _putenv_s((name), (value))
#define unsetenv(name) _putenv_s((name), "")
/* not root: a file without the write permission is read-only */
#define geteuid() 1

/* The tests remove their folders with "rm -rf dir" (or 'dir'): rmdir /s /q on Windows. */
static int win_system(const char *cmd)
{
    char path[600], line[700];
    size_t i, k = 0;
    struct stat st;

    if (strncmp(cmd, "rm -rf ", 7) != 0)
        return system(cmd);
    for (i = 7; cmd[i] && k + 1 < sizeof path; i++)
        if (cmd[i] != '\'')
            path[k++] = cmd[i] == '/' ? '\\' : cmd[i];
    path[k] = '\0';
    snprintf(line, sizeof line, "rmdir /s /q \"%s\" 2>nul", path);
    system(line);
    return stat(path, &st) == 0 ? 1 : 0;
}
#define system(cmd) win_system(cmd)
#endif

#endif
