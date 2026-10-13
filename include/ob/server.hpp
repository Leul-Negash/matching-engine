#pragma once

#include "ob/engine.hpp"
#include "ob/journal.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace ob {

struct ServerConfig {
    std::string bind_address = "127.0.0.1";  // IPv4
    std::uint16_t port = 9000;               // 0 = any free port
    bool cancel_on_disconnect = true;        // cancel a session's resting orders when it drops
    std::size_t max_sessions = 1000;
    std::size_t max_output_buffer = 8u << 20;  // bytes queued for one client before it is dropped
    std::uint32_t max_msgs_per_sec = 100'000;  // per session; 0 = unlimited
    std::size_t queue_capacity = 1u << 16;     // entries in each inter-thread queue
    // Both threads spin instead of sleeping when idle: lowest latency (no
    // wake-ups), but each keeps a CPU core at 100%. For dedicated machines.
    bool busy_poll = false;
};

struct ServerStats {
    std::uint64_t sessions_accepted = 0;
    std::uint64_t sessions_rejected = 0;     // over max_sessions
    std::uint64_t slow_consumers_dropped = 0;
    std::uint64_t messages_in = 0;
    std::uint64_t protocol_errors = 0;
    std::uint64_t throttled = 0;
    std::uint64_t commands = 0;              // sent to the engine
    std::uint64_t outputs = 0;               // received from the engine
};

// TCP order entry gateway in front of the matching engine.
//
// Two threads:
//   gateway (the caller of run()): epoll loop over non-blocking sockets;
//     parses messages, maps client order ids to engine order ids, routes
//     execution reports and market data back to sessions.
//   matching: pops command batches from a lock-free queue, journals and
//     commits them (group commit), runs them through the engine, and pushes
//     outputs to a second lock-free queue.
// The engine and books are touched only by the matching thread, so matching
// needs no locks. Acknowledgements are sent only after the command is in the
// journal.
class Server {
public:
    // `engine` must already contain its instruments (and any recovered
    // state). `journal` may be null (no persistence). Both must outlive run().
    Server(const ServerConfig& config, Engine& engine, JournalWriter* journal);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds and listens; returns the bound port. Throws std::runtime_error.
    std::uint16_t listen();

    // Serves until stop() is called or a fatal error occurs. Returns false if
    // it stopped because of a fatal error (e.g. the journal could not be written).
    bool run();

    // Requests a graceful stop. Safe to call from any thread or a signal handler.
    void stop();

    // Valid after run() returns.
    ServerStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ob
