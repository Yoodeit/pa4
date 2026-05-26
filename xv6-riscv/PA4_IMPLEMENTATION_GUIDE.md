# PA4 Page Replacement Implementation Guide

This document explains how this xv6 PA4 implementation adds page-level
swapping, LRU tracking, and clock page replacement on top of the skeleton code.

## Big Picture

The goal of PA4 is to let xv6 continue running user processes even when there
are no free physical pages. When `kalloc()` cannot find a free page, the kernel
evicts one swappable user page to disk, reuses that physical page, and later
brings the evicted page back when the process accesses it.

The implementation has four main pieces:

1. Track swappable user pages in a circular LRU list.
2. Choose a victim page with the clock algorithm.
3. Swap victim pages out to the disk swap area.
4. Swap pages back in on page faults or kernel copy paths.

Only user pages are added to the LRU list. Kernel pages, page-table pages,
kernel stacks, buffers, and other internal kernel allocations are not
swappable.

## Important PTE Bits

File: `kernel/riscv.h`

Two page-table bits/macros were added:

```c
#define PTE_A (1L << 6) // accessed (hardware sets this)
#define PTE_S (1L << 8) // swapped out (software RSW bit)

#define PTE_SWAPSLOT(pte)  ((pte) >> 10)
#define PTE_MKSWAP(slot, flags) (((uint64)(slot) << 10) | (flags))
```

`PTE_A` is the hardware accessed bit. QEMU/RISC-V sets it when a mapped page is
accessed. The clock algorithm uses this bit to decide whether a page gets a
second chance.

`PTE_S` is a software-only marker meaning: this virtual page is not currently
in physical memory, but its data exists in a swap slot.

When a page is swapped out, the PTE no longer stores a physical page number.
Instead, the PPN field stores the swap slot number:

```text
valid page:
  PTE_V = 1, PPN = physical page number

swapped page:
  PTE_V = 0, PTE_S = 1, PPN field = swap slot number
```

## Page Metadata

File: `kernel/riscv.h`

The skeleton provides:

```c
struct page {
  struct page *next;
  struct page *prev;
  pagetable_t pagetable;
  char *vaddr;
};
```

File: `kernel/kalloc.c`

The global page metadata array is:

```c
struct page pages[PHYSTOP/PGSIZE];
struct page *page_lru_head;
int num_lru_pages;
```

Each physical page has one `struct page`. For swappable pages, the metadata
records:

- `pagetable`: which process page table maps this physical page
- `vaddr`: which user virtual address maps this physical page
- `next` / `prev`: links in the circular doubly linked LRU list

This lets the swap code find and update the exact PTE when evicting a physical
page.

## Initialization

File: `kernel/main.c`

The boot sequence now calls:

```c
kinit();
lru_init();
swap_init();
```

`swap_init()` clears the static swap bitmap before paging and user processes
begin.

## Swap Bitmap

File: `kernel/swap.c`

The assignment asks for one physical page worth of bitmap storage to track
swap-space allocation. The course Q&A says a static global array is acceptable,
so this implementation uses:

```c
static uint8 swap_bitmap[PGSIZE];
```

`swap_init()` clears this page-sized bitmap with `memset(swap_bitmap, 0,
PGSIZE)`.

Each bit represents one swap slot. One swap slot stores one page, which is
`PGSIZE / BSIZE` disk sectors.

Important constants:

```c
#define BLKS_PER_PAGE (PGSIZE / BSIZE)
#define NSWAPSLOTS (SWAPMAX / BLKS_PER_PAGE)
```

`bitmap_alloc()` finds a free swap slot and marks it used.

`bitmap_free(slot)` clears the bit when a swapped page is brought back into
memory or discarded during deallocation.

## LRU List

File: `kernel/swap.c`

The LRU list is a circular doubly linked list with no sentinel node.

```c
struct spinlock lru_lock;
```

`page_lru_head` points to the current clock hand / oldest candidate.

### Adding Pages

```c
void lru_add(uint64 pa, pagetable_t pt, uint64 va)
```

This function:

1. Converts the physical address to its `struct page`.
2. Records the owning `pagetable` and user `va`.
3. Inserts the page at the tail of the circular list.

Pages are added when user memory is created or restored:

