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

struct
{
  struct spinlock lock;
  uint32 ref[PGCNT];
} kref;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&kref.lock, "kref");
  freerange(end, (void *)PHYSTOP);
}

void
addpgcnt(void *pa)
{
  acquire(&kref.lock);
  kref.ref[PGIDX(pa)]++;
  release(&kref.lock);
}

// 将COW页划分开
// 如果引用计数为1，则应该设置当前页COW=0,W=1，返回1
// 如果引用计数大于1，将引用次数减1，之后返回2
// 如果引用次数为0，返回-1
int
splitpg(void *pa)
{
  acquire(&kref.lock);
  int index = PGIDX(pa);
  int count = kref.ref[index];
  if (count == 1)
  {
    release(&kref.lock);
    return 1;
  }else if (count > 1){
    kref.ref[index]--;
    release(&kref.lock);
    return 2;
  }
  release(&kref.lock);
  return -1;
}

void
freerange(void *pa_start, void *pa_end)
{
  char *pa;
  struct run *r;
  pa = (char*)PGROUNDUP((uint64)pa_start);
  for(; pa + PGSIZE <= (char*)pa_end; pa += PGSIZE)
  {
    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);

    r = (struct run *)pa;

    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
  }
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

  acquire(&kref.lock);
  int index = PGIDX(pa);
  if (kref.ref[index] > 1)
  {
    // 如果计数大于1，减1后返回
    kref.ref[index]--;
    release(&kref.lock);
    return;
  }
  else if (kref.ref[index] == 1)
  {
    // 计数等于1，释放然后返回
    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);

    r = (struct run *)pa;

    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    kref.ref[index] = 0;
    release(&kmem.lock);
    release(&kref.lock);
  }
  else
  {
    // 重复释放，panic
    release(&kref.lock);
    panic("kfree: re-release\n");
  }
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
  {
    kmem.freelist = r->next;
  }
  release(&kmem.lock);

  if(r)
  {
    memset((char *)r, 5, PGSIZE); // fill with junk
    // 空闲链表中的页计数必定为0，无需检查
    addpgcnt(r); // 注意kmem和kref锁序
  }
  return (void*)r;
}
