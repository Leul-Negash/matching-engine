# Limit Order Book and Matching Engine (C++20)

This is a multi-instrument exchange matching engine. It does price-time
priority matching and serves a TCP order-entry gateway. It keeps a crash-safe
write-ahead journal and passes the matching thread work through lock-free
queues. It has no dependencies beyond the C++ standard library and Linux.

```
$ ob_server --symbol AAPL --symbol MSFT:5:10 --journal data/ob.journal
```

```
A> SUB AAPL                              B> NEW b1 AAPL BUY 120 15005 IOC
A> NEW s1 AAPL SELL 100 15000            B< ACK b1 3
A> NEW s2 AAPL SELL 50 15010             B< FILL b1 15000 100 20
A< SUBSCRIBED AAPL                       B< CANCELED b1 20 UNFILLED
A< ACK s1 1                              B> BOOK AAPL
A< BBO AAPL - 0 15000 100                B< LEVEL AAPL ASK 15010 50 1
A< ACK s2 2                              B< BOOKEND AAPL
A< FILL s1 15000 100 0
A< TRADE AAPL 15000 100 BUY
A< BBO AAPL - 0 15010 50
```

## Features

**Matching**
- Price-time priority. Trades execute at the resting order's price.
- Limit and market orders, with GTC, IOC and FOK time in force, and post-only.
- Cancel and modify. Reducing an order's size keeps its queue position; a
  price change or a size increase re-queues it at the back.
- Self-trade prevention per account.
- Per-instrument rules: tick size, lot size, price range and maximum order
  size, with explicit reject reasons.
- 64-bit level volumes, and negative prices where configured.

**System**
- A gateway thread and a matching thread, connected by lock-free SPSC queues.
  Matching needs no locks.
- Write-ahead journal with a CRC-32 per record and group commit, with optional
  fsync.
- Recovery replays the journal through the engine, which is deterministic. A
  torn final write is discarded; mid-file corruption stops startup.
- Execution reports to both counterparties, a public trade and BBO feed, and
  depth snapshots. Every engine output carries a gap-free sequence number.
- Client order ids, cancel-on-disconnect, per-session rate limits,
  slow-consumer disconnect and a session limit.
- Graceful shutdown on SIGINT/SIGTERM, and an optional busy-poll mode for the
  lowest latency.

**Engineering**
- 61 unit and behaviour tests.
- A differential test against an independent reference model, covering 300k
  random operations per run.
- An end-to-end smoke test, including `kill -9` crash recovery.
- The test suite also runs under ASan+UBSan and under TSan. `make check` runs
  all of these plus the smoke test, and the CI workflow runs the same stages.
- A book microbenchmark and an end-to-end TCP load generator.

## Build and run

Requires Linux, a C++20 compiler, GNU make, and Python 3 for the smoke test.
It was developed with GCC 15.2; the CI workflow uses GCC 14.

```
make                 # build everything (release) into build/release
make test            # unit, behaviour and differential tests
make test-asan       # the same tests under AddressSanitizer + UBSan
make test-tsan       # the same tests under ThreadSanitizer
make smoke           # start the server and test it over TCP, including crash recovery
make check           # tests, sanitizer runs and smoke test (the stages CI runs)
make bench           # order book microbenchmark
make bench-e2e       # server + load generator, for each persistence mode
```

Run the server:

```
./build/release/ob_server --symbol AAPL --symbol MSFT:5:10 --port 9000 --journal data/ob.journal
```

`--symbol SYM[:tick[:lot]]` defines an instrument. Other options:

- `--fsync`: survive power loss, not just a process crash.
- `--busy-poll`: lowest latency, at the cost of two cores at 100%.
- `--no-cancel-on-disconnect`
- `--max-sessions N`
- `--max-rate N`: messages per second per session.
- `--bind ADDR`
- `--log-level LEVEL`

Connect with any line-based client, for example `nc 127.0.0.1 9000`. The
messages are documented in [docs/PROTOCOL.md](docs/PROTOCOL.md).

Measure it:

```
./build/release/ob_loadgen --port 9000 --symbol AAPL --connections 4 --window 32 --requests 100000
```

## Using the library

The book has no I/O and no threads, so it can be embedded directly:

