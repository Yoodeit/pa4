#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"

extern struct page pages[];
extern struct page *page_lru_head;
extern int num_lru_pages;

struct spinlock lru_lock;

#define BLKS_PER_PAGE (PGSIZE / BSIZE)
#define NSWAPSLOTS (SWAPMAX / BLKS_PER_PAGE)
#define SWAP_BITMAP_BYTES ((NSWAPSLOTS + 7) / 8)

static uint8 *swap_bitmap;
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
  if(SWAP_BITMAP_BYTES > PGSIZE)
    panic("swap bitmap too large");
  swap_bitmap = kalloc();
  if(swap_bitmap == 0)
    panic("swap bitmap");
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

int
lru_remove(uint64 pa)
{
  struct page *pg = pa2page(pa);

  acquire(&lru_lock);
  if (pg->next == 0 && pg->prev == 0) {

    release(&lru_lock);
    return 0;
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

  return 1;
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

extern struct proc proc[];

static int
is_swap_safe(pagetable_t pt)
{
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++) {
    if (p->pagetable == pt && p->state != UNUSED && p->state != ZOMBIE)
      return 1;
  }
  return 0;
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
    if (!is_swap_safe(pg->pagetable)) {
      pg = pg->next;
      continue;
    }
    pte = walk(pg->pagetable, (uint64)pg->vaddr, 0);
    if (pte == 0) {
      pg = pg->next;
      continue;
    }

    if (*pte & PTE_A) {

      struct page *next = pg->next;
      *pte &= ~PTE_A;
      if (pg->next != pg) {
        pg->prev->next = pg->next;
        pg->next->prev = pg->prev;
        if (page_lru_head == pg)
          page_lru_head = pg->next;

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

  if (victim == 0) {
    struct page *pg2 = page_lru_head;
    int n = num_lru_pages;
    while (n-- > 0) {
      if (is_swap_safe(pg2->pagetable)) {
        victim = pg2;
        break;
      }
      pg2 = pg2->next;
    }
  }

  if (victim == 0) {
    release(&lru_lock);
    return 0;
  }

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

  struct proc *owner = 0;
  struct proc *pp;
  for (pp = proc; pp < &proc[NPROC]; pp++) {
    if (pp->pagetable == pt && pp->state != UNUSED) {
      owner = pp;
      break;
    }
  }

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

  int pt_valid = (owner != 0 && owner->pagetable == pt && owner->state != UNUSED);

  if (pt_valid) {
    pte = walk(pt, va, 0);
    if (pte && (*pte & PTE_V) && PTE2PA(*pte) == pa) {
      uint flags = PTE_FLAGS(*pte);
      flags &= ~PTE_V;
      flags |= PTE_S;
      *pte = PTE_MKSWAP(slot, flags);
    } else {

      bitmap_free(slot);
      sfence_vma();
      victim->pagetable = 0;
      victim->vaddr = 0;
      kfree((void*)pa);
      return 0;
    }
  } else {
    bitmap_free(slot);
    sfence_vma();
    victim->pagetable = 0;
    victim->vaddr = 0;
    kfree((void*)pa);
    return 0;
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

