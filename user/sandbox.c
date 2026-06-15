#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int pid;
  int mask;

  if (argc < 4) {
    fprintf(2, "usage: sandbox mask path cmd [args ...]\n");
    exit(1);
  }

  mask = atoi(argv[1]);
  pid = fork();
  if (pid < 0) {
    fprintf(2, "sandbox: fork failed\n");
    exit(1);
  }

  if (pid == 0) {
    if (interpose(mask, argv[2]) < 0) {
      fprintf(2, "sandbox: interpose failed\n");
      exit(1);
    }
    exec(argv[3], &argv[3]);
    fprintf(2, "sandbox: exec %s failed\n", argv[3]);
    exit(1);
  }

  wait(0);
  exit(0);
}
