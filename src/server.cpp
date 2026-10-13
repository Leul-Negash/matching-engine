#include "ob/server.hpp"

#include "ob/log.hpp"
#include "ob/protocol.hpp"
#include "ob/spsc_queue.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iterator>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <string>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ob {

namespace {

// epoll user data: session ids count up from 1; these tags sit at the top.
constexpr std::uint64_t kListenTag = ~std::uint64_t{0};
constexpr std::uint64_t kWakeTag = kListenTag - 1;
constexpr std::uint64_t kStopTag = kListenTag - 2;

constexpr std::size_t kReadChunk = 64 * 1024;
constexpr std::size_t kBatch = 256;
constexpr int kIdleSpins = 2000;

inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

std::runtime_error sys_error(const char* what) {
    return std::runtime_error(std::string(what) + ": " + std::strerror(errno));
}

void signal_fd(int fd) {
    const std::uint64_t one = 1;
    // A full counter (never happens in practice) still leaves the fd readable.
    [[maybe_unused]] ssize_t n = ::write(fd, &one, sizeof one);
}

void drain_fd(int fd) {
    std::uint64_t v;
    [[maybe_unused]] ssize_t n = ::read(fd, &v, sizeof v);
}

}  // namespace

struct Server::Impl {
    // A client's view of one of its live orders.
    struct LiveOrder {
        OrderId id;
        InstrumentId instrument;
    };

    struct Session {
        int fd = -1;
        SessionId id = 0;
        std::string peer;
        std::string in;              // received bytes not yet parsed into lines
        std::string out;             // bytes waiting to be sent
        std::size_t out_sent = 0;    // prefix of `out` already sent
        bool want_write = false;     // EPOLLOUT registered
        bool closing = false;
        const char* close_reason = "";
        std::unordered_map<std::string, LiveOrder> live;  // clordid -> order
        std::unordered_set<InstrumentId> subs;
        std::chrono::steady_clock::time_point window_start{};
        std::uint32_t window_msgs = 0;
    };

    // Who owns an engine order, kept until the order is finished AND every
    // cancel/modify sent for it has been answered (so late replies can still
    // be routed with the right clordid).
    struct Owner {
        SessionId session;
        std::string clordid;
        std::uint32_t inflight = 0;
        bool terminal = false;
    };

    Impl(const ServerConfig& c, Engine& e, JournalWriter* j)
        : cfg(c), engine(e), journal(j), in_q(c.queue_capacity), out_q(c.queue_capacity) {
        for (InstrumentId i = 0; i < engine.instrument_count(); ++i) by_symbol.emplace(engine.symbol(i), i);
        subscribers.resize(engine.instrument_count());
        // Ids must not collide with orders recovered from the journal: order
        // ids for obvious reasons, and session ids because they double as the
        // self-trade prevention account.
        next_order_id = engine.max_order_id() + 1;
        next_session = engine.max_account() + 1;
        wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        stop_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        if (wake_fd < 0 || stop_fd < 0 || epoll_fd < 0) throw sys_error("server setup");
        add_fd(wake_fd, kWakeTag, EPOLLIN);
        add_fd(stop_fd, kStopTag, EPOLLIN);
    }

    ~Impl() {
        for (auto& [id, s] : sessions) ::close(s->fd);
        for (int fd : {listen_fd, wake_fd, stop_fd, epoll_fd}) {
            if (fd >= 0) ::close(fd);
        }
    }

    ServerConfig cfg;
    Engine& engine;
    JournalWriter* journal;

    int listen_fd = -1;
    int epoll_fd = -1;
    int wake_fd = -1;  // matching -> gateway: outputs are ready
    int stop_fd = -1;  // stop() -> gateway

    SpscQueue<Command> in_q;  // gateway -> matching
    SpscQueue<Output> out_q;  // matching -> gateway
    std::atomic<std::uint64_t> in_signal{0};  // bumped after pushes; the matching thread waits on it
    std::atomic<bool> matching_stop{false};
    std::atomic<bool> fatal{false};
    std::thread matching;

