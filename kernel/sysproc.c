#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "fcntl.h"
#include "file.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
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
sys_mmap(void)
{
  uint64 addr;
  int len, prot, flags, fd, offset;
  struct proc *p = myproc();
  struct file *f;
  
  argaddr(0, &addr);
  argint(1, &len);
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);
  argint(5, &offset);

  if (len <= 0) {
    return -1;
  }
  
  if(addr != 0){ // Lab requires addr to be 0
    return -1;
  }

  if(fd < 0 || fd >= NOFILE || (f = p->ofile[fd]) == 0){
    return -1;
  }

  if((prot & PROT_READ) && !f->readable){
    return -1;
  }

  if((prot & PROT_WRITE) && (flags & MAP_SHARED) && !f->writable){
    return -1;
  }

  struct vma *free_vma = 0;
  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].len == 0){
      free_vma = &p->vmas[i];
      break;
    }
  }
  if(free_vma == 0){
    return -1; 
  }

  // Find the next available address, leaving a guard page between mappings.
  uint64 va = 0x80000000;
  for (int i = 0; i < NVMA; i++) {
    if (p->vmas[i].len > 0) {
      uint64 end_addr = p->vmas[i].addr + p->vmas[i].len;
      if (end_addr >= va) { // Use >= to find the highest address
        va = PGROUNDUP(end_addr) + PGSIZE; // Add a guard page
      }
    }
  }
  addr = va;

  free_vma->addr = addr;
  free_vma->len = len;
  free_vma->prot = prot;
  free_vma->flags = flags;
  free_vma->f = filedup(f);
  free_vma->offset = offset;

  return addr;
}

uint64
sys_munmap(void)
{
  uint64 addr;
  int len;
  struct proc *p = myproc();
  struct vma *vma = 0;

  argaddr(0, &addr);
  argint(1, &len);

  // Find the VMA that the start of the range lies in.
  for (int i = 0; i < NVMA; i++) {
    if (p->vmas[i].len > 0 && addr >= p->vmas[i].addr && addr < p->vmas[i].addr + p->vmas[i].len) {
      vma = &p->vmas[i];
      break;
    }
  }

  if (!vma) {
    return -1;
  }

  uint64 start_page = PGROUNDDOWN(addr);
  uint64 end_page = PGROUNDUP(addr + len);

  // Write back if MAP_SHARED
  if ((vma->flags & MAP_SHARED)) {
    for (uint64 a = start_page; a < end_page; a += PGSIZE) {
      pte_t *pte = walk(p->pagetable, a, 0);
      if (pte != 0 && (*pte & PTE_V) && (*pte & PTE_D)) {
        begin_op();
        ilock(vma->f->ip);
        
        uint64 file_offset = vma->offset + (a - vma->addr);
        if (file_offset < vma->f->ip->size) {
          uint bytes_to_write = PGSIZE;
          if (file_offset + PGSIZE > vma->f->ip->size) {
            bytes_to_write = vma->f->ip->size - file_offset;
          }
          writei(vma->f->ip, 0, PTE2PA(*pte), file_offset, bytes_to_write);
        }
        
        iunlock(vma->f->ip);
        end_op();
      }
    }
  }

  for (uint64 a = start_page; a < end_page; a += PGSIZE) {
    pte_t *pte = walk(p->pagetable, a, 0);
    if (pte != 0 && (*pte & PTE_V)) {
      uvmunmap(p->pagetable, a, 1, 1);
    }
  }

  // Update VMA metadata correctly for partial unmaps.
  if (addr == vma->addr && len >= vma->len) {
    // Unmapping the whole region.
    vma->len = 0;
    fileclose(vma->f);
  } else if (addr == vma->addr) {
    // Unmapping from the start.
    vma->addr += len;
    vma->len -= len;
    vma->offset += len;
  } else if (addr + len >= vma->addr + vma->len) {
    // Unmapping from the end.
    vma->len = addr - vma->addr;
  }
  // The lab says we don't need to handle punching a hole in the middle.

  return 0;
}