// Benchmark: replays a pre-generated, repeatable order stream through the book
// and reports throughput plus per-operation latency percentiles.

#include "orderbook.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

using namespace ob;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::size_t kOps = 1'000'000;
constexpr std::uint64_t kSeed = 42;
constexpr Price kMid = 10'000;  // ticks
constexpr int kRuns = 5;
constexpr std::size_t kExpectedOrders = kOps;  // upper bound on resting orders

enum class Kind : std::uint8_t { Limit, Cancel, Market };

struct Op {
    Kind kind;
    Side side;
    OrderId id;
    Price price;
    Quantity qty;
};

// ~60% limit orders clustered around the mid (most sit within a few ticks of the
// top, a few cross the spread), ~30% cancels of earlier limit ids, ~10% market.
std::vector<Op> generate() {
    std::mt19937_64 rng(kSeed);
    std::uniform_real_distribution<double> kind_dist(0.0, 1.0);
    std::bernoulli_distribution side_dist(0.5);
    std::geometric_distribution<int> depth_dist(0.25);  // mean 3 ticks from the mid
    std::bernoulli_distribution cross_dist(0.05);
    std::uniform_int_distribution<Quantity> qty_dist(1, 100);

    std::vector<Op> ops;
    ops.reserve(kOps);
    std::vector<OrderId> cancellable;  // limit ids not yet chosen for a cancel
    cancellable.reserve(kOps);
    OrderId next_id = 1;

    for (std::size_t i = 0; i < kOps; ++i) {
        const double r = kind_dist(rng);
        const Side side = side_dist(rng) ? Side::Buy : Side::Sell;

        if (r < 0.30 && !cancellable.empty()) {
            // Pick a random earlier limit id; it may since have been filled, in
            // which case cancel() returns false, as it would in a real feed.
            std::uniform_int_distribution<std::size_t> pick(0, cancellable.size() - 1);
            const std::size_t j = pick(rng);
            const OrderId id = cancellable[j];
            cancellable[j] = cancellable.back();
            cancellable.pop_back();
            ops.push_back({Kind::Cancel, side, id, 0, 0});
        } else if (r < 0.40) {
            ops.push_back({Kind::Market, side, next_id++, 0, qty_dist(rng)});
        } else {
            // Passive orders rest 1+ ticks behind the mid; an occasional
            // aggressive one is priced through it and trades.
            Price offset = 1 + depth_dist(rng);
            if (cross_dist(rng)) offset = -offset;
            const Price price = side == Side::Buy ? kMid - offset : kMid + offset;
            const OrderId id = next_id++;
            ops.push_back({Kind::Limit, side, id, price, qty_dist(rng)});
            cancellable.push_back(id);
        }
    }
    return ops;
}

// One trade buffer reused for every call, so the hot loop never allocates it.
std::vector<Trade> trades;

inline std::uint64_t apply(OrderBook& book, const Op& op) {
    std::uint64_t traded = 0;
    switch (op.kind) {
        case Kind::Limit:
            book.add_limit(op.id, op.side, op.price, op.qty, trades);
            for (const Trade& t : trades) traded += t.quantity;
            break;
        case Kind::Market:
            book.add_market(op.id, op.side, op.qty, trades);
            for (const Trade& t : trades) traded += t.quantity;
            break;
        case Kind::Cancel:
            traded += book.cancel(op.id);
            break;
    }
    return traded;
}

double percentile_ns(const std::vector<std::int64_t>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    const std::size_t i = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
    return static_cast<double>(sorted[i]);
}

}  // namespace

int main() {
    const std::vector<Op> ops = generate();
    trades.reserve(1024);
    std::size_t counts[3] = {};
    for (const Op& op : ops) ++counts[static_cast<int>(op.kind)];
    std::printf("ops: %zu  (limit %zu, cancel %zu, market %zu), seed %llu\n\n", ops.size(),
                counts[0], counts[1], counts[2], static_cast<unsigned long long>(kSeed));

    // 1) Throughput: time the whole replay, no per-op clock reads. Each run uses
    //    a fresh book; report the median of kRuns runs.
    std::uint64_t checksum = 0;
    std::vector<double> rates;
    for (int run = 0; run < kRuns; ++run) {
        OrderBook book(kExpectedOrders);
        std::uint64_t sink = 0;
        const auto start = Clock::now();
        for (const Op& op : ops) sink += apply(book, op);
        const auto end = Clock::now();
        const double secs = std::chrono::duration<double>(end - start).count();
        rates.push_back(static_cast<double>(ops.size()) / secs);
        checksum = sink;  // identical every run: the stream is deterministic
    }
    std::sort(rates.begin(), rates.end());
    std::printf("throughput: %.2f M ops/s (median of %d runs; min %.2f, max %.2f)\n",
                rates[kRuns / 2] / 1e6, kRuns, rates.front() / 1e6, rates.back() / 1e6);

    // 2) Latency: time each operation individually on another fresh book.
    //    Includes ~20 ns of clock-read overhead per sample.
    std::vector<std::int64_t> all(ops.size());
    std::vector<std::int64_t> by_kind[3];
    for (int k = 0; k < 3; ++k) by_kind[k].reserve(counts[k]);
    {
        OrderBook book(kExpectedOrders);
        std::uint64_t sink = 0;
        for (std::size_t i = 0; i < ops.size(); ++i) {
            const auto start = Clock::now();
            sink += apply(book, ops[i]);
            const auto end = Clock::now();
            const std::int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            all[i] = ns;
            by_kind[static_cast<int>(ops[i].kind)].push_back(ns);
        }
        if (sink != checksum) std::printf("WARNING: checksum mismatch between runs\n");
        std::printf("resting orders at end: %zu\n\n", book.order_count());
    }

    std::sort(all.begin(), all.end());
    std::printf("latency (ns)   p50      p99      p99.9    max\n");
    auto row = [](const char* name, std::vector<std::int64_t>& v) {
        std::sort(v.begin(), v.end());
        std::printf("  %-10s %6.0f   %6.0f   %6.0f   %8lld\n", name, percentile_ns(v, 0.50),
                    percentile_ns(v, 0.99), percentile_ns(v, 0.999),
                    static_cast<long long>(v.empty() ? 0 : v.back()));
    };
    row("all", all);
    row("limit", by_kind[0]);
    row("cancel", by_kind[1]);
    row("market", by_kind[2]);

    // Printing the checksum keeps the optimizer from discarding the work.
    std::printf("\nchecksum (traded qty + successful cancels): %llu\n",
                static_cast<unsigned long long>(checksum));
    return 0;
}
