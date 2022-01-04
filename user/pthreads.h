void* _tsmalloc(uint);
void  _tsfree(void*);

typedef struct {
  void* lock;
} pthread_mutex_t;

typedef void* pthread_mutexattr_t; // unimplemented
typedef void* pthread_attr_t; // unimplemented

typedef int pthread_t;

int         pthread_mutex_destroy(pthread_mutex_t *);
int         pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
int         pthread_mutex_lock(pthread_mutex_t *);
int         pthread_mutex_trylock(pthread_mutex_t *);
int         pthread_mutex_unlock(pthread_mutex_t *);

int         pthread_join(pthread_t, void **);
pthread_t   pthread_self(void);
int         pthread_create(pthread_t *, const pthread_attr_t *,
                     void *(*)(void *), void *);