
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
#include <signal.h>

#define min(a,b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a: _b; })
#define max(a,b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a: _b; })

#define ensure(x) for ( ; !(x) ; exit(EXIT_FAILURE) )

static pthread_key_t key_self;
#define hself ((handler_t*)pthread_getspecific(key_self))
#define wself ((worker_t*)pthread_getspecific(key_self))

pthread_mutex_t stdout_mutex;
pthread_mutex_t stderr_mutex;

#define errorf(...) ({ \
  pthread_mutex_lock(&stderr_mutex); \
  fprintf(stderr, "%lx: ", (uint64_t)pthread_getspecific(key_self)); \
  fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); \
  pthread_mutex_unlock(&stderr_mutex); \
})

#define str_eq(a,b) (strcmp((a),(b)) == 0)

typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t cond_read;
  pthread_cond_t cond_write;
  void **queue;
  size_t cells;
  size_t backlog;
  size_t readers;
  size_t writers;
  int used;
} channel_t;

void
channel_init (channel_t *channel, size_t cells)
{
  channel->used = 1;
  channel->backlog = 0;
  channel->readers = 0;
  channel->writers = 0;
  channel->cells = cells;
  channel->queue = calloc(channel->cells, sizeof(void*));
  pthread_mutex_init(&channel->mutex, NULL);
  pthread_cond_init(&channel->cond_read, NULL);
  pthread_cond_init(&channel->cond_write, NULL);
}

void
channel_free (channel_t *channel)
{
  if (channel->used)
  {
    pthread_mutex_destroy(&channel->mutex);
    pthread_cond_destroy(&channel->cond_read);
    pthread_cond_destroy(&channel->cond_write);
    free(channel->queue);
    channel->used = 0;
  }
}

size_t
channel_backlog (channel_t *channel)
{
  pthread_mutex_lock(&channel->mutex);
  size_t backlog = channel->backlog;
  pthread_mutex_unlock(&channel->mutex);
  return backlog;
}

size_t
channel_readers (channel_t *channel)
{
  pthread_mutex_lock(&channel->mutex);
  size_t readers = channel->readers;
  pthread_mutex_unlock(&channel->mutex);
  return readers;
}

size_t
channel_writers (channel_t *channel)
{
  pthread_mutex_lock(&channel->mutex);
  size_t writers = channel->writers;
  pthread_mutex_unlock(&channel->mutex);
  return writers;
}

void*
channel_read (channel_t *channel)
{
  pthread_mutex_lock(&channel->mutex);
  channel->readers++;

  while (!channel->backlog)
  {
    pthread_cond_signal(&channel->cond_write);
    pthread_cond_wait(&channel->cond_read, &channel->mutex);
  }

  void *msg = channel->queue[0];
  memmove(&channel->queue[0], &channel->queue[1], sizeof(void*) * (channel->cells-1));
  channel->backlog--;

  channel->readers--;
  pthread_mutex_unlock(&channel->mutex);
  return msg;
}

void
channel_write (channel_t *channel, void *msg)
{
  pthread_mutex_lock(&channel->mutex);
  channel->writers++;

  while (channel->backlog == channel->cells-1)
    pthread_cond_wait(&channel->cond_write, &channel->mutex);

  channel->queue[channel->backlog++] = msg;
  pthread_cond_signal(&channel->cond_read);

  channel->writers--;
  pthread_mutex_unlock(&channel->mutex);
}

typedef struct {
  int tcp_port;
  int tcp_backlog;
  size_t max_workers;
  const char *worker_path;
  size_t max_handlers;
  const char *handler_path;
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
  channel_t *result;
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
  channel_t results;
} handler_t;

handler_t *handlers;

typedef struct {
  char *payload;
  channel_t *result;
} message_t;

typedef struct {
  int io;
} request_t;

channel_t jobs, reqs;

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
  message_t *message = channel_read(&jobs);
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
  ensure(cfg.worker_path)
    errorf("no workers");

  message_t *message = malloc(sizeof(message_t));
  message->result = &hself->results;
  char *payload = (char*)lua_popstring(lua);
  message->payload = strdup(payload);
  channel_write(&jobs, message);
  return 0;
}

int
job_collect (lua_State *lua)
{
  ensure(cfg.worker_path)
    errorf("no workers");

  char *payload = channel_read(&hself->results);
  lua_pushstring(lua, payload);
  free(payload);
  return 1;
}

