# Performance

Machine: Intel Core i5-8365U (4 cores / 8 threads, 1.6 GHz base, 4.1 GHz
turbo, 6 MiB L3), NVMe SSD, Ubuntu 26.04, Linux 7.0, GCC 15.2, `-O2`.

It's a laptop: turbo and thermal throttling move results by ±15% between runs.
The book benchmarks below were run pinned to one core (`taskset -c 3`). Where
two versions are compared, they were run alternately, and the tables show
medians.

## Order book (`make bench`)

The benchmark replays 1,000,000 pre-generated operations from a fixed seed:

- 60% limit orders around a fixed mid, with a geometric offset (mean 3 ticks);
  5% of them cross.
- 30% cancels of earlier orders, some of which have already been filled.
- 10% market orders.

About 220k orders are resting at the end.

- **Throughput:** the whole replay timed in one go.
- **Latency:** each operation timed separately in another pass. Each sample
  includes about 35 ns of clock overhead.

Current version:

| | p50 | p99 | p99.9 |
|---|---|---|---|
| all operations | 222 ns | 1.09 µs | 4.7 µs |
| limit | 211 ns | 0.81 µs | 3.0 µs |
| cancel | 382 ns | 1.07 µs | 6.0 µs |
| market | 317 ns | 1.47 µs | 5.9 µs |

Throughput is **3.8 M ops/s**.

### Optimization history

Each step changed one thing and was measured against the previous one.

| Change | M ops/s | p50 | p99 | worst op |
|---|---|---|---|---|
| Baseline: `std::map` levels, `std::list` queues, `unordered_map` index | 2.64 | 293 ns | 1285 ns | 21 ms |
| `reserve()` the index | 3.69 | 220 ns | 1139 ns | 68 µs |
| Reusable output buffer instead of a new vector per call | 4.13 | 218 ns | 993 ns | (noise) |
| Intrusive list over a pooled node array | 4.51 | 192 ns | 1058 ns | (noise) |
| Sorted vector of levels instead of `std::map` | 4.34 | 191 ns | 1032 ns | (noise) |
| Production features (see below) | 3.8 | 222 ns | 1090 ns | (noise) |

Worst-op times below about 1 ms come from the OS (preemption, interrupts) and
vary from run to run.

What each step taught:

- **Rehashing was the biggest tail problem.** Without `reserve`, one insert
  rehashed about 200k entries and took 21 ms, a 300× worse maximum, yet it
  barely moved the average. This is why trading systems watch p99 and p99.9
  rather than the mean: the rare stall lands during bursts, exactly when
  latency matters most.
- **Allocations cost throughput and tail latency.** Removing per-call vectors
  and per-order list nodes gave about 22% more throughput. `malloc` is usually
  fast but occasionally slow.
- **The sorted vector did not help, and that was expected.** The workload keeps
  only a few dozen levels per side, so the `std::map` was already in cache. I
  kept the vector because it is no worse, and slightly better at p99.
- **Production features cost about 10%.** These are an acknowledgement event
  per order, validation, self-trade checks and a 40-byte node (up from 32).

  I measured this against the previous commit, alternating, to separate it
  from thermal noise; a first, unpinned run had suggested 22%.

  I tested two explanations that turned out wrong:
  - *Node size crossing cache lines.* Padding the old node to 40 bytes changed
    nothing.
  - *A second hash lookup per order.* Removing it with the slot-reservation
    trick helped only a little.

  `gprof` showed cancel at about 35% of the time, dominated by the
  cache-missing hash lookup, as before. The remaining cost is spread across
  the extra work.
- **The remaining bottleneck is cache misses on cancel.** A cancel looks up a
  random old id in a 220k-entry hash table and touches a random pool node.
  Together these are several times the size of L3. A flat open-addressing
  table would cut this from several misses to about one.

## End to end (`make bench-e2e`)

`ob_server` and `ob_loadgen` run on the same machine over loopback TCP. The
mix is 70% GTC limit orders, 30% IOC orders that may cross, and cancels of
earlier orders. Round-trip time runs from just before `send()` to the arrival
of the first reply (`ACK`, `CANCELED` or `REJECT`).

The two scenarios:
- **Unloaded:** 1 connection with 1 request in flight, which shows pure
  latency.
- **Loaded:** 4 connections with 32 requests in flight each, which shows
  throughput.

| Mode | Unloaded p50 | Unloaded p99 | Loaded throughput | Loaded p99 |
|---|---|---|---|---|
| No journal | 54 µs | 458 µs | 288k req/s | 2.7 ms |
| Journal (write per batch) | 73 µs | 1.8 ms | 285k req/s | 1.9 ms |
| Journal + fsync per batch | 2.08 ms | 6.3 ms | 28k req/s | 8.6 ms |
| Journal, `--busy-poll` | 39 µs | 66 µs | 385k req/s | 0.96 ms |

The load generator shares the 4 cores with the server, so it competes for
CPU. Loaded latency is mostly queueing: by Little's law, 128 requests in
flight at about 290k req/s is about 440 µs of waiting.

- **Group commit.** With fsync, each sync takes about 2 ms on this SSD. One
  request at a time therefore manages 445 req/s, but under load one sync
  covers a whole batch and throughput reaches 28k req/s, 63× more. This is why
  the journal commits per batch rather than per command.
- **tmpfs makes fsync free.** The first fsync benchmark wrote to `/tmp`, which
  is RAM-backed `tmpfs` here, and fsync appeared to cost nothing. The
  benchmark now writes the journal under `build/` on the real disk.
- **Wake-ups dominate unloaded tail latency.** Each request crosses three
  sleeping threads. The gateway wakes from `epoll_wait`, the matching thread
  from a futex, and the gateway again from an `eventfd`. Each wake-up can cost
  tens of microseconds when the core is in a deep sleep state.

  `--busy-poll` keeps both threads spinning. With the journal on, that cuts
  unloaded p99 from 1.8 ms to 66 µs and raises loaded throughput from 285k to
  385k req/s (+35%). The price is two cores at 100%.
- **Where the time goes.** The book itself takes about 0.2 µs per operation;
  almost all of the 39 µs round trip is the kernel network stack, syscalls and
  thread hand-offs.

  Going much lower would take a different architecture:
  - kernel-bypass networking such as DPDK or Solarflare ef_vi;
  - a binary protocol;
  - one thread doing both I/O and matching, pinned to an isolated core.
