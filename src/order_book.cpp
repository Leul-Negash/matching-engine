#include "ob/order_book.hpp"

#include <stdexcept>
#include <tuple>

namespace ob {

namespace {

Event make_event(EventType type, Side side, OrderId id) {
    Event e{};
    e.type = type;
    e.side = side;
    e.order_id = id;
    return e;
}

}  // namespace

OrderBook::OrderBook(const BookConfig& config) : config_(config) {
    if (config_.tick_size <= 0) throw std::invalid_argument("tick_size must be positive");
    if (config_.lot_size == 0) throw std::invalid_argument("lot_size must be positive");
    if (config_.min_price > config_.max_price) throw std::invalid_argument("min_price > max_price");
    index_.reserve(config_.expected_orders);
    pool_.reserve(config_.expected_orders);
}

// ------------------------------------------------------------------ storage

OrderBook::NodeId OrderBook::alloc_node() {
    if (free_head_ != kNone) {
        const NodeId n = free_head_;
        free_head_ = pool_[n].next;
        return n;
    }
    if (pool_.size() >= kNone) throw std::length_error("order pool exhausted");
    pool_.emplace_back();
    return static_cast<NodeId>(pool_.size() - 1);
}

void OrderBook::free_node(NodeId n) {
    pool_[n].next = free_head_;
    free_head_ = n;
}

// --------------------------------------------------------------- validation

RejectReason OrderBook::validate_price(Price price) const {
    if (price < config_.min_price || price > config_.max_price) return RejectReason::InvalidPrice;
    if (price % config_.tick_size != 0) return RejectReason::InvalidPrice;
    return RejectReason::None;
}

RejectReason OrderBook::validate_qty(Quantity qty) const {
    if (qty == 0 || qty > config_.max_order_qty) return RejectReason::InvalidQuantity;
    if (qty % config_.lot_size != 0) return RejectReason::InvalidQuantity;
    return RejectReason::None;
}

bool OrderBook::would_cross(Side side, Price price) const {
    if (side == Side::Buy) return !asks_.empty() && asks_.best().price <= price;
    return !bids_.empty() && bids_.best().price >= price;
}

// ----------------------------------------------------------------- requests

void OrderBook::submit(const NewOrder& order, std::vector<Event>& events) {
    RejectReason reason = validate_qty(order.qty);
    if (reason == RejectReason::None && order.type == OrderType::Limit) {
        reason = validate_price(order.price);
    }
    if (reason == RejectReason::None && order.post_only &&
        (order.type == OrderType::Market || order.tif != TimeInForce::GTC)) {
        reason = RejectReason::InvalidOrderType;  // post-only only makes sense for resting orders
    }

    // An order that may rest reserves its index entry now: one hash lookup
    // both rejects a duplicate id and gives rest() the slot to fill in.
    const bool may_rest = order.type == OrderType::Limit && order.tif == TimeInForce::GTC;
    Index::iterator slot = index_.end();
    if (reason == RejectReason::None) {
        if (may_rest) {
            bool inserted = false;
            std::tie(slot, inserted) = index_.try_emplace(order.id, kNone);
            if (!inserted) {
                slot = index_.end();
                reason = RejectReason::DuplicateOrderId;
            }
        } else if (index_.contains(order.id)) {
            reason = RejectReason::DuplicateOrderId;
        }
    }
    if (reason == RejectReason::None && order.post_only && would_cross(order.side, order.price)) {
        reason = RejectReason::PostOnlyWouldCross;
        index_.erase(slot);
    }

    const Price price = order.type == OrderType::Limit ? order.price : 0;
    if (reason != RejectReason::None) {
        Event e = make_event(EventType::Rejected, order.side, order.id);
        e.reject = reason;
        e.qty = order.qty;
        e.price = price;
        events.push_back(e);
        return;
    }

    Event ack = make_event(EventType::Accepted, order.side, order.id);
    ack.qty = order.qty;
    ack.price = price;
    ack.leaves = order.qty;
    events.push_back(ack);

    execute(order, slot, events);
}

void OrderBook::execute(const NewOrder& order, Index::iterator slot, std::vector<Event>& events) {
    const std::optional<Price> limit =
        order.type == OrderType::Limit ? std::optional<Price>(order.price) : std::nullopt;

    // Called on every path that doesn't rest the order.
    auto cancel_remainder = [&](CancelReason why, Quantity qty) {
        if (slot != index_.end()) index_.erase(slot);
        Event e = make_event(EventType::Cancelled, order.side, order.id);
        e.cancel = why;
        e.qty = qty;
        e.price = limit.value_or(0);
        events.push_back(e);
    };

    if (order.tif == TimeInForce::FOK) {
        const bool ok = order.side == Side::Buy ? fillable(asks_, order, limit) : fillable(bids_, order, limit);
        if (!ok) {
            cancel_remainder(CancelReason::FillOrKill, order.qty);
            return;
        }
    }

    // Matching only erases other orders' index entries, which leaves `slot`
    // valid (unordered_map erase invalidates only the erased element).
    Quantity qty = order.qty;
    const Stop stop = order.side == Side::Buy ? match(asks_, order, limit, qty, events)
                                              : match(bids_, order, limit, qty, events);
    if (qty == 0) {
        if (slot != index_.end()) index_.erase(slot);
    } else if (stop == Stop::SelfTrade) {
        // Resting it would leave the book crossed against our own order.
        cancel_remainder(CancelReason::SelfTrade, qty);
    } else if (slot == index_.end()) {
        cancel_remainder(CancelReason::Unfilled, qty);  // IOC / FOK / market
    } else if (order.side == Side::Buy) {
        slot->second = rest(bids_, order, qty);
    } else {
        slot->second = rest(asks_, order, qty);
    }
}

void OrderBook::cancel(OrderId id, std::vector<Event>& events) {
    auto it = index_.find(id);
    if (it == index_.end()) {
        Event e = make_event(EventType::Rejected, Side::Buy, id);
        e.reject = RejectReason::UnknownOrder;
        events.push_back(e);
        return;
    }
    const NodeId n = it->second;
    const Node& node = pool_[n];
    Event e = make_event(EventType::Cancelled, node.side, id);
    e.cancel = CancelReason::Requested;
    e.qty = node.qty;
    e.price = node.price;
    index_.erase(it);
    unlink(n);
    events.push_back(e);
}

void OrderBook::modify(OrderId id, Price new_price, Quantity new_qty, std::vector<Event>& events) {
    auto reject = [&](RejectReason why, Side side) {
        Event e = make_event(EventType::Rejected, side, id);
        e.reject = why;
        e.qty = new_qty;
        e.price = new_price;
        events.push_back(e);
    };

    auto it = index_.find(id);
    if (it == index_.end()) return reject(RejectReason::UnknownOrder, Side::Buy);
    const NodeId n = it->second;
    Node& node = pool_[n];

    if (RejectReason r = validate_qty(new_qty); r != RejectReason::None) return reject(r, node.side);
    if (RejectReason r = validate_price(new_price); r != RejectReason::None) return reject(r, node.side);

    Event e = make_event(EventType::Modified, node.side, id);
    e.qty = new_qty;
    e.price = new_price;
    e.leaves = new_qty;

    if (new_price == node.price && new_qty <= node.qty) {
        // Pure size reduction: the order keeps its place in the queue.
        Level* level = node.side == Side::Buy ? bids_.find(node.price) : asks_.find(node.price);
        level->total -= node.qty - new_qty;
        node.qty = new_qty;
        events.push_back(e);
        return;
    }

    // Price change or size increase: cancel/replace, losing time priority.
    if (node.post_only && would_cross(node.side, new_price)) {
        return reject(RejectReason::PostOnlyWouldCross, node.side);
    }
    const NewOrder replacement{id, node.account, node.side, OrderType::Limit, TimeInForce::GTC,
                               new_price, new_qty, node.post_only};
    unlink(n);  // `node` dangles from here on; the index entry is reused as the slot
    it->second = kNone;
    events.push_back(e);
    execute(replacement, it, events);
}

// ----------------------------------------------------------------- matching

template <class SideLevels>
OrderBook::NodeId OrderBook::rest(SideLevels& levels, const NewOrder& order, Quantity qty) {
    Level& level = levels.get_or_insert(order.price);
    const NodeId n = alloc_node();
    pool_[n] = Node{order.id, order.price, qty, order.account, level.tail, kNone, order.side, order.post_only};
    if (level.tail != kNone) {
        pool_[level.tail].next = n;
    } else {
        level.head = n;
    }
    level.tail = n;
    level.total += qty;
    ++level.count;
    return n;
}

template <class SideLevels>
OrderBook::Stop OrderBook::match(SideLevels& opposite, const NewOrder& order, std::optional<Price> limit,
                                 Quantity& qty, std::vector<Event>& events) {
    // The level at price P is unacceptable exactly when the limit is strictly
    // better than P from the resting side's point of view:
    //   asks (less):    stop if limit < P   (buyer won't pay more than limit)
    //   bids (greater): stop if limit > P   (seller won't take less than limit)
    while (qty > 0) {
        if (opposite.empty()) return Stop::Empty;
        const Price price = opposite.best().price;
        if (limit && SideLevels::better(*limit, price)) return Stop::LimitReached;

        Level& level = opposite.best().level;  // no inserts below, so this stays valid
        while (qty > 0 && level.head != kNone) {
            const NodeId n = level.head;
            Node& resting = pool_[n];
            if (self_trade(order.account, resting.account)) return Stop::SelfTrade;

            const Quantity traded = std::min(qty, resting.qty);
            qty -= traded;  // traded <= both, so neither subtraction can wrap
            resting.qty -= traded;
            level.total -= traded;

            Event e = make_event(EventType::Trade, order.side, order.id);
            e.qty = traded;
            e.counter_id = resting.id;
            e.price = price;
            e.leaves = qty;
            e.counter_leaves = resting.qty;
            events.push_back(e);

            if (resting.qty == 0) {
                index_.erase(resting.id);
                level.head = resting.next;
                if (level.head != kNone) pool_[level.head].prev = kNone;
                --level.count;
                free_node(n);  // last: reuses resting.next, so read it first
            }
        }

        if (level.head == kNone) {
            level.tail = kNone;
            opposite.pop_best();  // `level` now dangles; the loop re-reads best()
        }
    }
    return Stop::Filled;
}

template <class SideLevels>
bool OrderBook::fillable(const SideLevels& opposite, const NewOrder& order, std::optional<Price> limit) const {
    const bool check_accounts = config_.self_trade_prevention && order.account != 0;
    Volume need = order.qty;
    for (std::size_t i = 0; i < opposite.size(); ++i) {
        const auto& entry = opposite.nth_best(i);
        if (limit && SideLevels::better(*limit, entry.price)) return false;
        if (!check_accounts) {
            // Fast path: whole levels at a time.
            if (entry.level.total >= need) return true;
            need -= entry.level.total;
            continue;
        }
        // Matching would stop at the first order of our own account.
        for (NodeId n = entry.level.head; n != kNone; n = pool_[n].next) {
            if (self_trade(order.account, pool_[n].account)) return false;
            if (pool_[n].qty >= need) return true;
            need -= pool_[n].qty;
        }
    }
    return false;
}

template <class SideLevels>
void OrderBook::unlink(SideLevels& levels, NodeId n) {
    const Node& node = pool_[n];
    Level& level = *levels.find(node.price);  // a resting order's level always exists
    level.total -= node.qty;
    --level.count;

    if (node.prev != kNone) {
        pool_[node.prev].next = node.next;
    } else {
        level.head = node.next;
    }
    if (node.next != kNone) {
        pool_[node.next].prev = node.prev;
    } else {
        level.tail = node.prev;
    }

    if (level.head == kNone) levels.erase(node.price);
    free_node(n);
}

void OrderBook::unlink(NodeId n) {
    if (pool_[n].side == Side::Buy) {
        unlink(bids_, n);
    } else {
        unlink(asks_, n);
    }
}

// ------------------------------------------------------------------ queries

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.best().price;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.best().price;
}

