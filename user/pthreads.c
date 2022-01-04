#include "kernel/types.h"
#include "kernel/futex.h"
#include "kernel/clone.h"
#include "user/pthreads.h"
#include "user/user.h"

struct futex_lock {
  int lock;
};

static int cmpxchg(int* atom, int expected, int desired) {
  __atomic_compare_exchange_n(atom, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  return expected;
}

static void acquire(struct futex_lock* l) {
  int c = cmpxchg(&l->lock, 0, 1);
  if (c != 0) {
    do {
      if (c == 2 || cmpxchg(&l->lock, 1, 2) != 0) {
        futex(FUTEX_WAIT, (int*)&l->lock);
      }
    } while ((c = cmpxchg(&l->lock, 0, 2)) != 0);
  }
}

static int try_acquire(struct futex_lock* l) {
  int c = cmpxchg(&l->lock, 0, 1);
  return c == 0;
}

static void release(struct futex_lock* l) {
  if(__atomic_fetch_sub(&l->lock, 1, __ATOMIC_SEQ_CST) != 1) {
    __atomic_store_n(&l->lock, 0, __ATOMIC_SEQ_CST);
    futex(FUTEX_WAKE, &l->lock);
  }
}

struct futex_lock memlock = {.lock = 0};

void* _tsmalloc(uint n) {
  acquire(&memlock);
  void* r = malloc(n);
  release(&memlock);

  return r;
}

void  _tsfree(void* p) {
  acquire(&memlock);
  free(p);
  release(&memlock);
}

int pthread_mutex_destroy(pthread_mutex_t* m) {
  _tsfree(m->lock);
  m->lock = 0;
  return 0;
}

int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t *) {
  m->lock = _tsmalloc(sizeof(struct futex_lock));
  ((struct futex_lock*)m->lock)->lock = 0;
  return 0;
}

int pthread_mutex_lock(pthread_mutex_t* m) {
  acquire(m->lock);
  return 0;
}
int pthread_mutex_trylock(pthread_mutex_t* m) {
  return try_acquire(m->lock);
}

int pthread_mutex_unlock(pthread_mutex_t* m) {
  release(m->lock);
  return 0;
}

int pthread_join(pthread_t thread, void **) {
  waitpid(thread, 0);
  return 0;
}

pthread_t pthread_self(void) {
  return getpid();
}

struct thread_helper {
  void *(*fn)(void *);
  void* arg;
};

static void clone_helper(void* arg) {
  struct thread_helper* th = arg;
  struct thread_helper copy = *th;
  _tsfree(th);

  copy.fn(copy.arg);
  exit(0);
}

int pthread_create(pthread_t* thread, const pthread_attr_t *, void *(*fn)(void *), void* arg) {
  struct thread_helper* th = _tsmalloc(sizeof(struct thread_helper));
  th->fn = fn;
  th->arg = arg;

  struct clone_args cl = {
      .flags = CLONE_VM | CLONE_FILES | CLONE_FS,
      .stack = (uint64) ((char*)_tsmalloc(4096) + 4095),
      .fn = (uint64) clone_helper,
      .arg = (uint64) th
  };

  int pid = clone(&cl);

  if(pid < 0) {
    return -1;
  }

  *thread = pid;
  return 0;
}