
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <regex.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <pwd.h>

#define ensure(x) for ( ; !(x) ; exit(EXIT_FAILURE) )
#define errorf(...) ({ fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); })

#define str_eq(a,b) (strcmp((a),(b)) == 0)

#define CHANNEL_CELLS 10

typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t cond_read;
  pthread_cond_t cond_write;
  void *queue[CHANNEL_CELLS];
  size_t backlog;
  size_t readers;
  size_t writers;
  int used;
} channel_t;

#define MAX_CHANNELS 1000
channel_t channels[MAX_CHANNELS];
int channel_count = 1;

int
channel_new ()
{
  int id = 0;

  for (int i = 1; i < channel_count; i++)
  {
    channel_t *channel = &channels[i];

    if (!channel->used)
    {
      id = i;
      break;
    }
  }

  if (!id)
    id = channel_count++;

  channel_t *channel = &channels[id];

  channel->used = 1;
  channel->backlog = 0;
  channel->readers = 0;
  channel->writers = 0;
  pthread_mutex_init(&channel->mutex, NULL);
  pthread_cond_init(&channel->cond_read, NULL);
  pthread_cond_init(&channel->cond_write, NULL);

  return id;
}

void
channel_free (int id)
{
  channel_t *channel = &channels[id];
  if (channel->used)
  {
    pthread_mutex_destroy(&channel->mutex);
    pthread_cond_destroy(&channel->cond_read);
    pthread_cond_destroy(&channel->cond_write);
    channel->used = 0;
  }
}

size_t
channel_backlog (int id)
{
  channel_t *channel = &channels[id];
  pthread_mutex_lock(&channel->mutex);
  size_t backlog = channel->backlog;
  pthread_mutex_unlock(&channel->mutex);
  return backlog;
}

size_t
channel_readers (int id)
{
  channel_t *channel = &channels[id];
  pthread_mutex_lock(&channel->mutex);
  size_t readers = channel->readers;
  pthread_mutex_unlock(&channel->mutex);
  return readers;
}

size_t
channel_writers (int id)
{
  channel_t *channel = &channels[id];
  pthread_mutex_lock(&channel->mutex);
  size_t writers = channel->writers;
  pthread_mutex_unlock(&channel->mutex);
  return writers;
}

void*
channel_read (int id)
{
  channel_t *channel = &channels[id];
  pthread_mutex_lock(&channel->mutex);
  channel->readers++;

  while (!channel->backlog)
  {
    pthread_cond_signal(&channel->cond_write);
    pthread_cond_wait(&channel->cond_read, &channel->mutex);
  }

  void *msg = channel->queue[0];
  memmove(&channel->queue[0], &channel->queue[1], sizeof(void*) * (CHANNEL_CELLS-1));
  channel->backlog--;

  channel->readers--;
  pthread_mutex_unlock(&channel->mutex);
  return msg;
}

void
channel_write (int id, void *msg)
{
  channel_t *channel = &channels[id];
  pthread_mutex_lock(&channel->mutex);
  channel->writers++;

  while (channel->backlog == CHANNEL_CELLS-1)
    pthread_cond_wait(&channel->cond_write, &channel->mutex);

  channel->queue[channel->backlog++] = msg;
  pthread_cond_signal(&channel->cond_read);

  channel->writers--;
  pthread_mutex_unlock(&channel->mutex);
}

typedef struct {
  int tcp_port;
  size_t max_workers;
  const char *worker_path;
  size_t max_requests;
  const char *request_path;
  const char *setuid_name;
} config_t;

config_t cfg;

typedef struct {
  pthread_mutex_t mutex;
  int active;
  int done;
  int rc;
  pthread_t thread;
  lua_State *lua;
  int result;
} worker_t;

worker_t *workers;

typedef struct {
  pthread_mutex_t mutex;
  int active;
  int done;
  int rc;
  int io;
  FILE *fio;
  pthread_t thread;
  lua_State *lua;
  int results;
} request_t;

request_t *requests;

typedef struct {
  char *payload;
  int result;
} message_t;

static pthread_key_t key_self;
#define rself ((request_t*)pthread_getspecific(key_self))
#define wself ((worker_t*)pthread_getspecific(key_self))

int jobs;

const char*
lua_popstring (lua_State *lua)
{
  const char *str = lua_tostring(lua, -1);
  lua_pop(lua, 1);
  return str;
}

int
job_accept (lua_State *lua)
{
  message_t *message = channel_read(jobs);
  lua_pushstring(lua, message->payload);
  wself->result = message->result;
  free(message->payload);
  free(message);
  return 1;
}

