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

/*
 * 维持多个桶，每个桶配置一把锁，每个桶里面是对应的环形缓冲区
 * 按照建议，所有缓冲区不再设立头，删除head字段改为使用最后一次使用的时间戳
 * 注意同一时刻必须只能持有一个桶锁，以避免ABBA死锁
 * 必须先获取全局锁再获取桶锁
 */

#define NBUCKETS 13

// 缓冲区实际占地
struct buf bbufs[NBUF];

// 一个桶
struct bbucket
{
  struct spinlock lock;
  char lock_name[16];
  struct buf *list;
};

// 缓冲桶数组
struct bbucket barray[NBUCKETS];
// 全局锁
struct spinlock b_lock;

// 将对应buf插到桶的头部
// 不加锁
static void insertList(struct buf *curr_buf, struct bbucket *curr_bucket)
{
  struct buf *head = curr_bucket->list;
  curr_buf->prev = 0;
  curr_buf->next = head;
  curr_bucket->list = curr_buf;
  if (head != 0)
  {
    head->prev = curr_buf;
  }
  curr_buf->bucket = curr_bucket;
}

// 缓冲区初始化
void binit(void)
{
  // 初始化全局锁
  initlock(&b_lock, "b_lock");

  // 初始化桶锁
  for (int i = 0; i < NBUCKETS; i++)
  {
    struct bbucket *curr_bucket = &barray[i];
    snprintf(curr_bucket->lock_name, 16, "bbucket_%d", i);
    initlock(&curr_bucket->lock, curr_bucket->lock_name);
    curr_bucket->list = 0;
  }

  // 初始化缓冲区锁，以及时间戳
  // 同时平均加入进桶
  for (int i = 0; i < NBUF; i++)
  {
    initsleeplock(&bbufs[i].lock, "buffer");
    bbufs[i].timestamp = ticks;
    insertList(&bbufs[i], &barray[i % NBUCKETS]);
  }
}

// 哈希函数，仅对块号取余
static int hashBuf(uint blockno)
{
  return blockno % NBUCKETS;
}

// 仅仅将其从链表中移除，即移出桶
static void removeBuf(struct buf *b, struct bbucket *bucket)
{
  struct buf *prev = b->prev;
  struct buf *next = b->next;
  if (prev != 0)
  {
    prev->next = next;
  }
  else
  {
    bucket->list = b->next;
  }
  if (next != 0)
  {
    next->prev = prev;
  }
  b->prev = 0;
  b->next = 0;
  b->bucket = 0;
}

// 用于占用空闲buf
// 写内容
// 放桶锁（必须持有桶锁）
// 不获取睡眠锁
// 没有放全局锁
static void takeBuf(struct buf *b, struct bbucket *bucket, int dev, int blockno)
{
  b->dev = dev;
  b->blockno = blockno;
  b->valid = 0;
  b->refcnt = 1;
  release(&bucket->lock);
}

// 用于对已有buf增加计数
// 会释放桶锁然后获取睡眠锁
// 必须持有桶锁
static void AddBufCnt(struct buf *b, struct bbucket *bucket)
{
  b->refcnt++;
  release(&bucket->lock);
  acquiresleep(&b->lock);
  b->timestamp = ticks;
}

// 尝试在桶中找到缓冲区
// 找不到就占用一个，可能从别的缓冲桶挪用
struct buf *bget(uint dev, uint blockno)
{
  // 尝试在对应桶中找已有的
  int bucket_index = hashBuf(blockno);
  struct bbucket *curr_bucket = &barray[bucket_index];
  acquire(&curr_bucket->lock);
  struct buf *curr_buf;

  // 桶中查找
  for (curr_buf = curr_bucket->list; curr_buf != 0; curr_buf = curr_buf->next)
  {
    if (curr_buf->dev == dev && curr_buf->blockno == blockno)
    {
      // 找到了
      AddBufCnt(curr_buf, curr_bucket);
      return curr_buf;
    }
  }

