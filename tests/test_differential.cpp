// Differential test: random request streams are run through the real book and
// through a deliberately naive reference model, and every event plus the
// resulting book state must match exactly.
//
// The reference keeps all resting orders in one vector and finds the best
// counterparty by linear scan, and checks fill-or-kill by trial matching on a
// copy, so it shares no data structures or shortcuts with the real book.

#include "ob/order_book.hpp"
#include "test.hpp"

#include <cstdio>
#include <optional>
#include <random>
#include <vector>

using namespace ob;

namespace {

class ReferenceBook {
public:
    explicit ReferenceBook(const BookConfig& cfg) : cfg_(cfg) {}

    void submit(const NewOrder& o, std::vector<Event>& out) {
        RejectReason r = RejectReason::None;
        if (o.qty == 0 || o.qty > cfg_.max_order_qty || o.qty % cfg_.lot_size != 0) {
            r = RejectReason::InvalidQuantity;
        } else if (o.type == OrderType::Limit && !price_ok(o.price)) {
            r = RejectReason::InvalidPrice;
        } else if (o.post_only && (o.type == OrderType::Market || o.tif != TimeInForce::GTC)) {
            r = RejectReason::InvalidOrderType;
        } else if (find(o.id)) {
            r = RejectReason::DuplicateOrderId;
        } else if (o.post_only && crosses(o.side, o.price)) {
            r = RejectReason::PostOnlyWouldCross;
        }
        const Price px = o.type == OrderType::Limit ? o.price : 0;
        if (r != RejectReason::None) {
            Event e{};
            e.type = EventType::Rejected;
            e.side = o.side;
            e.reject = r;
            e.qty = o.qty;
            e.order_id = o.id;
            e.price = px;
            out.push_back(e);
            return;
        }
        Event ack{};
        ack.type = EventType::Accepted;
        ack.side = o.side;
        ack.qty = o.qty;
        ack.order_id = o.id;
        ack.price = px;
        ack.leaves = o.qty;
        out.push_back(ack);
        execute(o, out);
    }

    void cancel(OrderId id, std::vector<Event>& out) {
        Order* ord = find(id);
        Event e{};
        e.order_id = id;
        if (!ord) {
            e.type = EventType::Rejected;
            e.reject = RejectReason::UnknownOrder;
            out.push_back(e);
            return;
        }
        e.type = EventType::Cancelled;
        e.side = ord->side;
        e.cancel = CancelReason::Requested;
        e.qty = ord->qty;
        e.price = ord->price;
        erase(id);
        out.push_back(e);
    }

    void modify(OrderId id, Price px, Quantity qty, std::vector<Event>& out) {
        Order* ord = find(id);
        auto reject = [&](RejectReason r, Side side) {
            Event e{};
            e.type = EventType::Rejected;
            e.side = side;
            e.reject = r;
            e.qty = qty;
            e.order_id = id;
            e.price = px;
            out.push_back(e);
        };
        if (!ord) return reject(RejectReason::UnknownOrder, Side::Buy);
        if (qty == 0 || qty > cfg_.max_order_qty || qty % cfg_.lot_size != 0) {
            return reject(RejectReason::InvalidQuantity, ord->side);
        }
        if (!price_ok(px)) return reject(RejectReason::InvalidPrice, ord->side);

        Event m{};
        m.type = EventType::Modified;
        m.side = ord->side;
        m.qty = qty;
        m.order_id = id;
        m.price = px;
        m.leaves = qty;
        if (px == ord->price && qty <= ord->qty) {
            ord->qty = qty;  // keeps its sequence number, i.e. its place in line
            out.push_back(m);
            return;
        }
        if (ord->post_only && crosses(ord->side, px)) return reject(RejectReason::PostOnlyWouldCross, ord->side);
        const NewOrder repl{id, ord->account, ord->side, OrderType::Limit, TimeInForce::GTC, px, qty, ord->post_only};
        erase(id);
        out.push_back(m);
        execute(repl, out);
    }

    std::size_t count() const { return orders_.size(); }

    TopOfBook top() const {
        TopOfBook t;
        for (const Order& o : orders_) {
            auto& best = o.side == Side::Buy ? t.bid : t.ask;
            const bool better = !best || (o.side == Side::Buy ? o.price > *best : o.price < *best);
            if (better) best = o.price;
        }
        for (const Order& o : orders_) {
            if (o.side == Side::Buy && o.price == t.bid) t.bid_qty += o.qty;
            if (o.side == Side::Sell && o.price == t.ask) t.ask_qty += o.qty;
        }
        return t;
    }

