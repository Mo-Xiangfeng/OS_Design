#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  char buf[512];
  char *args[MAXARG];
  int n = 0;
  int idx = 0;
  char *p = buf;

  if(argc < 2){
    fprintf(2, "usage: xargs cmd [args...]\n");
    exit(1);
  }

  for(int i = 1; i < argc; i++)
    args[idx++] = argv[i];

  while(read(0, p, 1) == 1){
    if(*p == '\n'){
      *p = 0;
      if(n == 0){
        p = buf;
        continue;
      }
      args[idx] = buf;
      args[idx+1] = 0;
      if(fork() == 0){
        exec(args[0], args);
        fprintf(2, "xargs: exec %s failed\n", args[0]);
        exit(1);
      }
      wait(0);
      p = buf;
      n = 0;
    } else {
      p++;
      n++;
    }
  }
  exit(0);
}
