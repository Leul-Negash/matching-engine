#include "ob/protocol.hpp"

#include <array>
#include <cctype>
#include <charconv>

namespace ob::proto {

namespace {

// Splits on runs of spaces/tabs; at most kMaxTokens tokens.
constexpr std::size_t kMaxTokens = 10;

struct Tokens {
    std::array<std::string_view, kMaxTokens> t;
    std::size_t n = 0;
    bool overflow = false;
};

Tokens split(std::string_view line) {
    Tokens out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i == line.size()) break;
        std::size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
        if (out.n == kMaxTokens) {
            out.overflow = true;
            break;
        }
        out.t[out.n++] = line.substr(i, j - i);
        i = j;
    }
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i])) != b[i]) return false;
    }
    return true;
}

bool valid_clordid(std::string_view s) {
    if (s.empty() || s.size() > kMaxClOrdIdLen) return false;
    for (char c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') return false;
    }
    return true;
}

bool valid_symbol(std::string_view s) {
    if (s.empty() || s.size() > kMaxSymbolLen) return false;
    for (char c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_') return false;
    }
    return true;
}

template <class T>
bool parse_num(std::string_view s, T& out) {
    if (s.empty()) return false;
    const char* first = s.data();
    const char* last = s.data() + s.size();
    if (*first == '+') {  // from_chars rejects a leading '+'; accept it, but not "+-5"
        ++first;
        if (first == last || *first == '-') return false;
    }
    auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
}

std::string upper(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

void append_num(std::string& out, std::int64_t v) {
    char buf[24];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, ptr);
}

void append_num(std::string& out, std::uint64_t v) {
    char buf[24];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, ptr);
}

// Appends " token" pieces; keeps the formatters short.
struct Line {
    std::string& out;
    explicit Line(std::string& o, std::string_view head) : out(o) { out.append(head); }
    Line& s(std::string_view v) {
        out.push_back(' ');
        out.append(v);
        return *this;
    }
    Line& i(std::int64_t v) {
        out.push_back(' ');
        append_num(out, v);
        return *this;
    }
    Line& u(std::uint64_t v) {
        out.push_back(' ');
        append_num(out, v);
        return *this;
    }
    ~Line() { out.push_back('\n'); }
};

}  // namespace

