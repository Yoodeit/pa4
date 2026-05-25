#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "user/user.h"

#define PGSIZE 4096
#define BLKS_PER_PAGE (PGSIZE / BSIZE)
#define MAX_PRESSURE_PAGES 40000
#define CHUNK_PAGES 64

#define FAIL(msg) do { \
  printf("FAIL: %s\n", msg); \
  exit(1); \
} while(0)

static void
getstat(int *r, int *w)
{
  *r = -1;
  *w = -1;
  swapstat(r, w);
  if(*r < 0 || *w < 0)
    FAIL("swapstat returned invalid counters");
}

static void
wait_ok(int pid, char *name)
{
  int st;

  if(pid < 0)
    FAIL("fork failed");
  wait(&st);
  if(st != 0){
    printf("child status %d\n", st);
    FAIL(name);
  }
  printf("PASS: %s\n", name);
}

static unsigned char
pat(int i)
{
  return (unsigned char)((i * 131 + 17) & 0xff);
}

static void
touch_page(char *p, int i)
{
  p[0] = pat(i);
  p[PGSIZE / 2] = pat(i + 3);
  p[PGSIZE - 1] = pat(i + 7);
}

static int
check_page(char *p, int i)
{
  return (unsigned char)p[0] == pat(i) &&
         (unsigned char)p[PGSIZE / 2] == pat(i + 3) &&
         (unsigned char)p[PGSIZE - 1] == pat(i + 7);
}

static char *
alloc_until_writes(int target_write_delta, int *out_pages, int *out_writes)
{
  int r0, w0, r1, w1;
  char *base;
  int pages;

  getstat(&r0, &w0);
  base = sbrk(0);
  pages = 0;

  while(pages < MAX_PRESSURE_PAGES){
    char *p = sbrk(CHUNK_PAGES * PGSIZE);
    if(p == (char*)-1)
      break;

    for(int i = 0; i < CHUNK_PAGES; i++)
      touch_page(p + i * PGSIZE, pages + i);
    pages += CHUNK_PAGES;

    getstat(&r1, &w1);
    if(w1 - w0 >= target_write_delta){
      *out_pages = pages;
      *out_writes = w1 - w0;
      return base;
    }
  }

  getstat(&r1, &w1);
  printf("allocated %d pages, swap write delta %d\n", pages, w1 - w0);
  FAIL("memory pressure did not cause swap-out");
  return 0;
}

static void
test_swap_syscalls(void)
{
  char *buf = sbrk(PGSIZE);
  int r0, w0, r1, w1, r2, w2;

  if(buf == (char*)-1)
    FAIL("sbrk failed");

  for(int i = 0; i < PGSIZE; i++)
    buf[i] = (char)(i * 29 + 5);

  getstat(&r0, &w0);
  swapwrite(buf, 0);
  getstat(&r1, &w1);

  for(int i = 0; i < PGSIZE; i++)
    buf[i] = 0;

  swapread(buf, 0);
  getstat(&r2, &w2);

  if(w1 - w0 != BLKS_PER_PAGE)
    FAIL("swapwrite counter delta is not one page");
  if(r2 - r1 != BLKS_PER_PAGE)
    FAIL("swapread counter delta is not one page");

  for(int i = 0; i < PGSIZE; i++)
    if((unsigned char)buf[i] != (unsigned char)(i * 29 + 5))
      FAIL("swapread returned corrupted data");
}

static void
test_pressure_swapin(void)
{
  int pages, writes;
  int r0, w0, r1, w1;
  char *base;
  int errors = 0;

  getstat(&r0, &w0);
  base = alloc_until_writes(BLKS_PER_PAGE, &pages, &writes);
  printf("swap-out after %d pages, wrote %d sectors\n", pages, writes);

  for(int i = 0; i < pages; i++){
    if(!check_page(base + i * PGSIZE, i))
      errors++;
  }

  getstat(&r1, &w1);
  if(errors)
    FAIL("data changed after swap-in");
  if(r1 <= r0)
    FAIL("re-access did not cause any swap reads");
}

