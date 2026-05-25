// pa4: Swap space management
// LRU list, swap bitmap, clock algorithm, kernel-side swap I/O

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

// ============================================================
// Extern declarations from kalloc.c (skeleton-provided)
// ============================================================
extern struct page pages[];
extern struct page *page_lru_head;
extern int num_lru_pages;

// Extern declarations from fs.c (skeleton-provided swap stat counters)
extern int nr_sectors_read;
extern int nr_sectors_write;

// ============================================================
// Locks
// ============================================================
struct spinlock lru_lock;

// ============================================================
// Swap bitmap: track which swap slots are in use
// ============================================================
#define BLKS_PER_PAGE (PGSIZE / BSIZE)
#define NSWAPSLOTS (SWAPMAX / BLKS_PER_PAGE)

static uint8 swap_bitmap[NSWAPSLOTS / 8 + 1];
static struct spinlock swap_bitmap_lock;

// ============================================================
// Macros: physical address <-> page struct
// ============================================================
#define pa2page(pa) (&pages[(uint64)(pa) / PGSIZE])
#define page2pa(pg) ((uint64)((pg) - pages) * PGSIZE)

// ============================================================
// Initialization
// ============================================================
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
  memset(swap_bitmap, 0, sizeof(swap_bitmap));
}

// ============================================================
// LRU list: circular doubly linked list (no sentinel)
// page_lru_head points to the head (oldest / clock hand start)
// ============================================================

// Add a page to the tail of LRU list (most recently used)
void
lru_add(uint64 pa, pagetable_t pt, uint64 va)
{
  struct page *pg = pa2page(pa);
  pg->pagetable = pt;
  pg->vaddr = (char *)va;

  acquire(&lru_lock);
  if (page_lru_head == 0) {
    // List is empty
    pg->next = pg;
    pg->prev = pg;
    page_lru_head = pg;
  } else {
    // Insert before head (= at tail of circular list)
    struct page *tail = page_lru_head->prev;
    pg->prev = tail;
    pg->next = page_lru_head;
    tail->next = pg;
    page_lru_head->prev = pg;
  }
  num_lru_pages++;
  release(&lru_lock);
}

// Remove a page from LRU list
int
lru_remove(uint64 pa)
{
  struct page *pg = pa2page(pa);

  acquire(&lru_lock);
  if (pg->next == 0 && pg->prev == 0) {
    // Not in the list
    release(&lru_lock);
    return 0;
  }
  if (pg->next == pg) {
    // Only element
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

// ============================================================
// Swap bitmap
// ============================================================

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

// ============================================================
// Kernel-side swap I/O (uses physical addresses directly)
// The skeleton's swapread/swapwrite in fs.c use user VAs,
// which don't work for kernel swap operations.
// ============================================================

static void
kswapwrite(uint64 pa, int slot)
{
  for (int i = 0; i < BLKS_PER_PAGE; i++) {
    struct buf *bp = bread(0, SWAPBASE + BLKS_PER_PAGE * slot + i);
    memmove(bp->data, (char *)pa + i * BSIZE, BSIZE);
    bwrite(bp);
    brelse(bp);
    nr_sectors_write++;
  }
}

static void
kswapread(uint64 pa, int slot)
{
  for (int i = 0; i < BLKS_PER_PAGE; i++) {
    struct buf *bp = bread(0, SWAPBASE + BLKS_PER_PAGE * slot + i);
    memmove((char *)pa + i * BSIZE, bp->data, BSIZE);
    brelse(bp);
    nr_sectors_read++;
  }
}


// Check if a pagetable is safe to swap from.
// Returns 1 if safe, 0 if not safe.
// Not safe if: (a) belongs to a RUNNING process, or
// (b) doesn't belong to ANY live process (stale/freed by exec).
extern struct proc proc[];

static int
is_swap_safe(pagetable_t pt)
{
  struct proc *p;
  int found = 0;
  for (p = proc; p < &proc[NPROC]; p++) {
    if (p->pagetable == pt && p->state != UNUSED) {
      if (p->state == RUNNING)
        return 0;  // Running - TLB can't be flushed
      found = 1;
    }
  }
  if (!found)
    return 0;  // No process owns this PT - stale (exec replaced it)
  return 1;
}

// ============================================================
// Clock algorithm: swap_out
// Evict one page using clock algorithm.
// Returns physical address of freed page, or 0 on failure.
// ============================================================
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

  // Clock: scan from head, max 2 full passes
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
      // Recently accessed: clear A bit, move on
      *pte &= ~PTE_A;
      pg = pg->next;
    } else {
      // Found victim
      victim = pg;
      break;
    }
  }

  // Fallback: take the current head
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

  // Advance head past victim (for next clock scan)
  if (victim->next == victim) {
    // Only element
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

  // Allocate swap slot
  slot = bitmap_alloc();
  if (slot < 0) {
    // No swap space: put victim back in LRU
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

  // Write page to swap space
  kswapwrite(pa, slot);

  int pt_valid = (owner != 0 && owner->pagetable == pt && owner->state != UNUSED);

  if (pt_valid) {
    pte = walk(pt, va, 0);
    if (pte && (*pte & PTE_V) && PTE2PA(*pte) == pa) {
      uint flags = PTE_FLAGS(*pte);
      flags &= ~PTE_V;
      flags |= PTE_S;
      *pte = PTE_MKSWAP(slot, flags);
    } else {
      // PTE already cleared by uvmunmap (race) — clean up
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

// ============================================================
// swap_in_page: bring a swapped page back into memory
// Returns physical address of new page, or 0 on failure.
// ============================================================
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

  // Allocate new physical page (may trigger another swap_out)
  char *mem = kalloc();
  if (mem == 0)
    return 0;

  // Read from swap into new page
  kswapread((uint64)mem, slot);

  // Free swap slot
  bitmap_free(slot);

  // Update PTE: set physical address, clear PTE_S, set PTE_V
  flags &= ~PTE_S;
  flags |= PTE_V;
  *pte = PA2PTE(mem) | flags;

  sfence_vma();

  // Add to LRU
  lru_add((uint64)mem, pagetable, va);

  return (uint64)mem;
}