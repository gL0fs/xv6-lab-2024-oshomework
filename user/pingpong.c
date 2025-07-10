#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int p1[2], p2[2];
  pipe(p1);
  pipe(p2);

  char buf[1];

  int pid = fork();

  if (pid == 0) {

    close(p1[1]);
    close(p2[0]); 

    if (read(p1[0], buf, 1) == 1) {
      printf("%d: received ping\n", getpid());
    }

    write(p2[1], buf, 1);
    
    close(p1[0]);
    close(p2[1]);

    exit(0);

  } else {
    close(p1[0]);
    close(p2[1]);

    write(p1[1], "a", 1);

    wait(0);

    if (read(p2[0], buf, 1) == 1) {
      printf("%d: received pong\n", getpid());
    }
    
    close(p1[1]);
    close(p2[0]);

    exit(0); 
  }
}
