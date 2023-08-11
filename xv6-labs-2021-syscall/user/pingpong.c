#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
    int fd1[2];
    int fd2[2];
    pipe(fd1);
    pipe(fd2);
    char mes[8];
    if (fork() == 0) {
        // 子进程
        read(fd1[0], mes, 4);
        printf("%d: received %s\n", getpid(), mes);
        write(fd2[1], "pong", strlen("pong"));
    }
    else {
        // 父进程
        write(fd1[1], "ping", strlen("ping"));
        wait(0);
        read(fd2[0], mes, 4);
        printf("%d: received %s\n", getpid(), mes);
    }
    exit(0);
}