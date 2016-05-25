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
#include <math.h>
#include <sqlite3.h>
#include <errno.h>
#include <time.h>

#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#define min(a,b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a: _b; })
#define max(a,b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a: _b; })

#define ensure(x) for ( ; !(x) ; exit(EXIT_FAILURE) )

static pthread_key_t key_self;
#define self ((thread_t*)pthread_getspecific(key_self))

pthread_mutex_t stdout_mutex;
pthread_mutex_t stderr_mutex;

#define outputf(...) ({ \
  pthread_mutex_lock(&stdout_mutex); \
  fprintf(stdout, __VA_ARGS__); fputc('\n', stdout); \
  pthread_mutex_unlock(&stdout_mutex); \
})

#define errorf(...) ({ \
  pthread_mutex_lock(&stderr_mutex); \
  fprintf(stderr, "%lx: ", (uint64_t)pthread_getspecific(key_self)); \
  fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); \
  pthread_mutex_unlock(&stderr_mutex); \
})

const char*
lua_popstring (lua_State *lua)
{
  const char *str = lua_tostring(lua, -1);
  lua_pop(lua, 1);
  return str;
}

double
lua_popnumber (lua_State *lua)
{
  double n = lua_tonumber(lua, -1);
  lua_pop(lua, 1);
  return n;
}

#include "str.c"
#include "channel.c"
#include "posix.c"
#include "json.c"
#include "pcre.c"

#define HANDLER 1
#define WORKER 2

#define MODE_TCP 1
#define MODE_STDIN 2

typedef struct {
  int mode;
  int tcp_port;
  int tcp_backlog;
  size_t max_workers;
  const char *worker_path;
  const char *worker_code;
  size_t max_handlers;
  const char *handler_path;
  const char *handler_code;
  const char *setuid_name;
  size_t max_jobs;
  size_t max_results;
  const char *db_path;
  const char *ssl_cert;
  const char *ssl_key;
} config_t;

config_t cfg;

typedef struct {
  char *payload;
  channel_t *result;
} message_t;

typedef struct {
  int io;
  char *ipv4;
  SSL *ssl;
  SSL_CTX *ssl_ctx;
} request_t;

typedef struct {
  int type;
  pthread_mutex_t mutex;
  int active;
  int done;
  int rc;
  request_t *request;
  pthread_t thread;
  lua_State *lua;
  channel_t results;
  channel_t *result;
  sqlite3 *db;
} thread_t;

thread_t *workers;
thread_t *handlers;

channel_t jobs, reqs, stuff;
pthread_cond_t all_workers_idle;

#include "work.c"
#include "db.c"

int
safe_print (lua_State *lua)
{
  ensure(pthread_mutex_lock(&stdout_mutex) == 0);
  int args = lua_gettop(lua);
  for (int i = 0; i < args; i++)
    printf("%s", lua_tostring(lua, i+1));
  printf("\n");
  lua_pop(lua, args);
  ensure(pthread_mutex_unlock(&stdout_mutex) == 0);
  return 0;
}

int
safe_error (lua_State *lua)
{
  ensure(pthread_mutex_lock(&stderr_mutex) == 0);
  int args = lua_gettop(lua);
  for (int i = 0; i < args; i++)
    fprintf(stderr, "%s", lua_tostring(lua, i+1));
  fprintf(stderr, "\n");
  lua_pop(lua, args);
  ensure(pthread_mutex_unlock(&stderr_mutex) == 0);
  return 0;
}

int
request_read (lua_State *lua)
{
  int ok = 1;
  size_t length = 0;
  size_t limit = lua_popnumber(lua);
  char *buffer = malloc(limit+1);

  while (length < limit)
  {
    size_t bytes = self->request->ssl
      ? SSL_read(self->request->ssl,  buffer + length, limit - length)
      : read(self->request->io, buffer + length, limit - length);

    if (bytes <= 0) { ok = 0; break; }
    length += bytes;
  }

  buffer[length] = 0;
  if (ok) lua_pushstring(lua, buffer); else lua_pushnil(lua);

  free(buffer);
  return 1;
}

int
request_read_line (lua_State *lua)
{
  lua_pushliteral(lua, "");

  int ok = 1;
  char c[2];

  while (1)
  {
    c[0] = 0;
    c[1] = 0;

    size_t bytes = self->request->ssl
      ? SSL_read(self->request->ssl, c, 1)
      : read(self->request->io, c, 1);

    if (bytes <= 0) { ok = 0; break; }
    if (c[0] == '\n') break;
    c[1] = 0;

    lua_pushstring(lua, c);
    lua_concat(lua, 2);
  }

  if (!ok)
  {
    lua_pop(lua, 1);
    lua_pushnil(lua);
  }

  return 1;
}

