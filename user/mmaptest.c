#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/riscv.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"
#include "user/user.h"

void mmap_test(void);
void fork_test(void);
char buf[BSIZE];

#define MAP_FAILED ((char *)-1)

char *testname = "???";

static void
err(char *why)
{
  printf("mmaptest: %s failed: %s, pid=%d\n", testname, why, getpid());
  exit(1);
}

static void
v1(char *p)
{
  for (int i = 0; i < PGSIZE * 2; i++) {
    if (i < PGSIZE + (PGSIZE / 2)) {
      if (p[i] != 'A') {
        printf("mismatch at %d, wanted 'A', got 0x%x\n", i, p[i]);
        err("v1 mismatch (1)");
      }
    } else {
      if (p[i] != 0) {
        printf("mismatch at %d, wanted zero, got 0x%x\n", i, p[i]);
        err("v1 mismatch (2)");
      }
    }
  }
}

static void
makefile(const char *f)
{
  int n = PGSIZE / BSIZE;

  unlink(f);
  int fd = open(f, O_WRONLY | O_CREATE);
  if (fd == -1)
    err("open");
  memset(buf, 'A', BSIZE);
  for (int i = 0; i < n + n / 2; i++) {
    if (write(fd, buf, BSIZE) != BSIZE)
      err("write makefile");
  }
  if (close(fd) == -1)
    err("close");
}

void
mmap_test(void)
{
  int fd;
  int i;
  const char *const f = "mmap.dur";
  printf("mmap_test starting\n");
  testname = "mmap_test";

  makefile(f);
  if ((fd = open(f, O_RDONLY)) == -1)
    err("open");

  char *p = mmap(0, PGSIZE * 2, PROT_READ, MAP_PRIVATE, fd, 0);
  if (p == MAP_FAILED)
    err("mmap (1)");
  v1(p);
  if (munmap(p, PGSIZE * 2) == -1)
    err("munmap (1)");

  p = mmap(0, PGSIZE * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (p == MAP_FAILED)
    err("mmap (2)");
  if (close(fd) == -1)
    err("close");
  v1(p);
  for (i = 0; i < PGSIZE * 2; i++)
    p[i] = 'Z';
  if (munmap(p, PGSIZE * 2) == -1)
    err("munmap (2)");

  if ((fd = open(f, O_RDONLY)) == -1)
    err("open");
  p = mmap(0, PGSIZE * 3, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p != MAP_FAILED)
    err("mmap should fail");
  if (close(fd) == -1)
    err("close");

  if ((fd = open(f, O_RDWR)) == -1)
    err("open");
  p = mmap(0, PGSIZE * 3, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED)
    err("mmap (3)");
  if (close(fd) == -1)
    err("close");

  v1(p);

  for (i = 0; i < PGSIZE * 2; i++)
    p[i] = 'Z';

  if (munmap(p, PGSIZE * 2) == -1)
    err("munmap (3)");

  if ((fd = open(f, O_RDWR)) == -1)
    err("open");
  for (i = 0; i < PGSIZE + (PGSIZE / 2); i++) {
    char b;
    if (read(fd, &b, 1) != 1)
      err("read");
    if (b != 'Z')
      err("file does not contain modifications");
  }
  if (close(fd) == -1)
    err("close");

  if (munmap(p + PGSIZE * 2, PGSIZE) == -1)
    err("munmap (4)");

  int fd1;
  if ((fd1 = open("mmap1", O_RDWR | O_CREATE)) < 0)
    err("open mmap1");
  if (write(fd1, "12345", 5) != 5)
    err("write mmap1");
  char *p1 = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd1, 0);
  if (p1 == MAP_FAILED)
    err("mmap mmap1");
  close(fd1);
  unlink("mmap1");

  int fd2;
  if ((fd2 = open("mmap2", O_RDWR | O_CREATE)) < 0)
    err("open mmap2");
  if (write(fd2, "67890", 5) != 5)
    err("write mmap2");
  char *p2 = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd2, 0);
  if (p2 == MAP_FAILED)
    err("mmap mmap2");
  close(fd2);
  unlink("mmap2");

  if (memcmp(p1, "12345", 5) != 0)
    err("mmap1 mismatch");
  if (memcmp(p2, "67890", 5) != 0)
    err("mmap2 mismatch");

  munmap(p1, PGSIZE);
  if (memcmp(p2, "67890", 5) != 0)
    err("mmap2 mismatch (2)");
  munmap(p2, PGSIZE);

  printf("mmap_test OK\n");
}

void
fork_test(void)
{
  int fd;
  int pid;
  const char *const f = "mmap.dur";

  printf("fork_test starting\n");
  testname = "fork_test";

  makefile(f);
  if ((fd = open(f, O_RDONLY)) == -1)
    err("open");
  unlink(f);
  char *p1 = mmap(0, PGSIZE * 2, PROT_READ, MAP_SHARED, fd, 0);
  if (p1 == MAP_FAILED)
    err("mmap (4)");
  char *p2 = mmap(0, PGSIZE * 2, PROT_READ, MAP_SHARED, fd, 0);
  if (p2 == MAP_FAILED)
    err("mmap (5)");

  if (*(p1 + PGSIZE) != 'A')
    err("fork mismatch (1)");

  if ((pid = fork()) < 0)
    err("fork");
  if (pid == 0) {
    v1(p1);
    munmap(p1, PGSIZE);
    exit(0);
  }

  int status = -1;
  wait(&status);
  if (status != 0) {
    printf("fork_test failed\n");
    exit(1);
  }

  v1(p1);
  v1(p2);

  printf("fork_test OK\n");
}

int
main(int argc, char *argv[])
{
  mmap_test();
  fork_test();
  printf("mmaptest: all tests succeeded\n");
  exit(0);
}
