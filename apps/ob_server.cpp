// Order book server: recovers state from the journal, then serves the text
// protocol over TCP until SIGINT/SIGTERM.
//
//   ob_server --symbol AAPL --symbol MSFT:5:10 --port 9000 --journal data/ob.journal

#include "ob/engine.hpp"
#include "ob/journal.hpp"
#include "ob/log.hpp"
#include "ob/server.hpp"

#include <cctype>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

ob::Server* g_server = nullptr;

void on_signal(int) {
    if (g_server) g_server->stop();  // only writes to an eventfd: async-signal-safe
}

void usage() {
    std::fprintf(stderr,
                 "usage: ob_server --symbol SYM[:tick[:lot]] [--symbol ...] [options]\n"
                 "  --bind ADDR                 IPv4 address to listen on (default 127.0.0.1)\n"
                 "  --port N                    TCP port (default 9000; 0 = any free port)\n"
                 "  --journal PATH              write-ahead journal; replayed on startup\n"
                 "  --fsync                     fsync the journal before acknowledging (survives power loss)\n"
                 "  --no-cancel-on-disconnect   leave a session's orders resting when it drops\n"
                 "  --busy-poll                 spin instead of sleeping when idle (lowest latency, 2 cores at 100%%)\n"
                 "  --max-sessions N            default 1000\n"
                 "  --max-rate N                messages per second per session, 0 = unlimited (default 100000)\n"
                 "  --log-level LEVEL           debug, info, warn or error (default info)\n");
}

template <class T>
std::optional<T> number(std::string_view s) {
    T v{};
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

// SYM[:tick[:lot]]
std::optional<ob::InstrumentConfig> parse_symbol(std::string_view spec) {
    ob::InstrumentConfig cfg;
    const auto c1 = spec.find(':');
    cfg.symbol = std::string(spec.substr(0, c1));
    for (char& ch : cfg.symbol) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (c1 == std::string_view::npos) return cfg;
    const std::string_view rest = spec.substr(c1 + 1);
    const auto c2 = rest.find(':');
    const auto tick = number<ob::Price>(rest.substr(0, c2));
    if (!tick) return std::nullopt;
    cfg.book.tick_size = *tick;
    if (c2 != std::string_view::npos) {
        const auto lot = number<ob::Quantity>(rest.substr(c2 + 1));
        if (!lot) return std::nullopt;
        cfg.book.lot_size = *lot;
    }
    return cfg;
}

}  // namespace

int main(int argc, char** argv) {
    ob::ServerConfig server_cfg;
    std::vector<ob::InstrumentConfig> instruments;
    std::string journal_path;
    bool fsync = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto value = [&]() -> std::string_view {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", argv[i]);
                std::exit(2);
            }
            return argv[++i];
        };
        auto bad = [&](std::string_view v) {
            std::fprintf(stderr, "invalid value '%.*s' for %.*s\n", static_cast<int>(v.size()), v.data(),
                         static_cast<int>(arg.size()), arg.data());
            std::exit(2);
        };

        if (arg == "--symbol") {
            const std::string_view v = value();
            auto inst = parse_symbol(v);
            if (!inst) bad(v);
            instruments.push_back(*inst);
        } else if (arg == "--bind") {
            server_cfg.bind_address = std::string(value());
        } else if (arg == "--port") {
            const std::string_view v = value();
            auto p = number<std::uint16_t>(v);
            if (!p) bad(v);
            server_cfg.port = *p;
        } else if (arg == "--journal") {
            journal_path = std::string(value());
        } else if (arg == "--fsync") {
            fsync = true;
        } else if (arg == "--busy-poll") {
            server_cfg.busy_poll = true;
        } else if (arg == "--no-cancel-on-disconnect") {
            server_cfg.cancel_on_disconnect = false;
        } else if (arg == "--max-sessions") {
            const std::string_view v = value();
            auto n = number<std::size_t>(v);
            if (!n || *n == 0) bad(v);
            server_cfg.max_sessions = *n;
        } else if (arg == "--max-rate") {
            const std::string_view v = value();
            auto n = number<std::uint32_t>(v);
            if (!n) bad(v);
            server_cfg.max_msgs_per_sec = *n;
        } else if (arg == "--log-level") {
            const std::string_view v = value();
            if (v == "debug") {
                ob::log::set_level(ob::log::Level::Debug);
            } else if (v == "info") {
                ob::log::set_level(ob::log::Level::Info);
            } else if (v == "warn") {
                ob::log::set_level(ob::log::Level::Warn);
            } else if (v == "error") {
                ob::log::set_level(ob::log::Level::Error);
            } else {
                bad(v);
            }
        } else if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            usage();
            return 2;
        }
    }
    if (instruments.empty()) {
        std::fprintf(stderr, "at least one --symbol is required\n");
        usage();
        return 2;
    }

    try {
        ob::Engine engine;
        for (const auto& inst : instruments) {
            engine.add_instrument(inst);
            OB_LOG(Info, "instrument %s tick=%lld lot=%u", inst.symbol.c_str(),
                   static_cast<long long>(inst.book.tick_size), inst.book.lot_size);
        }

        // Recovery: replay the journal through the (deterministic) engine.
        std::unique_ptr<ob::JournalWriter> journal;
        if (!journal_path.empty()) {
            std::vector<ob::Output> scratch;
            const ob::ReplayStats rs = ob::replay_journal(journal_path, [&](const ob::Command& c) {
                engine.process(c, scratch);
                scratch.clear();
            });
            std::size_t resting = 0;
            for (ob::InstrumentId i = 0; i < engine.instrument_count(); ++i) resting += engine.book(i).order_count();
            OB_LOG(Info, "journal %s: replayed %llu commands, %zu orders resting%s", journal_path.c_str(),
                   static_cast<unsigned long long>(rs.records), resting,
                   rs.discarded_torn_tail ? " (discarded a torn final record)" : "");
            journal = std::make_unique<ob::JournalWriter>(
                journal_path, fsync ? ob::JournalWriter::Sync::Fsync : ob::JournalWriter::Sync::None);
        } else {
            OB_LOG(Warn, "no --journal: state is lost on exit");
        }

        ob::Server server(server_cfg, engine, journal.get());
        const std::uint16_t port = server.listen();
        OB_LOG(Info, "listening on %s:%u", server_cfg.bind_address.c_str(), port);

        g_server = &server;
        struct sigaction sa{};
        sa.sa_handler = on_signal;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
        std::signal(SIGPIPE, SIG_IGN);

        const bool clean = server.run();
        g_server = nullptr;
        return clean ? 0 : 1;
    } catch (const std::exception& e) {
        OB_LOG(Error, "fatal: %s", e.what());
        return 1;
    }
}
