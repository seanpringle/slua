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

#include <openssl/md5.h>

int
hash_md5 (lua_State *lua)
{
  unsigned char digest[MD5_DIGEST_LENGTH];
  const char *str = lua_popstring(lua);

  MD5((unsigned char*)str, strlen(str), digest);

  char hstr[MD5_DIGEST_LENGTH*2+1];
  for(int i = 0; i < MD5_DIGEST_LENGTH; ++i)
    sprintf(&hstr[i*2], "%02x", (unsigned int)digest[i]);

  lua_pushstring(lua, hstr);
  return 1;
}

int
hash_sha1 (lua_State *lua)
{
  unsigned char digest[SHA_DIGEST_LENGTH];
  const char *str = lua_popstring(lua);

  SHA1((unsigned char*)str, strlen(str), digest);

  char hstr[SHA_DIGEST_LENGTH*2+1];
  for(int i = 0; i < SHA_DIGEST_LENGTH; ++i)
    sprintf(&hstr[i*2], "%02x", (unsigned int)digest[i]);

  lua_pushstring(lua, hstr);
  return 1;
}