    Volume volume_at(Side side, Price px) const {
        Volume v = 0;
        for (const Order& o : orders_) {
            if (o.side == side && o.price == px) v += o.qty;
        }
        return v;
    }

private:
    struct Order {
        OrderId id;
        AccountId account;
        Side side;
        Price price;
        Quantity qty;
        bool post_only;
        std::uint64_t seq;  // arrival time; lower = earlier
    };

    bool price_ok(Price p) const {
        return p >= cfg_.min_price && p <= cfg_.max_price && p % cfg_.tick_size == 0;
    }

    Order* find(OrderId id) {
        for (Order& o : orders_) {
            if (o.id == id) return &o;
        }
        return nullptr;
    }

    void erase(OrderId id) {
        for (std::size_t i = 0; i < orders_.size(); ++i) {
            if (orders_[i].id == id) {
                orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }

    // Best resting order on `side`: best price, then earliest arrival.
    std::optional<std::size_t> best(Side side) const {
        std::optional<std::size_t> b;
        for (std::size_t i = 0; i < orders_.size(); ++i) {
            const Order& o = orders_[i];
            if (o.side != side) continue;
            if (!b) {
                b = i;
                continue;
            }
            const Order& cur = orders_[*b];
            const bool better_price = side == Side::Buy ? o.price > cur.price : o.price < cur.price;
            if (better_price || (o.price == cur.price && o.seq < cur.seq)) b = i;
        }
        return b;
    }

    static bool acceptable(Side incoming, std::optional<Price> limit, Price resting) {
        if (!limit) return true;
        return incoming == Side::Buy ? resting <= *limit : resting >= *limit;
    }

    bool crosses(Side side, Price px) const {
        auto b = best(opposite(side));
        return b && acceptable(side, px, orders_[*b].price);
    }

    enum class Stop { Filled, Other, SelfTrade };

    // Pure matching on this->orders_; appends trades, returns remaining qty.
    Stop match(const NewOrder& o, std::optional<Price> limit, Quantity& qty, std::vector<Event>& out) {
        while (qty > 0) {
            auto b = best(opposite(o.side));
            if (!b || !acceptable(o.side, limit, orders_[*b].price)) return Stop::Other;
            Order& r = orders_[*b];
            if (cfg_.self_trade_prevention && o.account != 0 && o.account == r.account) return Stop::SelfTrade;
            const Quantity t = std::min(qty, r.qty);
            qty -= t;
            r.qty -= t;
            Event e{};
            e.type = EventType::Trade;
            e.side = o.side;
            e.qty = t;
            e.order_id = o.id;
            e.counter_id = r.id;
            e.price = r.price;
            e.leaves = qty;
            e.counter_leaves = r.qty;
            out.push_back(e);
            if (r.qty == 0) erase(r.id);
        }
        return Stop::Filled;
    }

    void execute(const NewOrder& o, std::vector<Event>& out) {
        const std::optional<Price> limit =
            o.type == OrderType::Limit ? std::optional<Price>(o.price) : std::nullopt;
        auto cancel_rest = [&](CancelReason why, Quantity q) {
            Event e{};
            e.type = EventType::Cancelled;
            e.side = o.side;
            e.cancel = why;
            e.qty = q;
            e.order_id = o.id;
            e.price = limit.value_or(0);
            out.push_back(e);
        };

        if (o.tif == TimeInForce::FOK) {
            ReferenceBook trial = *this;  // trial run on a copy
            std::vector<Event> scratch;
            Quantity q = o.qty;
            trial.match(o, limit, q, scratch);
            if (q != 0) return cancel_rest(CancelReason::FillOrKill, o.qty);
        }

        Quantity qty = o.qty;
        const Stop stop = match(o, limit, qty, out);
        if (qty == 0) return;
        if (stop == Stop::SelfTrade) return cancel_rest(CancelReason::SelfTrade, qty);
        if (o.type == OrderType::Market || o.tif != TimeInForce::GTC) return cancel_rest(CancelReason::Unfilled, qty);
        orders_.push_back({o.id, o.account, o.side, o.price, qty, o.post_only, next_seq_++});
    }

    BookConfig cfg_;
    std::vector<Order> orders_;
    std::uint64_t next_seq_ = 0;
};

bool same(const Event& a, const Event& b) {
    return a.type == b.type && a.side == b.side && a.reject == b.reject && a.cancel == b.cancel &&
           a.qty == b.qty && a.order_id == b.order_id && a.counter_id == b.counter_id &&
           a.price == b.price && a.leaves == b.leaves && a.counter_leaves == b.counter_leaves;
}

void print(const char* who, const std::vector<Event>& ev) {
    std::printf("    %s:\n", who);
    for (const Event& e : ev) {
        std::printf("      %-9s id=%llu ctr=%llu side=%s px=%lld qty=%u leaves=%u ctr_leaves=%u rej=%s cxl=%s\n",
                    std::string(to_string(e.type)).c_str(), static_cast<unsigned long long>(e.order_id),
                    static_cast<unsigned long long>(e.counter_id), std::string(to_string(e.side)).c_str(),
                    static_cast<long long>(e.price), e.qty, e.leaves, e.counter_leaves,
                    std::string(to_string(e.reject)).c_str(), std::string(to_string(e.cancel)).c_str());
    }
}

// Runs one random stream; returns false and prints the first divergence.
bool run_stream(std::uint64_t seed, int ops) {
    std::mt19937_64 rng(seed);
    auto uni = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };

    BookConfig cfg;
    cfg.tick_size = uni(0, 3) == 0 ? 5 : 1;
    cfg.lot_size = uni(0, 3) == 0 ? 10 : 1;
    cfg.max_order_qty = 500;
    cfg.min_price = 1;
    cfg.max_price = 1000;
    cfg.self_trade_prevention = uni(0, 4) != 0;

    OrderBook real(cfg);
    ReferenceBook ref(cfg);
    std::vector<Event> a;
    std::vector<Event> b;
    OrderId next_id = 1;

    for (int i = 0; i < ops; ++i) {
        a.clear();
        b.clear();
        const int kind = uni(0, 99);
        // Ids: mostly fresh, sometimes an old one (duplicate / unknown / dead).
        auto old_id = [&] { return static_cast<OrderId>(uni(1, static_cast<int>(next_id))); };
        // Prices cluster tightly so orders cross often; occasionally off-tick or out of range.
        auto price = [&]() -> Price {
            const int r = uni(0, 99);
            if (r == 0) return 0;
            if (r == 1) return 1005;
            Price p = 500 + uni(-6, 6) * cfg.tick_size;
            if (r < 5) p += 1;
            return p;
        };
        auto qty = [&]() -> Quantity {
            const int r = uni(0, 99);
            if (r == 0) return 0;
            if (r == 1) return 510;
            return static_cast<Quantity>(uni(1, 12) * cfg.lot_size);
        };

        if (kind < 55) {
            NewOrder o;
            o.id = uni(0, 30) == 0 ? old_id() : next_id++;
            o.account = static_cast<AccountId>(uni(0, 3));
            o.side = uni(0, 1) ? Side::Buy : Side::Sell;
            o.type = uni(0, 9) == 0 ? OrderType::Market : OrderType::Limit;
            const int tif = uni(0, 9);
            o.tif = tif < 7 ? TimeInForce::GTC : tif < 9 ? TimeInForce::IOC : TimeInForce::FOK;
            o.price = price();
            o.qty = qty();
            o.post_only = uni(0, 9) == 0;
            real.submit(o, a);
            ref.submit(o, b);
        } else if (kind < 80) {
            const OrderId id = old_id();
            real.cancel(id, a);
            ref.cancel(id, b);
        } else {
            const OrderId id = old_id();
            const Price p = price();
            const Quantity q = qty();
            real.modify(id, p, q, a);
            ref.modify(id, p, q, b);
        }

        bool ok = a.size() == b.size();
        for (std::size_t j = 0; ok && j < a.size(); ++j) ok = same(a[j], b[j]);
        ok = ok && real.order_count() == ref.count() && real.top() == ref.top();
        for (Price p = 470; ok && p <= 530; p += 1) {
            ok = real.volume_at(Side::Buy, p) == ref.volume_at(Side::Buy, p) &&
                 real.volume_at(Side::Sell, p) == ref.volume_at(Side::Sell, p);
        }
        if (!ok) {
            std::printf("    divergence: seed %llu, op %d\n", static_cast<unsigned long long>(seed), i);
            print("book", a);
            print("reference", b);
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(differential_against_reference_model) {
    bool ok = true;
    for (std::uint64_t seed = 1; seed <= 150 && ok; ++seed) {
        ok = run_stream(seed, 2000);
    }
    return ok;
}
