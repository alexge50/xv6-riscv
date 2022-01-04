#include <kernel/types.h>
#include <kernel/clone.h>
#include <kernel/futex.h>
#include <kernel/fcntl.h>
#include <user/user.h>
#include <user/pthreads.h>


pthread_mutex_t print_mutex;

void *print_message_function(void* p)
{
  pthread_mutex_lock(&print_mutex);
  printf("Hey, from %s\n", (char *)p);
  pthread_mutex_unlock(&print_mutex);

  return 0;
}

int main()
{
  pthread_t thread1, thread2;
  char* message1 = "Thread 1";
  char* message2 = "Thread 2";

  pthread_mutex_init(&print_mutex, 0);
  pthread_create(&thread1, 0, print_message_function, (void*) message1);
  pthread_create(&thread2, 0, print_message_function, (void*) message2);

  pthread_join(thread1, 0);
  pthread_join(thread2, 0);

  exit(0);
}
