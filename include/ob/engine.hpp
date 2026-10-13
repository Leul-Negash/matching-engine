#pragma once

#include "ob/order_book.hpp"
#include "ob/types.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace ob {

using InstrumentId = std::uint32_t;
using SessionId = std::uint32_t;  // opaque to the engine; 0 = none

enum class CommandType : std::uint8_t {
    New,
    Cancel,
    Modify,
    Snapshot,  // read-only: depth of book for one instrument
};

// One request to the engine. Fixed-size and trivially copyable so it can be
// passed through lock-free queues and written to the journal as raw bytes.
// Always start from Command{} so padding is zero (the journal checksums it).
struct Command {
    CommandType type;
    Side side;
    OrderType ord_type;
    TimeInForce tif;
    bool post_only;
    std::uint8_t reserved[3];
    InstrumentId instrument;
    AccountId account;
    SessionId session;  // copied to outputs so a gateway can route replies
    Quantity qty;       // New / Modify; Snapshot: number of levels per side
    OrderId order_id;
    Price price;        // New (limit) / Modify
};
static_assert(std::is_trivially_copyable_v<Command>);
static_assert(sizeof(Command) == 40, "Command must have no hidden padding");

// One engine output. Every output gets a gap-free, engine-wide sequence
// number, so a consumer can detect loss and the stream can be replayed.
struct Output {
    enum class Kind : std::uint8_t {
        Order,     // `event`: an execution report
        Quote,     // `quote`: top of book changed
        Depth,     // `level` on `depth_side`: one row of a snapshot
        DepthEnd,  // end of a snapshot
    };

    Kind kind;
    CommandType origin;  // the command that produced this output
    Side depth_side;
    InstrumentId instrument;
    SessionId session;  // the requesting session
    std::uint64_t seq;
    Event event;
    TopOfBook quote;
    DepthLevel level;
};
static_assert(std::is_trivially_copyable_v<Output>);

struct InstrumentConfig {
    std::string symbol;
    BookConfig book;
};

// Routes commands to per-instrument order books and turns their events into
// sequenced outputs. Deterministic: the same command sequence always gives the
// same outputs and book state, which is what makes journal replay a correct
// recovery mechanism. Not thread-safe: owned by the matching thread.
class Engine {
public:
    static constexpr std::uint32_t kDefaultDepth = 10;
    static constexpr std::uint32_t kMaxDepth = 1000;

    // Instruments are added at startup; throws std::invalid_argument on a
    // duplicate or empty symbol or an invalid book configuration.
    InstrumentId add_instrument(const InstrumentConfig& config);

    std::optional<InstrumentId> find_instrument(std::string_view symbol) const;
    const std::string& symbol(InstrumentId id) const { return instruments_.at(id)->symbol; }
    std::size_t instrument_count() const { return instruments_.size(); }
    const OrderBook& book(InstrumentId id) const { return instruments_.at(id)->book; }

    // Executes one command, appending its outputs to `out`.
    void process(const Command& cmd, std::vector<Output>& out);

    static bool mutates(const Command& cmd) { return cmd.type != CommandType::Snapshot; }

    std::uint64_t last_seq() const { return seq_; }
    OrderId max_order_id() const { return max_order_id_; }
    std::uint64_t commands_processed() const { return commands_; }

private:
    struct Instrument {
        std::string symbol;
        OrderBook book;
    };

    Output make_output(Output::Kind kind, const Command& cmd);

    struct StringHash {
        using is_transparent = void;  // lets find() take a string_view without a copy
        std::size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
    };

    std::vector<std::unique_ptr<Instrument>> instruments_;
    std::unordered_map<std::string, InstrumentId, StringHash, std::equal_to<>> by_symbol_;
    std::vector<Event> events_;       // reused per command
    std::vector<DepthLevel> depth_;   // reused per snapshot
    std::uint64_t seq_ = 0;
    std::uint64_t commands_ = 0;
    OrderId max_order_id_ = 0;
};

}  // namespace ob
