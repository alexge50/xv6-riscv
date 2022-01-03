#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "clone.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct vm_entry vm_entries[NPROC];
struct files_entry files_entries[NPROC];
struct fs_entry fs_entries[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

int vm_entries_count = 0;
struct spinlock vm_entries_lock;

int files_entries_count = 0;
struct spinlock files_entries_lock;

int fs_entries_count = 0;
struct spinlock fs_entries_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl) {
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->kstack = KSTACK((int) (p - proc));
  }
}

// initialise entry tables
void
proc_resource_entries_init()
{
  initlock(&vm_entries_lock, "vm_entries_lock");
  initlock(&files_entries_lock, "files_entries_lock");
  initlock(&fs_entries_lock, "fs_entries_lock");

  for (struct vm_entry *e = vm_entries; e < &vm_entries[NPROC]; e++) {
    initlock(&e->lock, "vm_entry");
    e->reference_count = 0;
  }

  for (struct files_entry *e = files_entries; e < &files_entries[NPROC]; e++) {
    initlock(&e->lock, "files_entry");
    e->reference_count = 0;
  }

  for (struct fs_entry *e = fs_entries; e < &fs_entries[NPROC]; e++) {
    initlock(&e->lock, "fs_entry");
    e->reference_count = 0;
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid() {
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

static uint64 proc_alloc_trapframe(struct proc* p);

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(struct vm_entry* vm, struct files_entry* files, struct fs_entry* fs)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    goto bad;
  }

  // An empty user page table.
  if(vm != 0) {
    acquire(&vm->lock);
    p->vm = vm;
    p->user_trapframe = proc_alloc_trapframe(p);
    p->vm->reference_count++;

    if(p->user_trapframe == 0) {
      goto bad;
    }

    p->vm->last_trapframe = p->user_trapframe;
  } else { // allocate new vm
    p->vm = alloc_vm_entry(p);
    if (p->vm == 0) {
      goto bad;
    }
    p->user_trapframe = TRAPFRAME;
    p->vm->last_trapframe = p->user_trapframe;
  }

  if(files != 0) {
    p->files = files;
    acquire(&p->files->lock);
    p->files->reference_count++;
  } else {
    p->files = alloc_files_entry();
    if (p->files == 0) {
      goto bad;
    }
  }

  if(fs != 0) {
    p->fs = fs;
    acquire(&p->fs->lock);
    p->fs->reference_count++;
  } else {
    p->fs = alloc_fs_entry();
    if (p->fs == 0) {
      goto bad;
    }
  }

  release(&p->fs->lock);
  release(&p->vm->lock);
  release(&p->files->lock);

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;

bad:
  if(holding(&p->vm->lock))
    release(&p->vm->lock);
  if(holding(&p->files->lock))
    release(&p->files->lock);
  if(holding(&p->fs->lock))
    release(&p->fs->lock);

  freeproc(p);

  release(&p->lock);

  return 0;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held, p->vm->lock must be held
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;

  if(p->user_trapframe)
    uvmunmap(p->vm->pagetable, p->user_trapframe, 1, 0);
  p->user_trapframe = 0;

  free_vm_entry(p->vm);
  p->vm = 0;

  free_files_entry(p->files);
  free_fs_entry(p->fs);
  p->fs = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// frees the entry if there is no other process sharing this resource
// e->lock must NOT be held.
void
free_vm_entry(struct vm_entry* e)
{
  if(e->reference_count == 0) {
    panic("already free'ed vm_entry");
  }

  acquire(&e->lock);

  if(e->reference_count == 1) {
    // free on the last reference held

    pagetable_t pagetable = e->pagetable;
    uint64 sz = e->sz;
    e->pagetable = 0;
    e->sz = 0;
    e->reference_count --;
    release(&e->lock);

    if (pagetable) {
      proc_freepagetable(pagetable, sz);
    }
  } else {
    e->reference_count --;
    release(&e->lock);
  }
}

struct vm_entry*
alloc_vm_entry(struct proc* p)
{
  struct vm_entry *entry = 0;

  for (struct vm_entry *e = vm_entries; e < &vm_entries[NPROC] && entry == 0; e++) {
    acquire(&e->lock);
    if (e->reference_count == 0) {
      entry = e;
    } else {
      release(&e->lock);
    }
  }

  if (entry == 0) {
    return 0;
  }

  entry->reference_count ++;
  entry->pagetable = proc_pagetable(p);
  if (entry->pagetable == 0) {
    release(&entry->lock);
    free_vm_entry(entry);
    return 0;
  }
  entry->last_trapframe = TRAPFRAME;
  entry->sz = 0;

  return entry;
}

// e->lock must NOT be held.
void
free_files_entry(struct files_entry* e)
{
  if(e == 0)
    return;

  acquire(&e->lock);

  if(e->reference_count == 0) {
    panic("already free'ed fs_entry");
  }

  if(e->reference_count == 1) {
    // free on the last reference held
    struct file *ofile[NOFILE];
    for(int i = 0; i < NOFILE; i++) {
      ofile[i] = e->ofile[i];
    }
    memset(e->ofile, 0, sizeof(struct file*) * NOFILE);
    e->reference_count --;
    release(&e->lock);

    for(int fd = 0; fd < NOFILE; fd++){
      if(ofile[fd]){
        struct file *f = ofile[fd];
        fileclose(f);
      }
    }
  } else {
    e->reference_count --;
    release(&e->lock);
  }
}

struct files_entry*
alloc_files_entry()
{
  struct files_entry *entry = 0;

  for (struct files_entry *e = files_entries; e < &files_entries[NPROC] && entry == 0; e++) {
    acquire(&e->lock);
    if (e->reference_count == 0) {
      entry = e;
      e->reference_count ++;
    } else {
      release(&e->lock);
    }
  }

  return entry;
}

// e->lock must NOT be held.
void
free_fs_entry(struct fs_entry* e)
{
  if(e == 0)
    return;

  acquire(&e->lock);

  if(e->reference_count == 0) {
    panic("already free'ed fs_entry");
  }

  if(e->reference_count == 1) {
    // free on the last reference held
    struct inode *cwd = e->cwd;
    e->cwd = 0;
    e->reference_count --;
    release(&e->lock);

    if(cwd != 0) {
      begin_op();
      iput(cwd);
      end_op();
    }

  } else {
    e->reference_count --;
    release(&e->lock);
  }
}

struct fs_entry*
alloc_fs_entry()
{
  struct fs_entry *entry = 0;

  for (struct fs_entry *e = fs_entries; e < &fs_entries[NPROC] && entry == 0; e++) {
    acquire(&e->lock);
    if (e->reference_count == 0) {
      entry = e;
      e->reference_count++;
    } else {
      release(&e->lock);
    }
  }

  return entry;
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Creates a new trapframe page for a process with shared memory
// vm->lock and p->lock must be held
static uint64
proc_alloc_trapframe(struct proc* p)
{
  uint64 user_trapframe = p->vm->last_trapframe - PGSIZE;

  if(mappages(p->vm->pagetable, user_trapframe, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    return 0;
  }

  return user_trapframe;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc(0, 0, 0);
  initproc = p;
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->vm->pagetable, initcode, sizeof(initcode));
  p->vm->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  acquire(&p->fs->lock);
  p->fs->cwd = namei("/");
  release(&p->fs->lock);

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  acquire(&p->vm->lock);

  sz = p->vm->sz;
  if(n > 0){
    if((sz = uvmalloc(p->vm->pagetable, sz, sz + n)) == 0) {
      release(&p->vm->lock);
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->vm->pagetable, sz, sz + n);
  }
  p->vm->sz = sz;

  release(&p->vm->lock);

  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  struct clone_args cl = {
    .flags = CLONE_FORK,
    .stack = 0
  };

  return clone(cl);
}

// Create a new process, setting up shared resources, or copying the parent
int
clone(struct clone_args cl)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc(
      cl.flags & CLONE_VM ? p->vm : 0,
      cl.flags & CLONE_FILES ? p->files : 0,
      cl.flags & CLONE_FS ? p->fs : 0
      )) == 0){
    return -1;
  }

  if(!(cl.flags & CLONE_VM)) {
    // Copy user memory from parent to child.
    acquire(&p->vm->lock);
    if(uvmcopy(p->vm->pagetable, np->vm->pagetable, p->vm->sz) < 0) {
      release(&p->vm->lock);
      freeproc(np);
      release(&np->lock);
      return -1;
    }
    np->vm->sz = p->vm->sz;
    release(&p->vm->lock);
  }

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  if(cl.flags & CLONE_VM) {
    np->trapframe->sp = cl.stack;
    np->trapframe->a0 = cl.arg;
    np->trapframe->epc = cl.fn;
  } else {
    // Cause clone to return 0 in the child.
    np->trapframe->a0 = 0;
  }



  // increment reference counts on open file descriptors.
  if(!(cl.flags & CLONE_FILES)) {
    acquire(&p->files->lock);
    acquire(&np->files->lock);

    for(i = 0; i < NOFILE; i++) {
      if(p->files->ofile[i])
        np->files->ofile[i] = filedup(p->files->ofile[i]);
    }

    release(&p->files->lock);
    release(&np->files->lock);
  }


  if(!(cl.flags & CLONE_FS)) {
    acquire(&np->fs->lock);
    acquire(&p->fs->lock);
    np->fs->cwd = idup(p->fs->cwd);
    release(&np->fs->lock);
    release(&p->fs->lock);
  }

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  free_files_entry(p->files);
  p->files = 0;

  // free_fs_entry
  free_fs_entry(p->fs);
  p->fs = 0;

  acquire(&wait_lock);

  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->vm->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }

          freeproc(np);

          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Wait for the child process with a given pid to exit.
// Return -1 if this process has no children.
int
waitpid(uint64 pid, uint64 addr)
{
  struct proc *np;
  struct proc *p = myproc();
  struct proc* child_proc = 0;

  acquire(&wait_lock);
  for(struct proc* np = proc; np < &proc[NPROC]; np++) {
    if(np->pid == pid && np->parent == p) {
      child_proc = np;
    }
  }

  if(child_proc == 0) {
    release(&wait_lock);
    return -1;
  }

  // wait for process
  while(1) {
    acquire(&child_proc->lock);
    if(child_proc->state == ZOMBIE) {
      if(addr != 0 && copyout(p->vm->pagetable, addr, (char *)&child_proc->xstate,
                                      sizeof(np->xstate)) < 0) {
        release(&child_proc->lock);
        release(&wait_lock);
        return -1;
      }

      freeproc(child_proc);

      release(&child_proc->lock);
      release(&wait_lock);

      return pid;
    }
    release(&child_proc->lock);

    if(p->killed || child_proc->pid != pid) {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
int
wakeup(void *chan)
{
  struct proc *p;
  int r = 0;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
      r ++;
    }
  }

  return r;
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->vm->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->vm->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
