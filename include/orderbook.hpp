#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ob {

using OrderId = std::uint64_t;
using Price = std::int64_t;     // price in ticks (e.g. cents), never floating point
using Quantity = std::uint32_t;

enum class Side { Buy, Sell };

struct Trade {
    OrderId buy_id;
    OrderId sell_id;
    Price price;       // the resting order's price
    Quantity quantity;
};

class OrderBook {
public:
    // expected_orders: peak number of resting orders to pre-size the id index
    // for, so it never rehashes (a multi-millisecond stall) mid-session.
    explicit OrderBook(std::size_t expected_orders = 0);

    // Adds a limit order. Matches it against the opposite side first
    // (price-time priority); any remainder rests in the book.
    // Returns the trades produced, in the order they happened.
    std::vector<Trade> add_limit(OrderId id, Side side, Price price, Quantity qty);

    // Matches against the opposite side at any price until filled or the
    // side is empty. A market order never rests in the book.
    std::vector<Trade> add_market(OrderId id, Side side, Quantity qty);

    // Allocation-free variants for the hot path: `trades` is cleared and then
    // filled, so one buffer can be reused for every call and keeps its capacity.
    void add_limit(OrderId id, Side side, Price price, Quantity qty, std::vector<Trade>& trades);
    void add_market(OrderId id, Side side, Quantity qty, std::vector<Trade>& trades);

    // Removes a resting order. Returns false if the id is unknown
    // (never added, already filled, or already cancelled).
    bool cancel(OrderId id);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    // Total resting quantity at one price level on one side (0 if none).
    Quantity volume_at(Side side, Price price) const;

    // Number of orders currently resting in the book.
    std::size_t order_count() const;

private:
    struct Order {
        OrderId id;
        Quantity qty;  // remaining (unfilled) quantity
    };

    // One price level: FIFO queue of resting orders at a single price.
    // std::list gives O(1) push_back, pop_front and erase-by-iterator, and
    // erasing one node never invalidates iterators to the others (O(1) cancel).
    struct Level {
        std::list<Order> orders;
        Quantity total = 0;  // cached sum of orders[i].qty, so volume_at is O(1)
    };

    // Sorted price levels per side; begin() is always the best price.
    // Bids sort descending (std::greater), asks ascending (std::less).
    std::map<Price, Level, std::greater<Price>> bids_;
    std::map<Price, Level, std::less<Price>> asks_;

    // Where a resting order lives, so cancel(id) needs no search.
    struct Locator {
        Side side;
        Price price;
        std::list<Order>::iterator it;
    };

    // Hash index id -> location: O(1) average lookup for cancel. Holds exactly
    // one entry per resting order, so its size() is order_count().
    std::unordered_map<OrderId, Locator> index_;

    // Appends an order to the back of its price level and indexes it.
    template <class Levels>
    void rest(Levels& levels, OrderId id, Side side, Price price, Quantity qty);

    // The one matching loop shared by limit and market orders. Trades the
    // incoming order against `opposite` (best level first, FIFO within a level)
    // while its price is acceptable; no limit means a market order.
    // Returns the quantity left unfilled.
    template <class Levels>
    Quantity match(Levels& opposite, OrderId id, Side side, std::optional<Price> limit,
                   Quantity qty, std::vector<Trade>& trades);

    // Removes one resting order (located via the index) from its level.
    template <class Levels>
    void erase_resting(Levels& levels, const Locator& loc);
};

}  // namespace ob
