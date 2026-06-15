#include "kernel/types.h"
#include "kernel/riscv.h"
#include "user/user.h"

static void
fail(const char *msg)
{
  printf("cowunittest: %s\n", msg);
  exit(1);
}

static void
test_cow_write_isolation(void)
{
  char *p = sbrk(PGSIZE);
  if (p == SBRK_ERROR)
    fail("sbrk failed");
  p[0] = 0x11;
  p[PGSIZE - 1] = 0x22;

  int pid = fork();
  if (pid < 0)
    fail("fork failed");
  if (pid == 0) {
    p[0] = 0x33;
    p[PGSIZE - 1] = 0x44;
    if (p[0] != 0x33 || p[PGSIZE - 1] != 0x44)
      fail("child write verification failed");
    exit(0);
  }

  int st = 0;
  wait(&st);
  if (st != 0)
    fail("child exited with failure");
  if (p[0] != 0x11 || p[PGSIZE - 1] != 0x22)
    fail("parent page changed after child write");
}

static void
test_copyout_on_cow(void)
{
  int syncpipe[2];
  int datapipe[2];
  if (pipe(syncpipe) < 0 || pipe(datapipe) < 0)
    fail("pipe failed");

  char *p = sbrk(PGSIZE);
  if (p == SBRK_ERROR)
    fail("sbrk failed");
  p[0] = 'A';

  int pid = fork();
  if (pid < 0)
    fail("fork failed");
  if (pid == 0) {
    close(syncpipe[1]);
    close(datapipe[0]);
    close(datapipe[1]);
    char token = 0;
    if (read(syncpipe[0], &token, 1) != 1)
      fail("child sync read failed");
    if (p[0] != 'A')
      fail("child observed parent copyout modification");
    exit(0);
  }

  close(syncpipe[0]);
  if (write(datapipe[1], "Z", 1) != 1)
    fail("write to datapipe failed");
  if (read(datapipe[0], p, 1) != 1)
    fail("parent read into COW page failed");
  if (p[0] != 'Z')
    fail("parent copyout verification failed");
  if (write(syncpipe[1], "x", 1) != 1)
    fail("parent sync write failed");

  int st = 0;
  wait(&st);
  if (st != 0)
    fail("child exited with failure in copyout test");
  close(syncpipe[1]);
  close(datapipe[1]);
  close(datapipe[0]);
}

int
main(void)
{
  test_cow_write_isolation();
  test_copyout_on_cow();
  printf("cowunittest: OK\n");
  exit(0);
}
