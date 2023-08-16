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

#define NBUK  13
#define hash(dev, blockno) ((dev * blockno) % NBUK)

struct bucket
{
  struct spinlock lock;
  struct buf head;
};


struct {
  struct spinlock lock; // 主要保护的是连接所有槽位的链表。
  struct buf buf[NBUF]; // 代表我们有30个槽位可用。

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  //struct buf head;
  struct bucket buckets[NBUK]; // the cache has 13 buckets
} bcache;

void
binit(void)
{
  struct buf *b;
  struct buf *prev_b;

  initlock(&bcache.lock, "bcache");
  // 将所有的 buf 都先放到 buckets[0] 中
  for(int i=0; i<NBUK; i++)
  {
    initlock(&bcache.buckets[i].lock, "bcache.bucket");
    bcache.buckets[i].head.next = (void *)0;
    // 我们首先将所有的bufs传给buckets[0]。
    if(i == 0)
    {
      prev_b = &bcache.buckets[i].head;
      for(b = bcache.buf; b < bcache.buf+NBUF; b++)
      {
        if(b == bcache.buf + NBUF - 1)  //buf[29]
        {
          b->next = (void*)0;
        }
        prev_b->next = b;
        b->timestamp = ticks; // 当初始化内核时，ticks == 0
        initsleeplock(&b->lock, "buffer");
        prev_b = b; 
      }
    }
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
// 根据输入的设备号和块号，扫描Buffer Cache中的所有缓存块。
// 如果缓存命中，bget更新引用计数refcnt，释放bcache.lock
// 先判断是否之前已经缓存过了硬盘中的这个块。如果有，那就直接返回对应的缓存，
// 如果没有，会去找到一个最长时间没有使用的缓存，并且把那个缓存分配给当前块。
// 所有的缓存被串到了一个双向链表里。链表的第一个元素是最近使用的，最后一个元素是很久没有使用的。
static struct buf *
bget(uint dev, uint blockno) // 设备号和块号
{
  struct buf *b;
  int buk_id = hash(dev, blockno);

  acquire(&bcache.buckets[buk_id].lock);
  //buckets[nik_id]有buf直接用
  for (b = bcache.buckets[buk_id].head.next; b; b=b->next)
  {
    if(b->dev == dev && b->blockno == blockno)
    {
      b->refcnt++;
      release(&bcache.buckets[buk_id].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.buckets[buk_id].lock);

  // 没有,遍历bucket
  // 从当前的bucket中steal，缓存到目标bucket[buk_id]
  // 释放buf时 cnt=0更新timestamp，
  //找到符合的LRU时， 找cnt=0，timestamp最大的
  int max_timestamp = 0;
  int lru_buk_id = -1;
  int is_better = 0; //是否有更好的lru_buk_id
  struct buf *lru_b = (void*)0;
  struct buf *prev_lru_b = (void*)0;

  // 当refcnt == 0时，找到lru_buk_id，并在每个桶[i]中获得最大的时间戳。
  struct buf *prev_b = (void*)0;
  for(int i=0; i<NBUK; ++i)
  {
    prev_b = &bcache.buckets[i].head;
    acquire(&bcache.buckets[i].lock);
    while(prev_b->next)
    {
      if (prev_b->next->refcnt == 0 && prev_b->next->timestamp >= max_timestamp)
      {
        max_timestamp = prev_b->next->timestamp;
        is_better = 1;
        prev_lru_b = prev_b; // get prev_lru_b
      }
      prev_b = prev_b->next;
    }
    if(is_better)
    {
      if(lru_buk_id != -1)
      {
        release(&bcache.buckets[lru_buk_id].lock);
      }
      lru_buk_id = i;
    }
    else{
      release(&bcache.buckets[i].lock);
    }
    is_better = 0;
  }
  //get lru_b
  lru_b = prev_lru_b->next;
  //steal
  if (lru_b)
  {
    prev_lru_b->next = prev_lru_b->next->next;
    release(&bcache.buckets[lru_buk_id].lock);
  }
  // 缓存lru_b到buckets[buk_id]。
  acquire(&bcache.lock);
  acquire(&bcache.buckets[buk_id].lock);
  if (lru_b)
  {
    lru_b->next = bcache.buckets[buk_id].head.next;
    bcache.buckets[buk_id].head.next = lru_b;
  }
  // 如果两个进程在buckets[lru_buk_id]中使用相同的块(相同的块号)。
  // 一个进程可以检查它，如果已经在这里，我们就可以得到它。
  // 否则，我们将在两个进程中使用相同的块，并进行双倍缓存
  b = bcache.buckets[buk_id].head.next; // buckets[buk_id]中的第一个buf。
  while (b)
  {
    if (b->dev == dev && b->blockno == blockno)
    {
      b->refcnt++;
      release(&bcache.buckets[buk_id].lock);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
    b = b->next;
  }
  // 在遍历每个桶的时候找不到lru_b
  if (lru_b == 0)
    panic("bget: no buffers");

  lru_b->dev = dev;
  lru_b->blockno = blockno;
  lru_b->valid = 0;
  lru_b->refcnt = 1;
  release(&bcache.buckets[buk_id].lock);
  release(&bcache.lock);
  acquiresleep(&lru_b->lock);
  return lru_b;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno); // 通过调用bget来获取指定磁盘块的缓存块
  if(!b->valid) {
    // 如果b->valid=0，说明这个槽位是刚被回收的，还没有缓存任何磁盘块，
    // 因此调用virtio_disk_rw来先从磁盘上读取相应磁盘块的内容，读取完成后更新b->valid。
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b; // 返回的是上锁的且可用的缓存块。
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
// 调用者结束对一个缓存块的处理(读或写)之后，
// 调用brelse更新bcache的链表，并且释放对应的缓存块的睡眠锁
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");
  // 先释放缓存块的睡眠锁
  releasesleep(&b->lock);

  int buk_id = hash(b->dev, b->blockno);
  acquire(&bcache.buckets[buk_id].lock);
  b->refcnt--; // 减去引用计数refcnt
  // 最近刚刚更新的块b->refcnt=1
  if (b->refcnt == 0) {
    b->timestamp = ticks;
  }
  release(&bcache.buckets[buk_id].lock);
}

void 
bpin(struct buf *b)
{
  int buk_id = hash(b->dev, b->blockno);
  acquire(&bcache.buckets[buk_id].lock);
  b->refcnt++;
  release(&bcache.buckets[buk_id].lock);
}

void 
bunpin(struct buf *b)
{
  int buk_id = hash(b->dev, b->blockno);
  acquire(&bcache.buckets[buk_id].lock);
  b->refcnt--;
  release(&bcache.buckets[buk_id].lock);
}