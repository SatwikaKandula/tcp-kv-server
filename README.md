# tcp-kv-server

A multithreaded, in-memory key-value server in C, built on POSIX sockets and pthreads.

## Features

- **TCP server on raw POSIX sockets**: `socket` / `bind` / `listen` / `accept`, with `poll()` on the listening socket.
- **Fixed-size thread pool**: worker threads pull client connections from a bounded circular queue guarded by a mutex and two condition variables. The bounded queue gives backpressure when the server is saturated.
- **Thread-safe hash table with lock striping**: 4,096 buckets share 64 reader-writer locks, so operations on different stripes never contend and concurrent readers share a stripe.
- **Manual memory management**: every key and value is heap-allocated and freed on overwrite, delete, and shutdown. AddressSanitizer and LeakSanitizer report zero leaks.
- **Stream-safe protocol parsing**: handles partial reads, multiple commands in one `recv()`, pipelining, and partial `send()` writes.
- **Graceful shutdown**: `SIGINT`/`SIGTERM` set an atomic flag; the accept loop stops, workers drain the queue, threads are joined, and all memory is freed.

## Protocol

One command per line:

| Command | Reply |
|---|---|
| `SET <key> <value>` | `OK` (values may contain spaces) |
| `GET <key>` | `VALUE <value>` or `NOT_FOUND` |
| `DEL <key>` | `DELETED` or `NOT_FOUND` |
| `PING` | `PONG` |
| `QUIT` | `BYE`, then the connection closes |

## Build and run

```bash
make                     # builds ./kvserver and ./bench
./kvserver -p 7070 -t 8  # port 7070, 8 worker threads
```

Try it from another terminal:

```bash
nc localhost 7070
SET name satwika
GET name
```

## Testing

```bash
make test                               # 16 functional, pipelining, concurrency, shutdown tests
make tsan && ./tests/run_tests.sh ./kvserver-tsan   # ThreadSanitizer: data races
make asan && ./tests/run_tests.sh ./kvserver-asan   # AddressSanitizer + UBSan + leak check
```

The concurrency test runs 32 clients in parallel and checks every reply, not just throughput.

## Benchmark

```bash
./bench -p 7070 -c 32 -n 20000 -r 80   # 32 clients, 20K requests each, 80% reads
```

Results on a 2-core Linux VM, 8 worker threads, 80% reads, 0 errors:

| Clients | Throughput | p50 latency | p99 latency |
|---|---|---|---|
| 8 | ~147K req/s | ~50 µs | ~129 µs |
| 32 | ~124K req/s | ~51 µs | ~182 µs |
| 64 | ~132K req/s | ~54 µs | ~182 µs |

Numbers vary by machine; rerun the benchmark on yours.

## Bug found by ThreadSanitizer

The first version used `volatile sig_atomic_t` for the shutdown flag. That type is only guaranteed safe between a signal handler and the thread it interrupts, but worker threads also read the flag, which is a data race under the C11 memory model. TSan flagged it, and the fix was a lock-free C11 `atomic_int`, which is both signal-safe and thread-safe. The handler also saves and restores `errno`.

## Design trade-offs and next steps

- **Thread-per-connection via the pool.** Each worker serves one connection until it closes, so with more open connections than workers, extra connections wait in the queue. That's fine for short-lived clients but limits idle long-lived ones. The next step is an `epoll`-based event loop that hands individual requests (not whole connections) to the pool.
- **Fixed bucket count.** The table doesn't resize; dynamic rehashing under striped locks is a planned extension.
- **No persistence.** Data lives only in memory; an append-only log would add durability.

## Layout

```
src/server.c      sockets, accept loop, protocol parsing, signal handling
src/threadpool.c  bounded job queue and worker threads
src/kvstore.c     striped-lock hash table
src/bench.c       concurrent load generator with reply verification
tests/run_tests.sh
```
