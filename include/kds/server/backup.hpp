#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/server/backup_manifest.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/record.hpp"

#include "kds/storage/device_page_store.hpp"

// The instance's backup control (BR-S2, S5,
// `instructions/v3.0.0/workorder-br-backup-restore.md`, `br-s0-census.md`).
//
// One object for the instance, owned by `Expeditor` and handed to every
// core's dispatcher: `BACKUP START` on any core pins the one log, and one
// backup runs at a time (BR-Q4).
//
// **The life of a backup**
//
//   START   reads the durable redo start `D` and pins it (BR-R3), logs
//           BACKUP_BEGIN, and notes the data file's capacity.
//   COPY    (path B) a copier thread reads the data file up to that capacity
//           through the device's write fence (BR-R5 replaced) into
//           `<dir>/kds.db`. Path D takes its data file from a snapshot
//           instead, between START and STOP.
//   STOP    refuses while the copier runs; copies the tail the file grew
//           since START; appends BACKUP_END and seals its segment in one
//           latched act, so the stop is where the copied log ends; makes it
//           durable; then writes the maps and copies every map page - after
//           the stop, so they carry the bit of every page a record up to the
//           stop names (census §3 items 1, 5). A worker thread then makes
//           the data file durable, copies the segments `[start, stop]`
//           (BR-Q6) and writes the manifest last; the pin goes with it.
//   ABORT   ends an open backup with no BACKUP_END - invalid by construction.
//
// A backup open with nothing running for `kAbandonNs` since its last
// activity is aborted (the order's `backup_abandon_ms`, `[provisional]`). It
// is not tied to a connection: path D's START and STOP may come from two.
//
// **Lock/atomic protocol.** `mu_` guards the fields below it. It is held
// across no I/O: START's one append is the exception, and its order is
// `mu_` → stream latch, which nothing reverses (the recycler runs under
// `mu_` from `Landed`, outside every latch). STOP marks `kStopping` under
// `mu_` and does its I/O without it, so a publish on another core's reactor
// never waits on a sync. The census's §3 item 4 holds because `D` is read
// and the pin set in one hold, and `Recycle` and `ReclaimBound` read the pin
// under it. Progress counters are atomics.
namespace kds::server {

class BackupControl {
public:
    struct Wiring {
        std::string wal_dir;
        std::uint64_t segment_size = 0;
        std::uint64_t lineage_id = 0;
        std::uint32_t superblock_version = 0;
        // The data file's device, read through its write fence.
        storage::PageDevice* data_device = nullptr;
        // The durable redo start `D` (`SuperBlockCheckpointAnchor`).
        std::function<wal::Lsn()> durable_redo_start;
        // The free and headerless maps as they stand in memory
        // (`DevicePageStore::MapImages`), which STOP writes into its copy.
        std::function<std::vector<storage::DevicePageStore::MapImage>()> map_images;
        Logger* log = nullptr;
    };

    explicit BackupControl(Wiring wiring);
    ~BackupControl();
    BackupControl(const BackupControl&) = delete;
    BackupControl& operator=(const BackupControl&) = delete;

    // `BACKUP START '<label>'`, through the calling core's log manager.
    StatusOr<std::string> Start(const std::string& label, wal::WalManager& wal);
    // `BACKUP COPY TO '<dir>'`: starts the data copy; returns at once.
    StatusOr<std::string> Copy(const std::string& dir);
    // `BACKUP STOP TO '<dir>'`: returns with the stop once it is durable; the
    // log copy and the manifest follow on a thread (`StatusLine`).
    StatusOr<std::string> Stop(const std::string& dir, wal::WalManager& wal,
                               std::uint64_t now_unix);
    // `BACKUP ABORT`.
    StatusOr<std::string> Abort();

    // `BACKUP STATUS`: one line, the error (if any) last.
    std::string StatusLine() const;
    // `SHOW META`'s block.
    void AppendMeta(std::ostream& os) const;

    // Core 0's tick: aborts an open backup idle past `kAbandonNs`.
    void ExpireIfAbandoned();

    // The recycler's bound (census §3 item 4): runs `recycle(min(d, pin))`
    // holding `mu_`.
    void Recycle(wal::Lsn d, const std::function<void(wal::Lsn)>& recycle);
    // The reclaim gate's bound: `min(d, pin)`, read under `mu_`.
    wal::Lsn ReclaimBound(wal::Lsn d) const;

    // Whether a backup is open (started, not yet whole or aborted).
    bool active() const;
    // Blocks until no thread of this control runs. Tests and shutdown.
    void WaitIdle();

    static constexpr wal::Lsn kNoPin = ~wal::Lsn{0};
    // `[provisional]` (BR-S10 measures): an hour idle ends an open backup.
    static constexpr std::uint64_t kAbandonNs = 3600ull * 1'000'000'000ull;
    // `[provisional]`: the copier's pages per fenced read, and its rate.
    static constexpr std::uint32_t kCopyChunkPages = 128;  // 1 MiB
    static constexpr std::uint64_t kCopyBytesPerSecond = 256ull << 20;

private:
    enum class State { kIdle, kOpen, kStopping, kFinishing };
    static const char* StateName(State state) noexcept;

    void RunCopy();
    void RunFinish(BackupManifest manifest, std::string dir);
    // Copies `[first, end)` of the data device into the copy file, fenced.
    Status CopyPages(std::uint32_t first, std::uint32_t end, bool throttled);
    // Ends the backup under `mu_`: records how, releases the pin, closes and
    // removes an unfinished copy.
    void EndLocked(const char* outcome, const std::string& error);
    void Touch() { last_activity_ = std::chrono::steady_clock::now(); }

    const Wiring wiring_;

    mutable std::mutex mu_;
    State state_ = State::kIdle;
    std::uint64_t backup_id_ = 0;
    std::string label_;
    wal::Lsn start_lsn_ = 0;
    wal::Lsn pin_ = kNoPin;
    std::uint32_t start_capacity_ = 0;  // data pages at START
    // A STOP whose BACKUP_END landed but whose sync failed retries without
    // appending a second one.
    wal::Lsn stop_lsn_ = 0;
    std::uint64_t stop_segment_ = 0;
    std::uint32_t stop_capacity_ = 0;  // the capacity that attempt copied to
    std::chrono::steady_clock::time_point last_activity_{};
    // The data copy (path B): its directory and file, once COPY ran.
    std::string copy_dir_;
    int copy_fd_ = -1;
    bool copier_running_ = false;
    std::string copy_error_;
    std::vector<std::byte> copy_buffer_;  // CopyPages', one chunk
    // The last backup that ended: its id, how, and why if it did not finish.
    std::uint64_t last_id_ = 0;
    std::string last_outcome_ = "none";
    wal::Lsn last_stop_lsn_ = 0;
    std::string last_error_;
    std::thread worker_;

    std::atomic<bool> copy_cancel_{false};
    std::atomic<std::uint64_t> pages_copied_{0};
    std::atomic<std::uint64_t> log_bytes_copied_{0};
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<std::uint64_t> failed_{0};
};

}  // namespace kds::server
