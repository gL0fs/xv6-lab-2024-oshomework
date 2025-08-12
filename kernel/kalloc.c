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

struct kmem {
  struct spinlock lock;
  struct run *freelist;
};

struct kmem kmem_arr[NCPU];

void
kinit()
{
  for (int i = 0; i < NCPU; i++) {
    char lock_name[8];
    snprintf(lock_name, sizeof(lock_name), "kmem_%d", i);
    initlock(&kmem_arr[i].lock, lock_name);
  }
  freerange(end, (void*)PHYSTOP);
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

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();
  int cpu_id = cpuid();
  acquire(&kmem_arr[cpu_id].lock);
  r->next = kmem_arr[cpu_id].freelist;
  kmem_arr[cpu_id].freelist = r;
  release(&kmem_arr[cpu_id].lock);
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off(); 
  int my_cpu = cpuid();
  acquire(&kmem_arr[my_cpu].lock);
  r = kmem_arr[my_cpu].freelist;
  if(r) {
    kmem_arr[my_cpu].freelist = r->next;
    release(&kmem_arr[my_cpu].lock);
  } else {
    release(&kmem_arr[my_cpu].lock);

    for(int i = 0; i < NCPU; i++){
      if(i == my_cpu) continue;

      acquire(&kmem_arr[i].lock);
      r = kmem_arr[i].freelist;
      if(r){
        kmem_arr[i].freelist = r->next;
        release(&kmem_arr[i].lock);
        break;
      }
      release(&kmem_arr[i].lock);
    }
  }
  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE);
  return (void*)r;
}
