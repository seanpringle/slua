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
  message_t *msg = channel_read(&shared->jobs);
  global.self->result = msg->respond;

  if (!msg->is_nil) lua_pushstring(lua, msg->payload); else lua_pushnil(lua);

  lua_getglobal(lua, "work");
  lua_pushstring(lua, "job");
  lua_pushvalue(lua, -3);
  lua_settable(lua, -3);
  lua_pop(lua, 1);

  free(msg);
  return 1;
}

int
work_answer (lua_State *lua)
{
  char *payload = lua_type(lua, -1) == LUA_TSTRING ? (char*)lua_popstring(lua): NULL;
  size_t length = sizeof(message_t) + (payload ? strlen(payload)+1: 0);

  message_t *msg = calloc(length, 1);
  msg->is_nil = payload ? 0:1;
  msg->respond = NULL;
  if (payload) strcpy(msg->payload, payload);

  channel_write(global.self->result, msg, length);

  free(msg);
  return 0;
}

int
work_submit (lua_State *lua)
{
  ensure(cfg.worker_path || cfg.worker_code)
    errorf("no workers");

  char *payload = lua_type(lua, -1) == LUA_TSTRING ? (char*)lua_popstring(lua): NULL;
  size_t length = sizeof(message_t) + (payload ? strlen(payload)+1: 0);

  message_t *msg = calloc(length, 1);
  msg->is_nil = payload ? 0:1;
  msg->respond = global.self->type == HANDLER ? &global.self->results: global.self->result;
  if (payload) strcpy(msg->payload, payload);

  channel_write(&shared->jobs, msg, length);

  free(msg);
  return 0;
}

int
work_collect (lua_State *lua)
{
  ensure(cfg.worker_path || cfg.worker_code)
    errorf("no workers");

  message_t *msg = channel_read(&global.self->results);
  if (!msg->is_nil) lua_pushstring(lua, msg->payload); else lua_pushnil(lua);
  free(msg);

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
