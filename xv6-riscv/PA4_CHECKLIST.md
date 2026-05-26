# PA4 Assignment Checklist

이 문서는 처음 제공된 PA4 과제 문서의 요구사항을 기준으로, 현재 구현이 각 항목을 충족하는지 점검한 체크리스트입니다.

상태 표기:

- 완료: 현재 코드에 구현되어 있고 로컬 `pa4test`로도 확인됨
- 부분: 핵심은 구현되었지만 문서와 완전히 같은 형태는 아니거나 추가 확인이 있으면 좋음
- 설명: 직접 구현 요구사항은 아니지만 관련 코드/설정이 존재함
- 해당 없음: 과제 안내/제출 안내처럼 코드 구현과 직접 관련 없는 항목

## Project Objective

| 문서 항목 | 상태 | 구현 위치 / 설명 |
|---|---:|---|
| Implement page-level swapping | 완료 | 전체 swap-in/out 흐름 구현 |
| Swap-in: move the victim page from backing store to main memory | 완료 | `kernel/swap.c`의 `swap_in_page()` |
| Swap-out: move the victim page from main memory to backing store | 완료 | `kernel/swap.c`의 `swap_out()` |
| Manage swappable pages with LRU list | 완료 | `lru_init()`, `lru_add()`, `lru_remove()` |
| Page replacement policy: clock algorithm | 완료 | `swap_out()`에서 `PTE_A` 기반 clock scan |
| Codes you need to create or modify in xv6: Swap-in, swap-out operation | 완료 | `kernel/swap.c`, `kernel/kalloc.c`, `kernel/trap.c` |
| Codes you need to create or modify in xv6: LRU list management | 완료 | `kernel/swap.c`, `kernel/vm.c` |
| Codes you need to create or modify in xv6: Some extras | 완료 | `uvmcopy()`, `uvmunmap()`, `copyin/out/instr()`, `fork()` lock fix 등 |

## What Is Swapping?

| 문서 항목 | 상태 | 구현 위치 / 설명 |
|---|---:|---|
| Swapping moves a page between memory and disk when physical memory is full | 완료 | `kalloc()`이 free page 부족 시 `swap_out()` 호출 |
| Swapping allows processes to continue running when physical memory is insufficient | 완료 | swap-out 후 물리 페이지 재사용, page fault 시 swap-in |
| Swap pages out of memory to backing store | 완료 | `swapwrite()` |
| Swap pages into memory from backing store | 완료 | `swapread()` |
| xv6에는 원래 swapping이 없음 | 설명 | 현재 구현으로 추가됨 |

## Swappable Pages In xv6

| 문서 항목 | 상태 | 구현 위치 / 설명 |
|---|---:|---|
| Only user pages are swappable | 완료 | user page 생성 경로에서만 `lru_add()` 호출 |
| Page table pages should not be swapped out | 완료 | `walk()`가 할당한 page-table page는 LRU에 추가하지 않음 |
| Some physical pages should not be swapped out | 완료 | kernel allocations, buffer pages, kernel stacks 등은 LRU에 없음 |
| Manage swappable pages with circular doubly linked LRU list | 완료 | `page_lru_head`, `next`, `prev` 기반 circular list |
| User virtual memory page mapped to physical memory -> add corresponding physical page to LRU | 완료 | `uvmfirst()`, `uvmalloc()`, `uvmcopy()`, `swap_in_page()` |
| User virtual memory page unmapped from physical memory -> remove corresponding physical page from LRU | 완료 | `uvmunmap()`에서 valid user page 제거, `swap_out()`에서 victim 제거 |

## Clock Algorithm

| 문서 항목 | 상태 | 구현 위치 / 설명 |
|---|---:|---|
| Use `PTE_A` in each PTE | 완료 | `kernel/riscv.h`에 `#define PTE_A (1L << 6)` |
| From `lru_head`, select victim page following next pointer | 완료 | `swap_out()`가 `page_lru_head`부터 scan |
| If `PTE_A == 1`, clear it | 완료 | `*pte &= ~PTE_A` |
| If `PTE_A == 1`, send page to tail of LRU list | 완료 | `swap_out()`에서 해당 page unlink 후 tail insert |
| If `PTE_A == 0`, evict page | 완료 | `victim = pg` 후 swap-out |
| QEMU automatically sets `PTE_A` bit when accessed | 설명 | RISC-V/QEMU 동작에 의존 |
| Define `#define PTE_A (1L << 6)` in `riscv.h` | 완료 | `kernel/riscv.h` |

## Swap-Out Operation