int
job_respond (lua_State *lua)
{
  char *payload = (char*)lua_popstring(lua);
  channel_write(wself->result, strdup(payload));
  return 0;
}

int
job_submit (lua_State *lua)
{
  message_t *message = malloc(sizeof(message_t));
  message->result = rself->results;
  char *payload = (char*)lua_popstring(lua);
  message->payload = strdup(payload);
  channel_write(jobs, message);
  return 0;
}

int
job_receive (lua_State *lua)
{
  char *payload = channel_read(rself->results);
  lua_pushstring(lua, payload);
  free(payload);
  return 1;
}

int
close_io (lua_State *lua)
{
  if (rself->fio)
  {
    fclose(rself->fio);
    rself->fio = NULL;
    rself->io = 0;
  }
  return 0;
}

void*
main_worker (void *ptr)
{
  worker_t *worker = ptr;
  pthread_mutex_lock(&worker->mutex);

  ensure(pthread_setspecific(key_self, worker) == 0)
    errorf("pthread_setspecific failed");

  ensure((worker->lua = luaL_newstate()))
    errorf("luaL_newstate failed");

  luaL_openlibs(worker->lua);

  worker->rc = EXIT_SUCCESS;

  lua_pushcfunction(worker->lua, job_accept);
  lua_setglobal(worker->lua, "job_accept");
  lua_pushcfunction(worker->lua, job_respond);
  lua_setglobal(worker->lua, "job_respond");

  if (luaL_dofile(worker->lua, cfg.worker_path) != 0)
  {
    errorf("lua error: %s", lua_tostring(worker->lua, -1));
    worker->rc = EXIT_FAILURE;
  }

  lua_close(worker->lua);

  worker->done = 1;
  pthread_mutex_unlock(&worker->mutex);

  return NULL;
}

void*
main_request (void *ptr)
{
  request_t *request = ptr;
  pthread_mutex_lock(&request->mutex);

  ensure(pthread_setspecific(key_self, request) == 0)
    errorf("pthread_setspecific failed");

  ensure((request->lua = luaL_newstate()))
    errorf("luaL_newstate failed");

  luaL_openlibs(request->lua);

  request->fio = fdopen(request->io, "r+");

  ensure(request->fio)
    errorf("fdopen failed");

  request->results = channel_new();

#ifdef LUA51
  FILE **fio = lua_newuserdata(request->lua, sizeof(FILE*));
  *fio = request->fio;
  luaL_getmetatable(request->lua, LUA_FILEHANDLE);
  lua_setmetatable(request->lua, -2);
  lua_createtable(request->lua, 0, 1);
  lua_pushcfunction(request->lua, close_io);
  lua_setfield(request->lua, -2, "__close");
  lua_setfenv(request->lua, -2);
  lua_setglobal(request->lua, "sock");
#endif

#ifdef LUA52
  luaL_Stream *s = lua_newuserdata(request->lua, sizeof(luaL_Stream));
  s->f = request->fio;
  s->closef = close_io;
  luaL_setmetatable(request->lua, LUA_FILEHANDLE);
  lua_setglobal(request->lua, "sock");
#endif

  request->rc = EXIT_SUCCESS;

  lua_pushcfunction(request->lua, job_submit);
  lua_setglobal(request->lua, "job_submit");
  lua_pushcfunction(request->lua, job_receive);
  lua_setglobal(request->lua, "job_receive");

  if (luaL_dofile(request->lua, cfg.request_path) != 0)
  {
    errorf("lua error: %s", lua_tostring(request->lua, -1));
    request->rc = EXIT_FAILURE;
  }

  close_io(request->lua);
  lua_close(request->lua);
  channel_free(request->results);

  request->done = 1;
  pthread_mutex_unlock(&request->mutex);

  return NULL;
}

