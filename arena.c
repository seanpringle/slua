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
  unsigned int bytes;
  unsigned int pages;
  unsigned int page_size;
  unsigned int start_scan;
  unsigned char flags[];
} arena_t;

static void*
arena_data (arena_t *arena)
{
  size_t offset = sizeof(arena_t) + arena->pages;
  return (void*)arena + offset + (offset % sizeof(void*) ? (sizeof(void*) - (offset % sizeof(void*))): 0); // aligned
}

static void*
arena_page (arena_t *arena, unsigned int page_id)
{
  return arena_data(arena) + (page_id * arena->page_size);
}

int
arena_open (void *pool, unsigned int bytes, unsigned int page_size)
{
  arena_t *arena = pool;
  memset(pool, 0, bytes);

  arena->bytes = bytes;
  arena->pages = bytes / page_size;
  arena->page_size = page_size;
  arena->start_scan = 0;

  while (arena->pages > 0 && (arena_data(arena) + (arena->pages * page_size)) - pool > bytes)
    arena->pages--;

  return 0;
}

int
arena_close (void *pool)
{
  memset(pool, 0, ((arena_t*)pool)->bytes);
  return 0;
}

void*
arena_alloc (void *pool, unsigned int bytes)
{
  void *ptr = NULL;

  arena_t *arena = pool;

  unsigned int pages = (bytes / arena->page_size) + (bytes % arena->page_size ? 1:0);
  unsigned int gap = 0;

  unsigned char *flags = &arena->flags[arena->start_scan];
  unsigned char *limit = arena->flags + arena->pages;

  while (flags < limit && (flags = memchr(flags, 0, limit - flags)) && flags && limit - flags >= pages)
  {
    for (gap = 1;
      gap && gap < pages;
      gap = flags[gap] ? 0: gap+1
    );

    if (gap == pages)
    {
      unsigned int page_id = flags - arena->flags;
      memset(&arena->flags[page_id], 1, pages);

      arena->flags[page_id+(pages-1)] = 3;
      ptr = arena_page(arena, page_id);

      if (page_id == arena->start_scan)
        arena->start_scan = page_id + pages;

      break;
    }
    flags++;
  }

  return ptr;
}

int
arena_free (void *pool, void *ptr)
{
  arena_t *arena = pool;
  if (!ptr) return 0;

  // address out of bounds
  if (ptr < pool || ptr > pool + arena->bytes) return 1;

  int page_id = (ptr - arena_data(arena)) / arena->page_size;

  // invalid address (not on a page boundary)
  if ((ptr - arena_data(arena)) % arena->page_size) return 2;

  if (page_id < arena->start_scan)
    arena->start_scan = page_id;

  int last_page = 0;

  while (!last_page && page_id < arena->pages)
  {
    last_page = arena->flags[page_id] & 2;
    arena->flags[page_id++] = 0;
  }
  return 0;
}
