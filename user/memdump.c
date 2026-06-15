#include "kernel/types.h"
#include "user/user.h"

void
memdump(char *fmt, char *data)
{
  uchar *p;
  int i;

  p = (uchar *)data;
  for (i = 0; fmt[i] != '\0'; i++) {
    switch (fmt[i]) {
    case 'i':
      printf("%d\n", *(int *)p);
      p += 4;
      break;
    case 'p':
      printf("%lx\n", *(uint64 *)p);
      p += 8;
      break;
    case 'h':
      printf("%d\n", *(ushort *)p);
      p += 2;
      break;
    case 'c':
      printf("%c\n", *p);
      p += 1;
      break;
    case 's':
      printf("%s\n", *(char **)p);
      p += 8;
      break;
    case 'S':
      printf("%s\n", (char *)p);
      while (*p != '\0')
        p++;
      p++;
      break;
    default:
      fprintf(2, "memdump: bad format char %c\n", fmt[i]);
      return;
    }
  }
}

static void
run_examples(void)
{
  struct {
    int a;
    int b;
  } e1 = {61810, 2025};
  struct {
    char *s;
  } e2 = {"a string"};
  char e3[] = "another";
  struct {
    uint64 p;
    int i;
    ushort h;
    char c;
    char s[6];
  } e4 = {0xBD0, 1819438967, 100, 'z', "xyzzy"};
  struct {
    char *s;
    char c[5];
  } e5 = {"hello", {'w', 'o', 'r', 'l', 'd'}};

  printf("Example 1:\n");
  memdump("ii", (char *)&e1);
  printf("Example 2:\n");
  memdump("s", (char *)&e2);
  printf("Example 3:\n");
  memdump("S", e3);
  printf("Example 4:\n");
  memdump("pihcS", (char *)&e4);
  printf("Example 5:\n");
  memdump("sccccc", (char *)&e5);
}

int
main(int argc, char *argv[])
{
  char data[2048];
  int n;
  int m;

  if (argc == 1) {
    run_examples();
    exit(0);
  }
  if (argc != 2) {
    fprintf(2, "usage: memdump [format]\n");
    exit(1);
  }

  m = 0;
  while ((n = read(0, data + m, sizeof(data) - m)) > 0) {
    m += n;
    if (m == sizeof(data))
      break;
  }
  if (n < 0) {
    fprintf(2, "memdump: read failed\n");
    exit(1);
  }
  if (m >= sizeof(data))
    m = sizeof(data) - 1;
  data[m] = '\0';
  memdump(argv[1], data);
  exit(0);
}
