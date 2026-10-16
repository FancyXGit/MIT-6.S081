#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

int
e1000_transmit(struct mbuf *m)
{
  //
  // Your code here.
  //
  // the mbuf contains an ethernet frame; program it into
  // the TX descriptor ring so that the e1000 sends it. Stash
  // a pointer so that it can be freed after sending.
  //

  acquire(&e1000_lock);

  // 读取TDT寄存器，得知TX环形缓冲区的末尾
  uint32 tdt = regs[E1000_TDT];
  // 检查环是否溢出，即TDT对应的描述符DD为0
  // TX:  [TDH ......... TDT)为硬件拥有的区间
  // TDH为消费者硬件推进，每发完一个包就将DD置为1，然后TDH++
  // TDT为软件推进，每要发一个包就在TDT填入数据，然后TDT++
  // 当TDT为0，说明TDT向前赶到了TDH，硬件占用了全部，环满
  struct tx_desc *d = &tx_ring[tdt];
  if ((d->status & E1000_TXD_STAT_DD) == 0)
  {
    release(&e1000_lock);
    return -1;
  }
  // 没有溢出，先释放原来的缓存
  struct mbuf *prev_mbuf = tx_mbufs[tdt];
  if (prev_mbuf != 0)
  {
    mbuffree(prev_mbuf);
  }
  // 填充描述符
  d->addr = (uint64)m->head;
  d->length = m->len;
  d->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
  d->status = 0;
  // 填充mbuf
  tx_mbufs[tdt] = m;
  // 更新TDT，并向硬件发消息
  regs[E1000_TDT] = (tdt + 1) % TX_RING_SIZE;

  release(&e1000_lock);
  return 0;
}

static void
e1000_recv(void)
{
  //
  // Your code here.
  //
  // Check for packets that have arrived from the e1000
  // Create and deliver an mbuf for each packet (using net_rx()).
  //

  // recv无需加锁，中断是串行的只会有一个CPU执行
  // 如果加锁调用net_rx，net_rx会回reply调用transmit死锁

  // 读RDT下一个
  uint32 next = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
  struct rx_desc *d = &rx_ring[next];
  // DD为0，说明碰到了RDH，缓冲区满了，硬件还没写完
  while (d->status & E1000_RXD_STAT_DD)
  {
    // 为什么要先分配呢，因为如果分配失败了，此时已经交包了，那么下次读到的数据就不对，所以分配失败立刻回退
    // 分配新mbuf
    struct mbuf *new = mbufalloc(0);
    if (new == 0)
    {
      // 当前失败，回退
      regs[E1000_RDT] = (next + RX_RING_SIZE - 1) % RX_RING_SIZE;
      return;
    }

    // 原包交给网络栈
    rx_mbufs[next]->len = d->length;
    net_rx(rx_mbufs[next]);

    // 写入
    rx_mbufs[next] = new;

    // 写回，等待硬件读取
    d->addr = (uint64)rx_mbufs[next]->head;
    d->status = 0;
    // 读下一个
    next = (next + 1) % RX_RING_SIZE;
    d = &rx_ring[next];
  }

  // 更新RDT
  // next指向DD == 1的，所以应该放到前一个
  regs[E1000_RDT] = (next + RX_RING_SIZE - 1) % RX_RING_SIZE;
}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
