// Tests for the event API: execution reports, time in force, post-only,
// self-trade prevention, modify, validation and book queries.

#include "ob/order_book.hpp"
#include "test.hpp"

#include <stdexcept>

using namespace ob;

namespace {

struct Harness {
    OrderBook book;
    std::vector<Event> ev;

    explicit Harness(const BookConfig& cfg = {}) : book(cfg) {}

    const std::vector<Event>& submit(const NewOrder& o) {
        ev.clear();
        book.submit(o, ev);
        return ev;
    }
    const std::vector<Event>& limit(OrderId id, Side side, Price px, Quantity qty, AccountId acct = 0,
                                    TimeInForce tif = TimeInForce::GTC, bool post_only = false) {
        return submit({id, acct, side, OrderType::Limit, tif, px, qty, post_only});
    }
    const std::vector<Event>& market(OrderId id, Side side, Quantity qty, AccountId acct = 0) {
        return submit({id, acct, side, OrderType::Market, TimeInForce::IOC, 0, qty, false});
    }
    const std::vector<Event>& cancel(OrderId id) {
        ev.clear();
        book.cancel(id, ev);
        return ev;
    }
    const std::vector<Event>& modify(OrderId id, Price px, Quantity qty) {
        ev.clear();
        book.modify(id, px, qty, ev);
        return ev;
    }
};

bool is(const Event& e, EventType t) { return e.type == t; }

}  // namespace

// ------------------------------------------------------------- event stream

TEST(events_ack_trade_then_rest) {
    bool ok = true;
    Harness h;
    auto& a = h.limit(1, Side::Sell, 100, 10);
    CHECK(a.size() == 1 && is(a[0], EventType::Accepted) && a[0].leaves == 10);

    auto& b = h.limit(2, Side::Buy, 100, 15);
    CHECK(b.size() == 2);
    if (b.size() == 2) {
        CHECK(is(b[0], EventType::Accepted) && b[0].order_id == 2 && b[0].qty == 15);
        CHECK(is(b[1], EventType::Trade));
        CHECK(b[1].order_id == 2 && b[1].counter_id == 1 && b[1].side == Side::Buy);
        CHECK(b[1].qty == 10 && b[1].price == 100);
        CHECK(b[1].leaves == 5 && b[1].counter_leaves == 0);
    }
    CHECK(h.book.volume_at(Side::Buy, 100) == 5);
    return ok;
}

TEST(events_are_appended_not_cleared) {
    bool ok = true;
    OrderBook book;
    std::vector<Event> ev;
    book.submit({1, 0, Side::Buy, OrderType::Limit, TimeInForce::GTC, 100, 1, false}, ev);
    book.submit({2, 0, Side::Buy, OrderType::Limit, TimeInForce::GTC, 100, 1, false}, ev);
    CHECK(ev.size() == 2 && ev[0].order_id == 1 && ev[1].order_id == 2);
    return ok;
}

TEST(cancel_reports_quantity_and_reason) {
    bool ok = true;
    Harness h;
    h.limit(1, Side::Buy, 100, 7);
    auto& e = h.cancel(1);
    CHECK(e.size() == 1 && is(e[0], EventType::Cancelled));
    if (!e.empty()) CHECK(e[0].cancel == CancelReason::Requested && e[0].qty == 7 && e[0].price == 100);
    auto& again = h.cancel(1);
    CHECK(again.size() == 1 && is(again[0], EventType::Rejected) && again[0].reject == RejectReason::UnknownOrder);
    return ok;
}

// --------------------------------------------------------------- validation

TEST(rejects_invalid_orders) {
    bool ok = true;
    BookConfig cfg;
    cfg.tick_size = 5;
    cfg.lot_size = 10;
    cfg.max_order_qty = 1000;
    cfg.min_price = 5;
    cfg.max_price = 1000;
    Harness h(cfg);

    auto reason = [&](const std::vector<Event>& e) {
        return e.size() == 1 && is(e[0], EventType::Rejected) ? e[0].reject : RejectReason::None;
    };
    CHECK(reason(h.limit(1, Side::Buy, 100, 0)) == RejectReason::InvalidQuantity);
    CHECK(reason(h.limit(1, Side::Buy, 100, 15)) == RejectReason::InvalidQuantity);    // off lot
    CHECK(reason(h.limit(1, Side::Buy, 100, 1010)) == RejectReason::InvalidQuantity);  // > max
    CHECK(reason(h.limit(1, Side::Buy, 102, 10)) == RejectReason::InvalidPrice);       // off tick
    CHECK(reason(h.limit(1, Side::Buy, 0, 10)) == RejectReason::InvalidPrice);         // < min
    CHECK(reason(h.limit(1, Side::Buy, 1005, 10)) == RejectReason::InvalidPrice);      // > max
    CHECK(reason(h.submit({1, 0, Side::Buy, OrderType::Market, TimeInForce::IOC, 0, 10, true})) ==
          RejectReason::InvalidOrderType);
    CHECK(reason(h.limit(1, Side::Buy, 100, 10, 0, TimeInForce::IOC, true)) == RejectReason::InvalidOrderType);
    CHECK(h.book.order_count() == 0);

    CHECK(h.limit(1, Side::Buy, 100, 10).size() == 1);
    CHECK(reason(h.limit(1, Side::Sell, 200, 10)) == RejectReason::DuplicateOrderId);
    CHECK(h.book.order_count() == 1);
    return ok;
}