- `uvmfirst()` for the first init user page
- `uvmalloc()` for newly allocated user heap/stack/text pages
- `uvmcopy()` for copied child pages during `fork()`
- `swap_in_page()` after a swapped page is restored

### Removing Pages

```c
void lru_remove(uint64 pa)
```

This unlinks a page from the circular list and clears its metadata.

Pages are removed when:

- a valid user page is unmapped/freed in `uvmunmap()`
- a page is selected as a swap-out victim

## Swap I/O

Files:

- `kernel/fs.c`
- `kernel/swap.c`

The skeleton provides global `swapread()` and `swapwrite()` in `kernel/fs.c`.
This implementation uses those same functions for both:

- user-visible swap syscalls
- kernel page replacement swap-in/swap-out

The functions were extended to detect kernel physical/direct-mapped page
addresses:

```c
int kernel_dst = (ptr >= KERNBASE && ptr < PHYSTOP);
int kernel_src = (ptr >= KERNBASE && ptr < PHYSTOP);
```

If the pointer is a kernel physical/direct-mapped page address, the functions
copy with `either_copyin(..., 0, ...)` or `either_copyout(0, ...)`.

If the pointer is a user address from the syscall interface, they copy with
`either_copyin(..., 1, ...)` or `either_copyout(1, ...)`.

Both update the same swap statistics counters:

```c
nr_sectors_read
nr_sectors_write
```

## Swap-Out Flow

File: `kernel/swap.c`

Main function:

```c
uint64 swap_out(void)
```

This is called by `kalloc()` when the freelist is empty.

High-level flow:

1. Lock the LRU list.
2. Start scanning from `page_lru_head`.
3. For each candidate page:
   - Find its PTE using `walk(pg->pagetable, pg->vaddr, 0)`.
   - If `PTE_A` is set:
     - clear `PTE_A`
     - move the page to the tail
     - continue scanning
   - If `PTE_A` is clear:
     - select this page as the victim
4. Remove the victim from the LRU list.
5. Allocate a swap slot from the bitmap.
6. Write the physical page to disk with `swapwrite()`.
7. Update the victim PTE:
   - clear `PTE_V`
   - set `PTE_S`
   - store the swap slot in the PPN field
8. Flush the TLB with `sfence_vma()`.
9. Return the freed physical page address to `kalloc()`.

The returned physical page is immediately reused by `kalloc()`.

## kalloc Integration

File: `kernel/kalloc.c`

Original xv6 `kalloc()` only returns a page from `kmem.freelist`. This PA4
implementation adds:

```c
if(r == 0){
  push_off();
  int held = mycpu()->noff;
  pop_off();

  if(held == 1){
    uint64 pa = swap_out();
    if(pa != 0)
      r = (struct run*)pa;
  }
  if(r == 0){
    printf("kalloc: out of memory\n");
    return 0;
  }
}
```

The `held == 1` check avoids trying to swap out while the kernel is already
inside a nested critical section. Swapping uses disk and buffer-cache paths, so
doing it under arbitrary locks would be dangerous.

If no LRU victim exists, or swap space is full, `kalloc()` prints an OOM message
and returns `0`, as required by the assignment.

## Swap-In Flow

File: `kernel/swap.c`

Main function:

```c
uint64 swap_in_page(pagetable_t pagetable, uint64 va)
```

High-level flow:

1. Round `va` down to a page boundary.
2. Find the PTE.
3. Confirm the page is swapped:
   - `PTE_V` is clear
   - `PTE_S` is set
4. Extract the swap slot from the PTE.
5. Allocate a new physical page with `kalloc()`.
6. Read page contents from disk with `swapread()`.
7. Free the swap slot in the bitmap.
8. Update the PTE:
   - PPN = new physical page
   - set `PTE_V`
   - clear `PTE_S`
9. Flush the TLB.
10. Add the restored page back to the LRU list.

## Page Fault Handling

File: `kernel/trap.c`

`usertrap()` now handles page faults:

```c
r_scause() == 12 // instruction page fault
r_scause() == 13 // load page fault
r_scause() == 15 // store/AMO page fault
```

On a page fault:

1. Read the faulting address from `r_stval()`.
2. Reject invalid addresses outside the process size.
3. Look up the PTE.
4. If `PTE_S` is set, call `swap_in_page()`.
5. Otherwise kill the process as a real invalid page fault.

This is the normal user-mode swap-in path.

## VM Changes

