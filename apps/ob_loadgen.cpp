// Load generator: measures end-to-end round-trip latency and throughput of a
// running ob_server over TCP.
//
// Each connection runs on its own thread and keeps `--window` requests in
// flight. A request is a NEW order (70% GTC near the mid, 30% IOC that may
// cross) or a CANCEL of one of the connection's earlier resting orders.
// Latency is measured from just before send() to the arrival of the first
// reply for that order (ACK / REJECT for NEW, CANCELED / REJECT for CANCEL).
//
//   ob_loadgen --port 9000 --symbol AAPL --connections 4 --requests 200000

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <random>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 9000;
    std::string symbol = "AAPL";
    int connections = 4;
    std::uint64_t requests = 200'000;  // per connection
    std::uint32_t window = 32;         // requests in flight per connection
    long mid = 10'000;
};

struct Result {
    std::vector<std::int64_t> latencies_ns;
    std::uint64_t fills = 0;
    std::uint64_t rejects = 0;
    std::uint64_t errors = 0;
    bool failed = false;
};

int connect_to(const Options& o) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(o.port);
    if (::inet_pton(AF_INET, o.host.c_str(), &addr.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return -1;
    }
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

bool send_all(int fd, const std::string& s) {
    std::size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

std::string_view token(std::string_view line, int index) {
    std::size_t pos = 0;
    for (int i = 0; i <= index; ++i) {
        const std::size_t start = pos;
        pos = line.find(' ', start);
        if (i == index) return line.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start);
        if (pos == std::string_view::npos) return {};
        ++pos;
    }
    return {};
}

void run_connection(const Options& o, int conn, std::atomic<bool>& go, Result& r) {
    const int fd = connect_to(o);
    if (fd < 0) {
        r.failed = true;
        return;
    }
    std::mt19937_64 rng(1000 + static_cast<std::uint64_t>(conn));
    std::uniform_int_distribution<int> pct(0, 99);
    std::uniform_int_distribution<int> offset(1, 5);
    std::uniform_int_distribution<int> qty(1, 10);

    std::unordered_map<std::string, Clock::time_point> pending;  // request key -> send time
    std::vector<std::string> resting;                            // our GTC orders, candidates for cancel
    r.latencies_ns.reserve(o.requests);

    std::uint64_t sent = 0;
    std::uint64_t order_seq = 0;
    std::string out;
    std::string in;
    char buf[64 * 1024];

    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();

    auto send_more = [&]() -> bool {
        out.clear();
        std::vector<std::string> keys;
        while (pending.size() + keys.size() < o.window && sent < o.requests) {
            ++sent;
            if (!resting.empty() && pct(rng) < 30) {
                std::string id = resting.back();
                resting.pop_back();
                out += "CANCEL " + id + "\n";
                keys.push_back("C" + id);
                continue;
            }
            const std::string id = "c" + std::to_string(conn) + "-" + std::to_string(++order_seq);
            const bool buy = pct(rng) < 50;
            const bool ioc = pct(rng) < 30;
            // Passive orders rest behind the mid; IOC orders reach across it.
            const long px = buy ? (ioc ? o.mid + offset(rng) : o.mid - offset(rng))
                                : (ioc ? o.mid - offset(rng) : o.mid + offset(rng));
            out += "NEW " + id + " " + o.symbol + (buy ? " BUY " : " SELL ") + std::to_string(qty(rng)) + " " +
                   std::to_string(px) + (ioc ? " IOC\n" : "\n");
            keys.push_back("N" + id);
            if (!ioc) resting.push_back(id);
        }
        if (out.empty()) return true;
        const auto now = Clock::now();
        for (auto& k : keys) pending.emplace(std::move(k), now);
        return send_all(fd, out);
    };

    if (!send_more()) r.failed = true;
    while (!r.failed && (!pending.empty() || sent < o.requests)) {
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) {
            r.failed = true;
            break;
        }
        const auto now = Clock::now();
        in.append(buf, static_cast<std::size_t>(n));
        std::size_t start = 0;
        for (;;) {
            const std::size_t nl = in.find('\n', start);
            if (nl == std::string::npos) break;
            const std::string_view line(in.data() + start, nl - start);
            start = nl + 1;

            const std::string_view kind = token(line, 0);
            const std::string id(token(line, 1));
            auto complete = [&](char prefix) {
                auto it = pending.find(prefix + id);
                if (it == pending.end()) return false;
                r.latencies_ns.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(now - it->second).count());
                pending.erase(it);
                return true;
            };
            if (kind == "ACK") {
                complete('N');
            } else if (kind == "REJECT") {
                ++r.rejects;
                if (!complete('C')) complete('N');
            } else if (kind == "CANCELED") {
                if (!complete('C')) complete('N');  // IOC remainders are CANCELED without an ACK wait
            } else if (kind == "FILL") {
                ++r.fills;
            } else if (kind == "ERROR") {
                ++r.errors;
            }
        }
        in.erase(0, start);
        if (!send_more()) r.failed = true;
    }
    ::close(fd);
}

