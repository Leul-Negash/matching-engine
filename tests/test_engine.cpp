// Tests for the multi-instrument engine and the write-ahead journal.

#include "ob/engine.hpp"
#include "ob/journal.hpp"
#include "test.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <unistd.h>

using namespace ob;

namespace {

Command new_order(InstrumentId inst, OrderId id, Side side, Price px, Quantity qty, AccountId acct = 0) {
    Command c{};
    c.type = CommandType::New;
    c.instrument = inst;
    c.order_id = id;
    c.side = side;
    c.ord_type = OrderType::Limit;
    c.tif = TimeInForce::GTC;
    c.price = px;
    c.qty = qty;
    c.account = acct;
    c.session = 1;
    return c;
}

Command cancel_cmd(InstrumentId inst, OrderId id) {
    Command c{};
    c.type = CommandType::Cancel;
    c.instrument = inst;
    c.order_id = id;
    return c;
}

Engine make_engine() {
    Engine e;
    e.add_instrument({"AAPL", BookConfig{}});
    BookConfig msft;
    msft.tick_size = 5;
    e.add_instrument({"MSFT", msft});
    return e;
}

// A fresh path in the system temp directory, removed on destruction.
struct TempFile {
    std::string path;
    TempFile() {
        static int n = 0;
        path = (std::filesystem::temp_directory_path() /
                ("ob_test_" + std::to_string(::getpid()) + "_" + std::to_string(n++) + ".journal"))
                   .string();
        std::filesystem::remove(path);
    }
    ~TempFile() { std::filesystem::remove(path); }
};

bool same_state(const Engine& a, const Engine& b) {
    if (a.instrument_count() != b.instrument_count()) return false;
    for (InstrumentId i = 0; i < a.instrument_count(); ++i) {
        const OrderBook& x = a.book(i);
        const OrderBook& y = b.book(i);
        if (x.order_count() != y.order_count() || !(x.top() == y.top())) return false;
        for (Side s : {Side::Buy, Side::Sell}) {
            std::vector<DepthLevel> dx, dy;
            x.depth(s, 1000, dx);
            y.depth(s, 1000, dy);
            if (dx.size() != dy.size()) return false;
            for (std::size_t k = 0; k < dx.size(); ++k) {
                if (dx[k].price != dy[k].price || dx[k].qty != dy[k].qty || dx[k].orders != dy[k].orders) return false;
            }
        }
    }
    return a.last_seq() == b.last_seq() && a.max_order_id() == b.max_order_id();
}

// A random but valid-ish command stream over both instruments.
std::vector<Command> random_commands(std::uint64_t seed, int n) {
    std::mt19937_64 rng(seed);
    auto uni = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    std::vector<Command> cmds;
    OrderId next = 1;
    for (int i = 0; i < n; ++i) {
        const InstrumentId inst = static_cast<InstrumentId>(uni(0, 1));
        const Price tick = inst == 0 ? 1 : 5;
        const int k = uni(0, 9);
        if (k < 6) {
            cmds.push_back(new_order(inst, next++, uni(0, 1) ? Side::Buy : Side::Sell, (100 + uni(-5, 5)) * tick,
                                     static_cast<Quantity>(uni(1, 20)), static_cast<AccountId>(uni(0, 3))));
        } else if (k < 8) {
            cmds.push_back(cancel_cmd(inst, static_cast<OrderId>(uni(1, static_cast<int>(next)))));
        } else {
            Command c{};
            c.type = CommandType::Modify;
            c.instrument = inst;
            c.order_id = static_cast<OrderId>(uni(1, static_cast<int>(next)));
            c.price = (100 + uni(-5, 5)) * tick;
            c.qty = static_cast<Quantity>(uni(1, 20));
            cmds.push_back(c);
        }
    }
    return cmds;
}

}  // namespace

// ------------------------------------------------------------------- engine

TEST(engine_routes_by_instrument_and_sequences_outputs) {
    bool ok = true;
    Engine e = make_engine();
    CHECK(e.find_instrument("AAPL") == 0u && e.find_instrument("MSFT") == 1u && !e.find_instrument("X"));
    std::vector<Output> out;
    e.process(new_order(0, 1, Side::Sell, 100, 10), out);
    e.process(new_order(1, 2, Side::Buy, 100, 10), out);
    e.process(new_order(0, 3, Side::Buy, 100, 4), out);
    CHECK(e.book(0).volume_at(Side::Sell, 100) == 6);
    CHECK(e.book(1).volume_at(Side::Buy, 100) == 10);

    std::uint64_t expect = 1;
    for (const Output& o : out) CHECK(o.seq == expect++);  // gap-free
    CHECK(e.last_seq() == out.size());
    CHECK(e.max_order_id() == 3);
    return ok;
}

TEST(engine_rejects_unknown_instrument) {
    bool ok = true;
    Engine e = make_engine();
    std::vector<Output> out;
    e.process(new_order(7, 1, Side::Buy, 100, 1), out);
    CHECK(out.size() == 1 && out[0].kind == Output::Kind::Order);
    if (!out.empty()) CHECK(out[0].event.reject == RejectReason::UnknownInstrument && out[0].event.order_id == 1);
    return ok;
}

TEST(engine_publishes_quote_only_when_top_changes) {
    bool ok = true;
    Engine e = make_engine();
    std::vector<Output> out;
    auto quotes = [&] {
        int n = 0;
        for (const Output& o : out) n += o.kind == Output::Kind::Quote;
        return n;
    };
    e.process(new_order(0, 1, Side::Buy, 100, 10), out);
    CHECK(quotes() == 1);
    out.clear();
    e.process(new_order(0, 2, Side::Buy, 99, 10), out);  // behind the top: no quote
    CHECK(quotes() == 0);
    out.clear();
    e.process(new_order(0, 3, Side::Buy, 100, 5), out);  // top size changes
    CHECK(quotes() == 1);
    if (!out.empty()) {
        const Output& q = out.back();
        CHECK(q.kind == Output::Kind::Quote && q.quote.bid == 100 && q.quote.bid_qty == 15 && !q.quote.ask);
        CHECK(q.session == 0);
    }
    return ok;
}