File: `kernel/vm.c`

### `uvmfirst()`

The first user page for `init` is now added to the LRU list.

Without this, the first user process page would be mapped but not swappable.

### `uvmalloc()`

Every newly allocated user page is added to the LRU list after mapping.

This covers heap growth through `sbrk()` and pages loaded during `exec()`.

### `uvmunmap()`

The function now handles both valid and swapped pages:

Valid page:

1. If it is a user page, remove it from the LRU list.
2. Free the physical page with `kfree()`.
3. Clear the PTE.

Swapped page:

1. Extract the swap slot from the PTE.
2. Free the bitmap slot.
3. Clear the PTE.

This matters when memory is deallocated before the process ever touches the
swapped-out page again.

### `freewalk()`

`freewalk()` now tolerates `PTE_S` entries after `uvmunmap()` has already
handled freeing the swap slot.

### `uvmcopy()`

Fork must copy valid pages and swapped-out pages.

For swapped-out parent pages:

1. Call `swap_in_page(old, i)`.
2. Reload the PTE.
3. Copy the restored physical page into the child.

The child copy is then mapped and added to the LRU list.

### `copyin()`, `copyout()`, `copyinstr()`

Kernel code often copies data to/from user buffers during syscalls. If the user
buffer page has been swapped out, the kernel copy path cannot simply fail.

These functions now check for `PTE_S` and call `swap_in_page()` before copying.

This is needed for cases like:

- `write(fd, swapped_user_buffer, len)`
- `read(fd, swapped_user_buffer, len)`
- `open(swapped_path_string, flags)`

## Fork Locking Fix

File: `kernel/proc.c`

The implementation releases the new child process lock before copying user
memory:

```c
release(&np->lock);
```

If `uvmcopy()` fails, it reacquires the lock before calling `freeproc(np)`:

```c
acquire(&np->lock);
freeproc(np);
release(&np->lock);
```

This avoids releasing a lock that is not held on the failure path.

## Build Integration

File: `Makefile`

`kernel/swap.o` was added to the kernel object list:

```make
$K/swap.o
```

The local test program was also added:

```make
$U/_pa4test
```

`pa4test` is for local verification. The actual page replacement logic does not
depend on the test program.

## User-Level Swap Syscalls

Files:

- `kernel/fs.c`
- `kernel/sysfile.c`
- `kernel/syscall.c`
- `kernel/syscall.h`
- `user/user.h`
- `user/usys.pl`
- `user/usys.S`

The provided user-visible swap helpers are:

```c
void swapread(uint64 ptr, int blkno);
void swapwrite(uint64 ptr, int blkno);
void swapstat(int *nr_sectors_read, int *nr_sectors_write);
```

These are useful for testing the swap space and reading the I/O counters.

The same functions are also used by the page replacement implementation. They
handle user pointers for syscalls and kernel physical/direct-mapped page
addresses for `swap_out()` and `swap_in_page()`.

## End-to-End Example

Suppose a process calls `sbrk()` many times and touches every page.

1. `sbrk()` calls `growproc()`.
2. `growproc()` calls `uvmalloc()`.
3. `uvmalloc()` calls `kalloc()` for each new page.
4. Once the freelist is empty, `kalloc()` calls `swap_out()`.
5. `swap_out()` chooses a user page from the LRU list using clock.
6. The victim page is written to disk.
7. The victim PTE becomes invalid but marked with `PTE_S`.
8. `kalloc()` reuses the victim physical page for the new allocation.
9. Later, the old process accesses the swapped page.
10. The invalid PTE causes a page fault.
11. `usertrap()` sees `PTE_S`.
12. `swap_in_page()` allocates memory, reads the page back, updates the PTE, and
    puts the page back on the LRU list.
13. The user instruction is retried and succeeds.

## Current Test Result

The local `pa4test` result:

```text
PASS: provided swap syscalls
PASS: kalloc swap-out and fault swap-in
PASS: fork copies swapped-out pages
PASS: kernel copy paths swap in pages
PASS: dealloc clears swapped PTEs
ALL PA4 TESTS PASSED
```

## Submission Notes

Before submitting, it is a good idea to run:

```sh
make clean
make qemu
pa4test
```

Then submit the `xv6-riscv` directory.

Make sure `kernel/swap.c` is included. It is a newly created source file and is
required by the Makefile.
