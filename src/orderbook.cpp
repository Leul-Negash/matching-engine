#include "orderbook.hpp"

#include <algorithm>

namespace ob {

OrderBook::OrderBook(std::size_t expected_orders) {
    index_.reserve(expected_orders);
    pool_.reserve(expected_orders);
}

OrderBook::NodeId OrderBook::alloc_node() {
    if (free_head_ != kNone) {
        const NodeId n = free_head_;
        free_head_ = pool_[n].next;
        return n;
    }
    pool_.emplace_back();
    return static_cast<NodeId>(pool_.size() - 1);
}

void OrderBook::free_node(NodeId n) {
    pool_[n].next = free_head_;
    free_head_ = n;
}

template <class Levels>
void OrderBook::rest(Levels& levels, OrderId id, Side side, Price price, Quantity qty) {
    Level& level = levels[price];  // creates the level if it doesn't exist yet
    const NodeId n = alloc_node();
    pool_[n] = Node{id, price, qty, side, level.tail, kNone};
    if (level.tail != kNone) {
        pool_[level.tail].next = n;
    } else {
        level.head = n;
    }
    level.tail = n;
    level.total += qty;
    index_.emplace(id, n);
}

template <class Levels>
Quantity OrderBook::match(Levels& opposite, OrderId id, Side side, std::optional<Price> limit,
                          Quantity qty, std::vector<Trade>& trades) {
    // The opposite map is sorted best-first, so its comparator answers "is a
    // better than b". The level at price P is unacceptable exactly when the
    // limit is strictly better than P from the resting side's view:
    //   asks (less):    stop if limit < P   (buyer won't pay more than limit)
    //   bids (greater): stop if limit > P   (seller won't take less than limit)
    const auto better = opposite.key_comp();

    while (qty > 0 && !opposite.empty()) {
        auto level_it = opposite.begin();
        const Price price = level_it->first;
        if (limit && better(*limit, price)) break;

        Level& level = level_it->second;
        while (qty > 0 && level.head != kNone) {
            const NodeId n = level.head;
            Node& resting = pool_[n];
            const Quantity traded = std::min(qty, resting.qty);

            if (side == Side::Buy) {
                trades.push_back({id, resting.id, price, traded});
            } else {
                trades.push_back({resting.id, id, price, traded});
            }

            qty -= traded;  // traded <= both, so neither subtraction can wrap
            resting.qty -= traded;
            level.total -= traded;

            if (resting.qty == 0) {
                index_.erase(resting.id);
                level.head = resting.next;
                if (level.head != kNone) pool_[level.head].prev = kNone;
                free_node(n);  // last: reuses resting.next, so read it first
            }
        }

        if (level.head == kNone) {
            level.tail = kNone;
            opposite.erase(level_it);  // level_it is now invalid; loop re-reads begin()
        }
    }
    return qty;
}

template <class Levels>
void OrderBook::erase_resting(Levels& levels, NodeId n) {
    const Node& node = pool_[n];
    auto level_it = levels.find(node.price);
    Level& level = level_it->second;
    level.total -= node.qty;

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

    if (level.head == kNone) {
        levels.erase(level_it);
    }
    free_node(n);
}

std::vector<Trade> OrderBook::add_limit(OrderId id, Side side, Price price, Quantity qty) {
    std::vector<Trade> trades;
    add_limit(id, side, price, qty, trades);
    return trades;
}

std::vector<Trade> OrderBook::add_market(OrderId id, Side side, Quantity qty) {
    std::vector<Trade> trades;
    add_market(id, side, qty, trades);
    return trades;
}

void OrderBook::add_limit(OrderId id, Side side, Price price, Quantity qty, std::vector<Trade>& trades) {
    trades.clear();
    if (qty == 0) return;
    if (side == Side::Buy) {
        qty = match(asks_, id, side, price, qty, trades);
        if (qty > 0) rest(bids_, id, side, price, qty);
    } else {
        qty = match(bids_, id, side, price, qty, trades);
        if (qty > 0) rest(asks_, id, side, price, qty);
    }
}

void OrderBook::add_market(OrderId id, Side side, Quantity qty, std::vector<Trade>& trades) {
    trades.clear();
    // Any unfilled remainder is simply dropped: a market order never rests.
    if (side == Side::Buy) {
        match(asks_, id, side, std::nullopt, qty, trades);
    } else {
        match(bids_, id, side, std::nullopt, qty, trades);
    }
}

bool OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    const NodeId n = it->second;
    index_.erase(it);
    if (pool_[n].side == Side::Buy) {
        erase_resting(bids_, n);
    } else {
        erase_resting(asks_, n);
    }
    return true;
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

Quantity OrderBook::volume_at(Side side, Price price) const {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second.total;
    }
    auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second.total;
}

std::size_t OrderBook::order_count() const {
    return index_.size();
}

}  // namespace ob
