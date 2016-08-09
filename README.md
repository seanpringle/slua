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

## Non-standard Lua functions

| Function | Description |
| --- | --- |
| `client.line()` | Read a line from the client socket. (Handlers only) |
| `client.read()` | Read all data from the client socket. (Handlers only) |
| `client.write(str)` | Write data to the client socket. (Handlers only) |
| `io.ls(path)` | List a directory. |
| `io.stat(path)` | Stat a file/directory/link. |
| `json.decode(str)` | JSON decode. |
| `json.encode(table)` | JSON encode (currently must be a table). |
| `os.usleep(usecs)` | A `usleep()` wrapper. |
| `print(...)` | Overridden version of Lua `print()` to use `client.write()`. |
| `eprint(...)` | Print to `stderr`. |
| `string.epoch(str,format)` | Convert a date string to UNIX timestamp. A `strptime()` wrapper. |
| `string.iconv(str,from,to)` | Convert character set. An `iconv()` wrapper. |
| `string.md5(str)` | MD5 hash. |
| `string.pcrematch(str)` | PCRE library. |
| `string.sha1(str)` | SHA1 hash. |
| `string.sha256(str)` | SHA256 hash. |
| `string.sha512(str)` | SHA512 hash. |
| `work.accept()` | Accept a job from the queue. |
| `work.active()` | True if jobs are still being processed. (Handlers only), (Atomic) |
| `work.answer()` | Return a job result. |
| `work.backlog()` | Length of the job queue. |
| `work.collect()` | Collect a job result. (Handlers only) |
| `work.idle()` | Number of idle workers. |
| `work.job` | The current job, set by each call to `work.accept()`. |
| `work.pool()` | Number of workers. |
| `work.submit(str)` | Submit a job to the queue. |
