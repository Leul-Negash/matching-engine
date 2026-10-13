# Architecture

The system is built in layers. Each layer depends only on the ones below it
and is tested on its own.

| Layer | Files | Responsibility |
|---|---|---|
| Order book | `order_book.hpp/.cpp`, `types.hpp` | One instrument: validation, price-time matching, execution reports. No I/O, no threads. |
| Engine | `engine.hpp/.cpp` | Many instruments; turns commands into sequenced outputs (reports, quotes, depth). Deterministic. |
| Journal | `journal.hpp/.cpp` | Write-ahead log of commands with CRC-32; replay for recovery. |
| Queue | `spsc_queue.hpp` | Lock-free single-producer/single-consumer ring buffer between threads. |
| Protocol | `protocol.hpp/.cpp` | Parse and format the text wire protocol ([PROTOCOL.md](PROTOCOL.md)). |
| Server | `server.hpp/.cpp` | TCP gateway thread plus matching thread; sessions, routing, back-pressure. |
| Apps | `apps/ob_server.cpp`, `apps/ob_loadgen.cpp` | Command-line server with recovery; load generator. |

## Threads and data flow

```
                 gateway thread                              matching thread
            +--------------------------+                +--------------------------+
 clients -->| epoll: accept/read/write |  Command (40 B) | pop batch (<= 256)       |
  (TCP)     | parse line -> Command    | -------------> | journal.append x n       |
            | clordid -> order id      |  SPSC queue in | journal.commit()         |  <- one write (+fsync)
            |                          |                | engine.process x n       |
            | Output -> ACK/FILL/...   | <------------- | push outputs             |
 clients <--| route to owning sessions | SPSC queue out | eventfd -> wake gateway  |
            +--------------------------+                +--------------------------+
                   in_signal.notify (atomic wait / futex) wakes the matching thread
```

Only the matching thread touches the engine and the books, so matching needs
no locks. The two threads share only the two SPSC queues and the wake-up
signals.

### Life of a `NEW` order

1. The gateway reads bytes from the socket into the session's input buffer and
   splits them into lines.
2. `proto::parse` validates the syntax. The gateway then checks that the
   symbol exists and that the clordid isn't live for this session.
3. The gateway assigns the next engine order id and records two mappings:
   `clordid -> (order id, instrument)` in the session, and
   `order id -> (session, clordid)` in `owners`. It pushes a `Command` to the
   inbound queue.
4. The matching thread pops a batch, appends every state-changing command to
   the journal and **commits once for the whole batch** (group commit). Only
   then does it run the batch through the engine. No effect of a command can
   be observed before the command is durable.
5. The book emits `Accepted`, then any `Trade`s, then possibly `Cancelled`. The
   engine wraps each event in an `Output` with a gap-free sequence number, and
   adds a `Quote` output if the top of book changed.
6. The gateway drains the outbound queue and turns each output into lines:
   `ACK` to the owner, `FILL` to **both** the aggressor's and the resting
   order's sessions, and `TRADE`/`BBO` to subscribers. It then flushes the
   socket buffers it wrote to.

## Order book

The design questions are the same as in a textbook order book: what the best
price is, who is first in line at it, and where an order is given its id.

| Structure | Choice | Why |
|---|---|---|
| Price levels per side | `std::vector<{price, Level}>` sorted worst -> best | The best level is `back()`, so reading it or removing it after a sweep is O(1). New levels mostly appear near the top, i.e. near the end of the vector, so inserts shift few elements. Lookup is a binary search over contiguous memory. Bids and asks share code through a `Better` comparator. |
| Queue at a level | Intrusive doubly linked list | Push to the back, pop from the front and unlink from the middle (cancel) are all O(1) with no allocation. |
| Order storage | `std::vector<Node>` pool plus a free list | No per-order heap allocation. Links are 32-bit indices, which stay valid when the vector grows. |
| Order lookup | `std::unordered_map<OrderId, NodeId>` | O(1) cancel and modify. It is reserved up front, because a rehash of a large table stalls for milliseconds. |
| Level totals | 64-bit `Volume` and an order count | `volume_at`, `depth` and `top` are O(1) per level, and summing many 32-bit quantities can't overflow. |

**One lookup per order.** A GTC limit order reserves its index slot with
`try_emplace` before matching. The same hash lookup rejects a duplicate id
and gives `rest()` the slot to fill in. Matching erases only *other* orders'
entries, and `unordered_map::erase` invalidates only the erased element, so
the slot stays valid.

**Self-trade prevention** stops matching when the next resting order belongs
to the incoming order's account. The incoming remainder is cancelled; resting
it would leave the book crossed against its own order.

**Fill-or-kill** pre-checks liquidity without changing the book. It walks
levels best-first and stops at the limit price or at the first order of the
same account, because that is exactly where matching would stop. With no
account it sums whole levels at a time.

