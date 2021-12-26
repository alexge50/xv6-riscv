#define CLONE_FORK    0x0
#define CLONE_VM      0x1
#define CLONE_FS      0x2
#define CLONE_FILES   0x4

struct clone_args {
  uint64 flags;            // clone flags
  uint64 stack;            // user address of the stack for the new process,
                           // only required when sharing the virtual memory (CLONE_VM)
};