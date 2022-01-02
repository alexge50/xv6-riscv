#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int loadseg(pde_t *pgdir, uint64 addr, struct inode *ip, uint offset, uint sz);

int
exec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  struct vm_entry* new_vm = 0;
  struct vm_entry* old_vm;
  uint64 old_trapframe;
  struct proc *p = myproc();

  begin_op();

  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);

  // Check ELF header
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((new_vm = alloc_vm_entry(p)) == 0)
    goto bad;
  release(&new_vm->lock);

  // Load program into memory.
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    uint64 sz1;
    if((sz1 = uvmalloc(new_vm->pagetable, sz, ph.vaddr + ph.memsz)) == 0)
      goto bad;
    sz = sz1;
    if((ph.vaddr % PGSIZE) != 0)
      goto bad;
    if(loadseg(new_vm->pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  p = myproc();
  acquire(&new_vm->lock);
  acquire(&p->vm->lock);

  // Allocate two pages at the next page boundary.
  // Use the second as the user stack.
  sz = PGROUNDUP(sz);
  uint64 sz1;
  if((sz1 = uvmalloc(new_vm->pagetable, sz, sz + 2*PGSIZE)) == 0)
    goto bad;
  sz = sz1;
  uvmclear(new_vm->pagetable, sz-2*PGSIZE);
  sp = sz;
  stackbase = sp - PGSIZE;

  // Push argument strings, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if(sp < stackbase)
      goto bad;
    if(copyout(new_vm->pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push the array of argv[] pointers.
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(copyout(new_vm->pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  p->trapframe->a1 = sp;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));

  // Make clean copy of `fs_entry` if the resource is shared
  acquire(&p->fs->lock);
  if(p->fs->reference_count > 1) {
    struct fs_entry* old_fs = p->fs;
    p->fs = alloc_fs_entry();
    p->fs->cwd = idup(old_fs->cwd);
    free_fs_entry(old_fs);
    release(&old_fs->lock);
  }
  release(&p->fs->lock);

  // Make a clean copy of `files_entry` if the resource is shared
  acquire(&p->files->lock);
  if(p->files->reference_count > 1) {
    struct files_entry* old_files = p->files;
    p->files = alloc_files_entry();
    for(i = 0; i < NOFILE; i++) {
      if(p->files->ofile[i])
        p->files->ofile[i] = filedup(old_files->ofile[i]);
    }
    free_files_entry(old_files);
    release(&old_files->lock);
  }
  release(&p->files->lock);

  // Commit to the user image.
  old_vm = p->vm;
  old_trapframe = p->user_trapframe;
  p->vm = new_vm;
  p->vm->sz = sz;
  p->vm->last_trapframe = TRAPFRAME;
  p->user_trapframe = TRAPFRAME;
  p->trapframe->epc = elf.entry;  // initial program counter = main
  p->trapframe->sp = sp; // initial stack pointer

  // Freeing previous process trap frame and vm entry
  uvmunmap(old_vm->pagetable, old_trapframe, 1, 0);
  release(&old_vm->lock);
  free_vm_entry(old_vm);
  release(&p->vm->lock);

  return argc; // this ends up in a0, the first argument to main(argc, argv)

 bad:
  if(holding(&p->vm->lock))
    release(&p->vm->lock);
  if(new_vm) {
    uvmunmap(new_vm->pagetable, TRAPFRAME, 1, 0);
    new_vm->sz = sz;
    if(holding(&new_vm->lock))
      release(&new_vm->lock);
    free_vm_entry(new_vm);
  }
  if(ip){
    iunlockput(ip);
    end_op();
  }
  return -1;
}

// Load a program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  for(i = 0; i < sz; i += PGSIZE){
    pa = walkaddr(pagetable, va + i);
    if(pa == 0)
      panic("loadseg: address should exist");
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, 0, (uint64)pa, offset+i, n) != n)
      return -1;
  }
  
  return 0;
}
