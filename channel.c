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

void
channel_init (channel_t *channel, size_t limit)
{
  channel->used = 1;
  channel->backlog = 0;
  channel->readers = 0;
  channel->writers = 0;
  channel->limit = limit;
  channel->list = NULL;
  channel->last = NULL;
  ensure(pthread_mutex_init(&channel->mutex, &global.mutexattr) == 0);
  ensure(pthread_cond_init(&channel->cond_read, &global.condattr) == 0);
  ensure(pthread_cond_init(&channel->cond_write, &global.condattr) == 0);
}

void
channel_free (channel_t *channel)
{
  if (channel->used)
  {
    while (channel->list)
    {
      channel_node_t *node = channel->list;
      channel->list = node->next;
      store_free(global.store, node);
    }
    pthread_mutex_destroy(&channel->mutex);
    pthread_cond_destroy(&channel->cond_read);
    pthread_cond_destroy(&channel->cond_write);
    store_free(global.store, channel->list);
    channel->used = 0;
  }
}

void*
channel_read (channel_t *channel)
{
  ensure(pthread_mutex_lock(&channel->mutex) == 0);
  channel->readers++;

  while (channel->backlog == 0)
    ensure(pthread_cond_wait(&channel->cond_read, &channel->mutex) == 0);

  channel->backlog--;

  channel_node_t *node = channel->list;
  channel->list = node->next;

  if (node == channel->last)
    channel->last = NULL;

  if (channel->writers)
    ensure(pthread_cond_signal(&channel->cond_write) == 0);

  channel->readers--;
  ensure(pthread_mutex_unlock(&channel->mutex) == 0);

  void *msg = malloc(node->length);
  memmove(msg, node->payload, node->length);

  store_free(global.store, node);
  return msg;
}

void
channel_write (channel_t *channel, void *msg, size_t len)
{
  channel_node_t *node = store_alloc(global.store, sizeof(channel_node_t) + len);
  node->next = NULL;
  node->length = len;
  memmove(node->payload, msg, len);

  ensure(pthread_mutex_lock(&channel->mutex) == 0);
  channel->writers++;

  while (channel->limit > 0 && channel->backlog == channel->limit)
    ensure(pthread_cond_wait(&channel->cond_write, &channel->mutex) == 0);

  channel->backlog++;

  if (!channel->list)
  {
    channel->list = node;
    channel->last = node;
  }
  else
  {
    channel->last->next = node;
    channel->last = node;
  }

  if (channel->readers)
    ensure(pthread_cond_signal(&channel->cond_read) == 0);

  channel->writers--;
  ensure(pthread_mutex_unlock(&channel->mutex) == 0);
}

void
channel_counters (channel_t *channel, size_t *limit, size_t *backlog, size_t *readers, size_t *writers)
{
  ensure(pthread_mutex_lock(&channel->mutex) == 0);
  if (limit)   *limit   = channel->limit;
  if (backlog) *backlog = channel->backlog;
  if (readers) *readers = channel->readers;
  if (writers) *writers = channel->writers;
  ensure(pthread_mutex_unlock(&channel->mutex) == 0);
}
