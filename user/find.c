#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/fs.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "user/user.h"

static int matchhere(char *re, char *text);
static int matchstar(int c, char *re, char *text);
enum { FIND_STACK_MAX = 128, FIND_PATH_MAX = 512 };
static char find_stack[FIND_STACK_MAX][FIND_PATH_MAX];

static int
match(char *re, char *text)
{
  if (re[0] == '^')
    return matchhere(re + 1, text);
  do {
    if (matchhere(re, text))
      return 1;
  } while (*text++ != '\0');
  return 0;
}

static int
matchhere(char *re, char *text)
{
  if (re[0] == '\0')
    return 1;
  if (re[1] == '*')
    return matchstar(re[0], re + 2, text);
  if (re[0] == '$' && re[1] == '\0')
    return *text == '\0';
  if (*text != '\0' && (re[0] == '.' || re[0] == *text))
    return matchhere(re + 1, text + 1);
  return 0;
}

static int
matchstar(int c, char *re, char *text)
{
  do {
    if (matchhere(re, text))
      return 1;
  } while (*text != '\0' && (*text++ == c || c == '.'));
  return 0;
}

static char *
base_name(char *path)
{
  char *p;

  for (p = path + strlen(path); p >= path && *p != '/'; p--)
    ;
  return p + 1;
}

static int
name_matches(char *name, char *pattern, int use_regex)
{
  char rebuf[MAXPATH + 3];
  int n;

  if (!use_regex)
    return strcmp(name, pattern) == 0;

  n = strlen(pattern);
  if (n + 2 >= sizeof(rebuf))
    return 0;
  rebuf[0] = '^';
  strcpy(rebuf + 1, pattern);
  rebuf[n + 1] = '$';
  rebuf[n + 2] = '\0';
  return match(rebuf, name);
}

static int
run_exec(char **exec_argv, int exec_argc, char *path)
{
  char *argv[MAXARG];
  int i;
  int pid;

  if (exec_argc + 2 > MAXARG) {
    fprintf(2, "find: too many exec args\n");
    return -1;
  }

  for (i = 0; i < exec_argc; i++)
    argv[i] = exec_argv[i];
  argv[exec_argc] = path;
  argv[exec_argc + 1] = 0;

  pid = fork();
  if (pid < 0) {
    fprintf(2, "find: fork failed\n");
    return -1;
  }
  if (pid == 0) {
    exec(argv[0], argv);
    fprintf(2, "find: exec %s failed\n", argv[0]);
    exit(1);
  }
  wait(0);
  return 0;
}

static void
maybe_report_match(char *path, char *pattern, int use_regex, char **exec_argv, int exec_argc)
{
  if (!name_matches(base_name(path), pattern, use_regex))
    return;

  if (exec_argv) {
    run_exec(exec_argv, exec_argc, path);
  } else {
    printf("%s\n", path);
  }
}

static void
find(char *start, char *pattern, int use_regex, char **exec_argv, int exec_argc)
{
  int head, tail;
  int fd;
  struct stat st;
  struct dirent de;
  char name[DIRSIZ + 1];
  char path[512];
  char buf[512];
  char *p;
  int nlen;

  if (strlen(start) >= FIND_PATH_MAX) {
    fprintf(2, "find: path too long\n");
    return;
  }
  head = 0;
  tail = 0;
  strcpy(find_stack[tail++], start);

  while (head < tail) {
    strcpy(path, find_stack[head++]);

    fd = open(path, O_RDONLY);
    if (fd < 0) {
      fprintf(2, "find: cannot open %s\n", path);
      continue;
    }
    if (fstat(fd, &st) < 0) {
      fprintf(2, "find: cannot stat %s\n", path);
      close(fd);
      continue;
    }

    maybe_report_match(path, pattern, use_regex, exec_argv, exec_argc);

    if (st.type != T_DIR) {
      close(fd);
      continue;
    }

    if (strlen(path) + 1 + DIRSIZ + 1 > sizeof(buf)) {
      fprintf(2, "find: path too long\n");
      close(fd);
      continue;
    }

    strcpy(buf, path);
    p = buf + strlen(buf);
    *p++ = '/';

    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
      if (de.inum == 0)
        continue;
      nlen = 0;
      while (nlen < DIRSIZ && de.name[nlen] != '\0') {
        name[nlen] = de.name[nlen];
        nlen++;
      }
      name[nlen] = '\0';
      if ((nlen == 1 && name[0] == '.') ||
          (nlen == 2 && name[0] == '.' && name[1] == '.'))
        continue;
      memmove(p, name, nlen);
      p[nlen] = '\0';
      if (strlen(buf) >= FIND_PATH_MAX) {
        fprintf(2, "find: path too long\n");
        continue;
      }
      if (tail >= FIND_STACK_MAX) {
        fprintf(2, "find: traversal stack full\n");
        break;
      }
      strcpy(find_stack[tail], buf);
      tail++;
    }

    close(fd);
  }
}

int
main(int argc, char *argv[])
{
  int i;
  int use_regex = 0;
  char **exec_argv = 0;
  int exec_argc = 0;

  if (argc < 3) {
    fprintf(2, "usage: find path name [-re] [-exec cmd ...]\n");
    exit(1);
  }

  for (i = 3; i < argc; i++) {
    if (strcmp(argv[i], "-re") == 0) {
      use_regex = 1;
    } else if (strcmp(argv[i], "-exec") == 0) {
      exec_argv = &argv[i + 1];
      exec_argc = argc - (i + 1);
      if (exec_argc < 1) {
        fprintf(2, "find: missing command for -exec\n");
        exit(1);
      }
      break;
    } else {
      fprintf(2, "find: unknown option %s\n", argv[i]);
      exit(1);
    }
  }

  find(argv[1], argv[2], use_regex, exec_argv, exec_argc);
  exit(0);
}