```cpp
#include "ob/order_book.hpp"

ob::BookConfig cfg;
cfg.tick_size = 5;
cfg.expected_orders = 1'000'000;      // pre-size storage: no rehash stalls
ob::OrderBook book(cfg);

std::vector<ob::Event> events;        // reused; the book only appends
book.submit({.id = 1, .account = 7, .side = ob::Side::Sell, .type = ob::OrderType::Limit,
             .tif = ob::TimeInForce::GTC, .price = 10'050, .qty = 100}, events);
book.submit({.id = 2, .account = 9, .side = ob::Side::Buy, .type = ob::OrderType::Limit,
             .tif = ob::TimeInForce::IOC, .price = 10'100, .qty = 40}, events);
// events: ACCEPTED 1, ACCEPTED 2, TRADE 2 x 1 @ 10050 qty 40 (leaves 0, counter leaves 60)
book.modify(1, 10'050, 50, events);   // size reduction: keeps queue position
book.cancel(1, events);
```

`ob::Engine` adds instruments, sequencing and quotes on top of this, and
`ob::Server` adds networking.

## Project layout

```
include/ob/   public headers: types, order_book, engine, journal, spsc_queue, protocol, server, log
src/          implementations
apps/         ob_server (gateway + recovery), ob_loadgen (load generator)
tests/        unit, behaviour, differential, engine/journal, queue and protocol tests
bench/        order book microbenchmark
scripts/      smoke_test.py (end to end), bench_e2e.sh
docs/         ARCHITECTURE.md, PROTOCOL.md, PERFORMANCE.md
```

## How it's tested

| Layer | Approach |
|---|---|
| Matching rules | Behaviour tests for every rule: priority, partial fills, each time in force, post-only, self-trade prevention, modify priority, validation. |
| Book correctness | **Differential testing.** 150 seeded random streams of 2,000 requests run through the book and a naive reference model, which uses linear scans and checks fill-or-kill by trial-matching on a copy. They must agree exactly on every event and on the book state after every step. The streams cover crossing prices, shared accounts, duplicate and dead ids and invalid input. When I planted an off-by-one in the FOK check, the test caught it within 418 operations and printed both event streams. |
| Journal | Replay rebuilds identical state from 5,000 commands; appends survive restarts; a torn tail is discarded and truncated; mid-file corruption and foreign files are refused; CRC-32 is checked against the standard check value. |
| Concurrency | A two-thread SPSC test with 2M items checks ordering and completeness, under ThreadSanitizer. Weakening one `release` store to `relaxed` makes TSan report the race. |
| Memory safety | The whole suite and the server run under ASan and UBSan. |
| System | `smoke_test.py` drives the real server over TCP. It covers fills reaching both counterparties, market data, rejects, cancel-on-disconnect, `kill -9` followed by recovery, id continuity after restart, the session limit and graceful shutdown. It fails on any sanitizer report from the server, and it has also been run against the ASan and TSan builds. |

## Performance

On an Intel i5-8365U laptop (details and method in
[docs/PERFORMANCE.md](docs/PERFORMANCE.md)):

| | |
|---|---|
| Order book, 1M mixed operations | 3.8M ops/s; p50 222 ns, p99 1.1 µs |
| End to end over TCP, unloaded, busy-poll | p50 39 µs, p99 66 µs round trip |
| End to end, 4 clients × 32 in flight | 285k req/s with the journal, 385k with busy-poll |
| End to end with fsync per batch | 28k req/s; group commit gives 63× the throughput of one request at a time |

## Design

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) covers the threading model, the
order book data structures and their complexity, the journal format and
recovery rules, gateway order ownership, back-pressure and failure handling.

## Limitations and next steps

- **No authentication or real accounts.** Each TCP connection is its own
  account. A real venue would have logins, entitlements and pre-trade risk
  checks (credit limits, position limits, price collars).
- **Order ownership isn't persisted.** After a restart, recovered orders keep
  trading, but their fills can't be routed to the original client because
  sessions don't survive a restart. A real system would persist session state
  or provide a drop copy.
- **The journal grows forever.** It needs periodic snapshots of the books, so
  that recovery replays only the tail, and log rotation.
- **One matching thread for all instruments.** The next step is to shard
  instruments across several matching threads, each with its own queues and
  journal.
- **No replication or failover.** Production engines replicate the sequenced
  command stream to a hot standby before acknowledging.
- **Text protocol over kernel TCP.** Lower latency would need a binary
  protocol (FIX/SBE or ITCH/OUCH style), UDP multicast market data, and kernel
  bypass.
- **Linux only** (epoll, eventfd), **IPv4 only**, and the journal files
  assume a little-endian machine.
- **Hash index on cancel.** A flat open-addressing hash table would cut cancel
  latency, the book's main remaining cost.