int
request_write (lua_State *lua)
{
  size_t bytes = 0;
  if (lua_type(lua, -1) == LUA_TSTRING)
  {
    const char *str = lua_popstring(lua);
    if (strlen(str))
    {
      bytes = self->request->ssl
        ? SSL_write(self->request->ssl, str, strlen(str))
        : write(self->request->io, str, strlen(str));
    }
  }
  if (bytes < 0)
    lua_pushnil(lua);
  else
    lua_pushnumber(lua, bytes);
  return 1;
}

struct function_map {
  const char *table;
  const char *name;
  lua_CFunction func;
};

struct function_map registry_common[] = {
  { .table = "io",    .name = "stat",        .func = posix_stat       },
  { .table = "io",    .name = "ls",          .func = posix_ls         },
  { .table = "os",    .name = "usleep",      .func = posix_usleep     },
  { .table = "table", .name = "json_encode", .func = json_encode      },
  { .table = "table", .name = "json_decode", .func = json_decode      },
  { .table = "string",.name = "pcre_match",  .func = pcre_match       },
  { .table = "db",    .name = "read",        .func = db_read          },
  { .table = "db",    .name = "write",       .func = db_write         },
  { .table = "db",    .name = "escape",      .func = db_escape        },
  { .table = NULL,    .name = "print",       .func = safe_print       },
  { .table = NULL,    .name = "eprint",      .func = safe_error       },
  { .table = "work",  .name = "submit",      .func = work_submit      },
  { .table = "work",  .name = "accept",      .func = work_accept      },
  { .table = "work",  .name = "answer",      .func = work_answer      },
  { .table = "work",  .name = "pool",        .func = work_pool        },
  { .table = "work",  .name = "idle",        .func = work_idle        },
  { .table = "work",  .name = "backlog",     .func = work_backlog     },
};

struct function_map registry_handler[] = {
  { .table = "work",  .name = "collect",     .func = work_collect     },
  { .table = "work",  .name = "active",      .func = work_active      },
  { .table = "client",.name = "read",        .func = request_read     },
  { .table = "client",.name = "read_line",   .func = request_read_line},
  { .table = "client",.name = "write",       .func = request_write    },
};

void
lua_functions(lua_State *lua, struct function_map *map, size_t cells)
{
  for (size_t i = 0; i < cells; i++)
  {
    struct function_map *fm = &map[i];

    if (fm->table)
    {
      lua_getglobal(lua, fm->table);
      lua_pushstring(lua, fm->name);
      lua_pushcfunction(lua, fm->func);
      lua_settable(lua, -3);
      lua_pop(lua, 1);
    }
    else
    {
      lua_pushcfunction(lua, fm->func);
      lua_setglobal(lua, fm->name);
    }
  }
}

void*
main_worker (void *ptr)
{
  thread_t *worker = ptr;
  pthread_mutex_lock(&worker->mutex);
  worker->type = WORKER;

  ensure(pthread_setspecific(key_self, worker) == 0)
    errorf("pthread_setspecific failed");

  ensure((worker->lua = luaL_newstate()))
    errorf("luaL_newstate failed");

  luaL_openlibs(worker->lua);

  worker->rc = EXIT_SUCCESS;

  db_open();

  lua_createtable(worker->lua, 0, 0);
  lua_pushstring(worker->lua, "NULL");
  lua_pushlightuserdata(worker->lua, NULL);
  lua_settable(worker->lua, -3);
  lua_setglobal(worker->lua, "db");

  lua_createtable(worker->lua, 0, 0);
  lua_pushstring(worker->lua, "self");
  lua_pushstring(worker->lua, "worker");
  lua_settable(worker->lua, -3);
  lua_setglobal(worker->lua, "work");

  lua_functions(worker->lua, registry_common, sizeof(registry_common) / sizeof(struct function_map));

  if ( (cfg.worker_path && luaL_dofile(worker->lua,   cfg.worker_path) != 0)
    || (cfg.worker_code && luaL_dostring(worker->lua, cfg.worker_code) != 0))
  {
    errorf("lua error: %s", lua_tostring(worker->lua, -1));
    worker->rc = EXIT_FAILURE;
  }

  lua_close(worker->lua);
  db_close();

  worker->done = 1;
  pthread_mutex_unlock(&worker->mutex);

  return NULL;
}

