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

// 哈希表大小
#define NBUFMAP_BUCKET 13
// 哈希函数
#define BUFMAP_HASH(dev, blockno) ((((dev) << 27) | (blockno)) % NBUFMAP_BUCKET)

struct {
  struct buf buf[NBUF]; // 该buf数组表示全部的buffer空间，用于初始化遍历
  struct spinlock eviction_lock; // 用于保护buffer的结构，需要修改桶中元素位置时申请该锁
  // 将buffer分到不同的桶中，降低锁的粒度
  struct buf bufmap[NBUFMAP_BUCKET]; 
  struct spinlock bufmap_locks[NBUFMAP_BUCKET]; // 分别保护每个桶的引用计数
} bcache;

void
binit(void)
{
  // 初始化bufmap
  for (int i = 0;  i < NBUFMAP_BUCKET; ++i) {
    initlock(&bcache.bufmap_locks[i], "bcache_bufmap");
    bcache.bufmap[i].next = 0;
  }

  // 初始化每个buffer空间
  for (int i = 0; i < NBUF; ++i) {
    struct buf *b = &bcache.buf[i];
    initsleeplock(&b->lock, "buffer"); // 初始化每个buf的锁
    b->lastuse = 0;
    b->refcnt = 0; // 引用计数设置为0
    // 初始化时将所有buf空间全部放在第一个桶下面
    b->next = bcache.bufmap[0].next;
    bcache.bufmap[0].next = b;
  }
  initlock(&bcache.eviction_lock, "bcache_eviction");
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
// 使用dev和blockno计算哈希值
  uint key = BUFMAP_HASH(dev, blockno);

  acquire(&bcache.bufmap_locks[key]);

  // 确认buffer中是否已经存在该磁盘块
  for (b = bcache.bufmap[key].next; b != (void*)0; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      // 找到相同的磁盘块，直接返回该缓冲区
      b->refcnt++;
      release(&bcache.bufmap_locks[key]);
      acquiresleep(&b->lock); // 使用睡眠锁确保当前块没有其他进程在使用
      return b;
    }
  }

  // 尝试在其他桶中窃取可用缓冲块
  // 首先释放当前缓冲块的锁，避免造成死锁
  release(&bcache.bufmap_locks[key]);
  // 需要修改缓冲结构，获取保护结构的锁
  acquire(&bcache.eviction_lock);
  // 由于在release与acquire之间可能会被中断，导致有缓冲区被重置
  // 所以获得锁之后要再次判断 避免在哈希表中同时缓冲了两个相同的磁盘区域 
  for (b = bcache.bufmap[key].next; b != (void*)0; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      acquire(&bcache.bufmap_locks[key]);
      b->refcnt++;
      release(&bcache.bufmap_locks[key]);
      release(&bcache.eviction_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // 再次确认后依旧没有所需要的缓冲区
  // 寻找一个最久没有使用过的缓冲区进行替换
  struct buf *brefore_least = 0;
  uint holding_bucket = -1;
  
  for (int i = 0; i < NBUFMAP_BUCKET; ++i) {
    acquire(&bcache.bufmap_locks[i]); // 保护refcnt
    int newfound = 0; // 在当前桶中是否找到的最近最少使用的块
    for (b = &bcache.bufmap[i]; b->next; b = b->next) {
      if (b->next->refcnt == 0
      && (!brefore_least // 还没有找过个合适的块
          || b->next->lastuse < brefore_least->next->lastuse) // 当前块释放时间更早 
          ) {
        brefore_least = b;
        newfound = 1;
      }
    }
    if (!newfound) {
      release(&bcache.bufmap_locks[i]);
    } else {
      // 释放存放上一个适合缓冲块的桶的锁，保留新的适合缓冲块的锁
      if (holding_bucket != -1) {
        release(&bcache.bufmap_locks[holding_bucket]);
      }
      holding_bucket = i;
      // 如果在当前桶中找到合适的块，不释放锁
    }
  }

  if (!brefore_least) {
    // 所有桶中都没有找到合适的块
    panic("bget: no buffers");
  }

  b = brefore_least->next; // 合适块的地址

  if (holding_bucket != key) {
    // 需要从其他的桶中拿取块
    brefore_least->next = b->next; // 将b从原来桶中链表断开
    release(&bcache.bufmap_locks[holding_bucket]); // 释放原来桶的锁
    acquire(&bcache.bufmap_locks[key]); // 获取待插入桶的锁
    // 将合适的块头插到key桶中
    b->next = bcache.bufmap[key].next;
    bcache.bufmap[key].next = b;
  }
  // 合适的块本身就在key桶中，无需从其他桶迁移
  // 无需再获得锁，之前已经保持了该桶的锁
  // acquire(&bcache.bufmap_locks[key]); 
  b->dev = dev;
  b->blockno = blockno;
  b->refcnt = 1;
  b->valid = 0;
  release(&bcache.bufmap_locks[key]); 
  release(&bcache.eviction_lock);
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

  uint key = BUFMAP_HASH(b->dev, b->blockno);
  acquire(&bcache.bufmap_locks[key]);
  b->refcnt--;
  if (b->refcnt == 0) {
    b->lastuse = ticks; // 如果当前缓冲块没有被引用，释放该区域时更新上次释放的时间
  }
  release(&bcache.bufmap_locks[key]);
}

void
bpin(struct buf *b) {
  uint key = BUFMAP_HASH(b->dev, b->blockno);
  acquire(&bcache.bufmap_locks[key]);
  b->refcnt++;
  release(&bcache.bufmap_locks[key]);
}

void
bunpin(struct buf *b) {
  uint key = BUFMAP_HASH(b->dev, b->blockno);
  acquire(&bcache.bufmap_locks[key]);
  b->refcnt--;
  release(&bcache.bufmap_locks[key]);
}