TEST(engine_snapshot_lists_levels_best_first) {
    bool ok = true;
    Engine e = make_engine();
    std::vector<Output> out;
    e.process(new_order(1, 1, Side::Buy, 500, 1), out);
    e.process(new_order(1, 2, Side::Buy, 505, 2), out);
    e.process(new_order(1, 3, Side::Sell, 515, 3), out);
    out.clear();
    Command snap{};
    snap.type = CommandType::Snapshot;
    snap.instrument = 1;
    snap.session = 9;
    e.process(snap, out);
    CHECK(out.size() == 4);
    if (out.size() == 4) {
        CHECK(out[0].kind == Output::Kind::Depth && out[0].depth_side == Side::Buy && out[0].level.price == 505);
        CHECK(out[1].level.price == 500);
        CHECK(out[2].depth_side == Side::Sell && out[2].level.price == 515 && out[2].level.qty == 3);
        CHECK(out[3].kind == Output::Kind::DepthEnd && out[3].session == 9);
    }
    CHECK(!Engine::mutates(snap));
    return ok;
}

TEST(engine_rejects_bad_instrument_config) {
    bool ok = true;
    Engine e = make_engine();
    auto throws = [&](const InstrumentConfig& c) {
        try {
            e.add_instrument(c);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    CHECK(throws({"AAPL", {}}));
    CHECK(throws({"", {}}));
    BookConfig bad;
    bad.tick_size = 0;
    CHECK(throws({"BAD", bad}));
    return ok;
}

// ------------------------------------------------------------------ journal

TEST(crc32_matches_standard_check_value) {
    bool ok = true;
    CHECK(crc32("123456789", 9) == 0xCBF43926u);
    return ok;
}

TEST(journal_replay_rebuilds_identical_state) {
    bool ok = true;
    TempFile f;
    Engine live = make_engine();
    std::vector<Output> out;
    {
        JournalWriter w(f.path, JournalWriter::Sync::None);
        for (const Command& c : random_commands(7, 5000)) {
            w.append(c);
            live.process(c, out);
            if (out.size() > 100) {
                w.commit();
                out.clear();
            }
        }
    }  // destructor commits the rest

    Engine recovered = make_engine();
    std::vector<Output> scratch;
    ReplayStats st = replay_journal(f.path, [&](const Command& c) {
        recovered.process(c, scratch);
        scratch.clear();
    });
    CHECK(st.records == 5000 && !st.discarded_torn_tail);
    CHECK(same_state(live, recovered));
    CHECK(live.book(0).order_count() > 0);
    return ok;
}

TEST(journal_appends_across_restarts) {
    bool ok = true;
    TempFile f;
    auto cmds = random_commands(3, 200);
    {
        JournalWriter w(f.path, JournalWriter::Sync::Fsync);
        for (int i = 0; i < 100; ++i) w.append(cmds[i]);
        w.commit();
    }
    {
        JournalWriter w(f.path, JournalWriter::Sync::None);
        for (int i = 100; i < 200; ++i) w.append(cmds[i]);
        w.commit();
    }
    std::vector<Command> back;
    replay_journal(f.path, [&](const Command& c) { back.push_back(c); });
    CHECK(back.size() == 200);
    bool equal = back.size() == cmds.size();
    for (std::size_t i = 0; equal && i < back.size(); ++i) equal = std::memcmp(&back[i], &cmds[i], sizeof(Command)) == 0;
    CHECK(equal);
    return ok;
}

TEST(journal_discards_torn_tail_and_keeps_going) {
    bool ok = true;
    TempFile f;
    auto cmds = random_commands(5, 10);
    {
        JournalWriter w(f.path, JournalWriter::Sync::None);
        for (const Command& c : cmds) w.append(c);
    }
    {
        std::ofstream out(f.path, std::ios::binary | std::ios::app);
        out.write("\x01\x02\x03\x04\x05\x06\x07", 7);  // half-written record from a crash
    }
    std::uint64_t n = 0;
    ReplayStats st = replay_journal(f.path, [&](const Command&) { ++n; });
    CHECK(n == 10 && st.discarded_torn_tail);

    {
        JournalWriter w(f.path, JournalWriter::Sync::None);  // truncates the torn tail
        w.append(cmds[0]);
    }
    st = replay_journal(f.path, [](const Command&) {});
    CHECK(st.records == 11 && !st.discarded_torn_tail);
    return ok;
}

TEST(journal_refuses_mid_file_corruption_and_foreign_files) {
    bool ok = true;
    TempFile f;
    {
        JournalWriter w(f.path, JournalWriter::Sync::None);
        for (const Command& c : random_commands(9, 10)) w.append(c);
    }
    {
        std::fstream io(f.path, std::ios::binary | std::ios::in | std::ios::out);
        io.seekp(16 + 44 * 3 + 10);  // inside the 4th record
        io.put('\x7f');
    }
    auto throws = [](const std::string& p) {
        try {
            replay_journal(p, [](const Command&) {});
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };
    CHECK(throws(f.path));

    TempFile g;
    {
        std::ofstream out(g.path, std::ios::binary);
        out << "this is not a journal at all";
    }
    CHECK(throws(g.path));

    TempFile missing;
    CHECK(!throws(missing.path));  // a missing journal is simply empty
    return ok;
}
