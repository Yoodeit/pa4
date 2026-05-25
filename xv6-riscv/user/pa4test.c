//
// pa4test.c — Project 4: Page Replacement 종합 테스트
//
// 과제문서 요구사항:
//   1. swap-out: kalloc 실패 시 victim을 swap space로 내보냄
//   2. swap-in: swapped page 접근 시 page fault → 다시 읽어옴
//   3. LRU list + clock algorithm (PTE_A 기반)
//   4. swapstat()으로 swap I/O 통계 확인
//   5. fork 시 swapped-out page도 복사
//   6. dealloc 시 swapped-out page의 bitmap도 정리
//

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096
#define FAIL(msg) do { printf("FAIL: %s\n", msg); exit(1); } while(0)
#define PASS(msg) printf("PASS: %s\n", msg)

// 메모리를 거의 다 소비해서 free page를 적게 만드는 헬퍼.
// 남길 페이지 수(leave)만큼 남기고 나머지를 할당.
// 할당한 총 바이트 수를 반환.
static uint64
eat_memory(int leave)
{
  uint64 total = 0;
  // 먼저 큰 단위로 할당
  while (1) {
    char *p = sbrk(100 * PGSIZE);
    if (p == (char*)-1)
      break;
    // 터치해서 실제 물리 페이지 할당
    for (int i = 0; i < 100; i++)
      *(p + i * PGSIZE) = 1;
    total += 100 * PGSIZE;
  }
  // 한 페이지씩 할당
  while (1) {
    char *p = sbrk(PGSIZE);
    if (p == (char*)-1)
      break;
    *p = 1;
    total += PGSIZE;
  }
  // leave 페이지만큼 돌려놓기
  if (leave > 0)
    sbrk(-(leave * PGSIZE));
  return total - (leave > 0 ? leave * PGSIZE : 0);
}

// ============================================================
// Test 1: 기본 swap-out / swap-in 동작
//   메모리를 거의 소진 → 추가 할당 시 swap-out 유발
//   이전에 쓴 데이터를 다시 읽어서 swap-in 확인
// ============================================================
void
test_swap_basic(void)
{
  printf("\n--- Test 1: Basic swap-out / swap-in ---\n");

  int pid = fork();
  if (pid < 0)
    FAIL("fork failed");

  if (pid == 0) {
    int rd0, wr0, rd1, wr1;
    swapstat(&rd0, &wr0);

    // 메모리 거의 다 소비 (200 페이지 남김)
    uint64 eaten = eat_memory(200);
    printf("  consumed %d pages\n", (int)(eaten / PGSIZE));

    // 추가 할당 → 이제 swap-out이 발생해야 함
    int npages = 300;
    char *base = sbrk(npages * PGSIZE);
    if (base == (char*)-1) {
      printf("  (could not allocate extra pages)\n");
      exit(1);
    }

    // 각 페이지에 고유 패턴 쓰기
    for (int i = 0; i < npages; i++) {
      char *p = base + i * PGSIZE;
      p[0] = (char)(i & 0xFF);
      p[1] = (char)((i >> 8) & 0xFF);
    }

    swapstat(&rd1, &wr1);
    printf("  swap writes: %d (delta: %d)\n", wr1, wr1 - wr0);

    if (wr1 <= wr0) {
      printf("FAIL: no swap-out occurred\n");
      exit(1);
    }

    // 처음부터 다시 읽기 → swap-in 유발
    int errors = 0;
    for (int i = 0; i < npages; i++) {
      char *p = base + i * PGSIZE;
      if (p[0] != (char)(i & 0xFF) || p[1] != (char)((i >> 8) & 0xFF))
        errors++;
    }

    int rd2, wr2;
    swapstat(&rd2, &wr2);
    printf("  swap reads after re-access: %d (delta: %d)\n", rd2, rd2 - rd1);

    if (errors > 0) {
      printf("FAIL: %d pages corrupted\n", errors);
      exit(1);
    }
    exit(0);
  } else {
    int status;
    wait(&status);
    if (status != 0)
      FAIL("Basic swap-out / swap-in");
    PASS("Basic swap-out / swap-in");
  }
}

