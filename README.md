# Limit Order Book (C++20)

A single-instrument limit order book with price-time priority matching. It
supports limit orders, market orders and cancels, and reports every trade at
the resting order's price. It has no dependencies, and it comes with behaviour
tests, a sanitizer build and a latency benchmark.

## Build and test

Requires a C++20 compiler (tested with GCC 15.2).

```
make test        # build and run the behaviour tests (19 tests)
make test-asan   # same tests under AddressSanitizer + UndefinedBehaviorSanitizer
make bench       # build with -O2 -DNDEBUG and run the benchmark
make clean
```

## Interface

```cpp
ob::OrderBook book(/*expected_orders=*/1'000'000);

auto trades = book.add_limit(1, ob::Side::Sell, 10'050, 100);  // price in ticks
trades      = book.add_market(2, ob::Side::Buy, 40);           // never rests
bool ok     = book.cancel(1);

book.best_bid();                          // std::optional<Price>
book.volume_at(ob::Side::Sell, 10'050);   // total resting quantity at a level
book.order_count();

// Hot-path overloads: fill a caller-owned buffer instead of returning a new vector.
std::vector<ob::Trade> buf;
book.add_limit(3, ob::Side::Buy, 10'040, 10, buf);
```

Prices are integer ticks, never floating point. Order ids must be unique among
resting orders.

## Design

The book has to answer three questions quickly: what the best price is on each
side, who is first in line at that price, and where a given order id is (for
cancel). Each structure below answers one of them.

| Structure | Answers | Why this choice |
|---|---|---|
| `Levels<Better>`: a `std::vector` of `{price, Level}` per side, sorted worst → best | best price; the level for a price | The best level is `back()`, so reading it, or removing it once it's traded through, is O(1). Most new orders land near the top of the book, which is near the end of the vector, so inserting a level shifts only a few elements. Lookup is a binary search over contiguous memory. Bids use `std::greater` and asks use `std::less`, so the same code serves both sides. |
| `Level`: head/tail of an intrusive doubly linked list, plus a cached `total` | who is first in line; `volume_at` | Time priority needs push-back, pop-front and erase from the middle (cancel), all O(1). A `std::vector` or `std::deque` can't erase from the middle in O(1). The cached total makes `volume_at` free of any queue walk. |
| `pool_`: a `std::vector<Node>` with a free list | storage for resting orders | Orders are not individually heap-allocated; a freed slot is reused by the next order. Links are 32-bit indices rather than pointers, so they stay valid when the vector grows. |
| `index_`: `std::unordered_map<OrderId, NodeId>` | where an order id lives | Cancel is O(1) on average with no search. The node records its side and price, so the index stays small. There is exactly one entry per resting order, so `order_count()` is `index_.size()`. |

There is one matching loop, `match()`, shared by `add_limit` and `add_market`,
so the matching rules exist in exactly one place. A market order is a match
with no limit price. Whatever it leaves unfilled is dropped, whereas a limit
order's remainder rests.

Invariants the code relies on:

- A fully filled resting order is removed from both its queue and the index
  in the same step. An incoming order is indexed only if part of it rests,
  which is why `cancel` on a filled id returns `false`.
- An empty level is removed immediately, so `back()` is always a real best
  price.
- Quantities are unsigned. Each fill subtracts `min(incoming, resting)` from
  both, so neither can wrap.

## Complexity

`L` is the number of price levels on one side, and `k` is the number of resting
orders an incoming order trades against.

| Operation | Cost |
|---|---|
| `best_bid` / `best_ask` | O(1) |
| `add_limit`, no cross | O(log L) to find the level, plus O(1) average for the index. Creating a level shifts the levels behind it, O(1) near the top and O(L) at worst. |
| matching | O(1) per fill: the front of the best level, an index erase and a node free. Clearing a level is a `pop_back`. In total O(k + log L). |
| `cancel` | O(1) average index lookup, O(log L) to find the level, O(1) unlink. O(L) only if it empties a level deep in the book. |
| `volume_at` | O(log L) |

## Testing

`tests/test_orderbook.cpp` runs 19 behaviour tests covering:

- resting orders and top of book
- full and partial fills at the resting price
- time priority, including a partially filled order that keeps its place
- price priority across levels, and a limit price that stops a sweep
- market orders, including on an empty side
- cancel from the front, middle and back of a queue, and of filled or unknown ids
- the reusable trade buffer
- a 1000-order sweep

`make test-asan` runs the same tests with ASan and UBSan. These catch memory
errors that tests can miss, such as a read through a dangling list iterator
that happens to return the right value or an out-of-bounds pool access. Every
optimization was checked against this build as well as against the benchmark
checksum, which must not change.

## Benchmark

`bench/bench.cpp` generates 1,000,000 operations with `std::mt19937_64`
(seed 42) before any timing starts:

- 60% limit orders, priced with a geometric offset (mean 3 ticks) from a fixed
  mid. 5% of them are priced through the mid and trade.
- 30% cancels of random earlier limit ids. Some of those orders have already
  been filled, as happens in a real feed.
- 10% market orders.

