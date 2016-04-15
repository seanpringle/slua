
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
#include <sys/stat.h>
#include <dirent.h>
#include <pwd.h>
#include <grp.h>
#include <math.h>

#define min(a,b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a: _b; })
#define max(a,b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a: _b; })

#define ensure(x) for ( ; !(x) ; exit(EXIT_FAILURE) )

static pthread_key_t key_self;
#define hself ((handler_t*)pthread_getspecific(key_self))
#define wself ((worker_t*)pthread_getspecific(key_self))

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

#define str_eq(a,b) (strcmp((a),(b)) == 0)

char*
strf (char *pattern, ...)
{
  char *result = NULL;
  va_list args;
  char buffer[8];

  va_start(args, pattern);
  int len = vsnprintf(buffer, sizeof(buffer), pattern, args);
  va_end(args);

  if (len > -1 && (result = malloc(len+1)) && result)
  {
    va_start(args, pattern);
    vsnprintf(result, len+1, pattern, args);
    va_end(args);
  }
  return result;
}

typedef struct _channel_node_t {
  void *payload;
  struct _channel_node_t *next;
} channel_node_t;

typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t cond_read;
  pthread_cond_t cond_write;
  channel_node_t *list;
  channel_node_t *last;
  size_t limit;
  size_t backlog;
  size_t readers;
  size_t writers;
  int used;
} channel_t;

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
  ensure(pthread_mutex_init(&channel->mutex, NULL) == 0);
  ensure(pthread_cond_init(&channel->cond_read, NULL) == 0);
  ensure(pthread_cond_init(&channel->cond_write, NULL) == 0);
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
      free(node);
    }
    pthread_mutex_destroy(&channel->mutex);
    pthread_cond_destroy(&channel->cond_read);
    pthread_cond_destroy(&channel->cond_write);
    free(channel->list);
    channel->used = 0;
  }
}

size_t
channel_backlog (channel_t *channel)
{
  ensure(pthread_mutex_lock(&channel->mutex) == 0);
  size_t backlog = channel->backlog;
  ensure(pthread_mutex_unlock(&channel->mutex) == 0);
  return backlog;
}

size_t
channel_readers (channel_t *channel)
{
  ensure(pthread_mutex_lock(&channel->mutex) == 0);
  size_t readers = channel->readers;
  ensure(pthread_mutex_unlock(&channel->mutex) == 0);
  return readers;
}

size_t
channel_writers (channel_t *channel)
{
  ensure(pthread_mutex_lock(&channel->mutex) == 0);
  size_t writers = channel->writers;
  ensure(pthread_mutex_unlock(&channel->mutex) == 0);
  return writers;
}

void*
channel_read (channel_t *channel)
{
  ensure(pthread_mutex_lock(&channel->mutex) == 0);
  channel->readers++;

  while (channel->backlog == 0)
    pthread_cond_wait(&channel->cond_read, &channel->mutex);

  channel->backlog--;

  channel_node_t *node = channel->list;
  channel->list = node->next;

  void *msg = node->payload;

  if (node == channel->last)
    channel->last = NULL;

  free(node);

  if (channel->writers)
    pthread_cond_signal(&channel->cond_write);

  channel->readers--;
  ensure(pthread_mutex_unlock(&channel->mutex) == 0);
  return msg;
}

void
channel_write (channel_t *channel, void *msg)
{
  ensure(pthread_mutex_lock(&channel->mutex) == 0);
  channel->writers++;

  while (channel->limit > 0 && channel->backlog == channel->limit)
    pthread_cond_wait(&channel->cond_write, &channel->mutex);

  channel->backlog++;

  channel_node_t *node = malloc(sizeof(channel_node_t));
  node->payload = msg;
  node->next = NULL;

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
    pthread_cond_signal(&channel->cond_read);

  channel->writers--;
  ensure(pthread_mutex_unlock(&channel->mutex) == 0);
}

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
posix_ls (lua_State *lua)
{
  char *path = (char*)lua_popstring(lua);

  DIR *dp;
  struct dirent *ep;

  if ((dp = opendir(path)))
  {
    int items = 0;
    lua_createtable(lua, 0, 0);

    while ((ep = readdir(dp)))
    {
      lua_pushnumber(lua, ++items);
      lua_pushstring(lua, ep->d_name);
      lua_settable(lua, -3);
    }
    closedir(dp);
  }
  else
  {
    lua_pushnil(lua);
  }
  return 1;
}

