#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "vm.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[]; // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

static void vmprintwalk(pagetable_t pagetable, int level, int depth, uint64 va);
static int pte_is_leaf(pte_t pte);
static pte_t *walkleaf(pagetable_t pagetable, uint64 va, int *level);
static pte_t *walkl1(pagetable_t pagetable, uint64 va, int alloc);
static int mapsuperpages(pagetable_t pagetable, uint64 va, uint64 pa, int perm);
static void demotesuperpage(pagetable_t pagetable, uint64 va, pte_t *superpte);
static uint64 cowcopy(pagetable_t pagetable, uint64 va);

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t)kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext,
         PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);

  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if (mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

static void
vmprintwalk(pagetable_t pagetable, int level, int depth, uint64 va)
{
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) == 0)
      continue;

    uint64 pteva = va + ((uint64)i << PXSHIFT(level));
    for (int d = 0; d < depth; d++) {
      printf(" ..");
    }
    printf("%p: pte %p pa %p\n", (void *)pteva, (void *)pte, (void *)PTE2PA(pte));

    if (level > 0 && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      vmprintwalk((pagetable_t)PTE2PA(pte), level - 1, depth + 1, pteva);
    }
  }
}

void
vmprint(pagetable_t pagetable)
{
  printf("page table %p\n", pagetable);
  vmprintwalk(pagetable, 2, 1, 0);
}

static int
pte_is_leaf(pte_t pte)
{
  return (pte & (PTE_R | PTE_W | PTE_X)) != 0;
}

static pte_t *
walkleaf(pagetable_t pagetable, uint64 va, int *level)
{
  pte_t *pte;

  if (va >= MAXVA)
    return 0;

  pte = &pagetable[PX(2, va)];
  if ((*pte & PTE_V) == 0)
    return 0;
  if (pte_is_leaf(*pte)) {
    if (level)
      *level = 2;
    return pte;
  }

  pagetable = (pagetable_t)PTE2PA(*pte);
  pte = &pagetable[PX(1, va)];
  if ((*pte & PTE_V) == 0)
    return 0;
  if (pte_is_leaf(*pte)) {
    if (level)
      *level = 1;
    return pte;
  }

  pagetable = (pagetable_t)PTE2PA(*pte);
  pte = &pagetable[PX(0, va)];
  if ((*pte & PTE_V) == 0)
    return 0;
  if (level)
    *level = 0;
  return pte;
}

static pte_t *
walkl1(pagetable_t pagetable, uint64 va, int alloc)
{
  if (va >= MAXVA)
    panic("walkl1");

  pte_t *pte2 = &pagetable[PX(2, va)];
  if (*pte2 & PTE_V) {
    if (pte_is_leaf(*pte2))
      return 0;
    pagetable = (pagetable_t)PTE2PA(*pte2);
  } else {
    if (!alloc || (pagetable = (pagetable_t)kalloc()) == 0)
      return 0;
    memset(pagetable, 0, PGSIZE);
    *pte2 = PA2PTE(pagetable) | PTE_V;
  }
  return &pagetable[PX(1, va)];
}

static int
mapsuperpages(pagetable_t pagetable, uint64 va, uint64 pa, int perm)
{
  if ((va % SUPERPGSIZE) != 0 || (pa % SUPERPGSIZE) != 0)
    panic("mapsuperpages: not aligned");
  pte_t *pte = walkl1(pagetable, va, 1);
  if (pte == 0)
    return -1;
  if (*pte & PTE_V)
    panic("mapsuperpages: remap");
  *pte = PA2PTE(pa) | perm | PTE_V;
  return 0;
}