static void
test_fork_copies_swapped_pages(void)
{
  int pages, writes;
  char *base = alloc_until_writes(BLKS_PER_PAGE, &pages, &writes);
  int keep = 128;
  int pid;

  // Keep the oldest pages, which are the likely clock victims, but free the
  // rest so fork has enough memory to copy the remaining address space.
  if(pages > keep){
    if(sbrk(-((pages - keep) * PGSIZE)) == (char*)-1)
      FAIL("failed to shrink pressure range before fork");
    pages = keep;
  }

  pid = fork();
  if(pid < 0)
    FAIL("fork under swap pressure failed");

  if(pid == 0){
    int errors = 0;
    for(int i = 0; i < pages; i++)
      if(!check_page(base + i * PGSIZE, i))
        errors++;
    exit(errors == 0 ? 0 : 1);
  }

  wait_ok(pid, "fork copies swapped-out pages");
}

static void
test_copyin_copyout_swapped_pages(void)
{
  int pages, writes;
  char *base = alloc_until_writes(BLKS_PER_PAGE, &pages, &writes);
  int fd;
  char tmp[16];

  fd = open("pa4tmp", 0x202);  // O_CREATE | O_RDWR
  if(fd < 0)
    FAIL("open pa4tmp failed");

  if(write(fd, base, PGSIZE) != PGSIZE)
    FAIL("write from swapped user buffer failed");
  close(fd);

  for(int i = 0; i < PGSIZE; i++)
    base[i] = 0;

  fd = open("pa4tmp", 0);
  if(fd < 0)
    FAIL("reopen pa4tmp failed");
  if(read(fd, base, PGSIZE) != PGSIZE)
    FAIL("read into swapped user buffer failed");
  close(fd);
  unlink("pa4tmp");

  if(!check_page(base, 0))
    FAIL("copyin/copyout path corrupted swapped page");

  // Also force copyinstr() to fault in a swapped page.
  strcpy(base, "pa4tmp");
  fd = open(base, 0x202);
  if(fd < 0)
    FAIL("open with swapped path buffer failed");
  write(fd, "ok", 2);
  close(fd);
  fd = open("pa4tmp", 0);
  if(fd < 0)
    FAIL("open pa4tmp after copyinstr failed");
  if(read(fd, tmp, sizeof(tmp)) != 2)
    FAIL("copyinstr follow-up read failed");
  close(fd);
  unlink("pa4tmp");
}

static void
test_dealloc_reclaims_swapped_slots(void)
{
  int pages, writes;
  char *oldbrk;

  oldbrk = sbrk(0);
  alloc_until_writes(BLKS_PER_PAGE, &pages, &writes);
  if(sbrk(-(pages * PGSIZE)) == (char*)-1)
    FAIL("dealloc pressure range failed");
  if(sbrk(0) != oldbrk)
    FAIL("break did not return to original value");

  alloc_until_writes(BLKS_PER_PAGE, &pages, &writes);
}

static void
run_child(void (*fn)(void), char *name)
{
  int pid = fork();

  if(pid < 0)
    FAIL("fork failed");
  if(pid == 0){
    fn();
    exit(0);
  }
  wait_ok(pid, name);
}

int
main(int argc, char *argv[])
{
  printf("pa4 page replacement tests\n");

  run_child(test_swap_syscalls, "provided swap syscalls");
  run_child(test_pressure_swapin, "kalloc swap-out and fault swap-in");
  run_child(test_fork_copies_swapped_pages, "fork copies swapped-out pages");
  run_child(test_copyin_copyout_swapped_pages, "kernel copy paths swap in pages");
  run_child(test_dealloc_reclaims_swapped_slots, "dealloc clears swapped PTEs");

  printf("ALL PA4 TESTS PASSED\n");
  exit(0);
}
