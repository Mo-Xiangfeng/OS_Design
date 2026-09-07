#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void
sieve(int in)
{
  int p;
  if(read(in, &p, sizeof(p)) != sizeof(p)){
    close(in);
    exit(0);
  }
  printf("prime %d\n", p);

  int out[2];
  if(pipe(out) < 0){
    fprintf(2, "primes: pipe failed\n");
    exit(1);
  }

  int pid = fork();
  if(pid < 0){
    fprintf(2, "primes: fork failed\n");
    exit(1);
  }

  if(pid == 0){
    close(out[1]);
    close(in);
    sieve(out[0]);
    exit(0);
  } else {
    int n;
    close(out[0]);
    while(read(in, &n, sizeof(n)) == sizeof(n)){
      if(n % p != 0)
        write(out[1], &n, sizeof(n));
    }
    close(in);
    close(out[1]);
    wait(0);
    exit(0);
  }
}

int
main(int argc, char *argv[])
{
  int fd[2];
  if(pipe(fd) < 0){
    fprintf(2, "primes: pipe failed\n");
    exit(1);
  }

  int pid = fork();
  if(pid < 0){
    fprintf(2, "primes: fork failed\n");
    exit(1);
  }

  if(pid == 0){
    close(fd[1]);
    sieve(fd[0]);
    exit(0);
  } else {
    int i;
    close(fd[0]);
    for(i = 2; i <= 35; i++)
      write(fd[1], &i, sizeof(i));
    close(fd[1]);
    wait(0);
    exit(0);
  }
}
