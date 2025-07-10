#include "kernel/types.h"
#include "user/user.h"

void sieve(int p_pipe_read_end) __attribute__((noreturn));

void sieve(int p_pipe_read_end) {
    int p;
    int n;

    if (read(p_pipe_read_end, &p, sizeof(int)) == 0) {
        close(p_pipe_read_end);
        exit(0);
    }

    printf("prime %d\n", p);

    int c_pipe[2];
    pipe(c_pipe);

    if (fork() == 0) {
        close(c_pipe[1]);
        close(p_pipe_read_end);
        sieve(c_pipe[0]);

    } else {
        close(c_pipe[0]);

        while (read(p_pipe_read_end, &n, sizeof(int)) > 0) {
            if (n % p != 0) {
                write(c_pipe[1], &n, sizeof(int));
            }
        }

        close(p_pipe_read_end);
        close(c_pipe[1]);

        wait(0);
        exit(0);
    }
}


int main(int argc, char *argv[]) {
    int p[2];
    pipe(p);

    if (fork() == 0) {
        close(p[1]);
        sieve(p[0]);

    } else {
        close(p[0]);
        for (int i = 2; i <= 280; i++) {
            write(p[1], &i, sizeof(int));
        }
        close(p[1]);
        wait(0);
        exit(0);
    }
    
    return 0;
}