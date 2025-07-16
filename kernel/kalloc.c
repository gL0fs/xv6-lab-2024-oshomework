// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

#define NSUPERPAGE 8
#define SUPERPAGE_POOL_START (PHYSTOP - (NSUPERPAGE * SUPERPGSIZE))

struct {
  struct spinlock lock;
  char used[NSUPERPAGE];
} spmem;

void
spinit()
{
  initlock(&spmem.lock, "spmem");
  for(int i = 0; i < NSUPERPAGE; i++) {
    spmem.used[i] = 0;
  }
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  spinit();
  freerange(end, (void*)SUPERPAGE_POOL_START);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= SUPERPAGE_POOL_START)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE);
  return (void*)r;
}

void *
superalloc(void)
{
  acquire(&spmem.lock);
  for(int i = 0; i < NSUPERPAGE; i++) {
    if(spmem.used[i] == 0) {
      spmem.used[i] = 1;
      release(&spmem.lock);
      uint64 pa = SUPERPAGE_POOL_START + i * SUPERPGSIZE;
      memset((void*)pa, 5, SUPERPGSIZE);
      return (void*)pa;
    }
  }

  release(&spmem.lock);
  return 0;
}

void
superfree(void *pa)
{
  if(((uint64)pa % SUPERPGSIZE) != 0 || (uint64)pa < SUPERPAGE_POOL_START || (uint64)pa >= PHYSTOP)
    panic("superfree: invalid pa");

  int idx = ((uint64)pa - SUPERPAGE_POOL_START) / SUPERPGSIZE;

  memset(pa, 1, SUPERPGSIZE);

  acquire(&spmem.lock);
  if(spmem.used[idx] == 0)
    panic("superfree: freeing a free page");
  spmem.used[idx] = 0;
  release(&spmem.lock);
}