std::optional<ClientMessage> parse(std::string_view line, std::string& error) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.size() > kMaxLineLen) {
        error = "line too long";
        return std::nullopt;
    }
    const Tokens tk = split(line);
    if (tk.overflow) {
        error = "too many fields";
        return std::nullopt;
    }
    if (tk.n == 0) {
        error = "empty message";
        return std::nullopt;
    }
    auto fail = [&](std::string why) -> std::optional<ClientMessage> {
        error = std::move(why);
        return std::nullopt;
    };

    const std::string_view cmd = tk.t[0];
    ClientMessage m;

    if (iequals(cmd, "NEW")) {
        if (tk.n < 6) return fail("usage: NEW <clordid> <symbol> <BUY|SELL> <qty> <price|MKT> [GTC|IOC|FOK] [POST]");
        m.type = MsgType::New;
        if (!valid_clordid(tk.t[1])) return fail("invalid clordid");
        m.clordid = tk.t[1];
        if (!valid_symbol(tk.t[2])) return fail("invalid symbol");
        m.symbol = upper(tk.t[2]);
        if (iequals(tk.t[3], "BUY")) {
            m.side = Side::Buy;
        } else if (iequals(tk.t[3], "SELL")) {
            m.side = Side::Sell;
        } else {
            return fail("side must be BUY or SELL");
        }
        if (!parse_num(tk.t[4], m.qty)) return fail("invalid quantity");
        if (iequals(tk.t[5], "MKT")) {
            m.ord_type = OrderType::Market;
            m.tif = TimeInForce::IOC;  // a market order never rests
        } else if (!parse_num(tk.t[5], m.price)) {
            return fail("invalid price");
        }
        bool tif_seen = false;
        for (std::size_t i = 6; i < tk.n; ++i) {
            const std::string_view opt = tk.t[i];
            if (iequals(opt, "POST")) {
                m.post_only = true;
            } else if (!tif_seen && (iequals(opt, "GTC") || iequals(opt, "IOC") || iequals(opt, "FOK"))) {
                tif_seen = true;
                m.tif = iequals(opt, "GTC") ? TimeInForce::GTC
                        : iequals(opt, "IOC") ? TimeInForce::IOC
                                              : TimeInForce::FOK;
            } else {
                return fail("unknown option '" + std::string(opt) + "'");
            }
        }
        if (m.ord_type == OrderType::Market && m.tif == TimeInForce::GTC) {
            return fail("market orders cannot be GTC");
        }
        return m;
    }
    if (iequals(cmd, "CANCEL")) {
        if (tk.n != 2) return fail("usage: CANCEL <clordid>");
        if (!valid_clordid(tk.t[1])) return fail("invalid clordid");
        m.type = MsgType::Cancel;
        m.clordid = tk.t[1];
        return m;
    }
    if (iequals(cmd, "MODIFY")) {
        if (tk.n != 4) return fail("usage: MODIFY <clordid> <price> <qty>");
        if (!valid_clordid(tk.t[1])) return fail("invalid clordid");
        m.type = MsgType::Modify;
        m.clordid = tk.t[1];
        if (!parse_num(tk.t[2], m.price)) return fail("invalid price");
        if (!parse_num(tk.t[3], m.qty)) return fail("invalid quantity");
        return m;
    }
    if (iequals(cmd, "BOOK")) {
        if (tk.n != 2 && tk.n != 3) return fail("usage: BOOK <symbol> [levels]");
        if (!valid_symbol(tk.t[1])) return fail("invalid symbol");
        m.type = MsgType::Book;
        m.symbol = upper(tk.t[1]);
        if (tk.n == 3 && !parse_num(tk.t[2], m.levels)) return fail("invalid levels");
        return m;
    }
    if (iequals(cmd, "SUB") || iequals(cmd, "UNSUB")) {
        if (tk.n != 2) return fail("usage: SUB|UNSUB <symbol>");
        if (!valid_symbol(tk.t[1])) return fail("invalid symbol");
        m.type = iequals(cmd, "SUB") ? MsgType::Subscribe : MsgType::Unsubscribe;
        m.symbol = upper(tk.t[1]);
        return m;
    }
    if (iequals(cmd, "PING")) {
        if (tk.n != 1) return fail("usage: PING");
        m.type = MsgType::Ping;
        return m;
    }
    return fail("unknown command '" + std::string(cmd) + "'");
}

void ack(std::string& out, std::string_view clordid, OrderId id) {
    Line(out, "ACK").s(clordid).u(id);
}

void reject(std::string& out, std::string_view clordid, std::string_view reason) {
    Line(out, "REJECT").s(clordid).s(reason);
}

void fill(std::string& out, std::string_view clordid, Price price, Quantity qty, Quantity leaves) {
    Line(out, "FILL").s(clordid).i(price).u(qty).u(leaves);
}

void canceled(std::string& out, std::string_view clordid, Quantity qty, CancelReason reason) {
    Line(out, "CANCELED").s(clordid).u(qty).s(to_string(reason));
}

void modified(std::string& out, std::string_view clordid, Price price, Quantity leaves) {
    Line(out, "MODIFIED").s(clordid).i(price).u(leaves);
}

void trade(std::string& out, std::string_view symbol, Price price, Quantity qty, Side aggressor) {
    Line(out, "TRADE").s(symbol).i(price).u(qty).s(to_string(aggressor));
}

void bbo(std::string& out, std::string_view symbol, const TopOfBook& top) {
    Line l(out, "BBO");
    l.s(symbol);
    if (top.bid) {
        l.i(*top.bid).u(top.bid_qty);
    } else {
        l.s("-").s("0");
    }
    if (top.ask) {
        l.i(*top.ask).u(top.ask_qty);
    } else {
        l.s("-").s("0");
    }
}

void level(std::string& out, std::string_view symbol, Side side, const DepthLevel& l) {
    Line(out, "LEVEL").s(symbol).s(side == Side::Buy ? "BID" : "ASK").i(l.price).u(l.qty).u(l.orders);
}

void book_end(std::string& out, std::string_view symbol) {
    Line(out, "BOOKEND").s(symbol);
}

void subscribed(std::string& out, std::string_view symbol, bool on) {
    Line(out, on ? "SUBSCRIBED" : "UNSUBSCRIBED").s(symbol);
}

void pong(std::string& out) {
    Line(out, "PONG");
}

void error(std::string& out, std::string_view message) {
    Line(out, "ERROR").s(message);
}

}  // namespace ob::proto
