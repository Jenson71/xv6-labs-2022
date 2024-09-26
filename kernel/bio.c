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

static uint hash(uint key,uint tablesize);

struct bucket_t{
  struct spinlock bucketlock;
  struct buf head;
};

struct {
  struct bucket_t bucket[BUCKETSIZE]; 
  struct buf buf[NBUF];
} bcache;

void initbucket(struct bucket_t* bucket)
{
  initlock(&bucket->bucketlock,"bcache.bucket");
  (bucket->head).prev=&bucket->head;
  (bucket->head).next=&bucket->head;
}
void
binit(void)
{
  struct buf* b;
  for(int i=0;i<BUCKETSIZE;i++){
    initbucket(&bcache.bucket[i]);
  }
  for(int i=0;i<NBUF;i++){
    b=&bcache.buf[i];
    initsleeplock(&b->lock,"buffer");
    b->prev=b->next=b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  uint key=hash(blockno,BUCKETSIZE);
  struct buf* b;
  struct bucket_t* bt=&bcache.bucket[key];

  acquire(&bt->bucketlock);

  for(b=bt->head.next;b!=&bt->head;b=b->next){
    if(b->dev==dev && b->blockno==blockno){
      b->refcnt++;
      release(&bt->bucketlock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  for(int i=0;i<NBUF;i++){
    if(!bcache.buf[i].used &&
      !__atomic_test_and_set(&bcache.buf[i].used, __ATOMIC_ACQUIRE)){
      b=&bcache.buf[i];
      b->dev=dev;
      b->blockno=blockno;
      b->valid=0;
      b->refcnt=1;

      (bt->head).next->prev=b;
      b->next=(bt->head).next;
      b->prev=&bt->head;
      (bt->head).next=b;
      release(&bt->bucketlock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget:no buffers!");
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

  uint key=hash(b->blockno,BUCKETSIZE);
  struct bucket_t* bt=&bcache.bucket[key];
  acquire(&bt->bucketlock);
  b->refcnt--;
  if(b->refcnt==0){
    b->prev->next=b->next;
    b->next->prev=b->prev;
    __atomic_clear(&b->used, __ATOMIC_RELEASE);
  }
  release(&bt->bucketlock);
}

void
bpin(struct buf *b) {
  uint key=hash(b->blockno,BUCKETSIZE);

  struct bucket_t* bt=&bcache.bucket[key];
  acquire(&bt->bucketlock);
  b->refcnt++;
  release(&bt->bucketlock);
}

void
bunpin(struct buf *b) {
  uint key=hash(b->blockno,BUCKETSIZE);

  struct bucket_t* bt=&bcache.bucket[key];
  acquire(&bt->bucketlock);
  b->refcnt--;
  release(&bt->bucketlock);
}

static uint hash(uint key,uint tablesize)
{
  return (key%tablesize);
}

