#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"

int main(int argc, char *argv[]) {
    char *new_argv[MAXARG];
    char buf[512];
    int i;

    if (argc < 2) {
        fprintf(2, "Usage: xargs <command> [args...]\n");
        exit(1);
    }

    for (i = 1; i < argc; i++) {
        new_argv[i - 1] = argv[i];
    }
    
    int char_idx = 0;
    while (read(0, &buf[char_idx], 1) > 0) {
        if (buf[char_idx] == '\n') {
            buf[char_idx] = 0;

            new_argv[argc - 1] = buf;

            new_argv[argc] = 0;

            if (fork() == 0) {
                exec(new_argv[0], new_argv);
                fprintf(2, "exec failed\n");
                exit(1);
            } else {
                wait(0);
            }
            char_idx = 0;
        } else {
            char_idx++;
        }
    }
    exit(0);
}