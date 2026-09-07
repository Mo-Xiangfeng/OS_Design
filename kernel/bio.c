// Buffer cache.
//
// The buffer cache is a set of buf structures holding
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
//
// Buffers are kept in a number of hash buckets, each with its own lock,
// to reduce lock contention.  Each buffer has a timestamp (updated when
// it is released) used as an approximate LRU order for eviction.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct {
  struct spinlock lock[NBUCKET];
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Each buffer belongs to the bucket determined by its block number.
  // head.next is most recent, head.prev is least.
  struct buf bucket[NBUCKET];
} bcache;

#define HASH(dev, blockno) (((dev) * 251 + (blockno)) % NBUCKET)

void
binit(void)
{
  struct buf *b;

  for(int i = 0; i < NBUCKET; i++){
    initlock(&bcache.lock[i], "bcache");
    bcache.bucket[i].prev = &bcache.bucket[i];
    bcache.bucket[i].next = &bcache.bucket[i];
  }

  // Create linked list of buffers, placing each into some bucket.
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    initsleeplock(&b->lock, "buffer");
    int i = HASH(0, b - bcache.buf);
    b->next = bcache.bucket[i].next;
    b->prev = &bcache.bucket[i];
    bcache.bucket[i].next->prev = b;
    bcache.bucket[i].next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int i = HASH(dev, blockno);
  uint best_time;
  int best_i;

retry:
  // Is the block already cached?
  acquire(&bcache.lock[i]);
  for(b = bcache.bucket[i].next; b != &bcache.bucket[i]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock[i]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.lock[i]);

  // Not cached: find the least recently used unused buffer
  // (smallest timestamp) among all buckets.
  best_i = -1;
  best_time = 0;
  for(int j = 0; j < NBUCKET; j++){
    acquire(&bcache.lock[j]);
    for(b = bcache.bucket[j].next; b != &bcache.bucket[j]; b = b->next){
      if(b->refcnt == 0 && (best_i == -1 || b->timestamp < best_time)){
        best_i = j;
        best_time = b->timestamp;
      }
    }
    release(&bcache.lock[j]);
  }

  if(best_i == -1)
    panic("bget: no buffers");

  // Remove the victim from its bucket.  Another CPU may have taken
  // the buffer in the meantime, in which case retry the whole search.
  acquire(&bcache.lock[best_i]);
  for(b = bcache.bucket[best_i].next; b != &bcache.bucket[best_i]; b = b->next){
    if(b->refcnt == 0 && b->timestamp == best_time)
      break;
  }
  if(b == &bcache.bucket[best_i]){
    release(&bcache.lock[best_i]);
    goto retry;
  }
  b->next->prev = b->prev;
  b->prev->next = b->next;
  release(&bcache.lock[best_i]);

  b->dev = dev;
  b->blockno = blockno;
  b->valid = 0;
  b->refcnt = 1;

  // Insert the buffer into its new bucket.
  acquire(&bcache.lock[i]);
  b->next = bcache.bucket[i].next;
  b->prev = &bcache.bucket[i];
  bcache.bucket[i].next->prev = b;
  bcache.bucket[i].next = b;
  release(&bcache.lock[i]);

  acquiresleep(&b->lock);
  return b;
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

  int i = HASH(b->dev, b->blockno);
  acquire(&bcache.lock[i]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->timestamp = ticks;
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.bucket[i].next;
    b->prev = &bcache.bucket[i];
    bcache.bucket[i].next->prev = b;
    bcache.bucket[i].next = b;
  }

  release(&bcache.lock[i]);
}

void
bpin(struct buf *b) {
  int i = HASH(b->dev, b->blockno);
  acquire(&bcache.lock[i]);
  b->refcnt++;
  release(&bcache.lock[i]);
}

void
bunpin(struct buf *b) {
  int i = HASH(b->dev, b->blockno);
  acquire(&bcache.lock[i]);
  b->refcnt--;
  release(&bcache.lock[i]);
}
