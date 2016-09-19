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
json_encode (lua_State *lua)
{
  char *null_str = NULL;

  if (lua_gettop(lua) == 2)
  {
    if (lua_type(lua, -1) == LUA_TSTRING)
      null_str = strdup(lua_tostring(lua, -1));
    lua_pop(lua, 1);
  }

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

          if (null_str && str_eq(null_str, (char*)lua_tostring(lua, -1)))
          {
            free(val);
            val = strdup("null");
          }

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
  free(null_str);

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
    lua_checkstack(lua, lua_gettop(lua)+10);

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
        while (json && *json && isalpha(*json)) json++;
        pushed++;
        break;

      case 't':
        lua_pushboolean(lua, 1);
        while (json && *json && isalpha(*json)) json++;
        pushed++;
        break;

      case 'f':
        lua_pushboolean(lua, 0);
        while (json && *json && isalpha(*json)) json++;
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
      errorf("unexpected state, mode: %d, pushed: %d, at %s", mode, pushed, last);
      while (json && *json) json++;
      lua_pushnil(lua);
      break;
    }
  }

  while (json && *json && strchr(", \t\r\n", *json)) json++;
  return json;
}

int
json_decode (lua_State *lua)
{
  char *str = (char*)lua_popstring(lua);
  char *json = str ? strdup(str): NULL;

  if (!json || !strchr("{[", *json))
  {
    free(json);
    lua_pushnil(lua);
    return 1;
  }

  int c = *json;
  json_decode_step(lua, json+1, c == '{' ? 2: 1);
  free(json);
  return 1;
}
