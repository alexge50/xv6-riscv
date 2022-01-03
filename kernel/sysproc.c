#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "clone.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->vm->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_clone(void)
{
  struct proc* p = myproc();
  uint64 cl_addr;

  struct clone_args cl;


  if(argaddr(0, &cl_addr) < 0) {
    return -1;
  }

  if(cl_addr >= p->vm->sz || cl_addr + sizeof(uint64) > p->vm->sz) {
    return -1;
  }

  if(copyin(p->vm->pagetable, (char *)&cl, cl_addr, sizeof(struct clone_args)) != 0) {
    return -1;
  }

  if(cl.flags & CLONE_VM && (cl.stack == 0))  {
    return -1;
  }

  return clone(cl);
}

uint64
sys_waitpid(void)
{
  int pid;
  uint64 p;
  if(argint(0, &pid) < 0 || argaddr(1, &p) < 0)
    return -1;
  return waitpid(pid, p);
}

uint64
sys_futex(void)
{
  int op;
  uint64 p;
  if(argint(0, &op) < 0 || argaddr(1, &p) < 0)
    return -1;


  return futex(op, p);
}