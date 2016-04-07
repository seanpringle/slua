
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

typedef struct {
  int tcp_port;
  size_t max_connections;
  const char *script_path;
  const char *setuid_name;
} config_t;

config_t cfg;

typedef struct {
  pthread_mutex_t mutex;
  int active;
  int done;
  int rc;
  int io;
  FILE *fio;
  pthread_t thread;
  lua_State *lua;
} request_t;

request_t *requests;

static pthread_key_t key_self;
#define self ((request_t*)pthread_getspecific(key_self))

int
close_io (lua_State *lua)
{
  if (self->fio)
  {
    fclose(self->fio);
    self->fio = NULL;
    self->io = 0;
  }
  return 0;
}

void*
handler (void *ptr)
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

  luaL_Stream *s = lua_newuserdata(request->lua, sizeof(luaL_Stream));
  s->f = request->fio;
  s->closef = close_io;
  luaL_setmetatable(request->lua, LUA_FILEHANDLE);
  lua_setglobal(request->lua, "sock");

  request->rc = EXIT_SUCCESS;

  if (luaL_dofile(request->lua, cfg.script_path) != 0)
  {
    errorf("lua error: %s", lua_tostring(request->lua, -1));
    request->rc = EXIT_FAILURE;
  }

  close_io(request->lua);
  lua_close(request->lua);

  request->done = 1;
  pthread_mutex_unlock(&request->mutex);

  return NULL;
}

int
main(int argc, char const *argv[])
{
  cfg.tcp_port = 80;
  cfg.max_connections = 100;
  cfg.script_path = NULL;
  cfg.setuid_name = NULL;

  for (int argi = 1; argi < argc; argi++)
  {
    if (str_eq(argv[argi], "-p") || str_eq(argv[argi], "--port"))
    {
      ensure(argi < argc-1) errorf("expected (-p|--port) <value>");
      cfg.tcp_port = strtol(argv[++argi], NULL, 0);
      continue;
    }
    if (str_eq(argv[argi], "-mc") || str_eq(argv[argi], "--max-connections"))
    {
      ensure(argi < argc-1) errorf("expected (-mc|--max-connections) <value>");
      cfg.max_connections = strtol(argv[++argi], NULL, 0);
      continue;
    }
    if (str_eq(argv[argi], "-su") || str_eq(argv[argi], "--setuid-name"))
    {
      ensure(argi < argc-1) errorf("expected (-su|--setuid-name) <value>");
      cfg.setuid_name = argv[++argi];
      continue;
    }

    cfg.script_path = argv[argi];
  }

  ensure(cfg.script_path)
    errorf("expected script");

  ensure(pthread_key_create(&key_self, NULL) == 0)
    errorf("pthread_key_create failed");

  size_t bytes = sizeof(request_t) * cfg.max_connections;

  ensure(requests = malloc(bytes))
    errorf("malloc failed %lu", bytes);

  memset(requests, 0, bytes);

  for (size_t reqi = 0; reqi < cfg.max_connections; reqi++)
  {
    ensure(pthread_mutex_init(&requests[reqi].mutex, NULL) == 0)
      errorf("pthread_mutex_init failed");
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
      for (int reqi = 0; !request && reqi < cfg.max_connections; reqi++)
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
        errorf("hit max_connections");
        usleep(10000);
      }
    }

    memset(request, 0, sizeof(request_t));

    request->io = fd;
    request->active = 1;

    ensure(pthread_create(&request->thread, NULL, handler, request) == 0)
      errorf("pthread_create failed");

    pthread_mutex_unlock(&request->mutex);
  }

  close(sock_fd);

  return EXIT_SUCCESS;
}