void*
main_handler (void *ptr)
{
  thread_t *handler = ptr;
  pthread_mutex_lock(&handler->mutex);
  handler->type = HANDLER;

  ensure(pthread_setspecific(key_self, handler) == 0)
    errorf("pthread_setspecific failed");

  channel_init(&handler->results, cfg.max_results);

  request_t *request = NULL;

  while ((request = channel_read(&reqs)))
  {
    handler->request = request;

    if (cfg.ssl_cert && cfg.ssl_key)
    {
      request->ssl_ctx = SSL_CTX_new(SSLv23_server_method());
      ensure(request->ssl_ctx) errorf("SSL_CTX_new failed");
      SSL_CTX_set_options(request->ssl_ctx, SSL_OP_SINGLE_DH_USE);

      ensure(SSL_CTX_use_certificate_file(request->ssl_ctx, cfg.ssl_cert, SSL_FILETYPE_PEM) == 1) errorf("SSL_CTX_use_certificate_file failed %s", cfg.ssl_cert);
      ensure(SSL_CTX_use_PrivateKey_file(request->ssl_ctx,  cfg.ssl_key,  SSL_FILETYPE_PEM) == 1) errorf("SSL_CTX_use_PrivateKey_file failed %s",  cfg.ssl_key);

      request->ssl = SSL_new(request->ssl_ctx);
      ensure(request->ssl) errorf("SSL_new failed");

      SSL_set_fd(request->ssl, request->io);

      int ssl_err = SSL_accept(request->ssl);

      if (ssl_err <= 0)
      {
        errorf("ssl negotiation failed");
        SSL_shutdown(request->ssl);
        SSL_free(request->ssl);
        close(request->io);
        free(request->ipv4);
        free(request);
        continue;
      }
    }

    ensure((handler->lua = luaL_newstate()))
      errorf("luaL_newstate failed");

    luaL_openlibs(handler->lua);
    handler->rc = EXIT_SUCCESS;

    lua_createtable(handler->lua, 0, 0);
    lua_pushstring(handler->lua, "ip");
    if (request->ipv4) lua_pushstring(handler->lua, request->ipv4); else lua_pushnil(handler->lua);
    lua_settable(handler->lua, -3);
    lua_setglobal(handler->lua, "client");

    db_open();

    lua_createtable(handler->lua, 0, 0);
    lua_pushstring(handler->lua, "NULL");
    lua_pushlightuserdata(handler->lua, NULL);
    lua_settable(handler->lua, -3);
    lua_setglobal(handler->lua, "db");

    lua_createtable(handler->lua, 0, 0);
    lua_pushstring(handler->lua, "self");
    lua_pushstring(handler->lua, "handler");
    lua_settable(handler->lua, -3);
    lua_setglobal(handler->lua, "work");

    lua_functions(handler->lua, registry_common, sizeof(registry_common) / sizeof(struct function_map));
    lua_functions(handler->lua, registry_handler, sizeof(registry_handler) / sizeof(struct function_map));

    if ( (cfg.handler_path && luaL_dofile(handler->lua,   cfg.handler_path) != 0)
      || (cfg.handler_code && luaL_dostring(handler->lua, cfg.handler_code) != 0))
    {
      errorf("lua error: %s", lua_tostring(handler->lua, -1));
      handler->rc = EXIT_FAILURE;
    }

    lua_close(handler->lua);
    db_close();

    if (cfg.mode == MODE_STDIN)
      break;

    if (request->ssl)
    {
      SSL_shutdown(request->ssl);
      SSL_free(request->ssl);
      SSL_CTX_free(request->ssl_ctx);
    }

    close(request->io);
    free(request->ipv4);
    free(request);
  }

  channel_free(&handler->results);

  handler->done = 1;
  pthread_mutex_unlock(&handler->mutex);

  if (cfg.mode == MODE_STDIN)
    channel_write(&stuff, NULL);

  return NULL;
}

void
stop(int rc)
{
  sqlite3_shutdown();
  exit(rc);
}

void
sig_int(int sig)
{
  errorf("SIGINT");
  stop(EXIT_SUCCESS);
}

void
sig_term(int sig)
{
  errorf("SIGTERM");
  stop(EXIT_SUCCESS);
}

