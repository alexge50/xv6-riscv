#include <kernel/types.h>
#include <kernel/clone.h>
#include <kernel/futex.h>
#include <kernel/fcntl.h>
#include <user/user.h>
#include <user/pthreads.h>

pthread_mutex_t print_mutex;

#define queue_size 1024

struct {
  int data[queue_size];
  int front, back;
} queue;

void queue_push(int);
int queue_pop();

void* sender(void* p) {
  int v = 5;

  while (1) {
    pthread_mutex_lock(&print_mutex);
    printf("[tx thread]: send %d\n", v);
    pthread_mutex_unlock(&print_mutex);

    queue_push(v);
    v = (v + 7) % 31;

    sleep(10);
  }

  return 0;
}

void* receiver(void* p) {
  while (1) {
    int v = queue_pop();

    pthread_mutex_lock(&print_mutex);
    printf("[rx thread]: recv %d\n", v);
    pthread_mutex_unlock(&print_mutex);

    sleep(11);
  }

  return 0;
}

int main()
{
  pthread_t thread1, thread2;

  pthread_mutex_init(&print_mutex, 0);
  pthread_create(&thread1, 0, sender, 0);
  pthread_create(&thread2, 0, receiver, 0);

  pthread_join(thread1, 0);
  pthread_join(thread2, 0);

  exit(0);
}

void queue_push(int t) {
  int back = __atomic_load_n(&queue.back, __ATOMIC_SEQ_CST);

  queue.data[back] = t;

  back = (back + 1) % queue_size;
  __atomic_store_n(&queue.back, back, __ATOMIC_SEQ_CST);

  futex(FUTEX_WAKE, &queue);
}

int queue_pop() {
  do {
    int front = __atomic_load_n(&queue.front, __ATOMIC_SEQ_CST);
    if (front != __atomic_load_n(&queue.back, __ATOMIC_SEQ_CST)) {
      int t = queue.data[front];

      front = (front + 1) % queue_size;

      __atomic_store_n(&queue.front, front, __ATOMIC_SEQ_CST);

      return t; 
    }

    futex(FUTEX_WAIT, &queue);

  } while (1);

  __builtin_unreachable();
}

