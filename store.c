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

/*
void
dump (void *ptr, int len)
{
  unsigned char *p = ptr, *e = ptr+len;
  while (p < e)
  {
    fprintf(stderr, "%lx  ", (uint64_t)p);
    for (int j = 0; j < 16; j++)
    {
      fprintf(stderr, "%02x ", p[j]);
    }
    fprintf(stderr, " ");
    for (int j = 0; j < 16; j++)
    {
      fprintf(stderr, "%c", isalnum(p[j]) ? p[j]: '.');
    }
    fprintf(stderr, "\n");
    p += 16;
  }
}
*/

pthread_mutex_t*
store_mutex (void *ptr)
{
  return (void*) (ptr);
}

int*
store_chunk (void *ptr)
{
  return ptr + sizeof(pthread_mutex_t);
}

int*
store_count (void *ptr)
{
  return ptr + sizeof(pthread_mutex_t) + sizeof(int);
}

int*
store_next (void *ptr)
{
  return ptr + sizeof(pthread_mutex_t) + sizeof(int) + sizeof(int);
}

int*
store_links (void *ptr)
{
  return ptr + sizeof(pthread_mutex_t) + sizeof(int) + sizeof(int) + sizeof(int);
}

int*
store_lengths (void *ptr)
{
  int array = (*(store_count(ptr)) * sizeof(int));
  return ptr + sizeof(pthread_mutex_t) + sizeof(int) + sizeof(int) + sizeof(int) + array;
}

void*
store_data (void *ptr)
{
  int array = (*(store_count(ptr)) * sizeof(int));
  return ptr + sizeof(pthread_mutex_t) + sizeof(int) + sizeof(int) + sizeof(int) + array + array;
}

void*
store_slot (void *ptr, int id)
{
  int chunk = *(store_chunk(ptr));
  void *data = store_data(ptr);
  return data + (id * chunk);
}

int
store_size (void *ptr)
{
  int chunk = *(store_chunk(ptr));
  int count = *(store_count(ptr));
  return
    sizeof(pthread_mutex_t)
    + sizeof(int) // chunk
    + sizeof(int) // count
    + sizeof(int) // free list
    + (sizeof(int) * count) // links
    + (sizeof(int) * count) // lengths
    + (chunk * count) // data
  ;
}

void*
store_create (const char *name, int chunk, int count)
{
  int size =
    sizeof(pthread_mutex_t)
    + sizeof(int) // chunk
    + sizeof(int) // count
    + sizeof(int) // free list
    + (sizeof(int) * count) // links
    + (sizeof(int) * count) // lengths
    + (chunk * count) // data
  ;

  char ipcname[100];
  snprintf(ipcname, sizeof(ipcname), "/slua_%d_%s", getpid(), name);

  int fd = shm_open(ipcname, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  if (fd < 0 || ftruncate(fd, size) != 0) return NULL;

  void *ptr = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (ptr == MAP_FAILED) return NULL;

  close(fd);

  pthread_mutexattr_t mutexattr;
  ensure(pthread_mutexattr_init(&mutexattr) == 0);
  ensure(pthread_mutexattr_setpshared(&mutexattr, PTHREAD_PROCESS_SHARED) == 0);
  ensure(pthread_mutex_init(store_mutex(ptr), &mutexattr) == 0);
  pthread_mutexattr_destroy(&mutexattr);

  *(store_chunk(ptr)) = chunk;
  *(store_count(ptr)) = count;
  *(store_next(ptr))  = 0;

  int *links = store_links(ptr);

  for (int i = 0; i < count; i++)
    links[i] = i+1;

  links[count-1] = -1;

  int *lengths = store_lengths(ptr);

  for (int i = 0; i < count; i++)
    lengths[i] = 0;

  return ptr;
}

void
store_destroy (const char *name, void *ptr)
{
  char ipcname[100];
  snprintf(ipcname, sizeof(ipcname), "/slua_%d_%s", getpid(), name);
  shm_unlink(ipcname);
}

int
store_set (void *ptr, void *src, int len)
{
  ensure(pthread_mutex_lock(store_mutex(ptr)) == 0);

  int chunk = *(store_chunk(ptr));
  int next = *(store_next(ptr));
  int *links = store_links(ptr);
  int *lengths = store_lengths(ptr);

  int id = next;

  ensure(id >= 0) errorf("store(%d) full", chunk);

  int slot = next;
  int bytes = chunk;

  while (bytes < len)
  {
    slot = links[slot];
    ensure (slot >= 0) errorf("store(%d) full", chunk);
    bytes += chunk;
  }

  slot = next;
  bytes = min(chunk, len);
  if (src)
  {
    memmove(store_slot(ptr, slot), src, bytes);
    src += bytes;
  }
  len -= bytes;
  lengths[slot] = bytes;

  while (len > 0)
  {
    slot = links[slot];
    bytes = min(chunk, len);
    if (src)
    {
      memmove(store_slot(ptr, slot), src, bytes);
      src += bytes;
    }
    len -= bytes;
    lengths[slot] = bytes;
  }

  next = links[slot];
  links[slot] = -1;

  *(store_next(ptr)) = next;

  ensure(pthread_mutex_unlock(store_mutex(ptr)) == 0);
  return id;
}

void*
store_get (void *ptr, int id, int remove)
{
  ensure(pthread_mutex_lock(store_mutex(ptr)) == 0);

  int *links = store_links(ptr);
  int *lengths = store_lengths(ptr);

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
  memmove(dst, store_slot(ptr, slot), len);

  while (links[slot] > -1)
  {
    slot = links[slot];
    memmove(dst + len, store_slot(ptr, slot), lengths[slot]);
    len += lengths[slot];
  }

  if (remove)
  {
    int slot = id;
    do {
      int next = links[slot];
      links[slot] = *(store_next(ptr));
      *(store_next(ptr)) = slot;
      slot = next;
    }
    while (slot > -1);
  }

  ensure(pthread_mutex_unlock(store_mutex(ptr)) == 0);
  return dst;
}

void*
store_alloc (void *ptr, int len)
{
  return store_slot(ptr, store_set(ptr, NULL, len));
}

void
store_free (void *ptr, void *ptr2)
{
  int chunk = *(store_chunk(ptr));
  int id = (ptr2 - store_slot(ptr, 0)) / chunk;
  free(store_get(ptr, id, 1));
}
