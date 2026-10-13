#include "ob/engine.hpp"

#include <stdexcept>

namespace ob {

InstrumentId Engine::add_instrument(const InstrumentConfig& config) {
    if (config.symbol.empty()) throw std::invalid_argument("empty instrument symbol");
    if (by_symbol_.contains(config.symbol)) throw std::invalid_argument("duplicate instrument " + config.symbol);
    const auto id = static_cast<InstrumentId>(instruments_.size());
    instruments_.push_back(std::make_unique<Instrument>(Instrument{config.symbol, OrderBook(config.book)}));
    by_symbol_.emplace(config.symbol, id);
    return id;
}

std::optional<InstrumentId> Engine::find_instrument(std::string_view symbol) const {
    auto it = by_symbol_.find(symbol);
    if (it == by_symbol_.end()) return std::nullopt;
    return it->second;
}

Output Engine::make_output(Output::Kind kind, const Command& cmd) {
    Output o{};
    o.kind = kind;
    o.origin = cmd.type;
    o.instrument = cmd.instrument;
    o.session = cmd.session;
    o.seq = ++seq_;
    return o;
}

void Engine::process(const Command& cmd, std::vector<Output>& out) {
    ++commands_;
    if (cmd.type == CommandType::New) {
        max_order_id_ = std::max(max_order_id_, cmd.order_id);
        max_account_ = std::max(max_account_, cmd.account);
    }

    if (cmd.instrument >= instruments_.size()) {
        Output o = make_output(Output::Kind::Order, cmd);
        o.event.type = EventType::Rejected;
        o.event.side = cmd.side;
        o.event.reject = RejectReason::UnknownInstrument;
        o.event.order_id = cmd.order_id;
        o.event.qty = cmd.qty;
        o.event.price = cmd.price;
        out.push_back(o);
        return;
    }
    OrderBook& book = instruments_[cmd.instrument]->book;

    if (cmd.type == CommandType::Snapshot) {
        const std::uint32_t levels = cmd.qty == 0 ? kDefaultDepth : std::min(cmd.qty, kMaxDepth);
        for (Side side : {Side::Buy, Side::Sell}) {
            depth_.clear();
            book.depth(side, levels, depth_);
            for (const DepthLevel& l : depth_) {
                Output o = make_output(Output::Kind::Depth, cmd);
                o.depth_side = side;
                o.level = l;
                out.push_back(o);
            }
        }
        out.push_back(make_output(Output::Kind::DepthEnd, cmd));
        return;
    }

    const TopOfBook before = book.top();
    events_.clear();
    switch (cmd.type) {
        case CommandType::New:
            book.submit({cmd.order_id, cmd.account, cmd.side, cmd.ord_type, cmd.tif, cmd.price, cmd.qty,
                         cmd.post_only},
                        events_);
            break;
        case CommandType::Cancel:
            book.cancel(cmd.order_id, events_);
            break;
        case CommandType::Modify:
            book.modify(cmd.order_id, cmd.price, cmd.qty, events_);
            break;
        case CommandType::Snapshot:
            break;  // handled above
    }
    for (const Event& e : events_) {
        Output o = make_output(Output::Kind::Order, cmd);
        o.event = e;
        out.push_back(o);
    }

    const TopOfBook after = book.top();
    if (!(after == before)) {
        Output o = make_output(Output::Kind::Quote, cmd);
        o.session = 0;  // public market data
        o.quote = after;
        out.push_back(o);
    }
}

}  // namespace ob
