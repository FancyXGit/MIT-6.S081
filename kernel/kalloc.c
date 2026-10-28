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

/*
 * 为每一个CPU维持一个空闲链表，遵循LRU原则
 * 由于只需要保证每个链表内部LRU，因此不需要维持全局LRU链表（参考CSAPP Malloc LAB)
 * 为每个链表维持一个锁
 * 必须注意死锁问题，所有的锁都按照CPU顺序拿，不能先拿自己的，再去拿别人的，会死锁
 * run结构体中的next指针，既是下一个链表节点的地址，也是空闲页的地址，因为节点就存在空闲页的前8个字节里面
 */

// run自己的地址既是空闲页物理地址，也是结构体的地址
struct run {
  struct run *next;
};

// 单独一条链表
struct kmem_list {
  struct spinlock lock;
  struct run *freelist;
  // 没地方存锁名字，干脆存这里
  char lock_name[16];
};

// 全局链表数组
struct kmem_list kmem_array[NCPU];

// kinit最开始只有CPU0调用
// 根据CPU只能操作本表的原则，简单起见将初始空闲页全部挂到CPU0的链表上
void
kinit()
{
  // 先初始化所有锁，将链表头设为NULL
  for (int i = 0; i < NCPU; i++)
  {
    snprintf(kmem_array[i].lock_name, 16, "kmem_%d", i);
    initlock(&kmem_array[i].lock, kmem_array[i].lock_name);
    kmem_array[i].freelist = 0;
  }
  // 释放全部，全部挂载到CPU0所在的链表
  freerange(end, (void *)PHYSTOP);
}

// freerange仅在启动初始化时调用，不加锁
void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// 将对应页释放，加在对应CPU的空闲链表上
// 需要获取对应的锁
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  // 关中断获取CPU序号
  push_off();
  int cpu = cpuid();
  

  struct kmem_list *curr = &kmem_array[cpu];

  acquire(&curr->lock);
  r->next = curr->freelist;
  curr->freelist = r;
  release(&curr->lock);

  // 直到现在才开中断，为了保持全过程CPUID都是准确的
  pop_off();
}

// 分配一页
void *
kalloc(void)
{
  struct run *r = 0;

  // 关中断获取CPU序号
  push_off();
  int cpu = cpuid();
  
  struct kmem_list *curr = &kmem_array[cpu];

  acquire(&curr->lock);
  r = curr->freelist;
  if(r)
  {
    // 找到了
    curr->freelist = r->next;
    release(&curr->lock);
  }
  else
  {
    // 没找到，需要找其他CPU的链表
    // 先释放锁退出
    release(&curr->lock);
    // 尝试获取别的CPU的链表
    for (int i = 0; i < NCPU; i++)
    {
      if (i == cpu)
      {
        continue;
      }
      struct kmem_list *dest = &kmem_array[i];
      acquire(&dest->lock);
      if (dest->freelist == 0)
      {
        // 没有，释放锁跳过
        release(&dest->lock);
      }
      else
      {
        // 找到有了的
        r = dest->freelist;
        dest->freelist = r->next;
        release(&dest->lock);
        break;
      }
    }
  }
  
  if(r)
  {
    memset((char *)r, 5, PGSIZE); // fill with junk
  }
  
  pop_off();
  return (void*)r;
}
