
int
db_read_cb (void *ptr, int cols, char **values, char **fields)
{
  lua_State *lua = ptr;

  int rows = lua_popnumber(lua);

  lua_pushnumber(lua, ++rows);

  // row
  lua_createtable(lua, 0, 0);

  for (int i = 0; i < cols; i++)
  {
    lua_pushstring(lua, fields[i]);
    if (values[i]) lua_pushstring(lua, values[i]); else lua_pushlightuserdata(lua, NULL);
    lua_settable(lua, -3);
  }

  // push to rows
  lua_settable(lua, -3);

  lua_pushnumber(lua, rows);

  return 0;
}

int
db_read (lua_State *lua)
{
  char *errmsg = NULL;
  char *sql = (char*)lua_popstring(lua);

  // rows
  lua_createtable(lua, 0, 0);

  // #rows
  lua_pushnumber(lua, 0);

  if (sqlite3_exec(self->db, sql, db_read_cb, lua, &errmsg) != SQLITE_OK)
  {
    // drop rows, #rows
    lua_pop(lua, 2);
    lua_pushnil(lua);
    lua_pushstring(lua, errmsg);

    if (errmsg) sqlite3_free(errmsg);

    return 2;
  }

  // drop #rows
  lua_pop(lua, 1);
  return 1;
}

int
db_write (lua_State *lua)
{
  char *errmsg = NULL;
  char *sql = (char*)lua_popstring(lua);

  if (sqlite3_exec(self->db, sql, NULL, NULL, &errmsg) != SQLITE_OK)
  {
    lua_pushnil(lua);
    lua_pushstring(lua, errmsg);

    if (errmsg) sqlite3_free(errmsg);

    return 2;
  }

  lua_pushnumber(lua, sqlite3_changes(self->db));
  return 1;
}

int
db_escape (lua_State *lua)
{
  char *str = (char*)lua_popstring(lua);

  if (!str)
  {
    lua_pushnil(lua);
    return 1;
  }

  char *res = malloc(strlen(str)*2+1), *p = res;

  while (*str)
  {
    if (*str == '\'')
      *p++ = '\'';
    *p++ = *str++;
  }

  *p = 0;

  lua_pushstring(lua, res);
  free(res);
  return 1;
}
