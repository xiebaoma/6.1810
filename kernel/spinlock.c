// Mutual exclusion spin locks.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"

void
initlock(struct spinlock *lk, char *name)
{
  lk->name = name;
  lk->locked = 0;
  lk->cpu = 0;
}

// Acquire the lock.
// Loops (spins) until the lock is acquired.
void
acquire(struct spinlock *lk)
{
  push_off(); // disable interrupts to avoid deadlock.
  if (holding(lk))
    panic("acquire");

  // On RISC-V, __atomic_exchange_n turns into an atomic swap:
  //   a5 = 1
  //   s1 = &lk->locked
  //   amoswap.w.aq a5, a5, (s1)
  //
  // Passing __ATOMIC_ACQUIRE to __atomic_exchange_n tells
  // the C compiler and the processor to not move loads or stores
  // past this point, to ensure that the critical section's memory
  // references happen strictly after the lock is acquired.
  while (__atomic_exchange_n(&lk->locked, 1, __ATOMIC_ACQUIRE) != 0)
    ;

  // Record info about lock acquisition for holding() and debugging.
  lk->cpu = mycpu();
}

// Release the lock.
void
release(struct spinlock *lk)
{
  if (!holding(lk))
    panic("release");

  lk->cpu = 0;

  // Release the lock, equivalent to lk->locked = 0.
  //
  // This code doesn't use a C assignment, since the C standard
  // implies that an assignment might be implemented with
  // multiple store instructions.
  //
  // On RISC-V, __atomic_store_n turns into a single atomic store:
  //   s1 = &lk->locked
  //   sw zero,0(s1)
  //
  // The __ATOMIC_RELEASE argument to __atomic_store_n tells the
  // the C compiler and the CPU to not move loads or stores past
  // this point, to ensure that all the stores in the critical
  // section are visible to other CPUs before the lock is released,
  // and that loads in the critical section occur strictly before
  // the lock is released.
  //
  // On RISC-V, this generates a fence instruction before the store:
  //   fence rw,w
  __atomic_store_n(&lk->locked, 0, __ATOMIC_RELEASE);

  pop_off();
}

// Check whether this cpu is holding the lock.
// Interrupts must be off.
int
holding(struct spinlock *lk)
{
  int r;
  r = (lk->locked && lk->cpu == mycpu());
  return r;
}

// push_off/pop_off are like intr_off()/intr_on() except that they are matched:
// it takes two pop_off()s to undo two push_off()s.  Also, if interrupts
// are initially off, then push_off, pop_off leaves them off.

void
push_off(void)
{
  int old = intr_get();

  // disable interrupts to prevent an involuntary context
  // switch while using mycpu().
  intr_off();

  if (mycpu()->noff == 0)
    mycpu()->intena = old;
  mycpu()->noff += 1;
}

void
pop_off(void)
{
  struct cpu *c = mycpu();
  if (intr_get())
    panic("pop_off - interruptible");
  if (c->noff < 1)
    panic("pop_off");
  c->noff -= 1;
  if (c->noff == 0 && c->intena)
    intr_on();
}

void
initrwlock(struct rwspinlock *lk)
{
  lk->readers = 0;
  lk->writer = 0;
  lk->pending_writers = 0;
}

void
read_acquire(struct rwspinlock *lk)
{
  push_off();
  for (;;) {
    while (__atomic_load_n(&lk->writer, __ATOMIC_ACQUIRE) ||
           __atomic_load_n(&lk->pending_writers, __ATOMIC_ACQUIRE))
      ;

    __atomic_fetch_add(&lk->readers, 1, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&lk->writer, __ATOMIC_ACQUIRE) == 0 &&
        __atomic_load_n(&lk->pending_writers, __ATOMIC_ACQUIRE) == 0)
      break;
    __atomic_fetch_sub(&lk->readers, 1, __ATOMIC_RELEASE);
  }
}

void
read_release(struct rwspinlock *lk)
{
  if (__atomic_fetch_sub(&lk->readers, 1, __ATOMIC_RELEASE) < 1)
    panic("read_release");
  pop_off();
}

