#include "kernel/types.h"
#include "user/user.h"

volatile int alarm_count = 0;
volatile int alarm_depth = 0;
volatile int alarm_max_depth = 0;

void
alarm_handler(void)
{
  alarm_depth++;
  if (alarm_depth > alarm_max_depth)
    alarm_max_depth = alarm_depth;

  for (volatile int i = 0; i < 200000; i++)
    ;

  alarm_count++;
  alarm_depth--;
  sigreturn();
}

static void
spin_ticks(int ticks)
{
  int start = uptime();
  while (uptime() - start < ticks) {
    for (volatile int i = 0; i < 10000; i++)
      ;
  }
}

int
main(int argc, char *argv[])
{
  if (argc != 1) {
    fprintf(2, "usage: alarmunittest\n");
    exit(1);
  }

  if (sigalarm(2, alarm_handler) < 0) {
    printf("alarmunittest: sigalarm register failed\n");
    exit(1);
  }

  spin_ticks(40);
  if (alarm_count < 3) {
    printf("alarmunittest: handler did not run enough times (%d)\n", alarm_count);
    exit(1);
  }
  if (alarm_max_depth != 1) {
    printf("alarmunittest: handler re-entered (max depth %d)\n",
           alarm_max_depth);
    exit(1);
  }

  if (sigalarm(0, 0) < 0) {
    printf("alarmunittest: sigalarm disable failed\n");
    exit(1);
  }
  int before = alarm_count;
  spin_ticks(20);
  if (alarm_count != before) {
    printf("alarmunittest: handler still ran after disable (%d -> %d)\n", before,
           alarm_count);
    exit(1);
  }

  printf("ALARM_TEST_PASS count=%d\n", alarm_count);
  exit(0);
}
