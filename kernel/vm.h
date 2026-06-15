#define SBRK_EAGER 1
#define SBRK_LAZY  2

#define SUPERPGSIZE  (512 * PGSIZE)
#define SUPERPGROUNDUP(sz)  (((sz) + SUPERPGSIZE - 1) & ~(SUPERPGSIZE - 1))
#define SUPERPGROUNDDOWN(a) (((a)) & ~(SUPERPGSIZE - 1))
