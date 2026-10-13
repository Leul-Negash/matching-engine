#include "ob/journal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace ob {

namespace {

constexpr char kMagic[8] = {'O', 'B', 'J', 'R', 'N', 'L', '0', '1'};
constexpr std::uint32_t kVersion = 1;

struct Header {
    char magic[8];
    std::uint32_t version;
    std::uint32_t record_size;
};
static_assert(sizeof(Header) == 16);

constexpr std::size_t kRecordSize = sizeof(std::uint32_t) + sizeof(Command);

std::runtime_error sys_error(const std::string& what, const std::string& path) {
    return std::runtime_error(what + " '" + path + "': " + std::strerror(errno));
}

const std::array<std::uint32_t, 256>& crc_table() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    return table;
}

// Reads exactly `len` bytes unless EOF comes first; returns bytes read.
std::size_t read_full(int fd, void* buf, std::size_t len) {
    auto* p = static_cast<unsigned char*>(buf);
    std::size_t done = 0;
    while (done < len) {
        const ssize_t n = ::read(fd, p + done, len - done);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("journal read failed: ") + std::strerror(errno));
        }
        done += static_cast<std::size_t>(n);
    }
    return done;
}

void write_full(int fd, const void* buf, std::size_t len) {
    const auto* p = static_cast<const unsigned char*>(buf);
    while (len > 0) {
        const ssize_t n = ::write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("journal write failed: ") + std::strerror(errno));
        }
        p += n;
        len -= static_cast<std::size_t>(n);
    }
}

}  // namespace

std::uint32_t crc32(const void* data, std::size_t len) {
    const auto& table = crc_table();
    const auto* p = static_cast<const unsigned char*>(data);
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

ReplayStats replay_journal(const std::string& path, const std::function<void(const Command&)>& fn) {
    ReplayStats stats;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return stats;
        throw sys_error("cannot open journal", path);
    }
    struct Closer {
        int fd;
        ~Closer() { ::close(fd); }
    } closer{fd};

    Header h{};
    const std::size_t got = read_full(fd, &h, sizeof h);
    // Empty or partial header: crashed while creating the file, before any
    // record could have been written. Treat as empty (the writer rewrites it).
    if (got < sizeof h && std::memcmp(&h, kMagic, std::min(got, sizeof kMagic)) == 0) return stats;
    if (got < sizeof h || std::memcmp(h.magic, kMagic, sizeof kMagic) != 0) {
        throw std::runtime_error("'" + path + "' is not an order book journal");
    }
    if (h.version != kVersion || h.record_size != sizeof(Command)) {
        throw std::runtime_error("'" + path + "' has an incompatible journal version or record size");
    }
    stats.valid_bytes = sizeof h;

    unsigned char rec[kRecordSize];
    bool bad_record_seen = false;
    for (;;) {
        const std::size_t n = read_full(fd, rec, kRecordSize);
        if (n == 0) break;
        if (bad_record_seen) {
            throw std::runtime_error("journal '" + path + "' is corrupt at offset " +
                                     std::to_string(stats.valid_bytes));
        }
        std::uint32_t stored = 0;
        std::memcpy(&stored, rec, sizeof stored);
        if (n < kRecordSize || stored != crc32(rec + sizeof stored, sizeof(Command))) {
            // Only acceptable as the very last thing in the file; checked on the next read.
            bad_record_seen = true;
            continue;
        }
        Command cmd;
        std::memcpy(&cmd, rec + sizeof stored, sizeof cmd);
        fn(cmd);
        ++stats.records;
        stats.valid_bytes += kRecordSize;
    }
    stats.discarded_torn_tail = bad_record_seen;
    return stats;
}

JournalWriter::JournalWriter(const std::string& path, Sync sync) : sync_(sync) {
    // Validate what is there (throws on corruption) and find the intact prefix.
    const ReplayStats existing = replay_journal(path, [](const Command&) {});

    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd_ < 0) throw sys_error("cannot open journal", path);

    if (existing.valid_bytes == 0) {
        if (::ftruncate(fd_, 0) != 0) throw sys_error("cannot truncate journal", path);
        Header h{};
        std::memcpy(h.magic, kMagic, sizeof kMagic);
        h.version = kVersion;
        h.record_size = sizeof(Command);
        write_full(fd_, &h, sizeof h);
    } else if (::ftruncate(fd_, static_cast<off_t>(existing.valid_bytes)) != 0) {
        throw sys_error("cannot truncate journal", path);
    }
    if (::lseek(fd_, 0, SEEK_END) < 0) throw sys_error("cannot seek journal", path);
    if (sync_ == Sync::Fsync && ::fsync(fd_) != 0) throw sys_error("cannot fsync journal", path);
    buffer_.reserve(64 * kRecordSize);
}

JournalWriter::~JournalWriter() {
    if (fd_ < 0) return;
    try {
        commit();
    } catch (...) {
        // Destructors must not throw; data not yet committed was never acknowledged.
    }
    ::close(fd_);
}

void JournalWriter::append(const Command& cmd) {
    const std::uint32_t crc = crc32(&cmd, sizeof cmd);
    const auto* c = reinterpret_cast<const unsigned char*>(&crc);
    const auto* b = reinterpret_cast<const unsigned char*>(&cmd);
    buffer_.insert(buffer_.end(), c, c + sizeof crc);
    buffer_.insert(buffer_.end(), b, b + sizeof cmd);
    ++records_;
}

void JournalWriter::commit() {
    if (buffer_.empty()) return;
    write_full(fd_, buffer_.data(), buffer_.size());
    buffer_.clear();
    if (sync_ == Sync::Fsync && ::fdatasync(fd_) != 0) {
        throw std::runtime_error(std::string("journal fsync failed: ") + std::strerror(errno));
    }
}

}  // namespace ob
