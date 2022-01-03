#include "types.h"
#include "riscv.h"
#include "spinlock.h"
#include "param.h"
#include "proc.h"
#include "defs.h"
#include "futex.h"

struct spinlock futex_lock;

void
futex_init()
{
  initlock(&futex_lock, "futex");
}

static int
futex_wait(uint64 addr)
{
  acquire(&futex_lock);
  sleep((void*)addr, &futex_lock);
  release(&futex_lock);
  return 0;
}

static int
futex_wake(uint64 addr)
{
  return wakeup((void*) addr);
}

int
futex(int op, uint64 addr)
{
  if(op == FUTEX_WAIT) {
    return futex_wait(addr);
  } else if (op == FUTEX_WAKE) {
    return futex_wake(addr);
  }

  return -1;
}