void
start(int argc, const char *argv[])
{
  struct stat st;

  ensure(sqlite3_threadsafe())
    errorf("sqlite3_threadsafe failed");

  sqlite3_initialize();

  long cores = sysconf(_SC_NPROCESSORS_ONLN);

  cfg.mode         = MODE_STDIN;
  cfg.tcp_port     = 0;
  cfg.tcp_backlog  = 32;
  cfg.max_workers  = cores;
  cfg.max_handlers = 1;
  cfg.handler_path = NULL;
  cfg.handler_code = NULL;
  cfg.worker_path  = NULL;
  cfg.worker_code  = NULL;
  cfg.setuid_name  = NULL;
  cfg.max_jobs     = 0;
  cfg.max_results  = 0;
  cfg.db_path      = NULL;
  cfg.ssl_cert     = NULL;
  cfg.ssl_key      = NULL;

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
      cfg.mode = MODE_TCP;
      continue;
    }
    if (str_eq(argv[argi], "-su") || str_eq(argv[argi], "--setuid"))
    {
      ensure(argi < argc-1) errorf("expected (-su|--setuid) <value>");
      cfg.setuid_name = argv[++argi];
      continue;
    }
    if (str_eq(argv[argi], "-w") || str_eq(argv[argi], "--worker"))
    {
      ensure(argi < argc-1) errorf("expected (-w|--worker) <value>");
      cfg.worker_path = argv[++argi];
      continue;
    }
    if (str_eq(argv[argi], "-wp") || str_eq(argv[argi], "--worker-pool"))
    {
      ensure(argi < argc-1) errorf("expected (-wp|--worker-pool) <value>");
      cfg.max_workers = strtol(argv[++argi], NULL, 0);
      continue;
    }
    if (str_eq(argv[argi], "-h") || str_eq(argv[argi], "--handler"))
    {
      ensure(argi < argc-1) errorf("expected (-h|--handler) <value>");
      cfg.handler_path = argv[++argi];
      continue;
    }
    if (str_eq(argv[argi], "-hp") || str_eq(argv[argi], "--handler-pool"))
    {
      ensure(argi < argc-1) errorf("expected (-hp|--handler-pool) <value>");
      cfg.max_handlers = strtol(argv[++argi], NULL, 0);
      continue;
    }
    if (str_eq(argv[argi], "-e") || str_eq(argv[argi], "--execute"))
    {
      ensure(argi < argc-1) errorf("expected (-e|--execute) <value>");
      cfg.handler_path = argv[++argi];
      cfg.worker_path  = argv[++argi];
      continue;
    }
    if (str_eq(argv[argi], "-db") || str_eq(argv[argi], "--database"))
    {
      ensure(argi < argc-1) errorf("expected (-db|--database) <value>");
      cfg.db_path = argv[++argi];
      continue;
    }
    if (str_eq(argv[argi], "-mj") || str_eq(argv[argi], "--max-jobs"))
    {
      ensure(argi < argc-1) errorf("expected (-mj|--max-jobs) <value>");
      cfg.max_jobs = strtol(argv[++argi], NULL, 0);
      continue;
    }
    if (str_eq(argv[argi], "-mr") || str_eq(argv[argi], "--max-results"))
    {
      ensure(argi < argc-1) errorf("expected (-mr|--max-results) <value>");
      cfg.max_results = strtol(argv[++argi], NULL, 0);
      continue;
    }
    if (str_eq(argv[argi], "--ssl-cert"))
    {
      ensure(argi < argc-1) errorf("expected --ssl-cert <value>");
      cfg.ssl_cert = argv[++argi];
      continue;
    }
    if (str_eq(argv[argi], "--ssl-key"))
    {
      ensure(argi < argc-1) errorf("expected --ssl-key <value>");
      cfg.ssl_key = argv[++argi];
      continue;
    }

    if (!cfg.handler_path && stat(argv[argi], &st) == 0)
    {
      cfg.handler_path = argv[argi];
      continue;
    }

    if (!cfg.worker_path && stat(argv[argi], &st) == 0)
    {
      cfg.worker_path = argv[argi];
      continue;
    }

    if (!cfg.handler_code)
    {
      cfg.handler_code = argv[argi];
      continue;
    }

    if (!cfg.worker_code)
    {
      cfg.worker_code = argv[argi];
      continue;
    }

    ensure(0) errorf("unexpected argument: %s", argv[argi]);
  }

  if (cfg.handler_path)
    cfg.handler_code = NULL;

  if (cfg.mode == MODE_TCP && cfg.max_handlers == 1)
    cfg.max_handlers = max(4, cores);

  ensure(cfg.handler_path || cfg.handler_code)
    errorf("expected lua -r script or inline code");

  if (cfg.ssl_cert || cfg.ssl_key)
    ensure(cfg.ssl_cert && cfg.ssl_key) errorf("expected SSL certificate and key");

  ensure(pthread_key_create(&key_self, NULL) == 0)
    errorf("pthread_key_create failed");

  size_t handlers_bytes = sizeof(thread_t) * cfg.max_handlers;

  ensure(handlers = malloc(handlers_bytes))
    errorf("malloc failed %lu", handlers_bytes);

  memset(handlers, 0, handlers_bytes);

  channel_init(&reqs, cfg.max_handlers);

  for (size_t ri = 0; ri < cfg.max_handlers; ri++)
  {
    ensure(pthread_mutex_init(&handlers[ri].mutex, NULL) == 0)
      errorf("pthread_mutex_init failed");

    thread_t *handler = &handlers[ri];

    handler->active = 1;

    ensure(pthread_create(&handler->thread, NULL, main_handler, handler) == 0)
      errorf("pthread_create failed");
  }

  channel_init(&jobs, cfg.max_jobs);

  if (cfg.worker_path || cfg.worker_code)
  {
    size_t workers_bytes = sizeof(thread_t) * cfg.max_workers;

    ensure(workers = malloc(workers_bytes))
      errorf("malloc failed %lu", workers_bytes);

    memset(workers, 0, workers_bytes);

    for (size_t wi = 0; wi < cfg.max_workers; wi++)
    {
      ensure(pthread_mutex_init(&workers[wi].mutex, NULL) == 0)
        errorf("pthread_mutex_init failed");

      thread_t *worker = &workers[wi];

      worker->active = 1;

      ensure(pthread_create(&worker->thread, NULL, main_worker, worker) == 0)
        errorf("pthread_create failed");
    }
  }
}

