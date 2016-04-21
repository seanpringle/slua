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
  message_t *message = channel_read(&jobs);
  if (message->payload) lua_pushstring(lua, message->payload); else lua_pushnil(lua);
  self->result = message->result;
  free(message->payload);
  free(message);
  return 1;
}

int
work_try_accept (lua_State *lua)
{
  message_t *message = NULL;

  if (channel_try_read(&jobs, (void**)&message))
  {
    lua_pushboolean(lua, 1);
    if (message->payload) lua_pushstring(lua, message->payload); else lua_pushnil(lua);
    self->result = message->result;
    free(message->payload);
    free(message);
    return 2;
  }

  lua_pushboolean(lua, 0);
  return 1;
}

int
work_answer (lua_State *lua)
{
  channel_write(self->result,
    lua_type(lua, -1) == LUA_TSTRING ? strdup((char*)lua_popstring(lua)) : NULL);
  return 0;
}

int
work_submit (lua_State *lua)
{
  ensure(cfg.worker_path || cfg.worker_code)
    errorf("no workers");

  message_t *message = malloc(sizeof(message_t));
  message->result = self->type == HANDLER ? &self->results: self->result;
  message->payload = lua_type(lua, -1) == LUA_TSTRING ? strdup((char*)lua_popstring(lua)) : NULL;

  channel_write(&jobs, message);
  return 0;
}

int
work_collect (lua_State *lua)
{
  ensure(cfg.worker_path || cfg.worker_code)
    errorf("no workers");

  char *payload = channel_read(&self->results);
  if (payload) lua_pushstring(lua, payload); else lua_pushnil(lua);
  free(payload);
  return 1;
}

int
work_try_collect (lua_State *lua)
{
  ensure(cfg.worker_path || cfg.worker_code)
    errorf("no workers");

  char *payload = NULL;
  if (channel_try_read(&self->results, (void**)&payload))
  {
    lua_pushboolean(lua, 1);
    if (payload) lua_pushstring(lua, payload); else lua_pushnil(lua);
    free(payload);
    return 2;
  }

  lua_pushboolean(lua, 0);
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
  lua_pushnumber(lua, channel_readers(&jobs));
  return 1;
}

int
work_backlog (lua_State *lua)
{
  lua_pushnumber(lua, channel_backlog(&jobs));
  return 1;
}

int
work_todo (lua_State *lua)
{
  ensure(pthread_mutex_lock(&jobs.mutex) == 0);
  int backlog = jobs.backlog;
  int readers = jobs.readers;
  ensure(pthread_mutex_unlock(&jobs.mutex) == 0);
  lua_pushboolean(lua, backlog == 0 && readers == cfg.max_workers);
  return 1;
}
