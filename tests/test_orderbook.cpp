// Behaviour tests for the order book. They define what "done" means:
// make test should print every test as PASS.

#include "orderbook.hpp"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace ob;

static int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("    line %d: CHECK(%s) failed\n", __LINE__, #cond); \
            ok = false;                                                    \
        }                                                                  \
    } while (0)

struct Test {
    std::string name;
    std::function<bool()> fn;
};

static std::vector<Test>& registry() {
    static std::vector<Test> tests;
    return tests;
}

struct Register {
    Register(const char* name, std::function<bool()> fn) { registry().push_back({name, std::move(fn)}); }
};

#define TEST(name)                                  \
    static bool name();                             \
    static Register reg_##name(#name, name);        \
    static bool name()

// ---------------------------------------------------------------- resting

TEST(empty_book_has_no_prices) {
    bool ok = true;
    OrderBook book;
    CHECK(!book.best_bid());
    CHECK(!book.best_ask());
    CHECK(book.order_count() == 0);
    return ok;
}

TEST(non_crossing_orders_rest) {
    bool ok = true;
    OrderBook book;
    CHECK(book.add_limit(1, Side::Buy, 100, 10).empty());
    CHECK(book.add_limit(2, Side::Sell, 105, 5).empty());
    CHECK(book.best_bid() == 100);
    CHECK(book.best_ask() == 105);
    CHECK(book.order_count() == 2);
    return ok;
}

TEST(best_prices_track_the_top_of_book) {
    bool ok = true;
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 1);
    book.add_limit(2, Side::Buy, 102, 1);
    book.add_limit(3, Side::Buy, 101, 1);
    book.add_limit(4, Side::Sell, 110, 1);
    book.add_limit(5, Side::Sell, 108, 1);
    CHECK(book.best_bid() == 102);
    CHECK(book.best_ask() == 108);
    return ok;
}

TEST(volume_aggregates_per_level) {
    bool ok = true;
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 10);
    book.add_limit(2, Side::Buy, 100, 15);
    book.add_limit(3, Side::Buy, 99, 7);
    CHECK(book.volume_at(Side::Buy, 100) == 25);
    CHECK(book.volume_at(Side::Buy, 99) == 7);
    CHECK(book.volume_at(Side::Sell, 100) == 0);
    return ok;
}

// ---------------------------------------------------------------- matching

TEST(full_fill_at_resting_price) {
    bool ok = true;
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 10);
    auto trades = book.add_limit(2, Side::Buy, 101, 10);  // willing to pay more
    CHECK(trades.size() == 1);
    if (trades.size() == 1) {
        CHECK(trades[0].buy_id == 2);
        CHECK(trades[0].sell_id == 1);
        CHECK(trades[0].price == 100);  // resting order sets the price
        CHECK(trades[0].quantity == 10);
    }
    CHECK(book.order_count() == 0);
    CHECK(!book.best_ask());
    CHECK(!book.best_bid());
    return ok;
}

TEST(partial_fill_leaves_remainder_resting) {
    bool ok = true;
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 4);
    auto trades = book.add_limit(2, Side::Buy, 100, 10);
    CHECK(trades.size() == 1 && trades[0].quantity == 4);
    CHECK(!book.best_ask());
    CHECK(book.best_bid() == 100);
    CHECK(book.volume_at(Side::Buy, 100) == 6);
    return ok;
}

TEST(partially_filled_resting_order_keeps_its_place) {
    bool ok = true;
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 10);
    book.add_limit(2, Side::Sell, 100, 10);
    book.add_limit(3, Side::Buy, 100, 4);  // takes 4 from order 1
    auto trades = book.add_limit(4, Side::Buy, 100, 8);
    CHECK(trades.size() == 2);
    if (trades.size() == 2) {
        CHECK(trades[0].sell_id == 1 && trades[0].quantity == 6);
        CHECK(trades[1].sell_id == 2 && trades[1].quantity == 2);
    }
    return ok;
}

TEST(time_priority_within_a_level) {
    bool ok = true;
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 5);
    book.add_limit(2, Side::Buy, 100, 5);
    book.add_limit(3, Side::Buy, 100, 5);
    auto trades = book.add_limit(4, Side::Sell, 100, 12);
    CHECK(trades.size() == 3);
    if (trades.size() == 3) {
        CHECK(trades[0].buy_id == 1 && trades[0].quantity == 5);
        CHECK(trades[1].buy_id == 2 && trades[1].quantity == 5);
        CHECK(trades[2].buy_id == 3 && trades[2].quantity == 2);
    }
    CHECK(book.volume_at(Side::Buy, 100) == 3);
    return ok;
}

TEST(price_priority_across_levels) {
    bool ok = true;
    OrderBook book;
    book.add_limit(1, Side::Sell, 103, 5);
    book.add_limit(2, Side::Sell, 101, 5);
    book.add_limit(3, Side::Sell, 102, 5);
    auto trades = book.add_limit(4, Side::Buy, 103, 12);
    CHECK(trades.size() == 3);
    if (trades.size() == 3) {
        CHECK(trades[0].sell_id == 2 && trades[0].price == 101);
        CHECK(trades[1].sell_id == 3 && trades[1].price == 102);
        CHECK(trades[2].sell_id == 1 && trades[2].price == 103 && trades[2].quantity == 2);
    }
    return ok;
}