  // 未找到
  // 获取全局锁
  release(&curr_bucket->lock);
  acquire(&b_lock);
  acquire(&curr_bucket->lock);
  // 进入之后必须再次检查是否已经存在
  // 因为可能有两个请求并发到达，一个请求走下面分配完了，另一个请求又重复分配
  // 从头遍历链表
  for (curr_buf = curr_bucket->list; curr_buf != 0; curr_buf = curr_buf->next)
  {
    if (curr_buf->dev == dev && curr_buf->blockno == blockno)
    {
      // 必须先释放全局锁，再尝试获取睡眠锁，因为这里buf已经被占用，不能保证睡眠锁获取立刻成功
      release(&b_lock);
      AddBufCnt(curr_buf, curr_bucket);
      return curr_buf;
    }
  }

  // 尝试先在自己的桶里面找有没有空闲的
  for (curr_buf = curr_bucket->list; curr_buf != 0; curr_buf = curr_buf->next)
  {
    if (curr_buf->refcnt == 0)
    {
      // 在本桶中找到了
      // 必须先放全局锁，因为睡眠锁可能睡（第一个快速查找不使用全局锁命中）
      takeBuf(curr_buf, curr_bucket, dev, blockno);
      release(&b_lock);
      acquiresleep(&curr_buf->lock);
      curr_buf->timestamp = ticks;
      return curr_buf;
    }
  }

  release(&curr_bucket->lock);
  // 遍历全部桶而不是全部BUF数组寻找空闲块
  while (1)
  {
    struct bbucket *t_bucket;
    struct buf *t_buf;
    struct buf *oldest_buf = 0;
    struct bbucket *oldest_bucket = 0;
    uint oldest_time = ~0u; // MAXINT
    for (int i = 0; i < NBUCKETS; i++)
    {
      t_bucket = &barray[i];
      acquire(&t_bucket->lock);

      for (t_buf = t_bucket->list; t_buf != 0; t_buf = t_buf->next)
      {
        // 跳过占用的
        if (t_buf->refcnt != 0)
        {
          continue;
        }
        // 由于空闲块不会被修改timestamp，占用空闲的必须持有b_lock锁，因此这里是安全的，保证串行
        if (t_buf->timestamp < oldest_time)
        {
          oldest_buf = t_buf;
          oldest_time = t_buf->timestamp;
          oldest_bucket = t_bucket;
        }
      }

      release(&t_bucket->lock);
    }

    // 没找到panic
    if (oldest_buf == 0)
    {
      panic("bget: no buffers");
    }

    // 找到了，加回本桶
    // 由于释放了桶锁，不能保证此时的缓冲区仍然是空闲的
    // 主要竞争不在于其余查询空闲块的线程（全局锁串行了）
    // 而是brelese不清0 dev和blockno，导致最开始尝试在对应桶中找已有的的时候会命中blockno一致，但是refcnt为0的，此时++然后占用
    // 因此这里要再次做检查

    acquire(&oldest_bucket->lock);
    if (oldest_buf->refcnt != 0)
    {
      // 回到循环开头，重新查所有桶
      release(&oldest_bucket->lock);
      continue;
    }

    // 如果和本桶相同直接跳过
    if (oldest_bucket != curr_bucket)
    {
      removeBuf(oldest_buf, oldest_bucket);
      release(&oldest_bucket->lock);

      acquire(&curr_bucket->lock);
      insertList(oldest_buf, curr_bucket);
      takeBuf(oldest_buf, curr_bucket, dev, blockno);
      release(&b_lock);
      acquiresleep(&oldest_buf->lock);
      oldest_buf->timestamp = ticks;

      return oldest_buf;
    }
    else
    {
      // buf相同，前面的acquire(&oldest_bucket->lock);就是获取了curr_buf的桶锁
      takeBuf(oldest_buf, curr_bucket, dev, blockno);
      release(&b_lock);
      acquiresleep(&oldest_buf->lock);
      oldest_buf->timestamp = ticks;

      return oldest_buf;
    }
  }
}

// Return a locked buf with the contents of the indicated block.
struct buf *
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if (!b->valid)
  {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void bwrite(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void brelse(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&b->bucket->lock);
  b->refcnt--;
  release(&b->bucket->lock);
}

void bpin(struct buf *b)
{
  acquire(&b->bucket->lock);
  b->refcnt++;
  release(&b->bucket->lock);
}

void bunpin(struct buf *b)
{
  acquire(&b->bucket->lock);
  b->refcnt--;
  release(&b->bucket->lock);
}
