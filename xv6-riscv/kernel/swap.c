#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "buf.h"

extern struct page pages[];
extern struct page *page_lru_head;
extern int num_lru_pages;

extern int nr_sectors_read;
extern int nr_sectors_write;

struct spinlock lru_lock;

#define BLKS_PER_PAGE (PGSIZE / BSIZE)
#define NSWAPSLOTS (SWAPMAX / BLKS_PER_PAGE)

static uint8 swap_bitmap[PGSIZE];
static struct spinlock swap_bitmap_lock;

#define pa2page(pa) (&pages[(uint64)(pa) / PGSIZE])
#define page2pa(pg) ((uint64)((pg) - pages) * PGSIZE)

void
lru_init(void)
{
  initlock(&lru_lock, "lru");
  page_lru_head = 0;
  num_lru_pages = 0;
}

void
swap_init(void)
{
  initlock(&swap_bitmap_lock, "swap_bitmap");
  memset(swap_bitmap, 0, PGSIZE);
}

void
lru_add(uint64 pa, pagetable_t pt, uint64 va)
{
  struct page *pg = pa2page(pa);
  pg->pagetable = pt;
  pg->vaddr = (char *)va;

  acquire(&lru_lock);
  if (page_lru_head == 0) {
    pg->next = pg;
    pg->prev = pg;
    page_lru_head = pg;
  } else {
    struct page *tail = page_lru_head->prev;
    pg->prev = tail;
    pg->next = page_lru_head;
    tail->next = pg;
    page_lru_head->prev = pg;
  }
  num_lru_pages++;
  release(&lru_lock);
}

void
lru_remove(uint64 pa)
{
  struct page *pg = pa2page(pa);

  acquire(&lru_lock);
  if (pg->next == 0 && pg->prev == 0) {
    release(&lru_lock);
    return;
  }
  if (pg->next == pg) {
    page_lru_head = 0;
  } else {
    pg->prev->next = pg->next;
    pg->next->prev = pg->prev;
    if (page_lru_head == pg)
      page_lru_head = pg->next;
  }
  pg->next = 0;
  pg->prev = 0;
  num_lru_pages--;
  release(&lru_lock);

  pg->pagetable = 0;
  pg->vaddr = 0;
}

static int
bitmap_alloc(void)
{
  acquire(&swap_bitmap_lock);
  for (int i = 0; i < NSWAPSLOTS; i++) {
    int byte = i / 8;
    int bit = i % 8;
    if ((swap_bitmap[byte] & (1 << bit)) == 0) {
      swap_bitmap[byte] |= (1 << bit);
      release(&swap_bitmap_lock);
      return i;
    }
  }
  release(&swap_bitmap_lock);
  return -1;
}

void
bitmap_free(int slot)
{
  if (slot < 0 || slot >= NSWAPSLOTS)
    return;
  acquire(&swap_bitmap_lock);
  int byte = slot / 8;
  int bit = slot % 8;
  swap_bitmap[byte] &= ~(1 << bit);
  release(&swap_bitmap_lock);
}

uint64
swap_out(void)
{
  struct page *victim = 0;
  pte_t *pte;
  uint64 pa;
  int slot;

  acquire(&lru_lock);

  if (page_lru_head == 0) {
    release(&lru_lock);
    return 0;
  }

  int max_iter = num_lru_pages * 2 + 1;
  struct page *pg = page_lru_head;

  while (max_iter-- > 0) {
    pte = walk(pg->pagetable, (uint64)pg->vaddr, 0);
    if (pte == 0) {
      pg = pg->next;
      continue;
    }

    if (*pte & PTE_A) {
      struct page *next = pg->next;

      *pte &= ~PTE_A;
      if (num_lru_pages > 1) {
        if (page_lru_head == pg)
          page_lru_head = pg->next;

        pg->prev->next = pg->next;
        pg->next->prev = pg->prev;

        struct page *tail = page_lru_head->prev;
        pg->prev = tail;
        pg->next = page_lru_head;
        tail->next = pg;
        page_lru_head->prev = pg;
      }

      pg = next;
    } else {
      victim = pg;
      break;
    }
  }

  if(victim == 0)
    victim = page_lru_head;

  if (victim->next == victim) {
    page_lru_head = 0;
  } else {
    victim->prev->next = victim->next;
    victim->next->prev = victim->prev;
    page_lru_head = victim->next;
  }
  victim->next = 0;
  victim->prev = 0;
  num_lru_pages--;

  pa = page2pa(victim);
  pagetable_t pt = victim->pagetable;
  uint64 va = (uint64)victim->vaddr;

  release(&lru_lock);

  slot = bitmap_alloc();
  if (slot < 0) {
    acquire(&lru_lock);
    if (page_lru_head == 0) {
      victim->next = victim;
      victim->prev = victim;
      page_lru_head = victim;
    } else {
      struct page *tail = page_lru_head->prev;
      victim->prev = tail;
      victim->next = page_lru_head;
      tail->next = victim;
      page_lru_head->prev = victim;
    }
    victim->pagetable = pt;
    victim->vaddr = (char *)va;
    num_lru_pages++;
    release(&lru_lock);
    return 0;
  }

  swapwrite(pa, slot);

  pte = walk(pt, va, 0);
  if (pte) {
    uint flags = PTE_FLAGS(*pte);
    flags &= ~PTE_V;
    flags |= PTE_S;
    *pte = PTE_MKSWAP(slot, flags);
  }

  sfence_vma();

  victim->pagetable = 0;
  victim->vaddr = 0;

  return pa;
}

uint64
swap_in_page(pagetable_t pagetable, uint64 va)
{
  va = PGROUNDDOWN(va);
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0)
    return 0;

  if ((*pte & PTE_V) || !(*pte & PTE_S))
    return 0;

  int slot = PTE_SWAPSLOT(*pte);
  uint flags = PTE_FLAGS(*pte);

  char *mem = kalloc();
  if (mem == 0)
    return 0;

  swapread((uint64)mem, slot);

  bitmap_free(slot);

  flags &= ~PTE_S;
  flags |= PTE_V;
  *pte = PA2PTE(mem) | flags;

  sfence_vma();

  lru_add((uint64)mem, pagetable, va);

  return (uint64)mem;
}
