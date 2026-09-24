# tcp_load_balancer

A Layer-4 TCP load balancer for Linux, written in C++17. Non-blocking POSIX
sockets, `epoll`, `std::thread` — no Boost, no libevent. It accepts client
TCP connections and relays bytes to a pool of backends with round-robin
selection, health checking, and per-IP rate limiting.

## Features

- **Full-duplex relay** — two 64 KB buffers per connection
  (`client→backend`, `backend→client`), partial-write leftovers kept with
  `EPOLLOUT`, backpressure via `EPOLLIN` toggling, half-close forwarding.
- **Multithreaded** — N workers, each with its own `epoll` instance and its
  own `SO_REUSEPORT` listen socket. No locks in the hot path.
- **Round-robin backend pool** — lock-free atomic counter, skips unhealthy
  backends, stays even when degraded.
- **Health checker** — background TCP probes with
  fail-after-K / healthy-after-M hysteresis, logs every transition.
- **Rate limiter** — per-client-IP token bucket, 16-way sharded map,
  idle-entry cleanup thread.
- **Hardened** — clean under AddressSanitizer+UBSan and ThreadSanitizer;
  RAII file descriptors, graceful shutdown on `SIGINT`/`SIGTERM`.

## Architecture

```text
                        ┌────────────────────────────────────────┐
                        │              Listener (main)           │
                        │  owns BackendPool, spawns N Workers,   │
                        │  HealthChecker + RateLimiter threads   │
                        └───────┬────────────────┬───────────────┘
                                │ shared (atomics│/sharded locks)
            ┌───────────────────┼────────────────┼───────────────────┐
            │                   │                │                   │
     ┌──────────────┐   ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
     │   Worker 0   │   │   Worker 1   │  │   Worker 2   │  │   Worker N   │
     │ listen+epoll │   │ listen+epoll │  │ listen+epoll │  │ listen+epoll │
     │ (REUSEPORT)  │   │ (REUSEPORT)  │  │ (REUSEPORT)  │  │ (REUSEPORT)  │
     │  accept loop │   │  accept loop │  │  accept loop │  │  accept loop │
     │ Connections* │   │ Connections* │  │ Connections* │  │ Connections* │
     └──────┬───────┘   └──────┬───────┘  └──────┬───────┘  └──────┬───────┘
            └──────────────────┼────────────────┼───────────────────┘
                      round-robin pick()  │  (skips unhealthy)
                                          ▼
                        ┌────────────────────────────────────────┐
                        │  BackendPool: 9001, 9002, 9003 ...     │
                        │  HealthChecker flips healthy flags     │
                        └────────────────────────────────────────┘

* Each Connection = client Fd + backend Fd + two 64 KB buffers,
  touched by exactly one worker thread.
```

**Connection flow:** accept (non-blocking) → rate-limit check → `pick()`
backend → non-blocking `connect()` (`EINPROGRESS`, finished on `EPOLLOUT` +
`SO_ERROR`) → relay in the worker's `epoll` loop → on `recv()==0`, flush
queued bytes and propagate `shutdown(WR)`; tear down when both sides hit
EOF and buffers drain, or on error. Either side closing never loses
already-read bytes.

## Requirements

- Linux, CMake ≥ 3.16, g++ (C++17) with pthreads.
- Python 3 for the test tools. No third-party C++ dependencies.

## Build

```bash
cmake -B build -S .            # -Wall -Wextra -O2, C++17
cmake --build build -j$(nproc)
./build/lb --help
```

Sanitizer builds (uses the `-DSANITIZE=` hook in `CMakeLists.txt`):

```bash
cmake -B build-asan -S . -DSANITIZE=address,undefined
cmake --build build-asan -j
cmake -B build-tsan -S . -DSANITIZE=thread
cmake --build build-tsan -j
```

> Note: in some containers ThreadSanitizer aborts at startup with
> `unexpected memory mapping` (sandbox ASLR layout — reproducible even
> with hello-world). Workaround: `setarch $(uname -m) -R ./build-tsan/lb`.

## Run

Start three echo backends and the balancer:

```bash
python3 tools/echo_server.py 9001 &
python3 tools/echo_server.py 9002 &
python3 tools/echo_server.py 9003 &
./build/lb --listen 8080 \
  --backends 127.0.0.1:9001,127.0.0.1:9002,127.0.0.1:9003 \
  --workers 4
printf 'hello' | nc -N 127.0.0.1 8080   # -> backend-900X: hello
```

Or via config file (see `lb.conf.example`):

```bash
./build/lb --config lb.conf.example
```

Shutdown is graceful: `SIGINT`/`SIGTERM` stops accepting on all workers,
drains in-flight connections (5 s cap), joins every thread, exits 0.

## Configuration

