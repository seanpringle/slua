# Slua

Multi-process Lua with channel-based IPC.

## Normal Mode

* One Lua process, like the stand-alone Lua interpreter with a few extra built-in functions

```
$ slua 'print("hello world")'
```

## STDIO Mode

* One Lua process submitting jobs, standard I/O
* Mutiple Lua workers processing jobs

```lua
-- handler.lua

work.submit('hello')
work.submit('goodbye')
work.submit('really, goodbye')

print(work.collect())
print(work.collect())
print(work.collect())

-- worker.lua

while true do
  work.answer(work.accept() .. 'world')
end
```

```
$ slua handler.lua worker.lua
hello world
goodbye world
really, goodbye world
```

## TCP Mode

* Mutiple Lua handlers submitting jobs, TCP I/O (optionally SSL)
* Mutiple Lua workers processing jobs
