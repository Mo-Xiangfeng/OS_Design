#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/sysinfo.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  struct sysinfo info;
  sysinfo(&info);
  printf("freemem before: %d\n", info.freemem);
  int pid = fork();
  if(pid == 0){
    char *p = sbrk(4096 * 10);
    for(char *q = p; q < p + 4096*10; q += 4096)
      *(int*)q = 1;
    sysinfo(&info);
    printf("freemem child: %d\n", info.freemem);
    exit(0);
  }
  wait(0);
  sysinfo(&info);
  printf("freemem after: %d\n", info.freemem);
  exit(0);
}
