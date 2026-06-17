#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0; // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if (t == SBRK_EAGER || n < 0) {
    if (growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if (addr + n < addr)
      return -1;
    if (addr + n > USYSCALL)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if (n < 0)
    n = 0;
  backtrace();
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (killed(myproc())) {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_sleep(void)
{
  return sys_pause();
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_interpose(void)
{
  int mask;
  struct proc *p = myproc();

  argint(0, &mask);
  if (argstr(1, p->sandbox_path, sizeof(p->sandbox_path)) < 0) {
    return -1;
  }
  p->sandbox_mask = (uint64)mask;
  return 0;
}

uint64
sys_kpgtbl(void)
{
  vmprint(myproc()->pagetable);
  return 0;
}

uint64
sys_sigalarm(void)
{
  int ticks;
  uint64 handler;
  struct proc *p = myproc();

  argint(0, &ticks);
  argaddr(1, &handler);
  if (ticks < 0)
    return -1;

  p->alarm_interval = ticks;
  p->alarm_ticks_left = ticks;
  p->alarm_handler = handler;
  p->alarm_active = 0;
  return 0;
}

uint64
sys_sigreturn(void)
{
  struct proc *p = myproc();

  memmove(p->trapframe, &p->alarm_trapframe, sizeof(struct trapframe));
  p->alarm_active = 0;
  p->alarm_ticks_left = p->alarm_interval;
  return p->trapframe->a0;
}

uint64
sys_rwlktest(void)
{
  int op;

  argint(0, &op);
  return rwspinlock_unittest(op);
}
