#pragma once

#include "ob/engine.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ob {

// Write-ahead journal of state-changing commands. The engine is
// deterministic, so replaying the journal rebuilds the exact book state
// after a restart or crash (event sourcing).
//
// File format (little-endian, same architecture only):
//   header:  8-byte magic "OBJRNL01", u32 version, u32 record size
//   records: u32 CRC-32 of the command bytes, then the Command bytes
//
// Recovery rules: a short or checksum-failing *last* record is a torn write
// from a crash and is discarded; a bad record followed by more data is
// corruption, and opening fails rather than silently dropping commands.

std::uint32_t crc32(const void* data, std::size_t len);

struct ReplayStats {
    std::uint64_t records = 0;
    std::uint64_t valid_bytes = 0;    // length of the intact prefix of the file
    bool discarded_torn_tail = false;
};

// Reads every intact command in order. A missing file is an empty journal.
// Throws std::runtime_error on a bad header or mid-file corruption.
ReplayStats replay_journal(const std::string& path, const std::function<void(const Command&)>& fn);

class JournalWriter {
public:
    enum class Sync {
        None,   // commit() hands data to the OS: survives a process crash
        Fsync,  // commit() also fsyncs: survives power loss, much slower
    };

    // Opens (creating if needed) for appending. An existing file is validated
    // and any torn tail is truncated first. Throws std::runtime_error.
    JournalWriter(const std::string& path, Sync sync);
    ~JournalWriter();

    JournalWriter(const JournalWriter&) = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;

    // Buffers one command. Nothing is durable until commit().
    void append(const Command& cmd);

    // Writes all buffered commands (and fsyncs if configured). Called once per
    // batch: one write covers many commands ("group commit").
    void commit();

    std::uint64_t records_written() const { return records_; }

private:
    int fd_ = -1;
    Sync sync_;
    std::vector<unsigned char> buffer_;
    std::uint64_t records_ = 0;
};

}  // namespace ob
