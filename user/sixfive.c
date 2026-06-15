#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static char separators[] = " -\r\t\n./,";

static int
is_separator(char c)
{
  return strchr(separators, c) != 0;
}

static void
flush_token(int in_token, int valid, int has_digit, int value)
{
  if (in_token && valid && has_digit) {
    if (value % 5 == 0 || value % 6 == 0)
      printf("%d\n", value);
  }
}

static int
process_fd(int fd, char *name)
{
  char c;
  int n;
  int in_token = 0;
  int valid = 1;
  int has_digit = 0;
  int value = 0;

  while ((n = read(fd, &c, 1)) == 1) {
    if (is_separator(c)) {
      flush_token(in_token, valid, has_digit, value);
      in_token = 0;
      valid = 1;
      has_digit = 0;
      value = 0;
      continue;
    }

    if (!in_token) {
      in_token = 1;
      valid = 1;
      has_digit = 0;
      value = 0;
    }

    if ('0' <= c && c <= '9') {
      if (valid) {
        has_digit = 1;
        value = value * 10 + (c - '0');
      }
    } else {
      valid = 0;
    }
  }

  if (n < 0) {
    fprintf(2, "sixfive: read error on %s\n", name);
    return -1;
  }

  flush_token(in_token, valid, has_digit, value);
  return 0;
}

int
main(int argc, char *argv[])
{
  int i;
  int fd;

  if (argc == 1) {
    if (process_fd(0, "stdin") < 0)
      exit(1);
    exit(0);
  }

  for (i = 1; i < argc; i++) {
    fd = open(argv[i], O_RDONLY);
    if (fd < 0) {
      fprintf(2, "sixfive: cannot open %s\n", argv[i]);
      exit(1);
    }
    if (process_fd(fd, argv[i]) < 0) {
      close(fd);
      exit(1);
    }
    close(fd);
  }

  exit(0);
}