int
posix_stat (lua_State *lua)
{
  char *path = (char*)lua_popstring(lua);

  struct stat st;
  int exists = stat(path, &st) == 0;

  if (exists)
  {
    char mode[11];
    strcpy(mode, "----------");
    int i = 0;

    if (S_ISDIR(st.st_mode))  mode[i] = 'd'; i++;
    if (st.st_mode & S_IRUSR) mode[i] = 'r'; i++;
    if (st.st_mode & S_IWUSR) mode[i] = 'w'; i++;
    if (st.st_mode & S_IXUSR) mode[i] = 'x'; i++;
    if (st.st_mode & S_IRGRP) mode[i] = 'r'; i++;
    if (st.st_mode & S_IWGRP) mode[i] = 'w'; i++;
    if (st.st_mode & S_IXGRP) mode[i] = 'x'; i++;
    if (st.st_mode & S_IROTH) mode[i] = 'r'; i++;
    if (st.st_mode & S_IWOTH) mode[i] = 'w'; i++;
    if (st.st_mode & S_IXOTH) mode[i] = 'x'; i++;

    int gsize = (int) sysconf(_SC_GETGR_R_SIZE_MAX);
    char *gbuff = malloc(gsize);
    struct group _grp, *grp = &_grp;

    int grs = getgrgid_r(st.st_gid, grp, gbuff, gsize, &grp);

    int psize = (int) sysconf(_SC_GETPW_R_SIZE_MAX);
    char *pbuff = malloc(psize);
    struct passwd _pwd, *pwd = &_pwd;

    int prs = getpwuid_r(st.st_uid, pwd, pbuff, psize, &pwd);

    lua_createtable(lua, 0, 0);

    lua_pushstring(lua, "size");
    lua_pushnumber(lua, st.st_size);
    lua_settable(lua, -3);
    lua_pushstring(lua, "inode");
    lua_pushnumber(lua, st.st_ino);
    lua_settable(lua, -3);
    lua_pushstring(lua, "uid");
    lua_pushnumber(lua, st.st_uid);
    lua_settable(lua, -3);
    lua_pushstring(lua, "gid");
    lua_pushnumber(lua, st.st_gid);
    lua_settable(lua, -3);
    lua_pushstring(lua, "user");
    lua_pushstring(lua, (prs == 0 && pwd ? pwd->pw_name: ""));
    lua_settable(lua, -3);
    lua_pushstring(lua, "group");
    lua_pushstring(lua, (grs == 0 && grp ? grp->gr_name: ""));
    lua_settable(lua, -3);
    lua_pushstring(lua, "mode");
    lua_pushstring(lua, mode);
    lua_settable(lua, -3);
    lua_pushstring(lua, "ctime");
    lua_pushnumber(lua, (long long)st.st_ctim.tv_sec);
    lua_settable(lua, -3);
    lua_pushstring(lua, "mtime");
    lua_pushnumber(lua, (long long)st.st_mtim.tv_sec);
    lua_settable(lua, -3);
    lua_pushstring(lua, "atime");
    lua_pushnumber(lua, (long long)st.st_atim.tv_sec);
    lua_settable(lua, -3);

    free(gbuff);
    free(pbuff);
  }
  else
  {
    lua_pushnil(lua);
  }
  return 1;
}

char*
str_quote (char *str)
{
  char *res = malloc(strlen(str)*2+3);
  char *rp = res, *sp = str;

  *rp++ = '"';

  while (sp && *sp)
  {
    int c = *sp++;
    if (c == '"') { *rp++ = '\\'; }
    else if (c == '\\') { *rp++ = '\\'; }
    else if (c == '\a') { *rp++ = '\\'; c = 'a'; }
    else if (c == '\b') { *rp++ = '\\'; c = 'b'; }
    else if (c == '\f') { *rp++ = '\\'; c = 'f'; }
    else if (c == '\n') { *rp++ = '\\'; c = 'n'; }
    else if (c == '\r') { *rp++ = '\\'; c = 'r'; }
    else if (c == '\t') { *rp++ = '\\'; c = 't'; }
    else if (c == '\v') { *rp++ = '\\'; c = 'v'; }
    *rp++ = c;
  }

  *rp++ = '"';
  *rp = 0;
  return res;
}

char*
str_unquote (char *str, char **err)
{
  char *res = malloc(strlen(str)+1);
  char *rp = res, *sp = str;

  sp++;

  while (sp && *sp)
  {
    int c = *sp++;
    if (c == '"') break;

    if (c == '\\')
    {
           if (*sp == 'a') c = '\a';
      else if (*sp == 'b') c = '\b';
      else if (*sp == 'f') c = '\f';
      else if (*sp == 'n') c = '\n';
      else if (*sp == 'r') c = '\r';
      else if (*sp == 't') c = '\t';
      else if (*sp == 'v') c = '\v';
      else c = *sp++;
    }
    *rp++ = c;
  }
  *rp = 0;

  if (err)
    *err = sp;

  return res;
}