// ============================================================
// Test 2: swapstat() 통계 확인
// ============================================================
void
test_swapstat(void)
{
  printf("\n--- Test 2: swapstat() counters ---\n");

  int pid = fork();
  if (pid < 0)
    FAIL("fork failed");

  if (pid == 0) {
    int rd0, wr0;
    swapstat(&rd0, &wr0);

    // 메모리 소진 후 추가 할당
    eat_memory(100);

    int extra = 200;
    char *base = sbrk(extra * PGSIZE);
    if (base == (char*)-1)
      exit(1);

    for (int i = 0; i < extra; i++)
      *(base + i * PGSIZE) = (char)i;

    int rd1, wr1;
    swapstat(&rd1, &wr1);
    printf("  after pressure: reads=%d, writes=%d\n", rd1, wr1);

    if (wr1 <= wr0) {
      printf("FAIL: swap write count did not increase\n");
      exit(1);
    }

    // 다시 읽기 → swap-in
    volatile char sink = 0;
    for (int i = 0; i < extra; i++)
      sink += *(base + i * PGSIZE);

    int rd2, wr2;
    swapstat(&rd2, &wr2);
    printf("  after re-read: reads=%d, writes=%d\n", rd2, wr2);

    if (rd2 <= rd1) {
      printf("FAIL: swap read count did not increase\n");
      exit(1);
    }
    exit(0);
  } else {
    int status;
    wait(&status);
    if (status != 0)
      FAIL("swapstat() counters");
    PASS("swapstat() counters");
  }
}

// ============================================================
// Test 3: fork 시 swapped-out page 복사
//   부모가 메모리 압박 상태에서 데이터 쓰고 fork
//   자식이 swap-in하면서 데이터 정확히 읽는지 확인
// ============================================================
void
test_fork_swap(void)
{
  printf("\n--- Test 3: fork copies swapped-out pages ---\n");

  int pid = fork();
  if (pid < 0)
    FAIL("fork failed");

  if (pid == 0) {
    // 메모리 소진
    eat_memory(100);

    int npages = 200;
    char *base = sbrk(npages * PGSIZE);
    if (base == (char*)-1)
      exit(1);

    // 패턴 쓰기 (일부 페이지는 swap될 것)
    for (int i = 0; i < npages; i++) {
      char *p = base + i * PGSIZE;
      p[0] = (char)(i & 0xFF);
      p[100] = (char)((i * 7) & 0xFF);
    }

    // 메모리 일부 반환해서 fork 가능하게
    sbrk(-(npages / 2) * PGSIZE);
    npages = npages / 2;

    int child = fork();
    if (child < 0) {
      printf("  fork in child failed\n");
      exit(1);
    }

    if (child == 0) {
      // 손자: 부모 데이터 검증
      int errors = 0;
      for (int i = 0; i < npages; i++) {
        char *p = base + i * PGSIZE;
        if (p[0] != (char)(i & 0xFF))
          errors++;
        if (p[100] != (char)((i * 7) & 0xFF))
          errors++;
      }
      if (errors > 0) {
        printf("  grandchild: %d errors\n", errors);
        exit(1);
      }
      exit(0);
    } else {
      int st;
      wait(&st);
      exit(st);
    }
  } else {
    int status;
    wait(&status);
    if (status != 0)
      FAIL("fork copies swapped-out pages");
    PASS("fork copies swapped-out pages");
  }
}

// ============================================================
// Test 4: dealloc 후 swap bitmap 정리
//   할당 → swap 발생 → 해제 → 다시 할당 가능해야 함
// ============================================================
void
test_dealloc_swap(void)
{
  printf("\n--- Test 4: dealloc clears swap space ---\n");

  int pid = fork();
  if (pid < 0)
    FAIL("fork failed");

  if (pid == 0) {
    // 1차: 메모리 거의 소진
    uint64 eaten = eat_memory(50);

    // 추가 할당으로 swap 유발
    int extra = 100;
    char *base = sbrk(extra * PGSIZE);
    if (base == (char*)-1)
      exit(1);
    for (int i = 0; i < extra; i++)
      *(base + i * PGSIZE) = (char)i;

    int rd1, wr1;
    swapstat(&rd1, &wr1);
    printf("  after first alloc: writes=%d\n", wr1);

    // 전부 해제
    sbrk(-(eaten + extra * PGSIZE));

    // 2차: 다시 같은 양 할당 — swap 공간이 정리되었으면 성공
    
    char *base2 = sbrk(extra * PGSIZE);
    if (base2 == (char*)-1) {
      printf("FAIL: second alloc failed (swap not reclaimed?)\n");
      exit(1);
    }

    for (int i = 0; i < extra; i++)
      *(base2 + i * PGSIZE) = (char)(i + 1);

    // 검증
    int errors = 0;
    for (int i = 0; i < extra; i++) {
      if (*(base2 + i * PGSIZE) != (char)(i + 1))
        errors++;
    }

    if (errors > 0) {
      printf("FAIL: data corruption in second allocation\n");
      exit(1);
    }
    exit(0);
  } else {
    int status;
    wait(&status);
    if (status != 0)
      FAIL("dealloc clears swap space");
    PASS("dealloc clears swap space");
  }
}

