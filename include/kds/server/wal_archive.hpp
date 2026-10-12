#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/wal/record.hpp"

// The WAL archive (BR-S4, `instructions/v3.0.0/workorder-br-backup-restore.md`,
// `br-s0-census.md`).
//
// **Archiving is on when one of `archive_dir` or `archive_command` is set,
// and there is no third name for that** (the order's `archive_mode` would
// be a second spelling of the same quantity - `CLAUDE.md`'s rule). A sealed
// segment is copied to `archive_dir` (copy, `fsync` the file, `fsync` the
// directory) or handed to `archive_command` (a shell command whose exit 0
// means durable; `%p` is the segment's path, `%f` the archive name).
//
// **Every archived name carries the volume's lineage** (BR-R10):
// `kds-<lineage, 16 hex>-<segment>.log`, so a restored instance - which
// takes a fresh lineage - never writes over the original's history.
//
// **The archiver touches nothing the engine latches.** It runs on its own
// thread, reads the stream's durable LSN (an atomic) to know which segments
// are sealed - every segment below the durable LSN's is, since a roll syncs
// the segment it leaves before the next exists - and reads those segments'
// files, which nothing writes again. Its floor (the first segment not yet
// archived) joins the recycler's bound, so no segment is removed before it
// is archived (T7). It archives in segment order and never skips one: a
// failure is retried with backoff, counted, and named in `SHOW META`.
//
// **Restart**: the archiver resumes from the first live segment. In
// `archive_dir` mode a name already present is compared byte for byte - the
// same body is skipped, a different one is a failure that stops archiving
// rather than overwrite (never a different body under one name). An
// `archive_command` must be idempotent, as any such hook must.
//
// **Fail-closed** (BR-Q2 (a)): while archiving is failing and the WAL
// directory's filesystem has less than `kReserveSegments` segments free,
// `blocking()` is true and every statement that writes the log is refused
// by name (the dispatcher's gate), until archiving recovers.
namespace kds::server {

struct ArchiveSettings {
    std::string archive_dir;
    std::string archive_command;
    bool enabled() const noexcept { return !archive_dir.empty() || !archive_command.empty(); }
};

// The archive name of `segment` under `lineage`.
std::string ArchiveName(std::uint64_t lineage, std::uint64_t segment);

class WalArchiver {
public:
    struct Wiring {
        ArchiveSettings settings;
        std::string wal_dir;
        std::uint64_t lineage_id = 0;
        std::uint64_t segment_size = 0;
        std::uint64_t first_segment = 0;  // the live log's first, at mount
        std::function<wal::Lsn()> durable_lsn;
        // Told the first segment not yet archived, at start and at every
        // advance: the log device keeps every detached segment from it on
        // (`FileLogDevice::HoldDetachedFrom`). May be empty.
        std::function<void(std::uint64_t)> hold_from;
        Logger* log = nullptr;
    };

    explicit WalArchiver(Wiring wiring);
    ~WalArchiver();
    WalArchiver(const WalArchiver&) = delete;
    WalArchiver& operator=(const WalArchiver&) = delete;

    void Start();
    void Stop();
    // Wakes the thread to look again (a seal, a test).
    void Kick();

    // The first byte of the first segment not yet archived: the recycler's
    // bound may not pass it.
    wal::Lsn floor() const noexcept {
        return next_.load(std::memory_order_acquire) * wiring_.segment_size;
    }
    std::uint64_t next_segment() const noexcept { return next_.load(std::memory_order_acquire); }
    bool blocking() const noexcept { return blocking_.load(std::memory_order_acquire); }
    // The last failure's text, empty while archiving keeps up.
    std::string last_error() const;

    void AppendMeta(std::ostream& os) const;

    // `[provisional]`: free WAL-filesystem space, in segments, below which a
    // failing archive refuses writes (BR-Q2 (a)).
    static constexpr std::uint64_t kReserveSegments = 4;

private:
    void Run();
    Status ArchiveOne(std::uint64_t segment);
    void UpdateBlocking(bool failing);

    const Wiring wiring_;
    std::atomic<std::uint64_t> next_;
    std::atomic<bool> blocking_{false};
    std::atomic<std::uint64_t> archived_{0};
    std::atomic<std::uint64_t> failures_{0};
    std::atomic<int> child_{0};  // a running archive_command's pid

    mutable std::mutex mu_;
    std::condition_variable wake_;
    bool stopping_ = false;
    bool kicked_ = false;
    std::string last_error_;
    std::thread thread_;
};

}  // namespace kds::server
