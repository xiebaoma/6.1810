// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "vm.h"

void freerange(void *pa_start, void *pa_end);
void superfree(void *pa);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

#define NPAGES      ((PHYSTOP - KERNBASE) / PGSIZE)
#define NSUPERPAGES ((PHYSTOP - KERNBASE) / SUPERPGSIZE)
#define STEAL_BATCH 32

struct kmem_cpu {
  struct spinlock lock;
  struct run *freelist;
};

struct {
  struct spinlock lock;
  struct kmem_cpu cpu[NCPU];
  struct run *superfreelist;
  uint64 super_start;
  uint64 super_end;
  int pageref[NPAGES];
  int superref[NSUPERPAGES];
} kmem;

static int
kmem_cpuid(void)
{
  int id;

  push_off();
  id = cpuid();
  pop_off();
  return id;
}

static int
is_super_pa(uint64 pa)
{
  return pa >= kmem.super_start && pa < kmem.super_end;
}

static int
page_index(uint64 pa)
{
  return (pa - KERNBASE) / PGSIZE;
}

static int
super_index(uint64 pa)
{
  return (pa - KERNBASE) / SUPERPGSIZE;
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  for (int i = 0; i < NCPU; i++) {
    initlock(&kmem.cpu[i].lock, "kmem");
    kmem.cpu[i].freelist = 0;
  }
  memset(kmem.pageref, 0, sizeof(kmem.pageref));
  memset(kmem.superref, 0, sizeof(kmem.superref));
  kmem.super_end = PHYSTOP;
  kmem.super_start = PHYSTOP - 4 * SUPERPGSIZE;
  if (kmem.super_start < (uint64)end) {
    kmem.super_start = (uint64)end;
  }
  kmem.super_start = SUPERPGROUNDUP(kmem.super_start);
  if (kmem.super_start > kmem.super_end) {
    kmem.super_start = kmem.super_end;
  }

  for (uint64 pa = kmem.super_start; pa + SUPERPGSIZE <= kmem.super_end;
       pa += SUPERPGSIZE) {
    kmem.superref[super_index(pa)] = 1;
    superfree((void *)pa);
  }
  freerange(end, (void *)kmem.super_start);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;

  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE) {
    kmem.pageref[page_index((uint64)p)] = 1;
    kfree(p);
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  int id;
  int idx;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  id = kmem_cpuid();
  acquire(&kmem.cpu[id].lock);
  acquire(&kmem.lock);
  idx = page_index((uint64)pa);
  if (kmem.pageref[idx] < 1)
    panic("kfree ref");
  kmem.pageref[idx]--;
  if (kmem.pageref[idx] > 0) {
    release(&kmem.lock);
    release(&kmem.cpu[id].lock);
    return;
  }
  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);
  r = (struct run *)pa;
  r->next = kmem.cpu[id].freelist;
  kmem.cpu[id].freelist = r;
  release(&kmem.lock);
  release(&kmem.cpu[id].lock);
}

static struct run *
steal_from(int from, int to)
{
  struct run *head, *tail, *rest, *r;
  int n;

  acquire(&kmem.cpu[from].lock);
  head = kmem.cpu[from].freelist;
  if (head == 0) {
    release(&kmem.cpu[from].lock);
    return 0;
  }

  tail = head;
  for (n = 1; n < STEAL_BATCH && tail->next; n++)
    tail = tail->next;
  kmem.cpu[from].freelist = tail->next;
  tail->next = 0;
  release(&kmem.cpu[from].lock);

  r = head;
  rest = head->next;
  r->next = 0;
  if (rest) {
    acquire(&kmem.cpu[to].lock);
    tail->next = kmem.cpu[to].freelist;
    kmem.cpu[to].freelist = rest;
    release(&kmem.cpu[to].lock);
  }
  return r;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  int id, i;

  id = kmem_cpuid();
  acquire(&kmem.cpu[id].lock);
  r = kmem.cpu[id].freelist;
  if (r) {
    kmem.cpu[id].freelist = r->next;
  }
  release(&kmem.cpu[id].lock);

  if (r == 0) {
    for (i = 1; i < NCPU; i++) {
      r = steal_from((id + i) % NCPU, id);
      if (r)
        break;
    }
  }

  if (r == 0)
    return 0;

  acquire(&kmem.lock);
  kmem.pageref[page_index((uint64)r)] = 1;
  release(&kmem.lock);

  memset((char *)r, 5, PGSIZE); // fill with junk
  return (void *)r;
}

void
kaddref(void *pa)
{
  uint64 a = (uint64)pa;

  acquire(&kmem.lock);
  if (is_super_pa(a)) {
    if ((a % SUPERPGSIZE) != 0)
      panic("kaddref super");
    if (kmem.superref[super_index(a)] < 1)
      panic("kaddref super ref");
    kmem.superref[super_index(a)]++;
  } else {
    if ((a % PGSIZE) != 0)
      panic("kaddref page");
    if (kmem.pageref[page_index(a)] < 1)
      panic("kaddref page ref");
    kmem.pageref[page_index(a)]++;
  }
  release(&kmem.lock);
}

void
superfree(void *pa)
{
  struct run *r;
  int idx;

  if (((uint64)pa % SUPERPGSIZE) != 0 || (uint64)pa < kmem.super_start ||
      (uint64)pa + SUPERPGSIZE > kmem.super_end) {
    panic("superfree");
  }
  acquire(&kmem.lock);
  idx = super_index((uint64)pa);
  if (kmem.superref[idx] < 1)
    panic("superfree ref");
  kmem.superref[idx]--;
  if (kmem.superref[idx] > 0) {
    release(&kmem.lock);
    return;
  }
  memset(pa, 1, SUPERPGSIZE);
  r = (struct run *)pa;
  r->next = kmem.superfreelist;
  kmem.superfreelist = r;
  release(&kmem.lock);
}

void *
superalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.superfreelist;
  if (r) {
    kmem.superfreelist = r->next;
    kmem.superref[super_index((uint64)r)] = 1;
  }
  release(&kmem.lock);

  if (r) {
    memset((char *)r, 5, SUPERPGSIZE);
  }
  return (void *)r;
}
