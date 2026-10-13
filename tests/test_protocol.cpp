// Tests for the text protocol parser and formatters.

#include "ob/protocol.hpp"
#include "test.hpp"

#include <string>

using namespace ob;
using namespace ob::proto;

namespace {

std::optional<ClientMessage> p(std::string_view line) {
    std::string err;
    return parse(line, err);
}

std::string err_of(std::string_view line) {
    std::string err;
    parse(line, err);
    return err;
}

}  // namespace

TEST(parse_new_limit_with_options) {
    bool ok = true;
    auto m = p("NEW o-1 aapl BUY 100 15025 IOC");
    CHECK(m && m->type == MsgType::New);
    if (m) {
        CHECK(m->clordid == "o-1" && m->symbol == "AAPL" && m->side == Side::Buy);
        CHECK(m->qty == 100 && m->price == 15025 && m->ord_type == OrderType::Limit);
        CHECK(m->tif == TimeInForce::IOC && !m->post_only);
    }
    auto post = p("new x MSFT sell 5 +200 post gtc\r");
    CHECK(post && post->side == Side::Sell && post->post_only && post->tif == TimeInForce::GTC && post->price == 200);
    auto neg = p("NEW n CL BUY 1 -37");
    CHECK(neg && neg->price == -37);
    return ok;
}

TEST(parse_new_market) {
    bool ok = true;
    auto m = p("NEW m1 AAPL SELL 10 MKT");
    CHECK(m && m->ord_type == OrderType::Market && m->tif == TimeInForce::IOC);
    auto fok = p("NEW m2 AAPL SELL 10 MKT FOK");
    CHECK(fok && fok->tif == TimeInForce::FOK);
    CHECK(!p("NEW m3 AAPL SELL 10 MKT GTC"));
    return ok;
}

TEST(parse_other_messages) {
    bool ok = true;
    auto c = p("CANCEL abc");
    CHECK(c && c->type == MsgType::Cancel && c->clordid == "abc");
    auto mod = p("MODIFY abc 101 7");
    CHECK(mod && mod->type == MsgType::Modify && mod->price == 101 && mod->qty == 7);
    auto b = p("BOOK msft 5");
    CHECK(b && b->type == MsgType::Book && b->symbol == "MSFT" && b->levels == 5);
    auto b0 = p("BOOK msft");
    CHECK(b0 && b0->levels == 0);
    CHECK(p("SUB aapl") && p("SUB aapl")->type == MsgType::Subscribe);
    CHECK(p("UNSUB aapl") && p("UNSUB aapl")->type == MsgType::Unsubscribe);
    CHECK(p("  ping  ") && p("ping")->type == MsgType::Ping);
    return ok;
}

TEST(parse_rejects_malformed_input) {
    bool ok = true;
    CHECK(!p(""));
    CHECK(!p("HELLO"));
    CHECK(!p("NEW a AAPL BUY 10"));                    // missing price
    CHECK(!p("NEW a AAPL HOLD 10 100"));               // bad side
    CHECK(!p("NEW a AAPL BUY -10 100"));               // negative qty
    CHECK(!p("NEW a AAPL BUY 5000000000 100"));        // qty overflow
    CHECK(!p("NEW a AAPL BUY 10 1.5"));                // fractional price
    CHECK(!p("NEW a AAPL BUY 10 +-5"));
    CHECK(!p("NEW a AAPL BUY 10 100 GTC IOC"));        // two TIFs
    CHECK(!p("NEW a AAPL BUY 10 100 NOW"));            // unknown option
    CHECK(!p("NEW bad/id AAPL BUY 10 100"));           // invalid clordid chars
    CHECK(!p("NEW " + std::string(33, 'x') + " AAPL BUY 10 100"));
    CHECK(!p("NEW a WAYTOOLONGSYMBOL123 BUY 10 100"));
    CHECK(!p("CANCEL"));
    CHECK(!p("CANCEL a b"));
    CHECK(!p("MODIFY a 100"));
    CHECK(!p("PING extra"));
    CHECK(!p(std::string(300, 'A')));
    CHECK(!p("NEW a b c d e f g h i j k"));            // too many fields
    CHECK(err_of("FOO") == "unknown command 'FOO'");
    return ok;
}

TEST(format_messages) {
    bool ok = true;
    std::string out;
    ack(out, "o1", 42);
    fill(out, "o1", -5, 10, 0);
    canceled(out, "o2", 3, CancelReason::SelfTrade);
    modified(out, "o3", 101, 7);
    reject(out, "o4", "UNKNOWN_ORDER");
    trade(out, "AAPL", 100, 5, Side::Sell);
    TopOfBook t;
    t.bid = 99;
    t.bid_qty = 12;
    bbo(out, "AAPL", t);
    level(out, "AAPL", Side::Sell, DepthLevel{101, 30, 2});
    book_end(out, "AAPL");
    pong(out);
    error(out, "bad input");
    CHECK(out ==
          "ACK o1 42\n"
          "FILL o1 -5 10 0\n"
          "CANCELED o2 3 SELF_TRADE\n"
          "MODIFIED o3 101 7\n"
          "REJECT o4 UNKNOWN_ORDER\n"
          "TRADE AAPL 100 5 SELL\n"
          "BBO AAPL 99 12 - 0\n"
          "LEVEL AAPL ASK 101 30 2\n"
          "BOOKEND AAPL\n"
          "PONG\n"
          "ERROR bad input\n");
    return ok;
}
