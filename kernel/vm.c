#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

#ifdef LAB_NET
  // PCI-E ECAM (configuration space), for pci.c
  kvmmap(kpgtbl, 0x30000000L, 0x30000000L, 0x10000000, PTE_R | PTE_W);

  // pci.c maps the e1000's registers here.
  kvmmap(kpgtbl, 0x40000000L, 0x40000000L, 0x20000, PTE_R | PTE_W);
#endif  

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

void kvminit(void) { kernel_pagetable = kvmmake(); }

// Switch h/w page table register to the kernel's page table,
// and enable paging.
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
  if(va >= MAXVA) return 0;
  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      if((*pte & (PTE_R|PTE_W|PTE_X)) != 0)
        return pte; // This is a leaf PTE.
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
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
  if (va >= MAXVA)
    return 0;
  pte = walk(pagetable, va, 0);
  if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
    return 0;
  if((*pte & (PTE_R|PTE_W|PTE_X)) == 0)
    return 0; // Not a leaf PTE
  
  return PTE2PA(*pte);
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
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
  if(size == 0) panic("mappages: size");

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

int
mappages_super(pagetable_t pagetable, uint64 va, uint64 pa, int perm)
{
  pte_t *pte;
  if (va % SUPERPGSIZE != 0 || pa % SUPERPGSIZE != 0)
    panic("mappages_super: alignment error");

  // Walk to L1 PTE slot. First, get L2 PTE to find L1 table.
  pte_t* l2_pte = &pagetable[PX(2, va)];
  pagetable_t l1_table;
  if (*l2_pte & PTE_V) {
    l1_table = (pagetable_t)PTE2PA(*l2_pte);
  } else {
    l1_table = (pagetable_t)kalloc();
    if (l1_table == 0) return -1;
    memset(l1_table, 0, PGSIZE);
    *l2_pte = PA2PTE(l1_table) | PTE_V;
  }
  pte = &l1_table[PX(1, va)];

  if(*pte & PTE_V)
    panic("mappages_super: remap");
  
  // PTE_S is a user-defined flag for superpage
  *pte = PA2PTE(pa) | perm | PTE_V | PTE_S;
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0) panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; ){
    if((pte = walk(pagetable, a, 0)) == 0 || (*pte & PTE_V) == 0){
      a += PGSIZE; 
      continue;
    }
    if((PTE_FLAGS(*pte) & PTE_V) == 0) panic("uvmunmap: not a leaf");

    uint64 mapped_size;
    if((*pte) & PTE_S){
      mapped_size = SUPERPGSIZE;
      if(do_free) superfree((void*)PTE2PA(*pte));
    } else {
      mapped_size = PGSIZE;
      if(do_free) kfree((void*)PTE2PA(*pte));
    }
    *pte = 0;
    a += mapped_size;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvmfirst(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("uvmfirst: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  // Code page should not be writable.
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}


// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned. Returns new size or 0 on error.
// This new version is "superpage-aware" and will try to use superpages
// for large, aligned allocations.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int perm)
{
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz;){
    // Try to allocate a superpage if the current address is superpage-aligned
    // and there's at least a full superpage of memory to allocate.
    if((a % SUPERPGSIZE == 0) && (a + SUPERPGSIZE <= newsz)) {
      char *mem = superalloc();
      if(mem != 0) {
        // If superpage allocation succeeds, map it.
        if(mappages_super(pagetable, a, (uint64)mem, perm) != 0){
          superfree(mem);
          uvmdealloc(pagetable, a, oldsz); // Clean up on mapping failure
          return 0;
        }
        a += SUPERPGSIZE;
        continue; // Continue to the next allocation address
      }
      // If superalloc() fails, fall through to try allocating with small pages.
    }

    // Fallback to allocating a small page.
    char *mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz); // Clean up on failure
      return 0;
    }
    memset(mem, 0, PGSIZE);
    // Use the provided permissions directly.
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, perm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz); // Clean up on mapping failure
      return 0;
    }
    a += PGSIZE;
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz. oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz. oldsz can be larger than the actual
// process size. Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
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
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
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
  char *mem;

  for(i = 0; i < sz;){
    if((pte = walk(old, i, 0)) == 0) panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0) panic("uvmcopy: page not present");

    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    
    // Use PTE_S as the superpage flag
    if(*pte & PTE_S){
      if((mem = superalloc()) == 0) goto err;
      memmove(mem, (char*)pa, SUPERPGSIZE);
      if(mappages_super(new, i, (uint64)mem, flags) != 0){
        superfree(mem);
        goto err;
      }
      i += SUPERPGSIZE;
    } else {
      if((mem = kalloc()) == 0) goto err;
      memmove(mem, (char*)pa, PGSIZE);
      if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
        kfree(mem);
        goto err;
      }
      i += PGSIZE;
    }
  }
  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n;
  while(len > 0){
    pte_t* pte = walk(pagetable, dstva, 0);

    if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0 || (*pte & PTE_W) == 0)
      return -1;

    uint64 pa_base = PTE2PA(*pte);
    uint64 page_size;

    if (*pte & PTE_S) {
      page_size = SUPERPGSIZE;
    } else {
      page_size = PGSIZE;
    }

    n = page_size - (dstva % page_size);
    if(n > len)
      n = len;

    uint64 phys_dst = pa_base + (dstva % page_size);

    memmove((void *)phys_dst, src, n);

    len -= n;
    src += n;
    dstva += n;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n;

  while(len > 0){
    pte_t* pte = walk(pagetable, srcva, 0);

    if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0 || (*pte & PTE_R) == 0)
      return -1;

    uint64 pa_base = PTE2PA(*pte);
    uint64 page_size;

    if (*pte & PTE_S) {
      page_size = SUPERPGSIZE;
    } else {
      page_size = PGSIZE;
    }

    n = page_size - (srcva % page_size);
    if(n > len)
      n = len;

    uint64 phys_src = pa_base + (srcva % page_size);

    memmove(dst, (void *)phys_src, n);

    len -= n;
    dst += n;
    srcva += n;
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
  uint64 n;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    pte_t* pte = walk(pagetable, srcva, 0);

    if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0 || (*pte & PTE_R) == 0)
      return -1;

    uint64 pa_base = PTE2PA(*pte);
    uint64 page_size;
    if (*pte & PTE_S) {
      page_size = SUPERPGSIZE;
    } else {
      page_size = PGSIZE;
    }

    n = page_size - (srcva % page_size);
    if(n > max)
      n = max;

    uint64 phys_src = pa_base + (srcva % page_size);
    char *p = (char *)phys_src;

    while(n > 0){
      if(*p == '\0'){
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
      srcva++;
    }
  }

  if(got_null){
    return 0;
  } else {
    return -1;
  }
}


#ifdef LAB_PGTBL
void vmprint_helper(pagetable_t pagetable, uint64 va, int level)
{
  char *dots;
  switch (level)
  {
  case 0:
    dots = " ..";
    break;
  case 1:
    dots = " .. ..";
    break;
  case 2:
    dots = " .. .. ..";
    break;
  default:
    dots = "............";
    break;
  }
  for (int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    if (pte & PTE_V)
    {
      uint64 child = PTE2PA(pte);
      uint64 child_va = va + (i << (9 * (2 - level) + 12));
      printf("%s%p: pte %p pa %p\n", dots, (void *)child_va, (void *)pte, (void *)child);
      if (level < 2)
      {
        vmprint_helper((pagetable_t)child, child_va, level + 1);
      }
    }
  }
}
void
vmprint(pagetable_t pagetable) {
  printf("page table %p\n", pagetable);
  vmprint_helper(pagetable, 0, 0);
}

#endif



#ifdef LAB_PGTBL
pte_t*
pgpte(pagetable_t pagetable, uint64 va) {
  return walk(pagetable, va, 0);
}
#endif