int
close_io (lua_State *lua)
{
  if (hself->fio)
  {
    fclose(hself->fio);
    hself->fio = NULL;
    hself->io = 0;
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

  errorf("worker");

  ensure((worker->lua = luaL_newstate()))
    errorf("luaL_newstate failed");

  luaL_openlibs(worker->lua);

  worker->rc = EXIT_SUCCESS;

  lua_createtable(worker->lua, 0, 0);

  lua_pushstring(worker->lua, "accept");
  lua_pushcfunction(worker->lua, job_accept);
  lua_settable(worker->lua, -3);

  lua_pushstring(worker->lua, "respond");
  lua_pushcfunction(worker->lua, job_respond);
  lua_settable(worker->lua, -3);

  lua_setglobal(worker->lua, "job");

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
main_handler (void *ptr)
{
  handler_t *handler = ptr;
  pthread_mutex_lock(&handler->mutex);

  ensure(pthread_setspecific(key_self, handler) == 0)
    errorf("pthread_setspecific failed");

  errorf("handler");

  channel_init(&handler->results, 10);

  request_t *request = NULL;

  while ((request = channel_read(&reqs)))
  {
    handler->io = request->io;
    free(request);

    ensure((handler->lua = luaL_newstate()))
      errorf("luaL_newstate failed");

    luaL_openlibs(handler->lua);

    handler->fio = fdopen(handler->io, "r+");

    ensure(handler->fio)
      errorf("fdopen failed");

  #ifdef LUA51
    FILE **fio = lua_newuserdata(handler->lua, sizeof(FILE*));
    *fio = handler->fio;
    luaL_getmetatable(handler->lua, LUA_FILEHANDLE);
    lua_setmetatable(handler->lua, -2);
    lua_createtable(handler->lua, 0, 1);
    lua_pushcfunction(handler->lua, close_io);
    lua_setfield(handler->lua, -2, "__close");
    lua_setfenv(handler->lua, -2);
    lua_setglobal(handler->lua, "sock");
  #endif

  #ifdef LUA52
    luaL_Stream *s = lua_newuserdata(handler->lua, sizeof(luaL_Stream));
    s->f = handler->fio;
    s->closef = close_io;
    luaL_setmetatable(handler->lua, LUA_FILEHANDLE);
    lua_setglobal(handler->lua, "sock");
  #endif

    handler->rc = EXIT_SUCCESS;

    lua_createtable(handler->lua, 0, 0);

    lua_pushstring(handler->lua, "submit");
    lua_pushcfunction(handler->lua, job_submit);
    lua_settable(handler->lua, -3);

    lua_pushstring(handler->lua, "collect");
    lua_pushcfunction(handler->lua, job_collect);
    lua_settable(handler->lua, -3);

    lua_setglobal(handler->lua, "job");

    if (luaL_dofile(handler->lua, cfg.handler_path) != 0)
    {
      errorf("lua error: %s", lua_tostring(handler->lua, -1));
      handler->rc = EXIT_FAILURE;
    }

    close_io(handler->lua);
    lua_close(handler->lua);
  }

  channel_free(&handler->results);

  handler->done = 1;
  pthread_mutex_unlock(&handler->mutex);

  return NULL;
}

void
stop()
{
  exit(EXIT_SUCCESS);
}

void
sig_int(int sig)
{
  errorf("SIGINT");
  stop();
}

void
sig_term(int sig)
{
  errorf("SIGTERM");
  stop();
}

void
start(int argc, const char *argv[])
{
  long cores = sysconf(_SC_NPROCESSORS_ONLN);

  cfg.tcp_port     = 80;
  cfg.tcp_backlog  = 32;
  cfg.max_workers  = cores;
  cfg.max_handlers = cores*2;
  cfg.handler_path = NULL;
  cfg.worker_path  = NULL;
  cfg.setuid_name  = NULL;

  signal(SIGINT,  sig_int);
  signal(SIGTERM, sig_term);
  signal(SIGPIPE, SIG_IGN);

  ensure(pthread_mutex_init(&stdout_mutex, NULL) == 0);
  ensure(pthread_mutex_init(&stderr_mutex, NULL) == 0);

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
    if (str_eq(argv[argi], "-h") || str_eq(argv[argi], "--handler"))
    {
      ensure(argi < argc-1) errorf("expected (-h|--handler) <value>");
      cfg.handler_path = argv[++argi];
      continue;
    }
    if (str_eq(argv[argi], "-mr") || str_eq(argv[argi], "--max-handlers"))
    {
      ensure(argi < argc-1) errorf("expected (-mr|--max-handlers) <value>");
      cfg.max_handlers = strtol(argv[++argi], NULL, 0);
      continue;
    }

    cfg.handler_path = argv[argi];
  }

  ensure(cfg.handler_path)
    errorf("expected script");

  ensure(pthread_key_create(&key_self, NULL) == 0)
    errorf("pthread_key_create failed");

  size_t handlers_bytes = sizeof(handler_t) * cfg.max_handlers;

  ensure(handlers = malloc(handlers_bytes))
    errorf("malloc failed %lu", handlers_bytes);

  memset(handlers, 0, handlers_bytes);

  for (size_t ri = 0; ri < cfg.max_handlers; ri++)
  {
    ensure(pthread_mutex_init(&handlers[ri].mutex, NULL) == 0)
      errorf("pthread_mutex_init failed");

    handler_t *handler = &handlers[ri];

    handler->active = 1;

    ensure(pthread_create(&handler->thread, NULL, main_handler, handler) == 0)
      errorf("pthread_create failed");
  }

  channel_init(&reqs, 100);

  if (cfg.worker_path)
  {
    size_t workers_bytes = sizeof(handler_t) * cfg.max_workers;

    ensure(workers = malloc(workers_bytes))
      errorf("malloc failed %lu", workers_bytes);

    memset(workers, 0, workers_bytes);

    for (size_t wi = 0; wi < cfg.max_workers; wi++)
    {
      ensure(pthread_mutex_init(&workers[wi].mutex, NULL) == 0)
        errorf("pthread_mutex_init failed");

      worker_t *worker = &workers[wi];

      worker->active = 1;

      ensure(pthread_create(&worker->thread, NULL, main_worker, worker) == 0)
        errorf("pthread_create failed");
    }

    channel_init(&jobs, 100);
  }
}

int
main(int argc, char const *argv[])
{
  start(argc, argv);

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
    request_t *request = malloc(sizeof(request_t));
    request->io = fd;

    channel_write(&reqs, request);
  }

  return EXIT_FAILURE;
}