All quantities are 1–100. About 220k orders are resting at the end.

The benchmark reports two kinds of numbers:

- **Throughput** is the whole replay timed with `steady_clock` and no per-op
  timing, on a fresh book. It is the median of 5 runs.
- **Latency** comes from a separate pass that times each operation and sorts
  the samples. Each sample includes about 35 ns of clock overhead (measured
  back-to-back `now()` calls on this machine).

The traded quantity is summed into a checksum, which is printed. This stops
the compiler from removing the work and confirms that every version produces
the same trades.

Machine: Intel Core i5-8365U @ 1.60 GHz (4C/8T, turbo to 4.1 GHz), 6 MiB L3,
Ubuntu 26.04, GCC 15.2, `-O2`. The benchmark was pinned to one core with
`taskset -c 3`.

### Optimization log

Each optimization was one change, benchmarked against the previous version.
The table was produced by building each commit and running all versions
round-robin 5 times, pinned to one core. Values are medians.

| Change | Throughput (M ops/s) | p50 (ns) | p99 (ns) | p99.9 (ns) | worst op |
|---|---|---|---|---|---|
| Baseline: `std::map` + `std::list` + `unordered_map` | 2.64 | 293 | 1285 | 5511 | 21 ms |
| `reserve()` the order index | 3.69 | 220 | 1139 | 4948 | 68 µs |
| Reusable trade buffer instead of returning a new vector | 4.13 | 218 | 993 | 4486 | (noise) |
| Intrusive list over a pooled node array | 4.51 | 192 | 1058 | 4889 | (noise) |
| Sorted vector of levels instead of `std::map` | 4.34 | 191 | 1032 | 4667 | (noise) |

Worst-op times below about 1 ms come from the OS (preemption and interrupts)
and vary from run to run, so they are not a property of the code.

Latency by operation type for the final version:

| Operation | p50 | p99 | p99.9 |
|---|---|---|---|
| limit | 177 ns | 671 ns | 3.2 µs |
| cancel | 430 ns | 1.2 µs | 6.2 µs |
| market | 279 ns | 1.5 µs | 5.2 µs |

### What I learned

- **Rehashing was the biggest tail-latency problem.** Without `reserve`, one
  limit order took about 21 ms, because inserting it made the hash map
  rehash all ~200k entries. The average barely shows this, but the maximum
  falls by 300×. That is why p99 and p99.9 matter more than the mean in
  trading. A strategy reacts to the market at its slowest moments (bursts are
  exactly when the book is busiest), and a rare multi-millisecond stall can
  mean missed fills or a stale quote at the worst possible time. A good
  average can hide a terrible tail.
- **Allocations cost throughput and tail latency.** The reusable buffer and the
  node pool each removed a `malloc`/`free` from the hot path. Together they
  gave about 22% more throughput. An allocator call is usually fast but
  occasionally slow (lock contention, or asking the OS for memory), and that
  occasional slowness lands in the tail.
- **The sorted vector did not help, and I know why.** Offsets cluster near
  the mid, so each side only has a few dozen levels. A `std::map` that small
  already sits in L1/L2, and the levels were never the bottleneck. When I ran
  the two versions alternately on one pinned core, throughput was equal within
  noise (the 4.51 vs 4.34 above is inside the run-to-run spread). I kept the
  vector because it is no worse, slightly better at p99, and doesn't allocate
  when a level is created.
- **The remaining bottleneck is cache misses on cancel.** Cancel is the
  slowest operation, at more than 2× the p50 of a limit order. It looks up a
  random old id in an index of about 220k entries and touches a random pool
  node. Those structures together are several times larger than the 6 MiB L3,
  so most cancels miss cache twice. Shrinking the level structure can't fix
  that.
- **Benchmark noise on a laptop is large.** Unpinned runs of the same binary
  varied by ±15% because of turbo and thermal throttling. Pinning to one core
  and alternating versions round-robin was what made the comparisons
  trustworthy.

## Limitations and next steps

- **No modify/replace.** Reducing quantity in place would keep queue priority.
  A price change or a quantity increase would be implemented as cancel plus
  new, which loses priority, because otherwise a trader could hold their place
  in line while growing the order.
- **Single instrument, single thread.** To scale, I would run one book per
  instrument, with each book owned by one thread (shard instruments across
  cores). Orders would arrive through lock-free SPSC queues, and trades would
  go out through another queue, so the matching path needs no locks.
- **No input validation.** A duplicate id, a zero or negative price, or a
  quantity overflow at a level are the caller's responsibility.
- **No persistence and no event log**, so the book can't be recovered or
  replayed after a crash.
- **No other order types**, such as IOC, FOK, stop or iceberg orders.
- **Further performance work:**
  - An open-addressing flat hash map, or a direct array if exchange-assigned
    ids are dense, so a cancel costs one cache miss instead of several.
  - A price-indexed array of levels for O(1) level access within a bounded
    price band.
  - Pre-faulting the pool memory.
  - Using `perf` to confirm the cache-miss explanation with counters. This
    wasn't possible on this machine because `perf_event_paranoid` is set to 4.