| Flag | File key | Default | Range | Meaning |
|---|---|---|---|---|
| `--listen PORT` | `listen_port` | `8080` | 1–65535 | Local port to accept on |
| `--backends a:b,...` | `backends` | — (required) | — | Backend `host:port` list |
| `--workers N` | `workers` | `4` | 1–64 | Worker threads (own epoll each) |
| `--health-interval SEC` | `health_interval` | `2` | 1–3600 | Probe period per backend |
| `--health-timeout MS` | `health_timeout_ms` | `1000` | 50–60000 | Connect timeout per probe |
| `--fail-after K` | `fail_after` | `2` | 1–100 | DOWN after K failed probes |
| `--healthy-after M` | `healthy_after` | `2` | 1–100 | UP after M successful probes |
| `--rate RPS` | `rate` | `0` | 0–1000000 | Tokens/sec per IP (`0` = unlimited) |
| `--burst N` | `burst` | `100` | 1–1000000 | Bucket capacity per IP |
| `--config PATH` | — | — | — | `key=value` file; CLI flags override it |

## Design decisions

- **Why `epoll` (level-triggered)?** One thread per worker drives
  hundreds of connections with plain non-blocking `recv`/`send` loops.
  Level-triggered mode means a missed event is re-delivered, so
  backpressure bookkeeping (disarming `EPOLLIN`) can't lose wakeups —
  correctness first, with no measurable cost at this scale.
- **Why per-worker loops + `SO_REUSEPORT`?** The alternative (single
  acceptor + queue/eventfd handoff) adds cross-thread fd passing, a queue
  mutex, and an acceptor bottleneck. With `SO_REUSEPORT` the kernel
  distributes accepts fairly, every worker is self-contained
  (listen + epoll + connections), and the hot path is 100% lock-free.
  Measured accept spread over 51 connections on 4 workers: 12/12/11/16.
- **Why sharded token bucket?** One global `mutex` around the IP map
  would serialize every accept across workers. 16 shards (hashed by IP,
  own mutex each) make contention negligible; refill is computed lazily
  from the monotonic clock, and a background thread expires entries idle
  over 60 s so memory can't grow with unique-IP scans.
- **Why counter-modulo-healthy round-robin?** The naive
  "slot-then-scan-forward" doubles traffic onto the successor of a dead
  node (measured 2/4 split with one of three down). Counting healthy
  first and taking `counter % healthy` keeps survivors even (measured
  3/3) with the same single atomic op.
- **Why hysteresis in health checks?** Single-probe flapping would
  bounce traffic on every slow probe. K-failures-down / M-successes-up
  (defaults 2/2) absorbs one-off blips; only transitions are logged.

## Benchmarks

Measured on this machine (loopback, plain `-O2` build). Reproduce with
the commands shown.

| Test | Command | Result |
|---|---|---|
| 1200 concurrent × 4 KB, verified echo | `python3 tools/load_test.py --conns 1200 --payload 4096 --workers 4` | **1200/1200 ok, 0 fail, 3.14 s** |
| 100 MB single-stream sha256 echo | `python3 tools/test_100mb.py --mb 100` | **104,857,600 B both ways, sha match, 0.56 s (~189 MB/s)** |
| 400 concurrent × 64 KB (backpressure) | `load_test.py --conns 400 --payload 65536` | **400/400 ok** |
| Distribution, 30 sequential reqs / 3 backends | `nc` loop | **10 / 10 / 10** |
| Failover (one backend killed) | 6 reqs | **3 / 3 / 0**, `UP -> DOWN` logged |
| Rejoin (backend restarted) | 6 reqs | **2 / 2 / 2**, `DOWN -> UP` logged |
| Rate limit (`--rate 10 --burst 5`, 100 rapid conns) | `python3 tools/test_ratelimit.py` | **5 accepted / 95 rejected**, all 95 logged |
| Edge cases | `python3 tools/edge_test.py` | backend-death EOF (no hang), RST cleanup, 2 MB slow-reader sha match, **fds 11 → 11** |

Notes: the 1200-conn figure is dominated by the Python harness (asyncio
client + thread-per-connection echo server), not the balancer — the
single-stream number (~189 MB/s loopback) is the closer proxy for
forwarding throughput. `ab`/`wrk` were deliberately not used: they speak
HTTP and the test backends echo raw bytes.

Sanitizers: `build-asan` (address+undefined, incl. 400×64KB run, RST and
mid-transfer-death paths) and `build-tsan` (1200-conn run plus limiter
traffic) reported **zero** warnings; LeakSanitizer is clean at shutdown.

## Testing

| Script | What it checks |
|---|---|
| `tools/echo_server.py [port] [--raw]` | Test backend (prefixes replies, or raw echo) |
| `tools/load_test.py --conns 1200` | 1000+ concurrent verified echoes + fd stability |
| `tools/test_100mb.py --mb 100` | Bulk-transfer sha256 through 64 KB buffers |
| `tools/test_ratelimit.py` | 100 rapid conns → mix of accepted/rejected |
| `tools/edge_test.py` | Backend death, client RST, slow reader, fd leaks |

## Layout

```text
include/  Config, Logger, Fd (RAII), SocketUtil,
          Listener (manager), Worker (per-thread loop), Connection (relay),
          Backend (pool), HealthChecker, RateLimiter
src/      one .cpp per header + main.cpp (signals, startup)
tools/    echo + load/edge/ratelimit/100MB test scripts
lb.conf.example   commented sample config
```
