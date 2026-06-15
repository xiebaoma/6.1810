#include "kernel/types.h"
#include "user/user.h"

static void
depth3(void)
{
  printf("backtracetest: trigger pause\n");
  if (pause(1) < 0) {
    printf("backtracetest: pause failed\n");
    exit(1);
  }
}

static void
depth2(void)
{
  depth3();
}

static void
depth1(void)
{
  depth2();
}

int
main(int argc, char *argv[])
{
  if (argc != 1) {
    fprintf(2, "usage: backtracetest\n");
    exit(1);
  }

  depth1();
  printf("BACKTRACE_TEST_DONE\n");
  exit(0);
}