static void
demotesuperpage(pagetable_t pagetable, uint64 va, pte_t *superpte)
{
  if ((va % SUPERPGSIZE) != 0)
    panic("demotesuperpage: va");

  uint64 pa = PTE2PA(*superpte);
  uint flags = PTE_FLAGS(*superpte);
  pagetable_t l0 = (pagetable_t)kalloc();
  if (l0 == 0)
    panic("demotesuperpage: oom");
  memset(l0, 0, PGSIZE);

  for (int i = 0; i < 512; i++) {
    l0[i] = PA2PTE(pa + i * PGSIZE) | flags;
  }
  *superpte = PA2PTE(l0) | PTE_V;
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if (va >= MAXVA)
    panic("walk");

  for (int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if (*pte & PTE_V) {
      if (pte_is_leaf(*pte))
        return 0;
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;
  int level;

  if (va >= MAXVA)
    return 0;

  pte = walkleaf(pagetable, va, &level);
  if (pte == 0)
    return 0;
  if ((*pte & PTE_V) == 0)
    return 0;
  if ((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  if (level == 1)
    return pa + (va & (SUPERPGSIZE - 1));
  return pa + (va & (PGSIZE - 1));
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if ((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if (size == 0)
    panic("mappages: size");

  a = va;
  last = va + size - PGSIZE;
  for (;;) {
    if ((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if (*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a, end;
  pte_t *pte;
  int level;

  if ((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  end = va + npages * PGSIZE;
  for (a = va; a < end;) {
    pte = walkleaf(pagetable, a, &level);
    if (pte == 0) { // leaf page table entry allocated?
      a += PGSIZE;
      continue;
    }
    if ((*pte & PTE_V) == 0) { // has physical page been allocated?
      a += PGSIZE;
      continue;
    }

    if (level == 1) {
      uint64 superva = SUPERPGROUNDDOWN(a);
      if (a == superva && end - a >= SUPERPGSIZE) {
        if (do_free) {
          uint64 pa = PTE2PA(*pte);
          superfree((void *)pa);
        }
        *pte = 0;
        a += SUPERPGSIZE;
        continue;
      }
      demotesuperpage(pagetable, superva, pte);
      continue;
    }

    if (do_free) {
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
    }
    *pte = 0;
    a += PGSIZE;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;
  int superok;

  if (newsz < oldsz)
    return oldsz;

  superok = (xperm == PTE_W) && (newsz >= oldsz) && (newsz - oldsz >= SUPERPGSIZE);
  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz;) {
    if (superok && (a % SUPERPGSIZE) == 0 && newsz - a >= SUPERPGSIZE) {
      mem = superalloc();
      if (mem == 0) {
        // fall back to regular pages when superpage pool is exhausted.
        superok = 0;
      } else {
        memset(mem, 0, SUPERPGSIZE);
        if (mapsuperpages(pagetable, a, (uint64)mem, PTE_R | PTE_U | xperm) !=
            0) {
          superfree(mem);
          uvmdealloc(pagetable, a, oldsz);
          return 0;
        }
        a += SUPERPGSIZE;
        continue;
      }
    }

    mem = kalloc();
    if (mem == 0) {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R | PTE_U | xperm) !=
        0) {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    a += PGSIZE;
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
    return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if (pte & PTE_V) {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if (sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  int level;
  int parent_changed = 0;

  for (i = 0; i < sz;) {
    if ((pte = walkleaf(old, i, &level)) == 0) {
      i += PGSIZE;
      continue; // page table entry hasn't been allocated
    }
    if ((*pte & PTE_V) == 0) {
      i += PGSIZE;
      continue; // physical page hasn't been allocated
    }

    if (level == 1 && i != SUPERPGROUNDDOWN(i)) {
      i = SUPERPGROUNDDOWN(i) + SUPERPGSIZE;
      continue;
    }

    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if (flags & PTE_W) {
      flags = (flags & ~PTE_W) | PTE_COW;
    }

    if (level == 1) {
      if (mapsuperpages(new, SUPERPGROUNDDOWN(i), pa, flags) != 0) {
        goto err;
      }
      if (*pte != (PA2PTE(pa) | flags)) {
        *pte = PA2PTE(pa) | flags;
        parent_changed = 1;
      }
      kaddref((void *)pa);
      i += SUPERPGSIZE;
      continue;
    } else if (level != 0) {
      panic("uvmcopy: level");
    }

    if (mappages(new, i, PGSIZE, pa, flags) != 0) {
      goto err;
    }
    if (*pte != (PA2PTE(pa) | flags)) {
      *pte = PA2PTE(pa) | flags;
      parent_changed = 1;
    }
    kaddref((void *)pa);
    i += PGSIZE;
  }
  if (parent_changed)
    sfence_vma();
  return 0;

err:
  uvmunmap(new, 0, PGROUNDUP(i) / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while (len > 0) {
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= MAXVA)
      return -1;

    pte = walkleaf(pagetable, va0, 0);
    if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
      return -1;
    if ((*pte & PTE_W) == 0) {
      if ((*pte & PTE_COW) == 0)
        return -1;
      if ((pa0 = vmfault(pagetable, va0, 0)) == 0)
        return -1;
    } else {
      pa0 = walkaddr(pagetable, va0);
      if (pa0 == 0) {
        if ((pa0 = vmfault(pagetable, va0, 0)) == 0)
          return -1;
      }
    }

    n = PGSIZE - (dstva - va0);
    if (n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while (len > 0) {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) {
      if ((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if (n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while (got_null == 0 && max > 0) {
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if (n > max)
      n = max;

    char *p = (char *)(pa0 + (srcva - va0));
    while (n > 0) {
      if (*p == '\0') {
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if (got_null) {
    return 0;
  } else {
    return -1;
  }
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  uint64 mem;
  struct proc *p = myproc();
  pte_t *pte;
  int level;
  int mapflags;

  if (va >= p->sz)
    return 0;
  va = PGROUNDDOWN(va);
  pte = walkleaf(pagetable, va, &level);
  if (pte && (*pte & PTE_V) && (*pte & PTE_U)) {
    if (read)
      return walkaddr(pagetable, va);
    if ((*pte & PTE_W) != 0)
      return walkaddr(pagetable, va);
    if ((*pte & PTE_COW) == 0)
      return 0;
    return cowcopy(pagetable, va);
  }
  if (ismapped(pagetable, va)) {
    return 0;
  }
  mem = (uint64)kalloc();
  if (mem == 0)
    return 0;
  memset((void *)mem, 0, PGSIZE);
  mapflags = PTE_W | PTE_U | PTE_R;
  if (mappages(pagetable, va, PGSIZE, mem, mapflags) != 0) {
    kfree((void *)mem);
    return 0;
  }
  return mem;
}

static uint64
cowcopy(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 oldpa;
  uint64 newpa;
  uint flags;
  int level;

  pte = walkleaf(pagetable, va, &level);
  if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_COW) == 0)
    return 0;

  oldpa = PTE2PA(*pte);
  flags = PTE_FLAGS(*pte);
  flags = (flags | PTE_W) & ~PTE_COW;

  if (level == 1) {
    void *mem = superalloc();
    if (mem == 0)
      return 0;
    memmove(mem, (void *)oldpa, SUPERPGSIZE);
    *pte = PA2PTE((uint64)mem) | flags;
    sfence_vma();
    superfree((void *)oldpa);
    return (uint64)mem + (va & (SUPERPGSIZE - 1));
  } else if (level == 0) {
    void *mem = kalloc();
    if (mem == 0)
      return 0;
    memmove(mem, (void *)oldpa, PGSIZE);
    newpa = (uint64)mem;
    *pte = PA2PTE(newpa) | flags;
    sfence_vma();
    kfree((void *)oldpa);
    return newpa + (va & (PGSIZE - 1));
  }
  panic("cowcopy level");
}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walkleaf(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V) {
    return 1;
  }
  return 0;
}
