#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/syscall.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"

#define BUFSZ  ((MAXOPBLOCKS+2)*BSIZE)

char buf[BUFSZ];


void testCAS() {
    int forks = 5;
    for (int i = 0; i < forks; i++) {
        fork();
    }
    for (int i = 0; i < 3; i++)
        printf("CPU: %d\n count: %d\n",i, cpu_process_count(i));
    // printf("PID: %d\n", getpid());
}

void
manywrites(char *s)
{
  int nchildren = 4;
  int howmany = 30; // increase to look for deadlock
  
  for(int ci = 0; ci < nchildren; ci++){
    int pid = fork();
    if(pid < 0){
      printf("fork failed\n");
      exit(1);
    }
    if(pid == 0){
      char name[3];
      name[0] = 'b';
      name[1] = 'a' + ci;
      name[2] = '\0';
      unlink(name);
      
      for(int iters = 0; iters < howmany; iters++){
        for(int i = 0; i < ci+1; i++){
          int fd = open(name, O_CREATE | O_RDWR);
          if(fd < 0){
            printf("%s: cannot create %s\n", s, name);
            exit(1);
          }
          int sz = sizeof(buf);
          int cc = write(fd, buf, sz);
          if(cc != sz){
            printf("%s: write(%d) ret %d\n", s, sz, cc);
            exit(1);
          }
          close(fd);
        }
        unlink(name);
      }

      unlink(name);
      exit(0);
    }
  }

  for(int ci = 0; ci < nchildren; ci++){
    int st = 0;
    wait(&st);
    if(st != 0)
      exit(st);
  }
}

void
twochildren(char *s)
{
  for(int i = 0; i < 1000; i++){
    int pid1 = fork();
    if(pid1 < 0){
      printf("%s: fork failed\n", s);
      exit(1);
    }
    if(pid1 == 0){
      exit(0);
    } else {
      int pid2 = fork();
      if(pid2 < 0){
        printf("%s: fork failed\n", s);
        exit(1);
      }
      if(pid2 == 0){
        exit(0);
      } else {
        wait(0);
        wait(0);
      }
    }
  }
}



int main(int argc, char** argv){
    // testCAS();
    manywrites("manywrites");
    // twochildren("twochildren");
    // printf("OK\n");
    exit(0);
}
