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

## Lua Libaries

The Lua instance in each process is normal, so `require()` and `LUA_PATH` should behave as expected.

## Non-standard Lua functions

Some of this functionality duplicates perfectly good community libraries. The intention isn't to re-invent a bunch of wheels, but to provide a little more variety in the standard library, plus take advantage of functionality offered by libraries that are going to be linked anyway (eg, OpenSSL).

The JSON methods are the exception: they're fine for basic tasks, but should probably be overriden for serious stuff.

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

## Command line arguments

| Argument | Long Form | Description |
| --- | --- | --- |
| -p *port* | --port *port* | TCP listen on *port*. |
| -su *user* | --setuid *user* | Switch to *user* (`setuid()`). |
| -w *path* | --worker *path* | Worker script. |
| -wp *size* | --worker-pool *size* | Worker pool size. |
| -h *path* | --handler *path* | Handler script. |
| -hp *size* | --handler-pool *size* | Handler maximum pool size. |
| -mj *value* | --max-jobs *value* | Maximum concurrent queued jobs. |
| -mr *value* | --max-results *value* | Maximum concurrent queued results per Handler. |
| -sh *MB* | --shared-memory *MB* | Amount of shared memory in MB to allocate for IPC. |
| -sp *B* | --shared-page *B* | Size of a page within shared memory (unlikely this would need changing). |
| n/a | --ssl-cert *path* | TCP SSL mode. Requires `--ssl-key` and `-p`. Implicitly turns on SSL mode. |
| n/a | --ssl-key *path* | TCP SSL mode. |
