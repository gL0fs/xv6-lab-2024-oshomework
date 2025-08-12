// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 13

static inline int
hash(uint blockno)
{
  return blockno % NBUCKET;
}

struct {
  struct spinlock lock[NBUCKET];
  struct buf buf[NBUF];
  struct buf head[NBUCKET];
} bcache;

void
binit(void)
{
  struct buf *b;
  char lock_name[12];

  for(int i = 0; i < NBUCKET; i++){
    snprintf(lock_name, sizeof(lock_name), "bcache_%d", i);
    initlock(&bcache.lock[i], lock_name);
  }

  for(int i = 0; i < NBUCKET; i++){
    bcache.head[i].prev = &bcache.head[i];
    bcache.head[i].next = &bcache.head[i];
  }

  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.head[0].next;
    b->prev = &bcache.head[0];
    initsleeplock(&b->lock, "buffer");
    bcache.head[0].next->prev = b;
    bcache.head[0].next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int bucket_idx = hash(blockno);

again:
  acquire(&bcache.lock[bucket_idx]);
  for(b = bcache.head[bucket_idx].next; b != &bcache.head[bucket_idx]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock[bucket_idx]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  for(b = bcache.head[bucket_idx].next; b != &bcache.head[bucket_idx]; b = b->next){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock[bucket_idx]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.lock[bucket_idx]);

  for (int i = 0; i < NBUCKET; i++) {
    if(i == bucket_idx) continue;

    acquire(&bcache.lock[i]);
    for (b = bcache.head[i].next; b != &bcache.head[i]; b = b->next) {
      if (b->refcnt == 0) {

        int old_bucket_idx = i;
        int new_bucket_idx = bucket_idx;

        release(&bcache.lock[old_bucket_idx]);

        if(old_bucket_idx < new_bucket_idx){
          acquire(&bcache.lock[old_bucket_idx]);
          acquire(&bcache.lock[new_bucket_idx]);
        } else {
          acquire(&bcache.lock[new_bucket_idx]);
          acquire(&bcache.lock[old_bucket_idx]);
        }

        if(b->refcnt != 0){
            release(&bcache.lock[old_bucket_idx]);
            release(&bcache.lock[new_bucket_idx]);
            goto again;
        }

        struct buf *b2;
        for(b2 = bcache.head[new_bucket_idx].next; b2 != &bcache.head[new_bucket_idx]; b2 = b2->next){
            if(b2->dev == dev && b2->blockno == blockno){
                b2->refcnt++;
                release(&bcache.lock[old_bucket_idx]);
                release(&bcache.lock[new_bucket_idx]);
                acquiresleep(&b2->lock);
                return b2;
            }
        }

        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;

        b->prev->next = b->next;
        b->next->prev = b->prev;

        b->next = bcache.head[new_bucket_idx].next;
        b->prev = &bcache.head[new_bucket_idx];
        bcache.head[new_bucket_idx].next->prev = b;
        bcache.head[new_bucket_idx].next = b;

        release(&bcache.lock[old_bucket_idx]);
        release(&bcache.lock[new_bucket_idx]);
        
        acquiresleep(&b->lock);
        return b;
      }
    }
    release(&bcache.lock[i]);
  }

  goto again;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int bucket_idx = hash(b->blockno);
  acquire(&bcache.lock[bucket_idx]);
  b->refcnt--;
  release(&bcache.lock[bucket_idx]);
}

void
bpin(struct buf *b) {
  int bucket_idx = hash(b->blockno);
  acquire(&bcache.lock[bucket_idx]);
  b->refcnt++;
  release(&bcache.lock[bucket_idx]);
}

void
bunpin(struct buf *b) {
  int bucket_idx = hash(b->blockno);
  acquire(&bcache.lock[bucket_idx]);
  b->refcnt--;
  release(&bcache.lock[bucket_idx]);
}