double pct_of(const std::vector<std::int64_t>& v, double p) {
    if (v.empty()) return 0;
    return static_cast<double>(v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))]);
}

template <class T>
bool parse(const char* s, T& out) {
    const char* end = s + std::strlen(s);
    auto [ptr, ec] = std::from_chars(s, end, out);
    return ec == std::errc{} && ptr == end;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        bool ok = v != nullptr;
        if (a == "--host" && ok) {
            o.host = v;
        } else if (a == "--port" && ok) {
            ok = parse(v, o.port);
        } else if (a == "--symbol" && ok) {
            o.symbol = v;
        } else if (a == "--connections" && ok) {
            ok = parse(v, o.connections) && o.connections > 0;
        } else if (a == "--requests" && ok) {
            ok = parse(v, o.requests);
        } else if (a == "--window" && ok) {
            ok = parse(v, o.window) && o.window > 0;
        } else if (a == "--mid" && ok) {
            ok = parse(v, o.mid);
        } else {
            std::fprintf(stderr,
                         "usage: ob_loadgen [--host H] [--port P] [--symbol S] [--connections N]\n"
                         "                  [--requests N per connection] [--window N in flight] [--mid PRICE]\n");
            return a == "-h" || a == "--help" ? 0 : 2;
        }
        if (!ok) {
            std::fprintf(stderr, "invalid value for %s\n", argv[i]);
            return 2;
        }
        ++i;
    }

    std::vector<Result> results(static_cast<std::size_t>(o.connections));
    std::vector<std::thread> threads;
    std::atomic<bool> go{false};
    for (int c = 0; c < o.connections; ++c) {
        threads.emplace_back(run_connection, std::cref(o), c, std::ref(go), std::ref(results[static_cast<std::size_t>(c)]));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let every connection establish
    const auto start = Clock::now();
    go.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();
    const double secs = std::chrono::duration<double>(Clock::now() - start).count();

    std::vector<std::int64_t> all;
    std::uint64_t fills = 0, rejects = 0, errors = 0;
    bool failed = false;
    for (auto& r : results) {
        all.insert(all.end(), r.latencies_ns.begin(), r.latencies_ns.end());
        fills += r.fills;
        rejects += r.rejects;
        errors += r.errors;
        failed = failed || r.failed;
    }
    std::sort(all.begin(), all.end());

    std::printf("connections %d, window %u, %zu round trips in %.2f s\n", o.connections, o.window, all.size(), secs);
    std::printf("throughput  %.0f requests/s\n", static_cast<double>(all.size()) / secs);
    std::printf("round trip  p50 %.1f us   p99 %.1f us   p99.9 %.1f us   max %.1f us\n", pct_of(all, 0.5) / 1e3,
                pct_of(all, 0.99) / 1e3, pct_of(all, 0.999) / 1e3,
                all.empty() ? 0.0 : static_cast<double>(all.back()) / 1e3);
    std::printf("fills %llu, rejects %llu, errors %llu\n", static_cast<unsigned long long>(fills),
                static_cast<unsigned long long>(rejects), static_cast<unsigned long long>(errors));
    if (failed) {
        std::fprintf(stderr, "a connection failed (server down or connection dropped)\n");
        return 1;
    }
    return 0;
}
