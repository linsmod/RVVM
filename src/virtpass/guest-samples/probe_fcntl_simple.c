/* Minimal fcntl record-lock probe: one process, one fd, no fork/pipe. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    struct flock fl;
    int fd;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("simple: start\n");
    fd = open("/root/fcntl-simple", O_RDWR | O_CREAT | O_TRUNC, 0600);
    printf("simple: open=%d errno=%d\n", fd, errno);
    if (fd < 0) return 1;

    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    errno = 0;
    printf("simple: setlk=%d errno=%d\n", fcntl(fd, F_SETLK, &fl), errno);

    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    errno = 0;
    printf("simple: getlk=%d errno=%d\n", fcntl(fd, F_GETLK, &fl), errno);
    printf("simple: getlk type=%d pid=%d\n", fl.l_type, fl.l_pid);

    fl.l_type = F_UNLCK;
    errno = 0;
    printf("simple: unlock=%d errno=%d\n", fcntl(fd, F_SETLK, &fl), errno);

    printf("simple: close=%d\n", close(fd));
    unlink("/root/fcntl-simple");
    printf("simple: done\n");
    return 0;
}