TEST(market_order_ignores_price_validation) {
    bool ok = true;
    BookConfig cfg;
    cfg.tick_size = 5;
    Harness h(cfg);
    h.limit(1, Side::Sell, 100, 5);
    auto& e = h.market(2, Side::Buy, 5);
    CHECK(e.size() == 2 && is(e[0], EventType::Accepted) && is(e[1], EventType::Trade));
    return ok;
}

TEST(bad_config_throws) {
    bool ok = true;
    auto throws = [](BookConfig c) {
        try {
            OrderBook b(c);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    BookConfig a;
    a.tick_size = 0;
    BookConfig b;
    b.lot_size = 0;
    BookConfig c;
    c.min_price = 10;
    c.max_price = 5;
    CHECK(throws(a));
    CHECK(throws(b));
    CHECK(throws(c));
    CHECK(!throws(BookConfig{}));
    return ok;
}

// ------------------------------------------------------------ time in force

TEST(ioc_cancels_unfilled_remainder) {
    bool ok = true;
    Harness h;
    h.limit(1, Side::Sell, 100, 5);
    auto& e = h.limit(2, Side::Buy, 101, 8, 0, TimeInForce::IOC);
    CHECK(e.size() == 3);
    if (e.size() == 3) {
        CHECK(is(e[1], EventType::Trade) && e[1].qty == 5);
        CHECK(is(e[2], EventType::Cancelled) && e[2].cancel == CancelReason::Unfilled && e[2].qty == 3);
    }
    CHECK(!h.book.best_bid());
    CHECK(h.book.order_count() == 0);
    return ok;
}

TEST(ioc_that_cannot_trade_is_cancelled_whole) {
    bool ok = true;
    Harness h;
    h.limit(1, Side::Sell, 105, 5);
    auto& e = h.limit(2, Side::Buy, 100, 8, 0, TimeInForce::IOC);
    CHECK(e.size() == 2 && is(e[1], EventType::Cancelled) && e[1].qty == 8);
    CHECK(h.book.order_count() == 1);
    return ok;
}

TEST(fok_fills_completely_across_levels) {
    bool ok = true;
    Harness h;
    h.limit(1, Side::Sell, 100, 5);
    h.limit(2, Side::Sell, 101, 5);
    auto& e = h.limit(3, Side::Buy, 101, 10, 0, TimeInForce::FOK);
    CHECK(e.size() == 3 && is(e[1], EventType::Trade) && is(e[2], EventType::Trade));
    CHECK(h.book.order_count() == 0);
    return ok;
}

TEST(fok_kills_without_touching_the_book) {
    bool ok = true;
    Harness h;
    h.limit(1, Side::Sell, 100, 5);
    h.limit(2, Side::Sell, 101, 5);
    auto& short_qty = h.limit(3, Side::Buy, 101, 11, 0, TimeInForce::FOK);
    CHECK(short_qty.size() == 2 && is(short_qty[1], EventType::Cancelled));
    if (short_qty.size() == 2) CHECK(short_qty[1].cancel == CancelReason::FillOrKill && short_qty[1].qty == 11);
    auto& short_px = h.limit(4, Side::Buy, 100, 10, 0, TimeInForce::FOK);
    CHECK(short_px.size() == 2 && is(short_px[1], EventType::Cancelled));
    CHECK(h.book.volume_at(Side::Sell, 100) == 5 && h.book.volume_at(Side::Sell, 101) == 5);
    return ok;
}

TEST(market_on_empty_side_is_cancelled) {
    bool ok = true;
    Harness h;
    auto& e = h.market(1, Side::Buy, 10);
    CHECK(e.size() == 2 && is(e[0], EventType::Accepted) && is(e[1], EventType::Cancelled));
    if (e.size() == 2) CHECK(e[1].cancel == CancelReason::Unfilled && e[1].qty == 10);
    return ok;
}

TEST(market_fok_needs_enough_liquidity_at_any_price) {
    bool ok = true;
    Harness h;
    h.limit(1, Side::Buy, 100, 5);
    h.limit(2, Side::Buy, 90, 5);
    auto& killed = h.submit({3, 0, Side::Sell, OrderType::Market, TimeInForce::FOK, 0, 11, false});
    CHECK(killed.size() == 2 && is(killed[1], EventType::Cancelled));
    auto& filled = h.submit({4, 0, Side::Sell, OrderType::Market, TimeInForce::FOK, 0, 10, false});
    CHECK(filled.size() == 3 && filled[2].price == 90);
    CHECK(h.book.order_count() == 0);
    return ok;
}

// ---------------------------------------------------------------- post-only

TEST(post_only_rests_or_rejects) {
    bool ok = true;
    Harness h;
    h.limit(1, Side::Sell, 101, 5);
    auto& rests = h.limit(2, Side::Buy, 100, 5, 0, TimeInForce::GTC, true);
    CHECK(rests.size() == 1 && is(rests[0], EventType::Accepted));
    auto& crosses = h.limit(3, Side::Buy, 101, 5, 0, TimeInForce::GTC, true);
    CHECK(crosses.size() == 1 && is(crosses[0], EventType::Rejected));
    if (!crosses.empty()) CHECK(crosses[0].reject == RejectReason::PostOnlyWouldCross);
    CHECK(h.book.volume_at(Side::Sell, 101) == 5);
    CHECK(h.book.order_count() == 2);
    return ok;
}

// ----------------------------------------------------- self-trade prevention

TEST(self_trade_cancels_incoming_remainder) {
    bool ok = true;
    Harness h;
    h.limit(1, Side::Sell, 100, 5, /*acct=*/7);
    auto& e = h.limit(2, Side::Buy, 100, 5, 7);
    CHECK(e.size() == 2 && is(e[1], EventType::Cancelled));
    if (e.size() == 2) CHECK(e[1].cancel == CancelReason::SelfTrade && e[1].qty == 5);
    CHECK(h.book.volume_at(Side::Sell, 100) == 5);  // resting order untouched
    CHECK(!h.book.best_bid());
    return ok;
}

TEST(self_trade_stops_after_trading_ahead_of_own_order) {
    bool ok = true;
    Harness h;
    h.limit(1, Side::Sell, 100, 5, 1);
    h.limit(2, Side::Sell, 100, 5, 2);
    h.limit(3, Side::Sell, 100, 5, 1);
    auto& e = h.limit(4, Side::Buy, 100, 15, 2);
    CHECK(e.size() == 3);
    if (e.size() == 3) {
        CHECK(is(e[1], EventType::Trade) && e[1].counter_id == 1 && e[1].qty == 5);
        CHECK(is(e[2], EventType::Cancelled) && e[2].cancel == CancelReason::SelfTrade && e[2].qty == 10);
    }
    CHECK(h.book.volume_at(Side::Sell, 100) == 10);
    return ok;
}

TEST(self_trade_rules_for_account_zero_and_disabled_config) {
    bool ok = true;
    Harness anonymous;
    anonymous.limit(1, Side::Sell, 100, 5, 0);
    CHECK(anonymous.limit(2, Side::Buy, 100, 5, 0).size() == 2);  // account 0 always trades

    BookConfig cfg;
    cfg.self_trade_prevention = false;
    Harness off(cfg);
    off.limit(1, Side::Sell, 100, 5, 9);
    auto& e = off.limit(2, Side::Buy, 100, 5, 9);
    CHECK(e.size() == 2 && is(e[1], EventType::Trade));
    return ok;
}

TEST(fok_counts_only_liquidity_ahead_of_own_orders) {
    bool ok = true;
    Harness h;
    h.limit(1, Side::Sell, 100, 5, 1);
    h.limit(2, Side::Sell, 100, 5, 2);
    auto& killed = h.limit(3, Side::Buy, 100, 10, 2, TimeInForce::FOK);
    CHECK(killed.size() == 2 && is(killed[1], EventType::Cancelled));
    if (killed.size() == 2) CHECK(killed[1].cancel == CancelReason::FillOrKill);
    auto& filled = h.limit(4, Side::Buy, 100, 5, 2, TimeInForce::FOK);
    CHECK(filled.size() == 2 && is(filled[1], EventType::Trade) && filled[1].counter_id == 1);
    return ok;
}

// ------------------------------------------------------------------- modify

TEST(modify_reduce_keeps_priority) {
    bool ok = true;
    Harness h;
    h.limit(1, Side::Sell, 100, 10);
    h.limit(2, Side::Sell, 100, 10);
    auto& m = h.modify(1, 100, 4);
    CHECK(m.size() == 1 && is(m[0], EventType::Modified) && m[0].leaves == 4);
    CHECK(h.book.volume_at(Side::Sell, 100) == 14);
    auto& e = h.limit(3, Side::Buy, 100, 5);
    CHECK(e.size() == 3);
    if (e.size() == 3) {
        CHECK(e[1].counter_id == 1 && e[1].qty == 4);
        CHECK(e[2].counter_id == 2 && e[2].qty == 1);
    }
    return ok;
}

TEST(modify_increase_loses_priority) {
    bool ok = true;
    Harness h;
    h.limit(1, Side::Sell, 100, 10);
    h.limit(2, Side::Sell, 100, 10);
    CHECK(h.modify(1, 100, 20).size() == 1);
    CHECK(h.book.volume_at(Side::Sell, 100) == 30);
    auto& e = h.limit(3, Side::Buy, 100, 5);
    CHECK(e.size() == 2 && e[1].counter_id == 2);
    return ok;
}

TEST(modify_to_crossing_price_trades) {
    bool ok = true;
    Harness h;
    h.limit(1, Side::Buy, 99, 10);
    h.limit(2, Side::Sell, 101, 5);
    auto& e = h.modify(1, 101, 10);
    CHECK(e.size() == 2);
    if (e.size() == 2) {
        CHECK(is(e[0], EventType::Modified) && e[0].price == 101);
        CHECK(is(e[1], EventType::Trade) && e[1].order_id == 1 && e[1].counter_id == 2 && e[1].leaves == 5);
    }
    CHECK(h.book.best_bid() == 101 && h.book.volume_at(Side::Buy, 101) == 5);
    CHECK(!h.book.best_ask());
    CHECK(h.book.volume_at(Side::Buy, 99) == 0);
    return ok;
}

TEST(rejected_modify_leaves_order_unchanged) {
    bool ok = true;
    Harness h;
    h.limit(1, Side::Buy, 100, 10, 0, TimeInForce::GTC, /*post_only=*/true);
    h.limit(2, Side::Sell, 102, 5);
    auto reason = [&](const std::vector<Event>& e) {
        return e.size() == 1 && is(e[0], EventType::Rejected) ? e[0].reject : RejectReason::None;
    };
    CHECK(reason(h.modify(9, 100, 5)) == RejectReason::UnknownOrder);
    CHECK(reason(h.modify(1, 100, 0)) == RejectReason::InvalidQuantity);
    CHECK(reason(h.modify(1, -5, 10)) == RejectReason::InvalidPrice);
    CHECK(reason(h.modify(1, 102, 10)) == RejectReason::PostOnlyWouldCross);
    auto info = h.book.find(1);
    CHECK(info && info->price == 100 && info->leaves == 10 && info->post_only);
    return ok;
}

// ------------------------------------------------------------------ queries

TEST(depth_top_and_find) {
    bool ok = true;
    Harness h;
    h.limit(1, Side::Buy, 100, 10, 3);
    h.limit(2, Side::Buy, 100, 5);
    h.limit(3, Side::Buy, 99, 7);
    h.limit(4, Side::Buy, 98, 1);
    h.limit(5, Side::Sell, 103, 4);

    std::vector<DepthLevel> d;
    h.book.depth(Side::Buy, 2, d);
    CHECK(d.size() == 2);
    if (d.size() == 2) {
        CHECK(d[0].price == 100 && d[0].qty == 15 && d[0].orders == 2);
        CHECK(d[1].price == 99 && d[1].qty == 7 && d[1].orders == 1);
    }
    d.clear();
    h.book.depth(Side::Sell, 10, d);
    CHECK(d.size() == 1 && d[0].price == 103);

    TopOfBook t = h.book.top();
    CHECK(t.bid == 100 && t.bid_qty == 15 && t.ask == 103 && t.ask_qty == 4);

    auto info = h.book.find(1);
    CHECK(info && info->account == 3 && info->side == Side::Buy && info->price == 100 && info->leaves == 10);
    CHECK(!h.book.find(42));
    return ok;
}

TEST(level_volume_does_not_overflow_quantity) {
    bool ok = true;
    Harness h;
    h.limit(1, Side::Buy, 100, 3'000'000'000u);
    h.limit(2, Side::Buy, 100, 3'000'000'000u);
    CHECK(h.book.volume_at(Side::Buy, 100) == 6'000'000'000ull);
    CHECK(h.book.top().bid_qty == 6'000'000'000ull);
    return ok;
}

TEST(negative_prices_when_configured) {
    bool ok = true;
    BookConfig cfg;
    cfg.min_price = -1000;  // e.g. spreads or some commodity futures
    Harness h(cfg);
    h.limit(1, Side::Sell, -10, 5);
    auto& e = h.limit(2, Side::Buy, -5, 5);
    CHECK(e.size() == 2 && is(e[1], EventType::Trade) && e[1].price == -10);
    return ok;
}