void
write_acquire(struct rwspinlock *lk)
{
  push_off();
  __atomic_fetch_add(&lk->pending_writers, 1, __ATOMIC_ACQUIRE);
  while (__atomic_exchange_n(&lk->writer, 1, __ATOMIC_ACQUIRE) != 0)
    ;
  while (__atomic_load_n(&lk->readers, __ATOMIC_ACQUIRE) != 0)
    ;
  __atomic_fetch_sub(&lk->pending_writers, 1, __ATOMIC_RELEASE);
}

void
write_release(struct rwspinlock *lk)
{
  if (__atomic_load_n(&lk->writer, __ATOMIC_ACQUIRE) == 0)
    panic("write_release");
  __atomic_store_n(&lk->writer, 0, __ATOMIC_RELEASE);
  pop_off();
}

enum {
  RWTEST_RESET = 0,
  RWTEST_READER = 1,
  RWTEST_WRITER = 2,
  RWTEST_CHECK = 3,
};

static struct rwspinlock rwtest_lk;
static uint rwtest_inited;
static uint rwtest_readers;
static uint rwtest_writers;
static uint rwtest_writer_waiting;
static uint rwtest_violations;
static uint rwtest_writes;

int
rwspinlock_unittest(int op)
{
  volatile int i;
  uint readers, writers, waiting, violations, writes;

  if (!rwtest_inited) {
    initrwlock(&rwtest_lk);
    rwtest_inited = 1;
  }

  switch (op) {
  case RWTEST_RESET:
    initrwlock(&rwtest_lk);
    __atomic_store_n(&rwtest_readers, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rwtest_writers, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rwtest_writer_waiting, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rwtest_violations, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rwtest_writes, 0, __ATOMIC_RELEASE);
    return 0;

  case RWTEST_READER:
    for (i = 0; i < 2000; i++) {
      read_acquire(&rwtest_lk);
      if (__atomic_load_n(&rwtest_writers, __ATOMIC_ACQUIRE) != 0 ||
          __atomic_load_n(&rwtest_writer_waiting, __ATOMIC_ACQUIRE) != 0)
        __atomic_fetch_add(&rwtest_violations, 1, __ATOMIC_ACQ_REL);
      __atomic_fetch_add(&rwtest_readers, 1, __ATOMIC_ACQ_REL);
      for (volatile int j = 0; j < 50; j++)
        ;
      __atomic_fetch_sub(&rwtest_readers, 1, __ATOMIC_ACQ_REL);
      read_release(&rwtest_lk);
    }
    return 0;

  case RWTEST_WRITER:
    for (i = 0; i < 500; i++) {
      __atomic_fetch_add(&rwtest_writer_waiting, 1, __ATOMIC_ACQ_REL);
      write_acquire(&rwtest_lk);
      if (__atomic_load_n(&rwtest_writers, __ATOMIC_ACQUIRE) != 0 ||
          __atomic_load_n(&rwtest_readers, __ATOMIC_ACQUIRE) != 0)
        __atomic_fetch_add(&rwtest_violations, 1, __ATOMIC_ACQ_REL);
      __atomic_store_n(&rwtest_writers, 1, __ATOMIC_RELEASE);
      __atomic_fetch_add(&rwtest_writes, 1, __ATOMIC_ACQ_REL);
      for (volatile int j = 0; j < 100; j++)
        ;
      __atomic_store_n(&rwtest_writers, 0, __ATOMIC_RELEASE);
      write_release(&rwtest_lk);
      __atomic_fetch_sub(&rwtest_writer_waiting, 1, __ATOMIC_ACQ_REL);
    }
    return 0;

  case RWTEST_CHECK:
    readers = __atomic_load_n(&rwtest_readers, __ATOMIC_ACQUIRE);
    writers = __atomic_load_n(&rwtest_writers, __ATOMIC_ACQUIRE);
    waiting = __atomic_load_n(&rwtest_writer_waiting, __ATOMIC_ACQUIRE);
    violations = __atomic_load_n(&rwtest_violations, __ATOMIC_ACQUIRE);
    writes = __atomic_load_n(&rwtest_writes, __ATOMIC_ACQUIRE);
    if (readers == 0 && writers == 0 && waiting == 0 && violations == 0 &&
        writes > 0)
      return 0;
    return -1;
  }

  return -1;
}
