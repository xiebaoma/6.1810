#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(void)
{
  int i, pid, st;
  int fail = 0;

  if (rwlktest(0) < 0) {
    printf("rwlktest: reset failed\n");
    exit(1);
  }

  for (i = 0; i < 4; i++) {
    pid = fork();
    if (pid < 0) {
      printf("rwlktest: fork failed\n");
      exit(1);
    }
    if (pid == 0) {
      if (i == 0) {
        if (rwlktest(2) < 0)
          exit(1);
      } else {
        if (rwlktest(1) < 0)
          exit(1);
      }
      exit(0);
    }
  }

  for (i = 0; i < 4; i++) {
    if (wait(&st) < 0 || st != 0)
      fail = 1;
  }

  if (rwlktest(3) < 0)
    fail = 1;

  if (fail) {
    printf("rwlktest: fail\n");
    exit(1);
  }

  printf("rwlktest: pass\n");
  exit(0);
}
