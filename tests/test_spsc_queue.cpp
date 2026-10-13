// Tests for the lock-free single-producer/single-consumer queue. The two
// thread test is the one that matters; run it under `make test-tsan`.

#include "ob/spsc_queue.hpp"
#include "test.hpp"

#include <cstdint>
#include <thread>
#include <vector>

using ob::SpscQueue;

TEST(spsc_rounds_capacity_and_reports_full_and_empty) {
    bool ok = true;
    SpscQueue<int> q(5);
    CHECK(q.capacity() == 8);
    int v = 0;
    CHECK(!q.try_pop(v));
    for (int i = 0; i < 8; ++i) CHECK(q.try_push(i));
    CHECK(!q.try_push(99));
    CHECK(q.size() == 8);
    CHECK(q.try_pop(v) && v == 0);
    CHECK(q.try_push(8));  // a freed slot is reusable
    return ok;
}

TEST(spsc_preserves_fifo_order_across_wraparound) {
    bool ok = true;
    SpscQueue<std::uint64_t> q(4);
    std::uint64_t next_in = 0;
    std::uint64_t next_out = 0;
    std::uint64_t buf[3];
    for (int round = 0; round < 1000; ++round) {
        while (q.try_push(next_in)) ++next_in;
        const std::size_t n = q.try_pop(buf, 3);
        for (std::size_t i = 0; i < n; ++i) CHECK(buf[i] == next_out++);
    }
    while (q.try_pop(buf[0])) CHECK(buf[0] == next_out++);
    CHECK(next_in == next_out && q.empty());
    return ok;
}

TEST(spsc_two_threads_deliver_everything_in_order) {
    bool ok = true;
    constexpr std::uint64_t kCount = 2'000'000;
    SpscQueue<std::uint64_t> q(1024);

    std::thread producer([&] {
        for (std::uint64_t i = 1; i <= kCount; ++i) {
            while (!q.try_push(i)) std::this_thread::yield();
        }
    });

    std::uint64_t expected = 1;
    std::uint64_t sum = 0;
    bool in_order = true;
    std::vector<std::uint64_t> batch(64);
    while (expected <= kCount) {
        const std::size_t n = q.try_pop(batch.data(), batch.size());
        if (n == 0) {
            std::this_thread::yield();
            continue;
        }
        for (std::size_t i = 0; i < n; ++i) {
            in_order = in_order && batch[i] == expected;
            sum += batch[i];
            ++expected;
        }
    }
    producer.join();

    CHECK(in_order);
    CHECK(sum == kCount * (kCount + 1) / 2);
    CHECK(q.empty());
    return ok;
}
