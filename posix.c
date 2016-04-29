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

#include <sys/stat.h>
#include <dirent.h>
#include <pwd.h>
#include <grp.h>

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

  struct stat st, sst;

  int lok = lstat(path, &st)  == 0;

  if (lok)
  {
    char mode[11];
    strcpy(mode, "----------");
    int i = 0;

    if (S_ISDIR(st.st_mode))  mode[0] = 'd'; i = 1;
    if (S_ISLNK(st.st_mode))  mode[0] = 'l'; i = 1;

    if (st.st_mode & S_IRUSR) mode[i] = 'r'; i++;
    if (st.st_mode & S_IWUSR) mode[i] = 'w'; i++;
    if (st.st_mode & S_IXUSR) mode[i] = 'x'; i++;
    if (st.st_mode & S_IRGRP) mode[i] = 'r'; i++;
    if (st.st_mode & S_IWGRP) mode[i] = 'w'; i++;
    if (st.st_mode & S_IXGRP) mode[i] = 'x'; i++;
    if (st.st_mode & S_IROTH) mode[i] = 'r'; i++;
    if (st.st_mode & S_IWOTH) mode[i] = 'w'; i++;
    if (st.st_mode & S_IXOTH) mode[i] = 'x'; i++;

    int gsize = max(8192, (int) sysconf(_SC_GETGR_R_SIZE_MAX));
    char *gbuff = malloc(gsize);
    struct group _grp, *grp = &_grp;

    int grs = getgrgid_r(st.st_gid, grp, gbuff, gsize, &grp);

    if (grs != 0)
      errorf("getgrgid_r %s %d", path, grs);

    int psize = max(8192, (int) sysconf(_SC_GETPW_R_SIZE_MAX));
    char *pbuff = malloc(psize);
    struct passwd _pwd, *pwd = &_pwd;

    int prs = getpwuid_r(st.st_uid, pwd, pbuff, psize, &pwd);

    if (prs != 0)
      errorf("getpwuid_r %s %d", path, prs);

    lua_createtable(lua, 0, 0);

    lua_pushstring(lua, "type");

         if (S_ISDIR(st.st_mode)) lua_pushstring(lua, "directory");
    else if (S_ISLNK(st.st_mode)) lua_pushstring(lua, "link");
    else if (S_ISCHR(st.st_mode)) lua_pushstring(lua, "cdev");
    else if (S_ISBLK(st.st_mode)) lua_pushstring(lua, "bdev");
    else if (S_ISFIFO(st.st_mode)) lua_pushstring(lua, "fifo");
    else if (S_ISREG(st.st_mode)) lua_pushstring(lua, "file");
    else lua_pushstring(lua, "unknown");

    lua_settable(lua, -3);

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

    if (S_ISLNK(st.st_mode))
    {
      int sok = stat(path, &sst) == 0;

      lua_pushstring(lua, "intact");
      lua_pushboolean(lua, sok);
      lua_settable(lua, -3);

      char *target = NULL;

      int limit = 1024;
      char *buffer = malloc(limit+1);

      while (buffer && limit < 1024*1024)
      {
        int bytes = readlink(path, buffer, limit);
        if (bytes < 0)
        {
          free(buffer);
          buffer = NULL;
          break;
        }
        if (bytes < limit)
        {
          buffer[bytes] = 0;
          target = buffer;
          buffer = NULL;
          break;
        }
        limit += 1024;
        buffer = realloc(buffer, limit+1);
      }

      free(buffer);

      lua_pushstring(lua, "target");
      lua_pushstring(lua, target ? target: "");
      lua_settable(lua, -3);

      free(target);
    }

    free(gbuff);
    free(pbuff);
  }
  else
  {
    lua_pushnil(lua);
  }
  return 1;
}
