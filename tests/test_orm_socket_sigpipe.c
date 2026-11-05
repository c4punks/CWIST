#include "../src/core/orm/orm_socket.c"

#include <assert.h>
#include <stdio.h>
#include <sys/wait.h>

int main(void) {
    int sv[2];
    int closed[2];
    assert(cwist_orm_socketpair_cloexec(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(pipe(closed) == 0);

    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(closed[1]);
        close(sv[0]);
        char byte;
        if (read(closed[0], &byte, 1) != 1) _exit(2);
        close(closed[0]);
        static const char response[] = "response";
        _exit(send_all(sv[1], response, sizeof(response)) == -1 ? 0 : 1);
    }

    close(closed[0]);
    close(sv[0]);
    close(sv[1]);
    assert(write(closed[1], "x", 1) == 1);
    close(closed[1]);
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    printf("test_orm_socket_sigpipe: ok\n");
    return 0;
}
