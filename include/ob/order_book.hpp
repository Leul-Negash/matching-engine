#pragma once

#include "ob/types.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ob {

// Per-instrument trading rules.
struct BookConfig {
    Price tick_size = 1;      // limit prices must be a multiple of this
    Quantity lot_size = 1;    // quantities must be a multiple of this
    Quantity max_order_qty = std::numeric_limits<Quantity>::max();  // fat-finger guard
    Price min_price = 1;
    Price max_price = std::numeric_limits<Price>::max();
    bool self_trade_prevention = true;  // orders of the same (non-zero) account never trade
    std::size_t expected_orders = 0;    // peak resting orders, to pre-size storage
};

struct DepthLevel {
    Price price;
    Volume qty;
    std::uint32_t orders;
};

struct TopOfBook {
    std::optional<Price> bid;
    Volume bid_qty = 0;
    std::optional<Price> ask;
    Volume ask_qty = 0;

    bool operator==(const TopOfBook&) const = default;
};

struct OrderInfo {
    OrderId id;
    AccountId account;
    Side side;
    Price price;
    Quantity leaves;
    bool post_only;
};

// A single-instrument limit order book with price-time priority.
//
// Every request appends execution reports (Events) to a caller-owned vector,
// in the order things happened; the vector is never cleared by the book, so
// a caller can reuse one buffer for every request and never allocate.
//
// Not thread-safe: one book is owned by one thread.
class OrderBook {
public:
    explicit OrderBook(const BookConfig& config = {});

    // Validates and executes a new order. Emits Accepted (or Rejected), then
    // any Trades, then a Cancelled for a remainder that does not rest.
    void submit(const NewOrder& order, std::vector<Event>& events);

    // Cancels a resting order: Cancelled, or Rejected(UnknownOrder).
    void cancel(OrderId id, std::vector<Event>& events);

    // Changes a resting order's price and/or open quantity (`new_qty` is the
    // new remaining quantity). Reducing the quantity at the same price keeps
    // the order's place in the queue; any other change sends it to the back,
    // and a new price may trade immediately. Emits Modified (then any Trades
    // and a possible Cancelled for self-trade), or Rejected, in which case the
    // order is unchanged.
    void modify(OrderId id, Price new_price, Quantity new_qty, std::vector<Event>& events);

    // ---- queries

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    TopOfBook top() const;

    // Total resting quantity at one price level on one side (0 if none).
    Volume volume_at(Side side, Price price) const;

    // Up to `max_levels` best levels of one side, best first, appended to `out`.
    void depth(Side side, std::size_t max_levels, std::vector<DepthLevel>& out) const;

    std::optional<OrderInfo> find(OrderId id) const;

    // Number of orders currently resting in the book.
    std::size_t order_count() const;

    const BookConfig& config() const { return config_; }

    // ---- convenience API (allocates; for tests and simple callers)

    // GTC limit / market order with no account; returns only the trades.
    std::vector<Trade> add_limit(OrderId id, Side side, Price price, Quantity qty);
    std::vector<Trade> add_market(OrderId id, Side side, Quantity qty);
    bool cancel(OrderId id);

private:
    // Index of an order node in pool_. Indices, unlike pointers or iterators,
    // stay valid when the pool's vector grows and reallocates.
    using NodeId = std::uint32_t;
    static constexpr NodeId kNone = ~NodeId{0};

    // A resting order, linked into its level's FIFO queue by index. The pool is
    // one contiguous vector, so orders are not individually heap-allocated.
    struct Node {
        OrderId id;
        Price price;
        Quantity qty;  // remaining (unfilled) quantity
        AccountId account;
        NodeId prev;
        NodeId next;  // also chains free nodes when the node is unused
        Side side;
        bool post_only;
    };

    // One price level: intrusive doubly linked FIFO queue of resting orders.
    struct Level {
        NodeId head = kNone;
        NodeId tail = kNone;
        Volume total = 0;          // cached sum of the queue's qty
        std::uint32_t count = 0;   // number of orders in the queue
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
        std::size_t size() const { return v_.size(); }
        Entry& best() { return v_.back(); }
        const Entry& best() const { return v_.back(); }
        void pop_best() { v_.pop_back(); }

        // i-th best level (0 = best).
        const Entry& nth_best(std::size_t i) const { return v_[v_.size() - 1 - i]; }

        // Is price a strictly better than price b for this side?
        static bool better(Price a, Price b) { return Better{}(a, b); }

        Level* find(Price price) {
            auto it = lower(price);
            return it != v_.end() && it->price == price ? &it->level : nullptr;
        }
        const Level* find(Price price) const { return const_cast<Levels*>(this)->find(price); }

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

    using BidLevels = Levels<std::greater<Price>>;
    using AskLevels = Levels<std::less<Price>>;

    // Why the matching loop stopped.
    enum class Stop : std::uint8_t { Filled, LimitReached, Empty, SelfTrade };

    BookConfig config_;
    BidLevels bids_;
    AskLevels asks_;

    // Node storage plus a free list of recycled slots.
    std::vector<Node> pool_;
    NodeId free_head_ = kNone;

    // id -> node of every resting order; size() is order_count(). While a
    // request is executing, the incoming order may hold a reserved entry
    // (node kNone) so the id is looked up only once (see execute()).
    using Index = std::unordered_map<OrderId, NodeId>;
    Index index_;

    NodeId alloc_node();
    void free_node(NodeId n);

    RejectReason validate_price(Price price) const;
    RejectReason validate_qty(Quantity qty) const;
    bool would_cross(Side side, Price price) const;

    // Matches, rests or cancels the remainder of an order that has already
    // been validated and acknowledged (shared by submit and modify). `slot` is
    // the order's reserved index entry if it may rest (GTC limit), else end():
    // it is filled in if the order rests and erased otherwise.
    void execute(const NewOrder& order, Index::iterator slot, std::vector<Event>& events);

    template <class SideLevels>
    NodeId rest(SideLevels& levels, const NewOrder& order, Quantity qty);

    // The one matching loop. Trades the incoming order against `opposite`
    // (best level first, FIFO within a level) while its price is acceptable;
    // no limit means a market order. Updates `qty` to the unfilled remainder.
    template <class SideLevels>
    Stop match(SideLevels& opposite, const NewOrder& order, std::optional<Price> limit,
               Quantity& qty, std::vector<Event>& events);

    // Can `qty` be filled right now, without breaking the limit or trading
    // with the same account? (Fill-or-kill pre-check; changes nothing.)
    template <class SideLevels>
    bool fillable(const SideLevels& opposite, const NewOrder& order, std::optional<Price> limit) const;

    // Unlinks a resting order from its level, dropping the level if it
    // empties, and frees the node. The caller removes the index entry.
    template <class SideLevels>
    void unlink(SideLevels& levels, NodeId n);
    void unlink(NodeId n);

    bool self_trade(AccountId a, AccountId b) const {
        return config_.self_trade_prevention && a != 0 && a == b;
    }
};

}  // namespace ob
