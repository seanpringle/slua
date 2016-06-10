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

int
work_accept (lua_State *lua)
{
  char *message = channel_read(&shared->jobs);

  process_t *process = (process_t*)strtoull(message, &message, 0);
  global.self->result = &process->results;

  if (strtol(message, &message, 0) == 1) lua_pushstring(lua, ++message); else lua_pushnil(lua);

  lua_getglobal(lua, "work");
  lua_pushstring(lua, "job");
  lua_pushvalue(lua, -3);
  lua_settable(lua, -3);
  lua_pop(lua, 1);

  store_free(global.store, message);
  return 1;
}

int
work_answer (lua_State *lua)
{
  char *payload = lua_type(lua, -1) == LUA_TSTRING ? (char*)lua_popstring(lua): NULL;
  char *message = strf("%d %s", payload ? 1:0, payload);
  channel_write(global.self->result, store_slot(global.store, store_set(global.store, message, strlen(message)+1)));
  free(message);
  return 0;
}

int
work_submit (lua_State *lua)
{
  ensure(cfg.worker_path || cfg.worker_code)
    errorf("no workers");

  char *payload = lua_type(lua, -1) == LUA_TSTRING ? (char*)lua_popstring(lua): NULL;
  char *message = strf("%lld %d %s", (uint64_t*)global.self, payload ? 1:0, payload);
  channel_write(&shared->jobs, store_slot(global.store, store_set(global.store, message, strlen(message)+1)));
  free(message);
  return 0;
}

int
work_collect (lua_State *lua)
{
  ensure(cfg.worker_path || cfg.worker_code)
    errorf("no workers");

  char *message = channel_read(&global.self->results);
  if (strtol(message, &message, 0) == 1) lua_pushstring(lua, ++message); else lua_pushnil(lua);
  store_free(global.store, message);

  return 1;
}

int
work_pool (lua_State *lua)
{
  lua_pushnumber(lua, cfg.max_workers);
  return 1;
}

int
work_idle (lua_State *lua)
{
  lua_pushnumber(lua, channel_readers(&shared->jobs));
  return 1;
}

int
work_backlog (lua_State *lua)
{
  lua_pushnumber(lua, channel_backlog(&shared->jobs));
  return 1;
}

int
work_active (lua_State *lua)
{
  size_t readers, writers, backlog;

  if (channel_backlog(&global.self->results) > 0)
  {
    lua_pushboolean(lua, 1);
    return 1;
  }

  for (;;)
  {
    channel_wait(&shared->jobs, 100000, &backlog, &readers, &writers);

    if (backlog > 0 || channel_backlog(&global.self->results) > 0)
    {
      lua_pushboolean(lua, 1);
      return 1;
    }

    if (readers == cfg.max_workers)
    {
      lua_pushboolean(lua, 0);
      return 1;
    }
  }
}