TopOfBook OrderBook::top() const {
    TopOfBook t;
    if (!bids_.empty()) {
        t.bid = bids_.best().price;
        t.bid_qty = bids_.best().level.total;
    }
    if (!asks_.empty()) {
        t.ask = asks_.best().price;
        t.ask_qty = asks_.best().level.total;
    }
    return t;
}

Volume OrderBook::volume_at(Side side, Price price) const {
    const Level* level = side == Side::Buy ? bids_.find(price) : asks_.find(price);
    return level ? level->total : 0;
}

void OrderBook::depth(Side side, std::size_t max_levels, std::vector<DepthLevel>& out) const {
    auto collect = [&](const auto& levels) {
        const std::size_t n = std::min(max_levels, levels.size());
        for (std::size_t i = 0; i < n; ++i) {
            const auto& e = levels.nth_best(i);
            out.push_back({e.price, e.level.total, e.level.count});
        }
    };
    if (side == Side::Buy) {
        collect(bids_);
    } else {
        collect(asks_);
    }
}

std::optional<OrderInfo> OrderBook::find(OrderId id) const {
    auto it = index_.find(id);
    if (it == index_.end()) return std::nullopt;
    const Node& n = pool_[it->second];
    return OrderInfo{n.id, n.account, n.side, n.price, n.qty, n.post_only};
}