int
json_encode (lua_State *lua)
{
  // check for array
  int table_length = 0;
  int has_strings = 0;
  int has_numbers = 0;
  lua_pushnil(lua);
  while (lua_next(lua, -2))
  {
    if (lua_type(lua, -2) == LUA_TNUMBER) has_numbers++;
    if (lua_type(lua, -2) == LUA_TSTRING) has_strings++;
    lua_pop(lua, 1);
    table_length++;
  }

  if (!table_length)
  {
    lua_pop(lua, 1);
    lua_pushstring(lua, "[]");
    return 1;
  }

  if (has_strings && has_numbers) has_numbers = 0;

  int is_hash = has_strings;

  int length = 0, limit = 1024;
  char *json = malloc(limit);

  json[length++] = is_hash ? '{': '[';

  lua_pushnil(lua);
  while (lua_next(lua, -2))
  {
    if (lua_type(lua, -2) == LUA_TSTRING || lua_type(lua, -2) == LUA_TNUMBER)
    {
      char *key = lua_type(lua, -2) == LUA_TSTRING
        ? str_quote((char*)lua_tostring(lua, -2)) : NULL;

      if (!key)
      {
        double num = lua_tonumber(lua, -2);
        key = strf("\"%ld\"", (int64_t)num);
      }

      char *val = NULL;

      switch (lua_type(lua, -1))
      {
        case LUA_TNIL:
          val = strdup("null");
          break;

        case LUA_TBOOLEAN:
          val = strdup(lua_toboolean(lua, -1) ? "true": "false");
          break;

        case LUA_TNUMBER:
          val = strdup((char*)lua_tostring(lua, -1));
          break;

        case LUA_TSTRING:
          val = str_quote((char*)lua_tostring(lua, -1));
          break;

        case LUA_TTABLE:
          json_encode(lua);
          val = strdup((char*)lua_tostring(lua, -1));
          break;
      }

      if (val)
      {
        limit += strlen(key) + strlen(val) + 3;
        json = realloc(json, limit);

        if (is_hash)
        {
          length += snprintf(json+length, limit-length, "%s:%s,", key, val);
        }
        else
        {
          length += snprintf(json+length, limit-length, "%s,", val);
        }
      }

      free(key);
      free(val);
    }
    lua_pop(lua, 1);
  }

  json[length-1] = is_hash ? '}': ']';
  json[length] = 0;

  lua_pop(lua, 1);
  lua_pushstring(lua, json);
  free(json);

  return 1;
}

char*
json_decode_step (lua_State *lua, char *json, int mode)
{
  lua_createtable(lua, 0, 0);
  int table_size = 0;

  int pushed = 0;
  char *str;
  double num;

  while (json && *json && strchr(", \t\r\n", *json)) json++;

  char *last = NULL;

  while (json && *json)
  {
    int c = *json++;

    if (c == ']' || c == '}' || last == json)
    {
      if (pushed)
        lua_pop(lua, pushed);
      break;
    }

    last = json;

    switch (c)
    {
      case '[':
        json = json_decode_step(lua, json, 1);
        pushed++;
        break;

      case '{':
        json = json_decode_step(lua, json, 2);
        pushed++;
        break;

      case '"':
        str = str_unquote(json-1, &json);
        lua_pushstring(lua, str);
        free(str);
        pushed++;
        break;

      case 'n':
        lua_pushnil(lua);
        pushed++;
        break;

      default:
        num = strtod(json-1, &json);
        lua_pushnumber(lua, num);
        pushed++;
        break;
    }

    while (json && *json && strchr(", \t\r\n", *json)) json++;

    if (*json == ':')
    {
      json++;
      while (json && *json && strchr(", \t\r\n", *json)) json++;

      if (mode == 1 && pushed)
      {
        lua_pop(lua, pushed);
        pushed = 0;
      }
      continue;
    }

    if (mode == 1 && pushed == 1)
    {
      lua_pushnumber(lua, ++table_size);
      lua_insert(lua, -2);
      lua_settable(lua, -3);
      pushed = 0;
    }
    else
    if (mode == 2 && pushed == 2)
    {
      lua_settable(lua, -3);
      pushed = 0;
    }
    else
    {
      errorf("unexpected state, mode: %d, pushed: %d", mode, pushed);
      break;
    }
  }

  while (json && *json && strchr(", \t\r\n", *json)) json++;
  return json;
}

int
json_decode (lua_State *lua)
{
  char *json = strdup((char*)lua_popstring(lua));

  if (!json || !strchr("{[", *json))
  {
    lua_pushnil(lua);
    return 1;
  }

  int c = *json;
  json_decode_step(lua, json+1, c == '{' ? 2: 1);
  free(json);
  return 1;
}

int
work_accept (lua_State *lua)
{
  message_t *message = channel_read(&jobs);
  if (message->payload) lua_pushstring(lua, message->payload); else lua_pushnil(lua);
  wself->result = message->result;
  free(message->payload);
  free(message);
  return 1;
}

