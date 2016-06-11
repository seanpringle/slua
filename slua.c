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
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>

#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#define min(a,b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a: _b; })
#define max(a,b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a: _b; })

#define ensure(x) for ( ; !(x) ; exit(EXIT_FAILURE) )

void
dump (void *ptr, unsigned int bytes)
{
  for (int i = 0; i < bytes; i += 16)
  {
    unsigned char *p = ptr + i;
    fprintf(stderr, "%ld ", (uint64_t)p);
    for (int j = 0; j < 16; j++) fprintf(stderr, "%02x ", p[j]);
    for (int j = 0; j < 16; j++) fprintf(stderr, "%c", isalnum(p[j]) ? p[j]: '.');
    fprintf(stderr, "\n");
  }
}

typedef struct _channel_node_t {
  struct _channel_node_t *next;
  size_t length;
  char payload[];
} channel_node_t;

typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t cond_read;
  pthread_cond_t cond_write;
  pthread_cond_t cond_active;
  channel_node_t *list;
  channel_node_t *last;
  size_t limit;
  size_t backlog;
  size_t readers;
  size_t writers;
  size_t waiters;
  int used;
} channel_t;

typedef struct {
  int io;
  char ipv4[16];
  SSL *ssl;
} request_t;

typedef struct {
  channel_t *respond;
  unsigned char is_nil;
  char payload[];
} message_t;

#define HANDLER 1
#define WORKER 2

typedef struct {
  int type;
  pid_t pid;
  channel_t results;
  channel_t *result;
  request_t *request;
} process_t;

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
  SSL_CTX *ssl_ctx;
  size_t shared_mem;
  size_t shared_page;
} config_t;

config_t cfg;

typedef struct {
  int multi;
  pthread_condattr_t condattr;
  pthread_mutexattr_t mutexattr;
  process_t *self;
  lua_State *lua;
  void *store;
} global_t;

global_t global;

#define outputf(...) ({ \
  if (global.multi) ensure(pthread_mutex_lock(&shared->stdout_mutex) == 0); \
  fprintf(stdout, __VA_ARGS__); fputc('\n', stdout); \
  if (global.multi) ensure(pthread_mutex_unlock(&shared->stdout_mutex) == 0); \
})

#define errorf(...) ({ \
  if (global.multi) ensure(pthread_mutex_lock(&shared->stderr_mutex) == 0); \
  fprintf(stderr, "%d: ", getpid()); \
  fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); \
  if (global.multi) ensure(pthread_mutex_unlock(&shared->stderr_mutex) == 0); \
})

typedef struct {
  pthread_mutex_t stdout_mutex;
  pthread_mutex_t stderr_mutex;
  channel_t jobs;
  process_t *workers;
} shared_t;

shared_t *shared;

#include "arena.c"
#include "store.c"
#include "channel.c"

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

int
safe_print (lua_State *lua)
{
  ensure(pthread_mutex_lock(&shared->stdout_mutex) == 0);
  int args = lua_gettop(lua);
  for (int i = 0; i < args; i++)
    printf("%s", lua_tostring(lua, i+1));
  printf("\n");
  lua_pop(lua, args);
  ensure(pthread_mutex_unlock(&shared->stdout_mutex) == 0);
  return 0;
}

int
safe_error (lua_State *lua)
{
  ensure(pthread_mutex_lock(&shared->stderr_mutex) == 0);
  int args = lua_gettop(lua);
  for (int i = 0; i < args; i++)
    fprintf(stderr, "%d %s", getpid(), lua_tostring(lua, i+1));
  fprintf(stderr, "\n");
  lua_pop(lua, args);
  ensure(pthread_mutex_unlock(&shared->stderr_mutex) == 0);
  return 0;
}

#include "str.c"
#include "work.c"
#include "posix.c"
#include "json.c"
#include "hash.c"
#include "pcre.c"

int
request_read (lua_State *lua)
{
  int ok = 1;
  size_t length = 0;
  size_t limit = lua_popnumber(lua);
  char *buffer = malloc(limit+1);

  while (length < limit)
  {
    size_t bytes = global.self->request->ssl
      ? SSL_read(global.self->request->ssl,  buffer + length, limit - length)
      : read(global.self->request->io, buffer + length, limit - length);

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

    size_t bytes = global.self->request->ssl
      ? SSL_read(global.self->request->ssl, c, 1)
      : read(global.self->request->io, c, 1);

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
      bytes = global.self->request->ssl
        ? SSL_write(global.self->request->ssl, str, strlen(str))
        : write(global.self->request->io, str, strlen(str));
    }
  }
  if (bytes < 0)
    lua_pushnil(lua);
  else
    lua_pushnumber(lua, bytes);
  return 1;
}

