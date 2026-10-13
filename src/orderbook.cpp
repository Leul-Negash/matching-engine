#include "orderbook.hpp"

#include <algorithm>

namespace ob {

OrderBook::OrderBook(std::size_t expected_orders) {
    index_.reserve(expected_orders);
}

template <class Levels>
void OrderBook::rest(Levels& levels, OrderId id, Side side, Price price, Quantity qty) {
    Level& level = levels[price];  // creates the level if it doesn't exist yet
    level.orders.push_back({id, qty});
    level.total += qty;
    index_.emplace(id, Locator{side, price, std::prev(level.orders.end())});
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
        while (qty > 0 && !level.orders.empty()) {
            Order& resting = level.orders.front();
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
                index_.erase(resting.id);  // erase from the index before the node dies
                level.orders.pop_front();
            }
        }

        if (level.orders.empty()) {
            opposite.erase(level_it);  // level_it is now invalid; loop re-reads begin()
        }
    }
    return qty;
}

template <class Levels>
void OrderBook::erase_resting(Levels& levels, const Locator& loc) {
    auto level_it = levels.find(loc.price);
    Level& level = level_it->second;
    level.total -= loc.it->qty;
    level.orders.erase(loc.it);
    if (level.orders.empty()) {
        levels.erase(level_it);
    }
}

std::vector<Trade> OrderBook::add_limit(OrderId id, Side side, Price price, Quantity qty) {
    std::vector<Trade> trades;
    if (qty == 0) return trades;
    if (side == Side::Buy) {
        qty = match(asks_, id, side, price, qty, trades);
        if (qty > 0) rest(bids_, id, side, price, qty);
    } else {
        qty = match(bids_, id, side, price, qty, trades);
        if (qty > 0) rest(asks_, id, side, price, qty);
    }
    return trades;
}

std::vector<Trade> OrderBook::add_market(OrderId id, Side side, Quantity qty) {
    std::vector<Trade> trades;
    // Any unfilled remainder is simply dropped: a market order never rests.
    if (side == Side::Buy) {
        match(asks_, id, side, std::nullopt, qty, trades);
    } else {
        match(bids_, id, side, std::nullopt, qty, trades);
    }
    return trades;
}

bool OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    const Locator& loc = it->second;
    if (loc.side == Side::Buy) {
        erase_resting(bids_, loc);
    } else {
        erase_resting(asks_, loc);
    }
    index_.erase(it);  // last: loc refers into this entry
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
