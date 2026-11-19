#ifndef XTED_SYS_H
#define XTED_SYS_H

/* The calls that differ between systems. On POSIX they are the libc's own, under another name
 * (so the program is the same it always was); on Windows sys_win.c has them. The rest of the
 * editor uses these names, never the calls themselves. */

#include <sys/stat.h>
#include <time.h>

#ifndef _WIN32

#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

/* the absolute form of path, in out (PATH_MAX bytes) or malloc'd when out is NULL; NULL on error */
#define sys_realpath(path, out) realpath((path), (out))
/* replaces to with from, also when to exists (the last step of a safe save) */
#define sys_rename(from, to) rename((from), (to))
#define sys_fsync(fd) fsync(fd)
/* the owner and the permissions of a file kept when it is saved again: best effort */
#define sys_fchown(fd, uid, gid) fchown((fd), (uid), (gid))
#define sys_fchmod(fd, mode) fchmod((fd), (mode))
#define sys_mkdir(path, mode) mkdir((path), (mode))
/* stat that does not follow a symbolic link */
#define sys_lstat(path, st) lstat((path), (st))
#define sys_setenv(name, value) setenv((name), (value), 1)
/* the path of the running program in buf (not terminated), its length; <= 0 when unknown */
#define sys_exe_path(buf, n) readlink("/proc/self/exe", (buf), (n))
/* when a file was last changed, as a struct timespec */
#define SYS_MTIME(st) ((st).st_mtim)
/* path starts at the root (a drive or a network share on Windows) */
#define sys_is_abs(path) ((path)[0] == '/')

#else

#include <stddef.h>

char *sys_realpath(const char *path, char *out);
int sys_rename(const char *from, const char *to);
int sys_fsync(int fd);
#define sys_fchown(fd, uid, gid) ((void)(fd), (void)(uid), (void)(gid), 0)
#define sys_fchmod(fd, mode) ((void)(fd), (void)(mode), 0)
int sys_mkdir(const char *path, int mode);
#define sys_lstat(path, st) stat((path), (st))
#define S_ISLNK(m) 0
int sys_setenv(const char *name, const char *value);
long sys_exe_path(char *buf, size_t n);
#define SYS_MTIME(st) (sys_timespec((st).st_mtime))
struct timespec sys_timespec(time_t t);
int sys_is_abs(const char *path);
/* the status of a command that ended (shell_capture, shell_reap) is its exit code */
#define WIFEXITED(s) 1
#define WEXITSTATUS(s) (s)
#define WIFSIGNALED(s) 0
#define WTERMSIG(s) 0

#endif

#endif