void
child_stop (int rc)
{
  process_t *process = global.self;
  if (process->type == HANDLER)
  {
    channel_free(&process->results);
    request_t *request = process->request;
    if (request->ssl)
    {
      SSL_shutdown(request->ssl);
      SSL_free(request->ssl);
    }
    close(request->io);

    if (process->type == HANDLER)
      store_free(global.store, process);
  }
  lua_close(global.lua);
  exit(rc);
}

void
child_sig_int (int sig)
{
  child_stop(EXIT_SUCCESS);
}

void
child_sig_term (int sig)
{
  child_stop(EXIT_SUCCESS);
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
  { .table = "os",    .name = "call",        .func = posix_command    },
  { .table = "table", .name = "json_encode", .func = json_encode      },
  { .table = "table", .name = "json_decode", .func = json_decode      },
  { .table = "string",.name = "pcre_match",  .func = pcre_match       },
  { .table = NULL,    .name = "print",       .func = safe_print       },
  { .table = NULL,    .name = "eprint",      .func = safe_error       },
  { .table = "work",  .name = "submit",      .func = work_submit      },
  { .table = "work",  .name = "accept",      .func = work_accept      },
  { .table = "work",  .name = "answer",      .func = work_answer      },
  { .table = "work",  .name = "pool",        .func = work_pool        },
  { .table = "work",  .name = "idle",        .func = work_idle        },
  { .table = "work",  .name = "backlog",     .func = work_backlog     },
  { .table = "string",.name = "md5",         .func = hash_md5         },
  { .table = "string",.name = "sha1",        .func = hash_sha1        },
  { .table = "string",.name = "sha256",      .func = hash_sha256      },
  { .table = "string",.name = "sha512",      .func = hash_sha512      },
  { .table = "string",.name = "epoch",       .func = posix_strtotime  },
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

pid_t
child (process_t *process)
{
  pid_t pid = fork();
  if (pid) return pid;

  global.self = process;

  signal(SIGINT,  child_sig_int);
  signal(SIGTERM, child_sig_term);
  signal(SIGPIPE, SIG_IGN);

  ensure((global.lua = luaL_newstate()));
  luaL_openlibs(global.lua);

  if (cfg.setuid_name)
  {
    struct passwd *pw = getpwnam(cfg.setuid_name);

    ensure(pw && setuid(pw->pw_uid) == 0)
      errorf("setuid %s failed", cfg.setuid_name);
  }

  lua_createtable(global.lua, 0, 0);
  lua_setglobal(global.lua, "work");

  if (process->type == HANDLER)
  {
    channel_init(&process->results, cfg.max_results);

    request_t *request = process->request;

    if (cfg.ssl_cert)
    {
      SSL_load_error_strings();
      SSL_library_init();
      OpenSSL_add_all_algorithms();

      cfg.ssl_ctx = SSL_CTX_new(SSLv23_server_method());
      ensure(cfg.ssl_ctx) errorf("SSL_CTX_new failed");
      SSL_CTX_set_options(cfg.ssl_ctx, SSL_OP_SINGLE_DH_USE);

      ensure(SSL_CTX_use_certificate_file(cfg.ssl_ctx, cfg.ssl_cert, SSL_FILETYPE_PEM) == 1) errorf("SSL_CTX_use_certificate_file failed %s", cfg.ssl_cert);
      ensure(SSL_CTX_use_PrivateKey_file(cfg.ssl_ctx,  cfg.ssl_key,  SSL_FILETYPE_PEM) == 1) errorf("SSL_CTX_use_PrivateKey_file failed %s",  cfg.ssl_key);
    }

    if (cfg.ssl_cert && cfg.ssl_key)
    {
      request->ssl = SSL_new(cfg.ssl_ctx);
      ensure(request->ssl) errorf("SSL_new failed");

      SSL_set_fd(request->ssl, request->io);
      int ssl_err = SSL_accept(request->ssl);

      if (ssl_err <= 0)
      {
        errorf("ssl negotiation failed");
        goto done;
      }
    }

    lua_createtable(global.lua, 0, 0);
    lua_setglobal(global.lua, "client");

    lua_functions(global.lua, registry_common, sizeof(registry_common) / sizeof(struct function_map));
    lua_functions(global.lua, registry_handler, sizeof(registry_handler) / sizeof(struct function_map));

    if ( (cfg.handler_path && luaL_dofile(global.lua,   cfg.handler_path) != 0)
      || (cfg.handler_code && luaL_dostring(global.lua, cfg.handler_code) != 0))
    {
      errorf("handler lua error: %s", lua_tostring(global.lua, -1));
      child_stop(EXIT_FAILURE);
    }
  }
  else
  {
    lua_functions(global.lua, registry_common, sizeof(registry_common) / sizeof(struct function_map));

    if ( (cfg.worker_path && luaL_dofile(global.lua,   cfg.worker_path) != 0)
      || (cfg.worker_code && luaL_dostring(global.lua, cfg.worker_code) != 0))
    {
      errorf("worker lua error: %s", lua_tostring(global.lua, -1));
      child_stop(EXIT_FAILURE);
    }
  }
done:

  child_stop(EXIT_SUCCESS);
  return pid;
}

void
stop (int rc)
{
  if (cfg.worker_path || cfg.worker_code)
  {
    for (int i = 0; i < cfg.max_workers; i++)
    {
      process_t *process = &shared->workers[i];
      int status = 0;
      kill(process->pid, SIGTERM);
      waitpid(process->pid, &status, 0);
    }
  }

  arena_dump(store_arena(global.store));

  pthread_mutex_destroy(&shared->stdout_mutex);
  pthread_mutex_destroy(&shared->stderr_mutex);
  pthread_mutexattr_destroy(&global.mutexattr);
  pthread_condattr_destroy(&global.condattr);

  global.multi = 0;

  store_destroy(global.store);
  exit(rc);
}

void
sig_int (int sig)
{
  stop(EXIT_SUCCESS);
}

void
sig_term (int sig)
{
  stop(EXIT_SUCCESS);
}

int
main (int argc, char const *argv[])
{
  global.multi = 0;

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
  cfg.ssl_ctx      = NULL;
  cfg.shared_mem   = 32 * 1024 * 1024;
  cfg.shared_page  = 1024;

  struct stat st;

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
      cfg.max_workers = max(1, strtol(argv[++argi], NULL, 0));
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
      cfg.max_handlers = max(1, strtol(argv[++argi], NULL, 0));
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
    if (str_eq(argv[argi], "-sm") || str_eq(argv[argi], "--shared-memory"))
    {
      ensure(argi < argc-1) errorf("expected (-sm|--shared-memory) <value>");
      cfg.shared_mem = strtol(argv[++argi], NULL, 0) * 1024 * 1024;
      continue;
    }
    if (str_eq(argv[argi], "-sp") || str_eq(argv[argi], "--shared-page"))
    {
      ensure(argi < argc-1) errorf("expected (-sp|--shared-page) <value>");
      cfg.shared_page = strtol(argv[++argi], NULL, 0);
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
  {
    ensure(cfg.mode == MODE_TCP) errorf("SSL requires MODE_TCP");
    ensure(cfg.ssl_cert && cfg.ssl_key) errorf("expected SSL certificate and key");
  }

  ensure(pthread_condattr_init(&global.condattr) == 0);
  ensure(pthread_condattr_setpshared(&global.condattr, PTHREAD_PROCESS_SHARED) == 0);

  ensure(pthread_mutexattr_init(&global.mutexattr) == 0);
  ensure(pthread_mutexattr_setpshared(&global.mutexattr, PTHREAD_PROCESS_SHARED) == 0);

  global.store = store_create("store", cfg.shared_mem, cfg.shared_page);
  shared = store_alloc(global.store, sizeof(shared_t));
  shared->workers = store_alloc(global.store, sizeof(process_t) * cfg.max_workers);

  ensure(pthread_mutex_init(&shared->stdout_mutex, &global.mutexattr) == 0);
  ensure(pthread_mutex_init(&shared->stderr_mutex, &global.mutexattr) == 0);

  global.multi = 1;

  channel_init(&shared->jobs, cfg.max_jobs);

  if (cfg.worker_path || cfg.worker_code)
  {
    for (int i = 0; i < cfg.max_workers; i++)
    {
      process_t *process = &shared->workers[i];
      process->type = WORKER;
      process->pid = child(process);
    }
  }

  signal(SIGINT,  sig_int);
  signal(SIGTERM, sig_term);
  signal(SIGPIPE, SIG_IGN);

  if (cfg.mode == MODE_TCP)
  {
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
        request_t r, *request = &r;

        memset(request, 0, sizeof(request_t));
        request->io = fd;
        inet_ntop(AF_INET, &caddr, request->ipv4, 16);

        process_t *process = store_alloc(global.store, sizeof(process_t));
        memset(process, 0, sizeof(process_t));
        process->type = HANDLER;
        process->request = request;

        child(process);
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
  request_t r, *request = &r;

  memset(request, 0, sizeof(request_t));
  request->io = fileno(stdin);

  process_t *process = store_alloc(global.store, sizeof(process_t));
  memset(process, 0, sizeof(process_t));
  process->type = HANDLER;
  process->request = request;

  int status = 0;
  pid_t pid = child(process);
  waitpid(pid, &status, 0);

  stop(status);
}
