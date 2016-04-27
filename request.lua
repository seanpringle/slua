--[[
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
--]]

db.write([[
  CREATE TABLE t1 (c1 int)
]])

db.write([[
  INSERT INTO t1 VALUES (42)
]])

work.submit("hello")
print(work.collect())

t = { hello = "world", "snaffle", 3.14, 1.0, { "what", "the", "heck?" } }
s = table.json_encode(t, "world")

print(s)
tt = table.json_decode(s)
print(tt["4"][2])

t = { "world", "snaffle", 3.14, 1.0, "what" }
s = table.json_encode(t)

error(s)

for k,v in pairs(table.json_decode(s)) do
  print(k.." => "..v)
end


print(1,2,3)

