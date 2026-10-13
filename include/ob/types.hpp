#pragma once

#include <cstdint>
#include <string_view>

namespace ob {

using OrderId = std::uint64_t;
using AccountId = std::uint32_t;  // 0 = no account (self-trade prevention off)
using Price = std::int64_t;       // price in ticks (e.g. cents), never floating point
using Quantity = std::uint32_t;   // size of one order
using Volume = std::uint64_t;     // sum of many orders' sizes; can't overflow in practice

enum class Side : std::uint8_t { Buy, Sell };

enum class OrderType : std::uint8_t {
    Limit,   // trades at the limit price or better
    Market,  // trades at any price; never rests
};

enum class TimeInForce : std::uint8_t {
    GTC,  // good till cancel: the unfilled remainder rests in the book
    IOC,  // immediate or cancel: trade what is possible now, cancel the rest
    FOK,  // fill or kill: trade the whole quantity now, or nothing at all
};

enum class RejectReason : std::uint8_t {
    None,
    InvalidQuantity,     // zero, above the maximum, or not a multiple of the lot size
    InvalidPrice,        // outside the allowed range or not on the tick grid
    InvalidOrderType,    // e.g. a post-only market order
    DuplicateOrderId,    // an order with this id is already resting
    UnknownOrder,        // cancel/modify of an id that is not resting
    PostOnlyWouldCross,  // a post-only order would have traded on arrival
    UnknownInstrument,   // engine level: no book for the instrument
};

enum class CancelReason : std::uint8_t {
    None,
    Requested,     // the owner asked for it
    Unfilled,      // IOC or market remainder that could not trade
    FillOrKill,    // FOK order that could not be filled in full
    SelfTrade,     // remainder would have traded with the same account
};

struct NewOrder {
    OrderId id = 0;
    AccountId account = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::GTC;
    Price price = 0;  // ignored for market orders
    Quantity qty = 0;
    bool post_only = false;  // reject instead of trading on arrival (limit only)
};

enum class EventType : std::uint8_t {
    Accepted,   // order passed validation; trades (if any) follow
    Rejected,   // request refused; book unchanged
    Trade,      // order_id (aggressor) traded with counter_id (resting order)
    Cancelled,  // qty was removed from the book or never rested
    Modified,   // order now has `price` and `leaves`
};

// One execution report. Every request produces a sequence of these, in the
// order things happened. Fields that don't apply to a type are zero.
struct Event {
    EventType type;
    Side side;              // side of order_id
    RejectReason reject;    // Rejected
    CancelReason cancel;    // Cancelled
    Quantity qty;           // Trade: traded qty; Cancelled: cancelled qty; Accepted/Modified: order qty
    OrderId order_id;       // the order the event is about (the aggressor for a Trade)
    OrderId counter_id;     // Trade: the resting order that was hit
    Price price;            // Trade: execution price (the resting order's); Accepted/Modified: limit price
    Quantity leaves;        // order_id's remaining open quantity after this event
    Quantity counter_leaves;  // Trade: counter_id's remaining open quantity
};

// A trade in the simple buyer/seller form.
struct Trade {
    OrderId buy_id;
    OrderId sell_id;
    Price price;  // the resting order's price
    Quantity quantity;
};

inline Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

std::string_view to_string(Side s);
std::string_view to_string(OrderType t);
std::string_view to_string(TimeInForce t);
std::string_view to_string(RejectReason r);
std::string_view to_string(CancelReason r);
std::string_view to_string(EventType t);

}  // namespace ob
