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

unsigned char msgbuf[1024];

void
message_send (channel_t *send, channel_t *recv, const char *payload)
{
  size_t length = sizeof(message_t) + (payload ? strlen(payload)+1: 0);

  message_t *msg = length <= sizeof(msgbuf) ? (message_t*)(&msgbuf): malloc(length);
  memset(msg, 0, sizeof(message_t));

  msg->is_nil = payload ? 0:1;
  msg->respond = recv;

  if (payload)
    strcpy(msg->payload, payload);

  channel_write(send, msg, length);

  if (length > sizeof(msgbuf)) free(msg);
}

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
  message_send(global.self->result, NULL, payload);
  return 0;
}

int
work_submit (lua_State *lua)
{
  ensure(cfg.worker_path || cfg.worker_code)
    errorf("no workers");

  char *payload = lua_type(lua, -1) == LUA_TSTRING ? (char*)lua_popstring(lua): NULL;
  message_send(&shared->jobs, global.self->type == HANDLER ? &global.self->results: global.self->result, payload);
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
  size_t readers;
  channel_counters(&shared->jobs, NULL, &readers, NULL, NULL);
  lua_pushnumber(lua, readers);
  return 1;
}

int
work_backlog (lua_State *lua)
{
  size_t backlog;
  channel_counters(&shared->jobs, NULL, &backlog, NULL, NULL);
  lua_pushnumber(lua, backlog);
  return 1;
}

int
work_active (lua_State *lua)
{
  size_t readers, writers, backlog;

  for (;;)
  {
    channel_counters(&shared->jobs, NULL, &backlog, &readers, &writers);

    if (backlog > 0 || writers > 0)
    {
      lua_pushboolean(lua, 1);
      return 1;
    }

    channel_counters(&global.self->results, NULL, &backlog, NULL, &writers);

    if (backlog > 0 || writers > 0)
    {
      lua_pushboolean(lua, 1);
      return 1;
    }

    if (readers == cfg.max_workers)
    {
      lua_pushboolean(lua, 0);
      return 1;
    }

    usleep(1000);
  }
}