std::size_t OrderBook::order_count() const {
    return index_.size();
}

// ------------------------------------------------------- convenience API

namespace {

std::vector<Trade> trades_of(const std::vector<Event>& events) {
    std::vector<Trade> trades;
    for (const Event& e : events) {
        if (e.type != EventType::Trade) continue;
        if (e.side == Side::Buy) {
            trades.push_back({e.order_id, e.counter_id, e.price, e.qty});
        } else {
            trades.push_back({e.counter_id, e.order_id, e.price, e.qty});
        }
    }
    return trades;
}

}  // namespace

std::vector<Trade> OrderBook::add_limit(OrderId id, Side side, Price price, Quantity qty) {
    std::vector<Event> events;
    submit({id, 0, side, OrderType::Limit, TimeInForce::GTC, price, qty, false}, events);
    return trades_of(events);
}

std::vector<Trade> OrderBook::add_market(OrderId id, Side side, Quantity qty) {
    std::vector<Event> events;
    submit({id, 0, side, OrderType::Market, TimeInForce::IOC, 0, qty, false}, events);
    return trades_of(events);
}

bool OrderBook::cancel(OrderId id) {
    std::vector<Event> events;
    cancel(id, events);
    return events.front().type == EventType::Cancelled;
}

}  // namespace ob
