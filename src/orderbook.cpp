#include "orderbook.hpp"

namespace ob {

template <class Levels>
void OrderBook::rest(Levels& levels, OrderId id, Side side, Price price, Quantity qty) {
    Level& level = levels[price];  // creates the level if it doesn't exist yet
    level.orders.push_back({id, qty});
    level.total += qty;
    index_.emplace(id, Locator{side, price, std::prev(level.orders.end())});
}

std::vector<Trade> OrderBook::add_limit(OrderId id, Side side, Price price, Quantity qty) {
    if (qty == 0) return {};
    if (side == Side::Buy) {
        rest(bids_, id, side, price, qty);
    } else {
        rest(asks_, id, side, price, qty);
    }
    return {};
}

std::vector<Trade> OrderBook::add_market(OrderId, Side, Quantity) {
    // TODO
    return {};
}

bool OrderBook::cancel(OrderId) {
    // TODO
    return false;
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
