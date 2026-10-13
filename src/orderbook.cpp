#include "orderbook.hpp"

namespace ob {

std::vector<Trade> OrderBook::add_limit(OrderId, Side, Price, Quantity) {
    // TODO
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
    // TODO
    return std::nullopt;
}

std::optional<Price> OrderBook::best_ask() const {
    // TODO
    return std::nullopt;
}

Quantity OrderBook::volume_at(Side, Price) const {
    // TODO
    return 0;
}

std::size_t OrderBook::order_count() const {
    // TODO
    return 0;
}

}  // namespace ob