    // ---- gateway thread only
    std::unordered_map<SessionId, std::unique_ptr<Session>> sessions;
    SessionId next_session = 1;
    std::unordered_map<OrderId, Owner> owners;
    std::unordered_map<std::string, InstrumentId> by_symbol;
    std::vector<std::vector<SessionId>> subscribers;  // per instrument
    std::vector<SessionId> dirty;                     // sessions with output to flush
    OrderId next_order_id = 1;
    bool notify_pending = false;
    ServerStats st;

    // ------------------------------------------------------------- epoll

    void add_fd(int fd, std::uint64_t tag, std::uint32_t events) {
        epoll_event ev{};
        ev.events = events;
        ev.data.u64 = tag;
        if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) throw sys_error("epoll_ctl add");
    }

    void set_write_interest(Session& s, bool on) {
        if (s.want_write == on) return;
        epoll_event ev{};
        ev.events = EPOLLIN | (on ? EPOLLOUT : 0u);
        ev.data.u64 = s.id;
        ::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, s.fd, &ev);
        s.want_write = on;
    }

    // ---------------------------------------------------- matching thread

    void matching_loop() {
        std::vector<Command> batch(kBatch);
        std::vector<Output> outs;
        outs.reserve(4096);
        try {
            for (;;) {
                const std::uint64_t seen = in_signal.load(std::memory_order_acquire);
                const std::size_t n = in_q.try_pop(batch.data(), batch.size());
                if (n == 0) {
                    if (matching_stop.load(std::memory_order_acquire)) {
                        if (in_q.empty()) break;
                        continue;
                    }
                    if (cfg.busy_poll) {
                        cpu_relax();
                        continue;
                    }
                    // Spin briefly (cheap wake-up under load), then sleep until
                    // the gateway bumps in_signal.
                    for (int i = 0; i < kIdleSpins && in_q.empty(); ++i) cpu_relax();
                    if (in_q.empty()) in_signal.wait(seen, std::memory_order_acquire);
                    continue;
                }

                // Write-ahead: the batch is in the journal before any of its
                // effects (acks, fills) become visible to anyone.
                if (journal) {
                    for (std::size_t i = 0; i < n; ++i) {
                        if (Engine::mutates(batch[i])) journal->append(batch[i]);
                    }
                    journal->commit();
                }
                for (std::size_t i = 0; i < n; ++i) engine.process(batch[i], outs);

                for (const Output& o : outs) {
                    while (!out_q.try_push(o)) {  // backpressure: wait for the gateway
                        if (!cfg.busy_poll) signal_fd(wake_fd);
                        std::this_thread::yield();
                    }
                }
                outs.clear();
                if (!cfg.busy_poll) signal_fd(wake_fd);  // a busy-polling gateway checks the queue anyway
            }
        } catch (const std::exception& e) {
            OB_LOG(Error, "matching thread stopped: %s", e.what());
            fatal.store(true, std::memory_order_release);
            signal_fd(stop_fd);
        }
        matching_finished.store(true, std::memory_order_release);
    }

    void notify_matching() {
        notify_pending = false;
        if (cfg.busy_poll && !matching_stop.load(std::memory_order_relaxed)) return;  // it never sleeps
        in_signal.fetch_add(1, std::memory_order_release);
        in_signal.notify_one();
    }

    void submit(const Command& c) {
        while (!in_q.try_push(c)) {
            // Queue full: make sure the matching thread is awake, and keep
            // draining its outputs so it can never block on us (no deadlock).
            notify_matching();
            drain_outputs();
            std::this_thread::yield();
        }
        ++st.commands;
        notify_pending = true;
    }

    // ----------------------------------------------------------- sessions

    Session* find_session(SessionId id) {
        auto it = sessions.find(id);
        return it == sessions.end() || it->second->closing ? nullptr : it->second.get();
    }

    void mark_dirty(Session& s) { dirty.push_back(s.id); }

    // Appends to a session's output; drops a client that can't keep up.
    template <class F>
    void send_to(SessionId id, F&& format) {
        Session* s = find_session(id);
        if (!s) return;
        format(s->out);
        mark_dirty(*s);
        if (s->out.size() - s->out_sent > cfg.max_output_buffer) {
            s->closing = true;
            s->close_reason = "slow consumer";
            ++st.slow_consumers_dropped;
        }
    }

    void accept_all() {
        for (;;) {
            sockaddr_in addr{};
            socklen_t len = sizeof addr;
            const int fd = ::accept4(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len,
                                     SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK) OB_LOG(Warn, "accept: %s", std::strerror(errno));
                return;
            }
            if (sessions.size() >= cfg.max_sessions) {
                static constexpr char kFull[] = "ERROR server full\n";
                [[maybe_unused]] ssize_t n = ::send(fd, kFull, sizeof kFull - 1, MSG_NOSIGNAL);
                ::close(fd);
                ++st.sessions_rejected;
                continue;
            }
            const int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);  // latency over batching

            auto s = std::make_unique<Session>();
            s->fd = fd;
            s->id = next_session++;
            char ip[INET_ADDRSTRLEN] = "?";
            ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof ip);
            s->peer = std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
            s->window_start = std::chrono::steady_clock::now();
            add_fd(fd, s->id, EPOLLIN);
            OB_LOG(Info, "session %u connected from %s", s->id, s->peer.c_str());
            ++st.sessions_accepted;
            sessions.emplace(s->id, std::move(s));
        }
    }

    void close_session(SessionId id) {
        auto it = sessions.find(id);
        if (it == sessions.end()) return;
        std::unique_ptr<Session> s = std::move(it->second);
        sessions.erase(it);  // from here on, replies to this session are dropped

        ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, s->fd, nullptr);
        ::close(s->fd);
        for (InstrumentId inst : s->subs) {
            auto& v = subscribers[inst];
            v.erase(std::remove(v.begin(), v.end(), id), v.end());
        }
        if (cfg.cancel_on_disconnect) {
            for (const auto& [clordid, lo] : s->live) {
                auto ow = owners.find(lo.id);
                if (ow != owners.end()) ++ow->second.inflight;
                Command c{};
                c.type = CommandType::Cancel;
                c.instrument = lo.instrument;
                c.order_id = lo.id;
                c.session = id;
                submit(c);
            }
        }
        OB_LOG(Info, "session %u closed (%s), %zu live orders %s", id, s->close_reason, s->live.size(),
               cfg.cancel_on_disconnect ? "cancelled" : "left resting");
    }

    void on_readable(Session& s) {
        char buf[kReadChunk];
        const ssize_t n = ::read(s.fd, buf, sizeof buf);  // one read per event: fair across sessions
        if (n == 0) {
            s.closing = true;
            s.close_reason = "disconnected";
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
            s.closing = true;
            s.close_reason = "read error";
        } else {
            s.in.append(buf, static_cast<std::size_t>(n));
        }

        std::size_t start = 0;
        for (;;) {
            const std::size_t nl = s.in.find('\n', start);
            if (nl == std::string::npos) break;
            handle_line(s, std::string_view(s.in).substr(start, nl - start));
            start = nl + 1;
        }
        s.in.erase(0, start);
        if (s.in.size() > proto::kMaxLineLen + 1) {
            proto::error(s.out, "line too long");
            s.closing = true;
            s.close_reason = "protocol violation";
            ++st.protocol_errors;
        }
        if (s.closing) mark_dirty(s);
    }

    bool throttled(Session& s) {
        if (cfg.max_msgs_per_sec == 0) return false;
        const auto now = std::chrono::steady_clock::now();
        if (now - s.window_start >= std::chrono::seconds(1)) {
            s.window_start = now;
            s.window_msgs = 0;
        }
        return ++s.window_msgs > cfg.max_msgs_per_sec;
    }

    std::optional<InstrumentId> instrument_of(const std::string& symbol) const {
        auto it = by_symbol.find(symbol);
        if (it == by_symbol.end()) return std::nullopt;
        return it->second;
    }

    void handle_line(Session& s, std::string_view line) {
        if (s.closing) return;
        if (line.empty() || line == "\r") return;
        ++st.messages_in;
        mark_dirty(s);

        std::string err;
        std::optional<proto::ClientMessage> msg = proto::parse(line, err);
        if (!msg) {
            ++st.protocol_errors;
            proto::error(s.out, err);
            return;
        }
        if (throttled(s)) {
            ++st.throttled;
            if (msg->clordid.empty()) {
                proto::error(s.out, "throttled");
            } else {
                proto::reject(s.out, msg->clordid, "THROTTLED");
            }
            return;
        }

        switch (msg->type) {
            case proto::MsgType::New: {
                const auto inst = instrument_of(msg->symbol);
                if (!inst) return proto::reject(s.out, msg->clordid, "UNKNOWN_SYMBOL");
                if (s.live.contains(msg->clordid)) return proto::reject(s.out, msg->clordid, "DUPLICATE_CLORDID");
                const OrderId id = next_order_id++;
                s.live.emplace(msg->clordid, LiveOrder{id, *inst});
                owners.emplace(id, Owner{s.id, msg->clordid});
                Command c{};
                c.type = CommandType::New;
                c.instrument = *inst;
                c.order_id = id;
                c.side = msg->side;
                c.ord_type = msg->ord_type;
                c.tif = msg->tif;
                c.post_only = msg->post_only;
                c.price = msg->price;
                c.qty = msg->qty;
                c.account = s.id;  // self-trade prevention works per session
                c.session = s.id;
                return submit(c);
            }
            case proto::MsgType::Cancel:
            case proto::MsgType::Modify: {
                auto it = s.live.find(msg->clordid);
                if (it == s.live.end()) return proto::reject(s.out, msg->clordid, "UNKNOWN_ORDER");
                ++owners.at(it->second.id).inflight;
                Command c{};
                c.type = msg->type == proto::MsgType::Cancel ? CommandType::Cancel : CommandType::Modify;
                c.instrument = it->second.instrument;
                c.order_id = it->second.id;
                c.price = msg->price;
                c.qty = msg->qty;
                c.session = s.id;
                return submit(c);
            }
            case proto::MsgType::Book: {
                const auto inst = instrument_of(msg->symbol);
                if (!inst) return proto::error(s.out, "unknown symbol " + msg->symbol);
                Command c{};
                c.type = CommandType::Snapshot;
                c.instrument = *inst;
                c.qty = msg->levels;
                c.session = s.id;
                return submit(c);
            }
            case proto::MsgType::Subscribe:
            case proto::MsgType::Unsubscribe: {
                const auto inst = instrument_of(msg->symbol);
                if (!inst) return proto::error(s.out, "unknown symbol " + msg->symbol);
                auto& subs = subscribers[*inst];
                const bool on = msg->type == proto::MsgType::Subscribe;
                if (on && s.subs.insert(*inst).second) subs.push_back(s.id);
                if (!on && s.subs.erase(*inst)) subs.erase(std::remove(subs.begin(), subs.end(), s.id), subs.end());
                return proto::subscribed(s.out, msg->symbol, on);
            }
            case proto::MsgType::Ping:
                return proto::pong(s.out);
        }
    }

    void flush(Session& s) {
        while (s.out_sent < s.out.size()) {
            const ssize_t n = ::send(s.fd, s.out.data() + s.out_sent, s.out.size() - s.out_sent, MSG_NOSIGNAL);
            if (n > 0) {
                s.out_sent += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                set_write_interest(s, true);  // resume when the socket drains
                if (s.out_sent > (1u << 20)) {  // don't let the sent prefix grow forever
                    s.out.erase(0, s.out_sent);
                    s.out_sent = 0;
                }
                return;
            }
            s.closing = true;
            s.close_reason = "write error";
            return;
        }
        s.out.clear();
        s.out_sent = 0;
        set_write_interest(s, false);
    }

    void flush_dirty() {
        // close_session() may submit cancels, which can drain outputs and
        // dirty more sessions, so iterate by index over a growing list.
        for (std::size_t i = 0; i < dirty.size(); ++i) {
            auto it = sessions.find(dirty[i]);
            if (it == sessions.end()) continue;
            Session& s = *it->second;
            if (!s.closing) flush(s);
            if (s.closing) {
                flush(s);  // best effort: e.g. deliver the final ERROR line
                close_session(s.id);
            }
        }
        dirty.clear();
    }

    // ------------------------------------------------------------ outputs

    Owner* owner(OrderId id) {
        auto it = owners.find(id);
        return it == owners.end() ? nullptr : &it->second;
    }

    // The order can't change any more: forget its clordid now, and its owner
    // record once no cancel/modify reply is still on its way.
    void finish(OrderId id, Owner& ow) {
        ow.terminal = true;
        if (Session* s = find_session(ow.session)) {
            auto it = s->live.find(ow.clordid);
            if (it != s->live.end() && it->second.id == id) s->live.erase(it);
        } else if (auto it = sessions.find(ow.session); it != sessions.end()) {
            it->second->live.erase(ow.clordid);  // session is closing; keep its map consistent
        }
        release_if_done(id, ow);
    }

    void release_if_done(OrderId id, const Owner& ow) {
        if (ow.terminal && ow.inflight == 0) owners.erase(id);
    }

    void on_event(const Output& o) {
        const Event& e = o.event;
        switch (e.type) {
            case EventType::Accepted:
                if (Owner* ow = owner(e.order_id)) {
                    send_to(ow->session, [&](std::string& out) { proto::ack(out, ow->clordid, e.order_id); });
                }
                break;

            case EventType::Rejected:
                if (Owner* ow = owner(e.order_id)) {
                    send_to(ow->session, [&](std::string& out) { proto::reject(out, ow->clordid, to_string(e.reject)); });
                    if (o.origin == CommandType::New) {
                        finish(e.order_id, *ow);
                    } else {
                        --ow->inflight;
                        release_if_done(e.order_id, *ow);
                    }
                }
                break;

            case EventType::Trade: {
                if (Owner* ow = owner(e.order_id)) {
                    send_to(ow->session,
                            [&](std::string& out) { proto::fill(out, ow->clordid, e.price, e.qty, e.leaves); });
                    if (e.leaves == 0) finish(e.order_id, *ow);
                }
                if (Owner* ow = owner(e.counter_id)) {
                    send_to(ow->session,
                            [&](std::string& out) { proto::fill(out, ow->clordid, e.price, e.qty, e.counter_leaves); });
                    if (e.counter_leaves == 0) finish(e.counter_id, *ow);
                }
                const std::string& sym = engine.symbol(o.instrument);
                for (SessionId sub : subscribers[o.instrument]) {
                    send_to(sub, [&](std::string& out) { proto::trade(out, sym, e.price, e.qty, e.side); });
                }
                break;
            }

            case EventType::Cancelled:
                if (Owner* ow = owner(e.order_id)) {
                    send_to(ow->session, [&](std::string& out) { proto::canceled(out, ow->clordid, e.qty, e.cancel); });
                    if (o.origin == CommandType::Cancel) --ow->inflight;
                    finish(e.order_id, *ow);
                }
                break;

            case EventType::Modified:
                if (Owner* ow = owner(e.order_id)) {
                    send_to(ow->session, [&](std::string& out) { proto::modified(out, ow->clordid, e.price, e.leaves); });
                    --ow->inflight;
                    release_if_done(e.order_id, *ow);
                }
                break;
        }
    }

    void on_output(const Output& o) {
        ++st.outputs;
        switch (o.kind) {
            case Output::Kind::Order:
                on_event(o);
                break;
            case Output::Kind::Quote: {
                const std::string& sym = engine.symbol(o.instrument);
                for (SessionId sub : subscribers[o.instrument]) {
                    send_to(sub, [&](std::string& out) { proto::bbo(out, sym, o.quote); });
                }
                break;
            }
            case Output::Kind::Depth:
                send_to(o.session, [&](std::string& out) {
                    proto::level(out, engine.symbol(o.instrument), o.depth_side, o.level);
                });
                break;
            case Output::Kind::DepthEnd:
                send_to(o.session, [&](std::string& out) { proto::book_end(out, engine.symbol(o.instrument)); });
                break;
        }
    }

    void drain_outputs() {
        Output batch[64];
        std::size_t n;
        while ((n = out_q.try_pop(batch, std::size(batch))) > 0) {
            for (std::size_t i = 0; i < n; ++i) on_output(batch[i]);
        }
    }

    // --------------------------------------------------------------- run

    bool run() {
        if (listen_fd < 0) throw std::logic_error("Server::listen() must be called before run()");
        matching = std::thread([this] { matching_loop(); });

        epoll_event events[64];
        bool stopping = false;
        while (!stopping) {
            const int n = ::epoll_wait(epoll_fd, events, 64, cfg.busy_poll ? 0 : -1);
            if (n < 0) {
                if (errno == EINTR) continue;
                OB_LOG(Error, "epoll_wait: %s", std::strerror(errno));
                fatal = true;
                break;
            }
            for (int i = 0; i < n; ++i) {
                const std::uint64_t tag = events[i].data.u64;
                if (tag == kListenTag) {
                    accept_all();
                } else if (tag == kWakeTag) {
                    drain_fd(wake_fd);
                } else if (tag == kStopTag) {
                    drain_fd(stop_fd);
                    stopping = true;
                } else if (Session* s = find_session(static_cast<SessionId>(tag))) {
                    if (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR)) on_readable(*s);
                    if (!s->closing && (events[i].events & EPOLLOUT)) flush(*s);
                }
            }
            if (notify_pending) notify_matching();
            drain_outputs();
            flush_dirty();
            if (notify_pending) notify_matching();  // cancel-on-disconnect may have queued commands
        }

        shutdown();
        return !fatal.load();
    }

    void shutdown() {
        OB_LOG(Info, "shutting down: stop accepting, drain the matching thread");
        if (listen_fd >= 0) {
            ::close(listen_fd);
            listen_fd = -1;
        }
        matching_stop.store(true, std::memory_order_release);
        notify_matching();
        // Keep draining while the matching thread finishes, so it can't block on a full queue.
        while (!matching_done()) {
            drain_outputs();
            std::this_thread::yield();
        }
        matching.join();
        drain_outputs();
        for (auto& [id, s] : sessions) flush(*s);  // best effort; resting orders stay (they're journaled)
        OB_LOG(Info, "stopped: %llu sessions, %llu messages, %llu commands, %llu outputs",
               static_cast<unsigned long long>(st.sessions_accepted), static_cast<unsigned long long>(st.messages_in),
               static_cast<unsigned long long>(st.commands), static_cast<unsigned long long>(st.outputs));
    }

    std::atomic<bool> matching_finished{false};
    bool matching_done() const { return matching_finished.load(std::memory_order_acquire); }
};

Server::Server(const ServerConfig& config, Engine& engine, JournalWriter* journal)
    : impl_(std::make_unique<Impl>(config, engine, journal)) {}

Server::~Server() {
    if (impl_->matching.joinable()) {
        impl_->matching_stop = true;
        impl_->notify_matching();
        impl_->matching.join();
    }
}

std::uint16_t Server::listen() {
    Impl& m = *impl_;
    m.listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (m.listen_fd < 0) throw sys_error("socket");
    const int one = 1;
    ::setsockopt(m.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m.cfg.port);
    if (::inet_pton(AF_INET, m.cfg.bind_address.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("invalid IPv4 bind address '" + m.cfg.bind_address + "'");
    }
    if (::bind(m.listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) throw sys_error("bind");
    if (::listen(m.listen_fd, SOMAXCONN) != 0) throw sys_error("listen");

    socklen_t len = sizeof addr;
    ::getsockname(m.listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
    m.add_fd(m.listen_fd, kListenTag, EPOLLIN);
    return ntohs(addr.sin_port);
}

bool Server::run() {
    return impl_->run();
}

void Server::stop() {
    signal_fd(impl_->stop_fd);
}

ServerStats Server::stats() const {
    return impl_->st;
}

}  // namespace ob
