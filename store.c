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
  pthread_mutex_t mutex;
  size_t size;
} store_t;

arena_t*
store_arena (store_t *store)
{
  return (void*)store + sizeof(store_t);
}

store_t*
store_create (int size, int page)
{
  store_t *store = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, 0, 0);
  if (store == MAP_FAILED) return NULL;

  ensure(pthread_mutex_init(&store->mutex, &global.mutexattr) == 0);

  store->size = size;

  ensure(arena_open(store_arena(store), size - sizeof(store_t), page) == 0);

  return store;
}

void
store_destroy (store_t *store)
{
  pthread_mutex_destroy(&store->mutex);
  arena_close(store_arena(store));
  munmap(store, store->size);
}

void*
store_alloc (store_t *store, int len)
{
  ensure(pthread_mutex_lock(&store->mutex) == 0);
  void *ptr = arena_alloc(store_arena(store), len);
  ensure(pthread_mutex_unlock(&store->mutex) == 0);
  ensure(ptr) errorf("store is full");
  return ptr;
}

void
store_free (store_t *store, void *ptr)
{
  ensure(pthread_mutex_lock(&store->mutex) == 0);
  ensure(arena_free(store_arena(store), ptr) == 0);
  ensure(pthread_mutex_unlock(&store->mutex) == 0);
}
