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
} kmem[NCPU]; // 为每个CPU都分配一个空闲列表，用独立的锁进行保护

void
kinit()
{
  // 初始化每一个锁
  for (int i = 0; i < NCPU; ++i) {
    initlock(&kmem[i].lock, "kmem");
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

// Free the page of physical memory pointed at by v,
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
  // 在需要获取CPUID时要关闭中断，避免在使用CPUID的过程中该进程被切换到其他CPU上执行
  push_off();
  int cpu = cpuid();
  // 只获取当前CPU上的锁，将该页面添加到当前CPU的空闲列表中
  acquire(&kmem[cpu].lock);
  r->next = kmem[cpu].freelist;
  kmem[cpu].freelist = r;
  release(&kmem[cpu].lock);
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
  int cpu = cpuid();
  acquire(&kmem[cpu].lock);
  if (!kmem[cpu].freelist) {
    // 如果当前CPU上没有空闲列表了，需要向其他CPU偷取页面
    int steal_page_nums = 64; // 定义一次最大偷取的页面数量
    for (int i = 0; i < NCPU; ++i) {
      if (i == cpu) {
        // 跳过自身
        continue;
      }
      // 获取待偷取CPU的空闲队列锁
      acquire(&kmem[i].lock); 
      struct run *other_freelist = kmem[i].freelist;
      while (other_freelist && steal_page_nums > 0) {
        // 其他CPU空闲列表不为空，并且还需要进行偷取
        // 使用头插法将偷取的空闲列表放入当前CPU的空闲列表中
        kmem[i].freelist = other_freelist->next;
        other_freelist->next = kmem[cpu].freelist;
        kmem[cpu].freelist = other_freelist;
        // 待偷取数量减一
        steal_page_nums--;
        // 重新定位待偷取页面
        other_freelist = kmem[i].freelist;
      }
      release(&kmem[i].lock);
      if (steal_page_nums <= 0) {
        // 数量满足，停止偷取
        break;
      }
    }
  }

  r = kmem[cpu].freelist;
  if(r)
    kmem[cpu].freelist = r->next;
  release(&kmem[cpu].lock);
  // 不再使用CPUID，开启中断
  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
