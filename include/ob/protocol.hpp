#pragma once

// Line-based text protocol for the order entry gateway. One message per line,
// tokens separated by spaces, keywords case-insensitive. See docs/PROTOCOL.md.
//
// Client -> server:
//   NEW <clordid> <symbol> <BUY|SELL> <qty> <price|MKT> [GTC|IOC|FOK] [POST]
//   CANCEL <clordid>
//   MODIFY <clordid> <price> <qty>
//   BOOK <symbol> [levels]
//   SUB <symbol> | UNSUB <symbol>
//   PING
//
// Server -> client: ACK, REJECT, FILL, CANCELED, MODIFIED, TRADE, BBO, LEVEL,
// BOOKEND, SUBSCRIBED, UNSUBSCRIBED, PONG, ERROR (formatted below).

#include "ob/order_book.hpp"
#include "ob/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ob::proto {

constexpr std::size_t kMaxClOrdIdLen = 32;
constexpr std::size_t kMaxSymbolLen = 16;
constexpr std::size_t kMaxLineLen = 256;

enum class MsgType : std::uint8_t { New, Cancel, Modify, Book, Subscribe, Unsubscribe, Ping };

struct ClientMessage {
    MsgType type = MsgType::Ping;
    std::string clordid;  // New / Cancel / Modify: client's own order id
    std::string symbol;   // New / Book / Subscribe / Unsubscribe
    Side side = Side::Buy;
    OrderType ord_type = OrderType::Limit;
    TimeInForce tif = TimeInForce::GTC;
    bool post_only = false;
    Price price = 0;
    Quantity qty = 0;
    std::uint32_t levels = 0;  // Book; 0 = server default
};

// Parses one line (without the trailing newline). On failure returns
// nullopt and sets `error` to a human-readable reason.
std::optional<ClientMessage> parse(std::string_view line, std::string& error);

// Formatters: each appends one complete line (with '\n') to `out`.
void ack(std::string& out, std::string_view clordid, OrderId id);
void reject(std::string& out, std::string_view clordid, std::string_view reason);
void fill(std::string& out, std::string_view clordid, Price price, Quantity qty, Quantity leaves);
void canceled(std::string& out, std::string_view clordid, Quantity qty, CancelReason reason);
void modified(std::string& out, std::string_view clordid, Price price, Quantity leaves);
void trade(std::string& out, std::string_view symbol, Price price, Quantity qty, Side aggressor);
void bbo(std::string& out, std::string_view symbol, const TopOfBook& top);
void level(std::string& out, std::string_view symbol, Side side, const DepthLevel& l);
void book_end(std::string& out, std::string_view symbol);
void subscribed(std::string& out, std::string_view symbol, bool on);
void pong(std::string& out);
void error(std::string& out, std::string_view message);

}  // namespace ob::proto
