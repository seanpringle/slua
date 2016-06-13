/*
Copyright (c) 2016 Sean Pringle sean.pringle@gmail.com

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

typedef struct {
  char name[64];
  pthread_mutex_t mutex;
  size_t size;
} store_t;

arena_t*
store_arena (store_t *store)
{
  return (void*)store + sizeof(store_t);
}

store_t*
store_create (const char *name, int size, int page)
{
  char ipcname[64];
  snprintf(ipcname, sizeof(ipcname), "/slua_%d_%s", getpid(), name);

  int fd = shm_open(ipcname, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  if (fd < 0 || ftruncate(fd, size) != 0) return NULL;

  store_t *store = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (store == MAP_FAILED) return NULL;

  close(fd);

  pthread_mutexattr_t mutexattr;
  ensure(pthread_mutexattr_init(&mutexattr) == 0);
  ensure(pthread_mutexattr_setpshared(&mutexattr, PTHREAD_PROCESS_SHARED) == 0);
  ensure(pthread_mutex_init(&store->mutex, &mutexattr) == 0);
  pthread_mutexattr_destroy(&mutexattr);

  store->size = size;
  strcpy(store->name, ipcname);

  ensure(arena_open(store_arena(store), size - sizeof(store_t), page) == 0);

  return store;
}

void
store_destroy (store_t *store)
{
  pthread_mutex_destroy(&store->mutex);
  arena_close(store_arena(store));
  char ipcname[64];
  strcpy(ipcname, store->name);
  munmap(store, store->size);
  shm_unlink(ipcname);
}

void*
store_alloc (store_t *store, int len)
{
  ensure(pthread_mutex_lock(&store->mutex) == 0);
  void *ptr = arena_alloc(store_arena(store), len);
  ensure(pthread_mutex_unlock(&store->mutex) == 0);
  return ptr;
}

void
store_free (store_t *store, void *ptr)
{
  ensure(pthread_mutex_lock(&store->mutex) == 0);
  ensure(arena_free(store_arena(store), ptr) == 0);
  ensure(pthread_mutex_unlock(&store->mutex) == 0);
}