int
main(int argc, char const *argv[])
{
  cfg.tcp_port = 80;
  cfg.max_workers = 10;
  cfg.max_requests = 100;
  cfg.request_path = NULL;
  cfg.worker_path = NULL;
  cfg.setuid_name = NULL;

  for (int argi = 1; argi < argc; argi++)
  {
    if (str_eq(argv[argi], "-p") || str_eq(argv[argi], "--port"))
    {
      ensure(argi < argc-1) errorf("expected (-p|--port) <value>");
      cfg.tcp_port = strtol(argv[++argi], NULL, 0);
      continue;
    }
    if (str_eq(argv[argi], "-u") || str_eq(argv[argi], "--user"))
    {
      ensure(argi < argc-1) errorf("expected (-u|--user) <value>");
      cfg.setuid_name = argv[++argi];
      continue;
    }
    if (str_eq(argv[argi], "-w") || str_eq(argv[argi], "--worker"))
    {
      ensure(argi < argc-1) errorf("expected (-w|--worker) <value>");
      cfg.worker_path = argv[++argi];
      continue;
    }
    if (str_eq(argv[argi], "-mw") || str_eq(argv[argi], "--max-workers"))
    {
      ensure(argi < argc-1) errorf("expected (-mw|--max-workers) <value>");
      cfg.max_workers = strtol(argv[++argi], NULL, 0);
      continue;
    }
    if (str_eq(argv[argi], "-r") || str_eq(argv[argi], "--request"))
    {
      ensure(argi < argc-1) errorf("expected (-r|--request) <value>");
      cfg.request_path = argv[++argi];
      continue;
    }
    if (str_eq(argv[argi], "-mr") || str_eq(argv[argi], "--max-requests"))
    {
      ensure(argi < argc-1) errorf("expected (-mr|--max-requests) <value>");
      cfg.max_requests = strtol(argv[++argi], NULL, 0);
      continue;
    }

    cfg.request_path = argv[argi];
  }

  ensure(cfg.request_path)
    errorf("expected script");

  ensure(pthread_key_create(&key_self, NULL) == 0)
    errorf("pthread_key_create failed");

  size_t requests_bytes = sizeof(request_t) * cfg.max_requests;

  ensure(requests = malloc(requests_bytes))
    errorf("malloc failed %lu", requests_bytes);

  memset(requests, 0, requests_bytes);

  for (size_t ri = 0; ri < cfg.max_requests; ri++)
  {
    ensure(pthread_mutex_init(&requests[ri].mutex, NULL) == 0)
      errorf("pthread_mutex_init failed");
  }

  if (cfg.worker_path)
  {
    size_t workers_bytes = sizeof(request_t) * cfg.max_workers;

    ensure(workers = malloc(workers_bytes))
      errorf("malloc failed %lu", workers_bytes);

    memset(workers, 0, workers_bytes);

    for (size_t wi = 0; wi < cfg.max_requests; wi++)
    {
      ensure(pthread_mutex_init(&workers[wi].mutex, NULL) == 0)
        errorf("pthread_mutex_init failed");

      worker_t *worker = &workers[wi];

      worker->active = 1;

      ensure(pthread_create(&worker->thread, NULL, main_worker, worker) == 0)
        errorf("pthread_create failed");
    }

    jobs = channel_new(0);
  }

  int sock_fd = socket(AF_INET, SOCK_STREAM, 0);

  ensure(sock_fd >= 0)
    errorf("socket failed");

  int enable = 1;

  ensure(setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) == 0)
    errorf("setsockopt(SO_REUSEADDR) failed");

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));

  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(cfg.tcp_port);

  ensure(bind(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0)
    errorf("bind failed");

  ensure(listen(sock_fd, 32) == 0)
    errorf("listen failed");

  if (cfg.setuid_name)
  {
    struct passwd *pw = getpwnam(cfg.setuid_name);

    ensure(pw && setuid(pw->pw_uid) == 0)
      errorf("setuid %s failed", cfg.setuid_name);
  }

  int fd;

  while ((fd = accept(sock_fd, NULL, NULL)) && fd >= 0)
  {
    request_t *request = NULL;

    while (!request)
    {
      for (int reqi = 0; !request && reqi < cfg.max_requests; reqi++)
      {
        request_t *req = &requests[reqi];

        if (pthread_mutex_trylock(&req->mutex) != 0)
          continue;

        if (req->active && req->done)
        {
          ensure(pthread_join(req->thread, NULL) == 0)
            errorf("pthread_join failed");

          request = req;
        }
        else
        if (!req->active)
        {
          request = req;
        }
        else
        {
          pthread_mutex_unlock(&req->mutex);
        }
      }

      if (!request)
      {
        errorf("hit max_requests");
        usleep(10000);
      }
    }

    memset(request, 0, sizeof(request_t));

    request->io = fd;
    request->active = 1;

    ensure(pthread_create(&request->thread, NULL, main_request, request) == 0)
      errorf("pthread_create failed");

    pthread_mutex_unlock(&request->mutex);
  }

  close(sock_fd);

  return EXIT_SUCCESS;
}