| 문서 항목 | 상태 | 구현 위치 / 설명 |
|---|---:|---|
| Use swap-out when free page is not obtained by `kalloc()` | 완료 | `kernel/kalloc.c`에서 freelist empty 시 `swap_out()` |
| Use `swapwrite()` to write victim page in swap space | 완료 | `swap_out()`에서 `swapwrite(pa, slot)` 호출 |
| Set victim PTE's PPN field to offset in swap space | 완료 | `PTE_MKSWAP(slot, flags)` |
| Clear `PTE_V` | 완료 | `flags &= ~PTE_V` 후 PTE 갱신 |
| Victim physical page becomes reusable | 완료 | `swap_out()`이 victim physical address를 `kalloc()`에 반환 |

## Swap-In Operation

| 문서 항목 | 상태 | 구현 위치 / 설명 |
|---|---:|---|
| Use swap-in when accessing a page that has been swapped out | 완료 | `usertrap()`, `copyin()`, `copyout()`, `copyinstr()`, `uvmcopy()` |
| Get a new physical page | 완료 | `swap_in_page()`에서 `kalloc()` |
| Use `swapread()` to load from swap space into physical page | 완료 | `swap_in_page()`에서 `swapread((uint64)mem, slot)` 호출 |
| Update PPN value to physical address of physical page | 완료 | `*pte = PA2PTE(mem) | flags` |
| Do not need to call `mappages()` because page table already allocated | 완료 | `swap_in_page()`는 기존 PTE를 직접 갱신 |

## Swap Bitmap And Swap Space

| 문서 항목 | 상태 | 구현 위치 / 설명 |
|---|---:|---|
| Use 1 physical page for bitmap to track swap space | 완료 | Q&A 답변에 맞춰 `static uint8 swap_bitmap[PGSIZE]` 사용 |
| Set a bit in bitmap when a page is swapped out | 완료 | `bitmap_alloc()` |
| Clear a bit when a page is swapped in | 완료 | `swap_in_page()`에서 `bitmap_free(slot)` |
| Clear bitmap bit when swapped-out page is deallocated | 완료 | `uvmunmap()`에서 `PTE_S` page 처리 |

## Copying User Virtual Memory

| 문서 항목 | 상태 | 구현 위치 / 설명 |
|---|---:|---|
| When user virtual memory page is copied, valid pages should be copied | 완료 | `uvmcopy()`의 valid PTE copy |
| Swapped-out pages should also be copied | 완료 | `uvmcopy()`에서 `PTE_S` 확인 |
| Swap in swapped-out pages and then copy them | 완료 | `uvmcopy()`에서 `swap_in_page(old, i)` 후 `memmove()` |

## Deallocating User Virtual Memory

| 문서 항목 | 상태 | 구현 위치 / 설명 |
|---|---:|---|
| Valid pages should be freed | 완료 | `uvmunmap()`에서 `kfree()` |
| Set PTE bits to 0 for valid pages | 완료 | `uvmunmap()`에서 `*pte = 0` |
| Remove valid pages from LRU list | 완료 | `uvmunmap()`에서 `PTE_U` page에 `lru_remove(pa)` |
| Swapped-out pages should also be cleared | 완료 | `uvmunmap()`에서 `PTE_S` page 처리 |
| Clear corresponding bitmap bits for swapped-out pages | 완료 | `bitmap_free(slot)` |
| Set PTE bits to 0 for swapped-out pages | 완료 | `uvmunmap()`에서 `*pte = 0` |

## OOM Behavior

| 문서 항목 | 상태 | 구현 위치 / 설명 |
|---|---:|---|
| When swap-out is needed and there is no page in LRU list, OOM should occur | 완료 | `swap_out()`이 LRU empty면 `0` 반환 |
| Just printf error message inside `kalloc()` | 완료 | `printf("kalloc: out of memory\n")` |
| `kalloc()` returns 0 when OOM occurs | 완료 | `kalloc()` failure path |

## Fork Lock Consideration

| 문서 항목 | 상태 | 구현 위치 / 설명 |
|---|---:|---|
| Lock should be considered when you do fork | 완료 | `proc.c`에서 child lock release/reacquire failure path 정리 |
| Fork copies swapped pages correctly | 완료 | `uvmcopy()`에서 swapped parent page를 swap-in 후 child에 copy |

## Page Structure Requirement

| 문서 항목 | 상태 | 구현 위치 / 설명 |
|---|---:|---|
| All pages are managed using page structure | 완료 | `pages[PHYSTOP/PGSIZE]`, `pa2page()`, `page2pa()` |
| `struct page *next` for LRU | 완료 | `riscv.h`, `swap.c` |
| `struct page *prev` for LRU | 완료 | `riscv.h`, `swap.c` |
| `pagetable` field | 완료 | `lru_add()` stores owner page table |
| `vaddr` field | 완료 | `lru_add()` stores user virtual address |

