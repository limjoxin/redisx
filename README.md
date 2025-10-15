# redisx

A tiny Redis-like server written in modern C++20 using standalone Asio.
It speaks RESP, supports strings and hashes, TTL/expiration, and is designed to scale using a sharded, single-writer-per-shard threading model.

**Status:** experimental / learning project. API/behavior may change.

## Features

- **RESP protocol** (compatible with `redis-cli` for the supported commands)
- **String** and **Hash** data types
- **TTL / Expire** with Redis semantics  
  - `TTL key` → `-2` (no key), `-1` (no TTL), or remaining seconds `≥ 0`
  - `EX`/`PX` on `SET`, `EXPIRE`, `PEXPIRE`, `PERSIST`
- **Lazy + periodic expiration**  
  - *Lazy:* keys are removed when accessed if expired  
  - *Periodic:* background sweep per shard  
  - Optional min-heap index for precise wakeups
- **Multithreaded design**  
  - One thread per shard (single writer), plus I/O thread for acceptor  
  - No cross-shard locks on the hot path
- **Clean separation**  
  - Session (network I/O) → Router (routing) → Store/Shard (execution)

## Supported commands (MVP)

### Strings
- `PING [msg]`, `ECHO msg`
- `SET key value [EX sec | PX ms]`
- `GET key`
- `DEL key`
- `EXISTS key [key ...]`
- `MGET key [key ...]`
- `MSET key value [key value ...]`

### TTL
- `TTL key`, `EXPIRE key sec`, `PEXPIRE key ms`, `PERSIST key`

### Hashes
- `HSET key field value`, `HGET key field`, `HDEL key field`
- `HEXISTS key field`, `HLEN key`, `HGETALL key`
- `HMGET key field [field ...]`

> **Note:** Multi-key commands operate per key; true cross-shard ops are a future improvement.

### Misc
- `TYPE key` – returns one of `none|string|hash`


## Build & Test Quickstart

Below are the build and smoke-test steps to get you running quickly.

### Prerequisites

- A C++20 compiler (GCC 11+, Clang 12+, MSVC 19.3x+)
- CMake **3.20+**
- On Linux/macOS: POSIX threads (usually already available)
- Git (optional, to fetch Asio)

### Getting Asio (standalone)

This project uses standalone Asio (no Boost). The CMake file expects headers at `deps/asio/include`. Choose one:

**Option A – Git submodule (recommended)**

```bash
git submodule add https://github.com/chriskohlhoff/asio.git deps/asio
git -C deps/asio checkout asio-1-28-0   # or any recent tag
```

**Option B – Manual download**

1. Download a release of standalone Asio.  
2. Place the folder so headers are available at `deps/asio/include/asio.hpp`.

### Configure & build

```bash
# From the repo root
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This will produce two binaries:

- `build/redisx-server`
- `build/redis-cli`

(On Windows, binaries will be under your generator’s output directory, e.g. `build/Release/…`.)

### Run the server

```bash
./build/redisx-server
```

Common flags:

- `--port N` or `-p N` – listen on port `N` (default `6379`)
- `--shards N` – number of shards (default: auto, based on hardware concurrency)
- `--help` or `-?` – show usage

Examples:

```bash
./build/redisx-server --port 6380
./build/redisx-server --shards 8
```

On startup you should see something like:

```
redisx RESP server on 6379 with 8 shards ...
```

The server performs a TTL sweep roughly every ~200 ms to expire keys.

### Use the interactive CLI

```bash
./build/redis-cli
```

Connect flags:

- `-h, --host HOST` – host to connect to (default `127.0.0.1`)
- `-p, --port PORT` – port to connect to (default `6379`)
- `-?, --help` – show usage

Examples:

```bash
./build/redis-cli                  # default localhost:6379
./build/redis-cli -h 127.0.0.1 -p 6380
```

Quit the client by typing `QUIT` or pressing `Ctrl+C`.

### Smoke test

1. Start the server in one terminal:
   ```bash
   ./build/redisx-server
   ```

2. In another terminal, start the client and run a few commands:
   ```text
   $ ./build/redis-cli
   Connected to 127.0.0.1:6379
   Type commands like:  PING  |  SET a "hello"  |  GET a  |  EXPIRE a 2
   Ctrl+C to quit.
   > PING
   PONG
   > SET a "hello"
   OK
   > GET a
   "hello"
   > EXPIRE a 1
   (integer) 1
   > GET a
   "hello"
   # wait ~1 second
   > GET a
   (nil)
   ```

**Notes on typing commands**

- The client accepts shell-like tokenization with quotes and simple escapes, so both of these are valid:
  - `SET msg "hello world"`
  - `SET msg 'hello world'`
- To include special characters inside double quotes you can escape: `"`, `
`, `	`, etc.


## Repository layout

```
.
├─ app/
│  ├─ main.cpp          # redisx-server entrypoint
│  └─ redis-cli.cpp     # interactive client
├─ include/redisx/      # project headers (expected)
├─ src/                 # project sources (expected)
├─ deps/asio/include/   # standalone Asio headers (expected)
└─ CMakeLists.txt
```

> **Note**: The `include/redisx`, `src`, and `deps/asio/include` folders are expected by the build. If they’re missing locally, follow the steps above to add Asio and place your headers/sources accordingly.


## Design & Implementation Notes

- **Sharding & routing:** Keys hash to shards; commands route by key so operations are single-writer per shard. Multi-key commands operate per-key with no cross-shard transactions.

- **Data structures:** Strings live in a `map<string,string>`, hashes in `unordered_map<string, unordered_map<string,string>>`, with a `ttl` map storing absolute expiration time points.

- **Lazy expiration:** On access, if a key's TTL is in the past, the key (string or hash) and its TTL metadata are removed.

- **Periodic sweep:** Each shard periodically scans TTLs and erases expired keys.

- **TTL semantics:** `TTL` returns `-2` when the key does not exist or has already expired, `-1` when it exists without TTL, otherwise remaining seconds (rounded up from ms). `EXPIRE`/`PEXPIRE`/`PERSIST` update or clear TTL only when the key exists.

- **Type checks:** A command operating on strings will reply `-WRONGTYPE` if the key currently holds a hash (and vice versa) for safety.
