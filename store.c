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
  int used;
  int chunk;
  int count;
  int next;
} store_t;

int*
store_links (store_t *store)
{
  return (void*)store + sizeof(store_t);
}

int*
store_lengths (store_t *store)
{
  int array = store->count * sizeof(int);
  return (void*)store_links(store) + array;
}

void*
store_data (store_t *store)
{
  int array = store->count * sizeof(int);
  return (void*)store_lengths(store) + array;
}

void*
store_slot (store_t *store, int id)
{
  return store_data(store) + (id * store->chunk);
}

int
store_size (store_t *store)
{
  return sizeof(store_t)
    + (sizeof(int) * store->count) // links
    + (sizeof(int) * store->count) // lengths
    + (store->chunk * store->count); // data
}

store_t*
store_create (const char *name, int chunk, int count)
{
  int size = sizeof(store_t)
    + (sizeof(int) * count) // links
    + (sizeof(int) * count) // lengths
    + (chunk * count); // data

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

  store->used  = 0;
  store->chunk = chunk;
  store->count = count;
  store->next  = 0;

  strcpy(store->name, ipcname);

  int *links = store_links(store);
  for (int i = 0; i < count; links[i] = i+1, i++);
  links[count-1] = -1;

  int *lengths = store_lengths(store);
  for (int i = 0; i < count; lengths[i++] = 0);

  return store;
}

void
store_destroy (store_t *store)
{
  char ipcname[64];
  strcpy(ipcname, store->name);
  munmap(store, store_size(store));
  shm_unlink(ipcname);
}

int
store_set (store_t *store, void *src, int len)
{
  ensure(pthread_mutex_lock(&store->mutex) == 0);

  int *links = store_links(store);
  int *lengths = store_lengths(store);

  int id = store->next;

  ensure(id >= 0) errorf("store(%d) full", store->chunk);

  int slot = store->next;
  int bytes = store->chunk;

  while (bytes < len)
  {
    slot = links[slot];
    ensure (slot >= 0) errorf("store(%d) full", store->chunk);
    bytes += store->chunk;
  }

  slot = store->next;
  bytes = min(store->chunk, len);
  if (src)
  {
    memmove(store_slot(store, slot), src, bytes);
    src += bytes;
  }
  len -= bytes;
  lengths[slot] = bytes;
  store->used++;

  while (len > 0)
  {
    slot = links[slot];
    bytes = min(store->chunk, len);
    if (src)
    {
      memmove(store_slot(store, slot), src, bytes);
      src += bytes;
    }
    len -= bytes;
    lengths[slot] = bytes;
    store->used++;
  }

  store->next = links[slot];
  links[slot] = -1;

  ensure(pthread_mutex_unlock(&store->mutex) == 0);
  return id;
}

void*
store_get (store_t *store, int id, int remove)
{
  ensure(pthread_mutex_lock(&store->mutex) == 0);

  int *links = store_links(store);
  int *lengths = store_lengths(store);

  int slot = id;
  int len = lengths[slot];

  while (links[slot] > -1)
  {
    slot = links[slot];
    len += lengths[slot];
  }

  void *dst = malloc(len);

  slot = id;
  len = lengths[slot];
  memmove(dst, store_slot(store, slot), len);

  while (links[slot] > -1)
  {
    slot = links[slot];
    memmove(dst + len, store_slot(store, slot), lengths[slot]);
    len += lengths[slot];
  }

  if (remove)
  {
    int slot = id;
    do {
      int next = links[slot];
      links[slot] = store->next;
      store->next = slot;
      slot = next;
      store->used--;
    }
    while (slot > -1);
  }

  ensure(pthread_mutex_unlock(&store->mutex) == 0);
  return dst;
}

void*
store_alloc (store_t *store, int len)
{
  return store_slot(store, store_set(store, NULL, len));
}

void
store_free (store_t *store, void *ptr)
{
  free(store_get(store, (ptr - store_slot(store, 0)) / store->chunk, 1));
}
