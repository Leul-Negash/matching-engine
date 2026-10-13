#include "ob/types.hpp"

namespace ob {

std::string_view to_string(Side s) {
    return s == Side::Buy ? "BUY" : "SELL";
}

std::string_view to_string(OrderType t) {
    return t == OrderType::Limit ? "LIMIT" : "MARKET";
}

std::string_view to_string(TimeInForce t) {
    switch (t) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
    }
    return "?";
}

std::string_view to_string(RejectReason r) {
    switch (r) {
        case RejectReason::None: return "NONE";
        case RejectReason::InvalidQuantity: return "INVALID_QUANTITY";
        case RejectReason::InvalidPrice: return "INVALID_PRICE";
        case RejectReason::InvalidOrderType: return "INVALID_ORDER_TYPE";
        case RejectReason::DuplicateOrderId: return "DUPLICATE_ORDER_ID";
        case RejectReason::UnknownOrder: return "UNKNOWN_ORDER";
        case RejectReason::PostOnlyWouldCross: return "POST_ONLY_WOULD_CROSS";
        case RejectReason::UnknownInstrument: return "UNKNOWN_INSTRUMENT";
    }
    return "?";
}

std::string_view to_string(CancelReason r) {
    switch (r) {
        case CancelReason::None: return "NONE";
        case CancelReason::Requested: return "REQUESTED";
        case CancelReason::Unfilled: return "UNFILLED";
        case CancelReason::FillOrKill: return "FILL_OR_KILL";
        case CancelReason::SelfTrade: return "SELF_TRADE";
    }
    return "?";
}

std::string_view to_string(EventType t) {
    switch (t) {
        case EventType::Accepted: return "ACCEPTED";
        case EventType::Rejected: return "REJECTED";
        case EventType::Trade: return "TRADE";
        case EventType::Cancelled: return "CANCELLED";
        case EventType::Modified: return "MODIFIED";
    }
    return "?";
}

}  // namespace ob
