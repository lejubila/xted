/* The calls of sys.h that are functions on POSIX too (the others are macros there). */

#include "sys.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

int sys_lock_file(const char *path)
{
    struct flock fl;
    int fd = open(path, O_RDWR | O_CREAT, 0600);

    if (fd < 0)
        return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &fl) < 0) {
        close(fd);
        return -1;
    }
    return 0;                   /* the descriptor stays open: the lock lasts as this xted */
}