**Modify** reduces size in place when the price is unchanged and the quantity
does not grow, which keeps time priority. Otherwise the order is unlinked and
re-executed as a fresh GTC limit order with the same id; this is
cancel/replace, and it may trade. A post-only order is checked before
anything changes, so a rejected modify leaves the order exactly as it was.

| Operation | Cost (`L` = levels on a side, `k` = fills) |
|---|---|
| best bid/ask, top of book | O(1) |
| add, no cross | O(log L) find level + O(1) average index; O(L) worst case to insert a new level |
| match | O(1) per fill; a fully traded level is a `pop_back` |
| cancel | O(1) average index + O(log L) find level + O(1) unlink |
| FOK check | O(levels touched), or O(orders touched) with self-trade prevention |
| depth(n) | O(n) |

## Engine and determinism

`Engine::process` is a pure function of the engine's state and the command.
It uses no clock, no randomness and no thread interleaving. Replaying the same
command sequence therefore always rebuilds the same books, sequence numbers
and order-id high-water mark. That property makes the journal a complete
recovery mechanism (event sourcing). It is also what lets the differential
test compare the book with a reference model step by step.

`Command` is a 40-byte trivially copyable struct with no hidden padding (a
`static_assert` checks this). Callers start from `Command{}`, so all bytes are
defined. That matters because the journal checksums the raw bytes.

## Journal and recovery

```
header   "OBJRNL01" | u32 version | u32 record size
record   u32 CRC-32(command bytes) | Command (40 bytes)
...
```

- **Write-ahead, group commit.** The matching thread appends a whole batch and
  then makes one `write()`, plus one `fdatasync()` with `--fsync`, before
  processing the batch. Under load one sync covers hundreds of commands; in
  the benchmark this gave 63x the throughput of syncing every request.
- **Durability modes.**
  - Without `--fsync`, a committed command has been handed to the kernel. It
    survives a crash of the process but not of the machine.
  - With `--fsync` it survives power loss, at roughly 2 ms per sync on the
    test machine.
- **Torn writes.** A short or checksum-failing *final* record is what a crash
  in the middle of a `write()` looks like. It is discarded and truncated
  before new appends; it was never acknowledged, because acknowledgements
  follow the commit.
- **Corruption.** A bad record *followed by more records* can't be a torn
  write, so startup fails rather than silently dropping later commands.
- **Ids after recovery.** The gateway starts order ids above the highest
  journaled order id. It starts session ids, which double as self-trade
  accounts, above the highest journaled account. Without the second rule a
  new connection could inherit a pre-crash session's account, and its orders
  would be wrongly blocked as self-trades against recovered orders. The smoke
  test caught exactly this bug.

## Gateway

**Order ownership.** `owners` maps an engine order id to its session and
clordid. The entry is removed when the order is finished *and* no cancel or
modify for it is still in flight (`inflight == 0`).

This matters in a race. A client cancels an order that is filled before the
cancel reaches the engine. The fill finishes the order first, but the
engine's `UNKNOWN_ORDER` reject for the cancel arrives afterwards. It can still
be reported with the right clordid, because the entry was kept for it.

**Back-pressure without deadlock.** If the inbound queue is full, the gateway
keeps draining the outbound queue while it waits. The matching thread can
therefore always make progress, even if it is itself blocked on a full
outbound queue.

**Slow consumers.** Replies queue in a per-session buffer. `EPOLLOUT` is
registered only while a socket can't accept more data. More than 8 MiB queued
disconnects the client, so one stalled reader can't grow memory without
bound.

**Fairness.** Each readiness event gets one `read()` of at most 64 KiB, so a
very busy client can't starve the others.

**Cancel on disconnect.** On disconnect the gateway sends a cancel for every
live order of the session. A graceful *server* shutdown does not cancel
anything: GTC orders are meant to survive restarts, and the journal restores
them.

## Waking threads

The matching thread spins briefly when its queue is empty, then sleeps on a
C++20 `atomic::wait` (a futex). The gateway bumps the counter and calls
`notify_one` after pushing.

Lost wake-ups are impossible. The matching thread reads the counter *before*
checking the queue, and `wait(seen)` returns immediately if the counter
changed in between.

The matching thread wakes the gateway by writing to an `eventfd` that is part
of the gateway's epoll set.

`--busy-poll` replaces both sleeps with spinning: `epoll_wait` with a zero
timeout, and a pause loop. Each wake-up costs microseconds on a laptop CPU
with deep sleep states, so this removes most of the tail latency. The price is
two cores at 100%, which is the normal trade on dedicated trading hardware.

## Failure handling

- **Journal write failure.** The matching thread stops and the server shuts
  down with a non-zero exit code. Matching without durability would
  acknowledge orders that could vanish after a restart.
- **Graceful shutdown on SIGINT or SIGTERM.**
  1. Stop accepting new connections.
  2. Let the matching thread finish what is queued; the gateway keeps
     draining outputs so it can't block.
  3. Flush replies to clients.
  4. Exit 0.
- **Invalid configuration** (tick size or lot size of 0, an inverted price
  range, duplicate symbols) is rejected at startup with an exception.