pthread_mutex_t *ssl_locks;

static void
ssl_lock(int mode, int type, const char *file, int line)
{
  if (mode & CRYPTO_LOCK)
    pthread_mutex_lock(&(ssl_locks[type]));
  else
    pthread_mutex_unlock(&(ssl_locks[type]));
}

static unsigned long
ssl_thread_id(void)
{
  return pthread_self();
}

int
main(int argc, char const *argv[])
{
  start(argc, argv);

  if (cfg.mode == MODE_TCP)
  {
    if (cfg.ssl_cert)
    {
      SSL_load_error_strings();
      SSL_library_init();
      OpenSSL_add_all_algorithms();

      ssl_locks = malloc(CRYPTO_num_locks() * sizeof(pthread_mutex_t));

      for (int i = 0; i < CRYPTO_num_locks(); i++)
        pthread_mutex_init(&ssl_locks[i], NULL);

      CRYPTO_set_id_callback(ssl_thread_id);
      CRYPTO_set_locking_callback(ssl_lock);
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
    struct sockaddr caddr;
    memset(&caddr, 0, sizeof(caddr));
    socklen_t clen = 0;

    for (;;)
    {
      fd = accept(sock_fd, &caddr, &clen);

      if (fd >= 0)
      {
        request_t *request = malloc(sizeof(request_t));
        request->io = fd;
        request->ipv4 = NULL;
        request->ssl = NULL;
        request->ssl_ctx = NULL;

        char *ipv4 = malloc(16);
        request->ipv4 = (char*)inet_ntop(AF_INET, &caddr, ipv4, 16);
        if (!request->ipv4) free(ipv4);

        channel_write(&reqs, request);
        continue;
      }

      switch (errno)
      {
        case EAGAIN:
        case EINTR:
        case EPROTO:
        case ECONNABORTED:
          continue;
        default:
          ensure(0) errorf("accept() failed %d", errno);
      }
    }

    return EXIT_FAILURE;
  }

  // MODE_STDIN
  request_t *request = malloc(sizeof(request_t));
  request->io = fileno(stdin);
  request->ipv4 = NULL;
  channel_write(&reqs, request);

  channel_init(&stuff, 0);
  channel_read(&stuff);

  stop(handlers[0].rc);

  return EXIT_FAILURE;
}
