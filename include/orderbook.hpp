#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
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
    // Index of an order node in pool_. Indices, unlike pointers or iterators,
    // stay valid when the pool's vector grows and reallocates.
    using NodeId = std::uint32_t;
    static constexpr NodeId kNone = ~NodeId{0};

    // A resting order, linked into its level's FIFO queue by index. The pool is
    // one contiguous vector, so orders are not individually heap-allocated and
    // a level's queue is walked through nearby memory.
    struct Node {
        OrderId id;
        Price price;
        Quantity qty;  // remaining (unfilled) quantity
        Side side;
        NodeId prev;
        NodeId next;   // also chains free nodes when the node is unused
    };

    // One price level: intrusive doubly linked FIFO queue of resting orders.
    // O(1) push to the back, pop from the front, and unlink from the middle
    // (cancel), without allocating.
    struct Level {
        NodeId head = kNone;
        NodeId tail = kNone;
        Quantity total = 0;  // cached sum of the queue's qty, so volume_at is O(1)
    };

    // Price levels of one side in a contiguous vector sorted worst -> best, so
    // the best level is back(): reading or removing it is O(1), and a new level
    // near the top of the book (where most orders land) shifts only a few
    // elements. Lookup by price is a binary search. `Better` orders prices
    // best-first: std::greater for bids, std::less for asks.
    template <class Better>
    class Levels {
    public:
        struct Entry {
            Price price;
            Level level;
        };

        bool empty() const { return v_.empty(); }
        Entry& best() { return v_.back(); }
        const Entry& best() const { return v_.back(); }
        void pop_best() { v_.pop_back(); }

        // Is price a strictly better than price b for this side?
        static bool better(Price a, Price b) { return Better{}(a, b); }

        Level* find(Price price) {
            auto it = lower(price);
            return it != v_.end() && it->price == price ? &it->level : nullptr;
        }
        const Level* find(Price price) const {
            return const_cast<Levels*>(this)->find(price);
        }

        // The returned reference is valid until the next insert or erase.
        Level& get_or_insert(Price price) {
            auto it = lower(price);
            if (it == v_.end() || it->price != price) it = v_.insert(it, Entry{price, Level{}});
            return it->level;
        }

        void erase(Price price) { v_.erase(lower(price)); }

    private:
        // First entry whose price is not worse than `price`.
        typename std::vector<Entry>::iterator lower(Price price) {
            return std::lower_bound(v_.begin(), v_.end(), price,
                                    [](const Entry& e, Price p) { return better(p, e.price); });
        }

        std::vector<Entry> v_;
    };

    Levels<std::greater<Price>> bids_;
    Levels<std::less<Price>> asks_;

    // Node storage plus a free list of recycled slots: after warm-up, adding an
    // order reuses a slot instead of calling the allocator.
    std::vector<Node> pool_;
    NodeId free_head_ = kNone;

    // Hash index id -> node (which records side and price), so cancel(id)
    // needs no search. Holds exactly one entry per resting order, so its
    // size() is order_count().
    std::unordered_map<OrderId, NodeId> index_;

    NodeId alloc_node();
    void free_node(NodeId n);

    // Appends an order to the back of its price level and indexes it.
    template <class SideLevels>
    void rest(SideLevels& levels, OrderId id, Side side, Price price, Quantity qty);

    // The one matching loop shared by limit and market orders. Trades the
    // incoming order against `opposite` (best level first, FIFO within a level)
    // while its price is acceptable; no limit means a market order.
    // Returns the quantity left unfilled.
    template <class SideLevels>
    Quantity match(SideLevels& opposite, OrderId id, Side side, std::optional<Price> limit,
                   Quantity qty, std::vector<Trade>& trades);

    // Unlinks a resting order from its level, dropping the level if it empties.
    template <class SideLevels>
    void erase_resting(SideLevels& levels, NodeId n);
};

}  // namespace ob