int
work_result (lua_State *lua)
{
  channel_write(wself->result,
    lua_type(lua, -1) == LUA_TSTRING ? strdup((char*)lua_popstring(lua)) : NULL);
  return 0;
}

int
work_submit (lua_State *lua)
{
  ensure(cfg.worker_path)
    errorf("no workers");

  message_t *message = malloc(sizeof(message_t));
  message->result = &hself->results;
  message->payload = lua_type(lua, -1) == LUA_TSTRING ? strdup((char*)lua_popstring(lua)) : NULL;

  channel_write(&jobs, message);
  return 0;
}

int
work_collect (lua_State *lua)
{
  ensure(cfg.worker_path)
    errorf("no workers");

  char *payload = channel_read(&hself->results);
  if (payload) lua_pushstring(lua, payload); else lua_pushnil(lua);
  free(payload);
  return 1;
}

int
results_backlog (lua_State *lua)
{
  lua_pushnumber(lua, channel_backlog(&hself->results));
  return 1;
}

void
lua_functions(lua_State *lua)
{
  lua_getglobal(lua, "io");

  lua_pushstring(lua, "stat");
  lua_pushcfunction(lua, posix_stat);
  lua_settable(lua, -3);
  lua_pushstring(lua, "ls");
  lua_pushcfunction(lua, posix_ls);
  lua_settable(lua, -3);

  lua_pop(lua, 1);

  lua_getglobal(lua, "table");

  lua_pushstring(lua, "json_encode");
  lua_pushcfunction(lua, json_encode);
  lua_settable(lua, -3);
  lua_pushstring(lua, "json_decode");
  lua_pushcfunction(lua, json_decode);
  lua_settable(lua, -3);

  lua_pop(lua, 1);
}

int
close_io (lua_State *lua)
{
  if (hself->fio && hself->io != fileno(stdin))
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

  ensure((worker->lua = luaL_newstate()))
    errorf("luaL_newstate failed");

  luaL_openlibs(worker->lua);

  worker->rc = EXIT_SUCCESS;

  lua_createtable(worker->lua, 0, 0);

  lua_pushstring(worker->lua, "accept");
  lua_pushcfunction(worker->lua, work_accept);
  lua_settable(worker->lua, -3);

  lua_pushstring(worker->lua, "result");
  lua_pushcfunction(worker->lua, work_result);
  lua_settable(worker->lua, -3);

  lua_setglobal(worker->lua, "work");

  lua_functions(worker->lua);

  if ( (cfg.worker_path && luaL_dofile(worker->lua,   cfg.worker_path) != 0)
    || (cfg.worker_code && luaL_dostring(worker->lua, cfg.worker_code) != 0))
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

  channel_init(&handler->results, cfg.max_results);

  request_t *request = NULL;

  while ((request = channel_read(&reqs)))
  {
    handler->io = request->io;
    free(request);

    ensure((handler->lua = luaL_newstate()))
      errorf("luaL_newstate failed");

    luaL_openlibs(handler->lua);

    handler->fio = handler->io == fileno(stdin) ? stdin: fdopen(handler->io, "r+");

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
    lua_pushcfunction(handler->lua, work_submit);
    lua_settable(handler->lua, -3);

    lua_pushstring(handler->lua, "collect");
    lua_pushcfunction(handler->lua, work_collect);
    lua_settable(handler->lua, -3);

    lua_pushstring(handler->lua, "done");
    lua_pushcfunction(handler->lua, results_backlog);
    lua_settable(handler->lua, -3);

    lua_setglobal(handler->lua, "work");

    lua_functions(handler->lua);

    if ( (cfg.handler_path && luaL_dofile(handler->lua,   cfg.handler_path) != 0)
      || (cfg.handler_code && luaL_dostring(handler->lua, cfg.handler_code) != 0))
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
stop(int rc)
{
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

    ensure(argv[argi][0] != '-')
      errorf("huh? %s", argv[argi]);

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

  channel_init(&reqs, cfg.max_handlers);

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

    channel_init(&jobs, cfg.max_jobs);
  }
}

int
main(int argc, char const *argv[])
{
  start(argc, argv);

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

    while ((fd = accept(sock_fd, NULL, NULL)) && fd >= 0)
    {
      request_t *request = malloc(sizeof(request_t));
      request->io = fd;

      channel_write(&reqs, request);
    }

    return EXIT_FAILURE;
  }

  // MODE_STDIN
  request_t *request = malloc(sizeof(request_t));
  request->io = fileno(stdin);
  channel_write(&reqs, request);

  while (!channel_readers(&reqs) || channel_backlog(&reqs))
    usleep(1000);

  stop(handlers[0].rc);

  return EXIT_FAILURE;
}