TEST(limit_price_stops_the_sweep) {
    bool ok = true;
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5);
    book.add_limit(2, Side::Sell, 105, 5);
    auto trades = book.add_limit(3, Side::Buy, 102, 10);
    CHECK(trades.size() == 1 && trades[0].sell_id == 1);
    CHECK(book.best_bid() == 102);  // remainder rests at its own limit
    CHECK(book.volume_at(Side::Buy, 102) == 5);
    CHECK(book.best_ask() == 105);
    return ok;
}

// ---------------------------------------------------------------- market

TEST(market_order_sweeps_and_never_rests) {
    bool ok = true;
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 5);
    book.add_limit(2, Side::Buy, 98, 5);
    auto trades = book.add_market(3, Side::Sell, 20);
    CHECK(trades.size() == 2);
    if (trades.size() == 2) {
        CHECK(trades[0].price == 100 && trades[1].price == 98);
    }
    CHECK(book.order_count() == 0);
    CHECK(!book.best_ask());  // unfilled remainder is dropped
    return ok;
}

TEST(market_order_on_empty_side_does_nothing) {
    bool ok = true;
    OrderBook book;
    CHECK(book.add_market(1, Side::Buy, 10).empty());
    CHECK(book.order_count() == 0);
    return ok;
}

// ---------------------------------------------------------------- cancel

TEST(cancel_removes_resting_order) {
    bool ok = true;
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 5);
    book.add_limit(2, Side::Buy, 100, 7);
    CHECK(book.cancel(1));
    CHECK(book.volume_at(Side::Buy, 100) == 7);
    CHECK(book.order_count() == 1);
    return ok;
}

TEST(cancel_last_order_clears_the_level) {
    bool ok = true;
    OrderBook book;
    book.add_limit(1, Side::Buy, 101, 5);
    book.add_limit(2, Side::Buy, 100, 5);
    CHECK(book.cancel(1));
    CHECK(book.best_bid() == 100);
    return ok;
}

TEST(cancel_unknown_or_filled_returns_false) {
    bool ok = true;
    OrderBook book;
    CHECK(!book.cancel(42));
    book.add_limit(1, Side::Sell, 100, 5);
    book.add_limit(2, Side::Buy, 100, 5);  // fills order 1 completely
    CHECK(!book.cancel(1));
    CHECK(!book.cancel(2));  // the incoming order never rested
    return ok;
}

TEST(cancelled_order_is_skipped_by_matching) {
    bool ok = true;
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5);
    book.add_limit(2, Side::Sell, 100, 5);
    book.cancel(1);
    auto trades = book.add_limit(3, Side::Buy, 100, 5);
    CHECK(trades.size() == 1 && trades[0].sell_id == 2);
    return ok;
}

TEST(cancel_from_middle_and_back_keeps_fifo_order) {
    bool ok = true;
    OrderBook book;
    for (OrderId id = 1; id <= 5; ++id) book.add_limit(id, Side::Sell, 100, 1);
    CHECK(book.cancel(3));  // middle
    CHECK(book.cancel(5));  // back
    book.add_limit(6, Side::Sell, 100, 1);  // must queue behind 4, not in 5's old slot
    CHECK(book.volume_at(Side::Sell, 100) == 4);
    auto trades = book.add_limit(7, Side::Buy, 100, 10);
    CHECK(trades.size() == 4);
    if (trades.size() == 4) {
        CHECK(trades[0].sell_id == 1 && trades[1].sell_id == 2);
        CHECK(trades[2].sell_id == 4 && trades[3].sell_id == 6);
    }
    CHECK(book.volume_at(Side::Buy, 100) == 6);
    CHECK(book.order_count() == 1);
    return ok;
}

// ---------------------------------------------------------------- stress

TEST(many_orders_stay_consistent) {
    bool ok = true;
    OrderBook book;
    // 1000 sells at 1000..1099 (10 per level), then sweep half with one buy.
    for (OrderId id = 1; id <= 1000; ++id) {
        book.add_limit(id, Side::Sell, 1000 + Price((id - 1) / 10), 1);
    }
    CHECK(book.order_count() == 1000);
    auto trades = book.add_limit(5000, Side::Buy, 1049, 1000);
    CHECK(trades.size() == 500);
    CHECK(book.best_ask() == 1050);
    CHECK(book.best_bid() == 1049);
    CHECK(book.volume_at(Side::Buy, 1049) == 500);
    CHECK(book.order_count() == 501);
    return ok;
}

// ---------------------------------------------------------------- buffer API

TEST(reusable_trade_buffer_is_cleared_each_call) {
    bool ok = true;
    OrderBook book;
    std::vector<Trade> trades;
    book.add_limit(1, Side::Sell, 100, 5, trades);
    CHECK(trades.empty());
    book.add_limit(2, Side::Buy, 100, 3, trades);
    CHECK(trades.size() == 1 && trades[0].sell_id == 1 && trades[0].quantity == 3);
    book.add_market(3, Side::Buy, 10, trades);  // previous trade must not linger
    CHECK(trades.size() == 1 && trades[0].buy_id == 3 && trades[0].quantity == 2);
    book.add_limit(4, Side::Buy, 99, 1, trades);
    CHECK(trades.empty());
    CHECK(book.order_count() == 1);
    return ok;
}

int main() {
    for (auto& t : registry()) {
        bool passed = t.fn();
        std::printf("%s  %s\n", passed ? "PASS" : "FAIL", t.name.c_str());
        if (!passed) ++failures;
    }
    std::printf("\n%zu tests, %d failed\n", registry().size(), failures);
    return failures == 0 ? 0 : 1;
}