// ============================================================
// Test 5: 여러 프로세스 동시 swap
// ============================================================
void
test_multiproc_swap(void)
{
  printf("\n--- Test 5: multiple processes with swap ---\n");

  // 먼저 부모가 메모리를 소비해서 압박 환경 조성
  uint64 eaten = eat_memory(2000);
  printf("  parent consumed %d pages\n", (int)(eaten / PGSIZE));

  int nchildren = 4;
  int npages = 200;

  for (int c = 0; c < nchildren; c++) {
    int pid = fork();
    if (pid < 0) {
      printf("  fork %d failed\n", c);
      // 남은 자식들 기다리기
      for (int j = 0; j < c; j++) {
        int st;
        wait(&st);
      }
      sbrk(-eaten);
      FAIL("fork failed in multiproc test");
    }
    if (pid == 0) {
      char *base = sbrk(npages * PGSIZE);
      if (base == (char*)-1)
        exit(1);

      for (int i = 0; i < npages; i++)
        *(base + i * PGSIZE) = (char)((c + i) & 0xFF);

      int errors = 0;
      for (int i = 0; i < npages; i++) {
        if (*(base + i * PGSIZE) != (char)((c + i) & 0xFF))
          errors++;
      }
      exit(errors > 0 ? 1 : 0);
    }
  }

  int fail_count = 0;
  for (int c = 0; c < nchildren; c++) {
    int status;
    wait(&status);
    if (status != 0)
      fail_count++;
  }

  sbrk(-eaten);

  if (fail_count > 0)
    FAIL("some children failed");
  PASS("multiple processes with swap");
}

// ============================================================
// Test 6: OOM 처리
// ============================================================
void
test_oom(void)
{
  printf("\n--- Test 6: OOM handling ---\n");

  int pid = fork();
  if (pid < 0)
    FAIL("fork failed");

  if (pid == 0) {
    uint64 total = 0;
    while (1) {
      char *p = sbrk(PGSIZE);
      if (p == (char*)-1)
        break;
      *p = 1;
      total += PGSIZE;
    }
    printf("  allocated %d pages before OOM\n", (int)(total / PGSIZE));
    if (total == 0)
      exit(1);
    exit(0);
  } else {
    int status;
    wait(&status);
    if (status != 0)
      FAIL("OOM handling");
    PASS("OOM handling");
  }
}

// ============================================================
// Test 7: 전체 페이지 데이터 무결성 (4096바이트 패턴)
// ============================================================
void
test_full_page_integrity(void)
{
  printf("\n--- Test 7: full page data integrity ---\n");

  int pid = fork();
  if (pid < 0)
    FAIL("fork failed");

  if (pid == 0) {
    eat_memory(100);

    int npages = 150;
    char *base = sbrk(npages * PGSIZE);
    if (base == (char*)-1)
      exit(1);

    // 각 페이지 전체를 패턴으로 채움
    for (int i = 0; i < npages; i++) {
      char *p = base + i * PGSIZE;
      for (int j = 0; j < PGSIZE; j++)
        p[j] = (char)((i + j) & 0xFF);
    }

    // 역순으로 읽어서 swap-in 유발
    int errors = 0;
    for (int i = npages - 1; i >= 0; i--) {
      char *p = base + i * PGSIZE;
      for (int j = 0; j < PGSIZE; j++) {
        if (p[j] != (char)((i + j) & 0xFF)) {
          errors++;
          break;
        }
      }
    }

    if (errors > 0) {
      printf("  %d pages corrupted\n", errors);
      exit(1);
    }
    exit(0);
  } else {
    int status;
    wait(&status);
    if (status != 0)
      FAIL("full page data integrity");
    PASS("full page data integrity");
  }
}

// ============================================================
// main
// ============================================================
int
main(int argc, char *argv[])
{
  printf("=== PA4 Page Replacement Test ===\n");

  test_swap_basic();
  test_swapstat();
  test_fork_swap();
  test_dealloc_swap();
  test_multiproc_swap();
  test_oom();
  test_full_page_integrity();

  printf("\n=== ALL PA4 TESTS PASSED ===\n");
  exit(0);
}