## Skeleton Provided Functions

| 문서 항목 | 상태 | 구현 위치 / 설명 |
|---|---:|---|
| `void swapread(uint64 ptr, int blkno)` provided | 완료 | `kernel/fs.c` |
| `void swapwrite(uint64 ptr, int blkno)` provided | 완료 | `kernel/fs.c` |
| `void swapstat(int* nr_sectors_read, int* nr_sectors_write)` provided | 완료 | syscall path in `kernel/sysfile.c` |
| `swapread/swapwrite` measure sectors through counters | 완료 | `nr_sectors_read`, `nr_sectors_write` |
| Kernel swap path can read/write swap space | 완료 | `swapread()`, `swapwrite()` support both user pointers and kernel physical/direct-mapped addresses |

## File System / Disk Layout

| 문서 항목 | 상태 | 구현 위치 / 설명 |
|---|---:|---|
| File system size expanded to use swap space | 완료 | `kernel/param.h`: `FSSIZE 30000` |
| Disk layout in `mkfs/mkfs.c` | 설명 | Skeleton-provided area assumed; current implementation uses `SWAPBASE` |
| `FSSize` in `kernel/param.h` | 완료 | `FSSIZE` present |
| `Swap Space` in `kernel/param.h` | 완료 | `SWAPBASE`, `SWAPMAX` |

## Testing Guidance

| 문서 항목 | 상태 | 구현 위치 / 설명 |
|---|---:|---|
| `main()` calls `kinit()` at startup | 완료 | `kernel/main.c` |
| `kinit()` makes memory available | 완료 | original xv6 allocator |
| Reduce `PHYSTOP` to reduce free pages for testing | 설명 | Current `PHYSTOP` remains `128MB`; local `pa4test` still causes swap pressure successfully |
| Use many processes / `ls` / `sbrk()` to consume memory | 완료 | `pa4test` uses `sbrk()` pressure |
| Monitor swap using `swapstat()` | 완료 | `pa4test` checks read/write counters |
| Monitor LRU length and swap operations | 부분 | `num_lru_pages` exists, but no permanent debug print is enabled |

## Local Test Result

| Test | 상태 | 설명 |
|---|---:|---|
| provided swap syscalls | 완료 | `PASS` |
| kalloc swap-out and fault swap-in | 완료 | `PASS` |
| fork copies swapped-out pages | 완료 | `PASS` |
| kernel copy paths swap in pages | 완료 | `PASS` |
| dealloc clears swapped PTEs | 완료 | `PASS` |
| ALL PA4 TESTS PASSED | 완료 | 확인됨 |

## Submission-Related Items

| 문서 항목 | 상태 | 설명 |
|---|---:|---|
| Begin with skeleton code | 설명 | 현재 repo는 PA4 skeleton 기반으로 보임 |
| Submit with `~swe3004/bin/submit pa4 xv6-riscv` | 해당 없음 | 제출 환경에서 실행 |
| Can submit several times, last submission graded | 해당 없음 | 제출 정책 |
| Do not copy | 해당 없음 | 정책 준수 필요 |
| Due date | 해당 없음 | 제출 일정 |
| Ask questions on i-campus | 해당 없음 | 운영 안내 |

## Extra Submission Checks

| 체크 항목 | 상태 | 설명 |
|---|---:|---|
| `kernel/swap.c` is included in build | 완료 | `Makefile`에 `$K/swap.o` 추가 |
| `kernel/swap.c` is a new file and must be submitted | 확인 필요 | 현재 git 기준 untracked였으므로 제출 방식이 git 기반이면 반드시 포함 필요 |
| Build artifacts should not be submitted if avoidable | 확인 필요 | 제출 전 `make clean` 추천 |
| `PHYSTOP` was not permanently reduced | 완료 | `kernel/memlayout.h` remains `128MB` |
| Extra local test `pa4test` does not affect kernel behavior | 완료 | user program only |

## Summary

현재 구현은 과제의 핵심 요구사항을 모두 만족합니다.

현재 구현은 문서 요구처럼 page replacement 경로에서도 `swapread()` / `swapwrite()`를 직접 호출합니다. 이 함수들은 user syscall용 pointer와 kernel의 physical/direct-mapped page address를 모두 처리하도록 확장되어 있습니다.

제출 전 가장 중요한 확인 사항은 다음입니다.

1. `make clean`
2. `make qemu`
3. xv6 shell에서 `pa4test`
4. `kernel/swap.c`가 제출에 포함되는지 확인
