#include "kds/server/backup.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "kds/base/version.hpp"
#include "kds/server/restore.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/page_device.hpp"
#include "kds/wal/payload.hpp"

namespace kds::server {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMaxLabelBytes = 256;
constexpr const char* kCopyTempName = "kds.db.tmp";

std::array<std::byte, wal::kBackupMarkPayloadSize> MarkPayload(std::uint64_t id, wal::Lsn start) {
    std::array<std::byte, wal::kBackupMarkPayloadSize> payload{};
    (void)wal::EncodeBackupMark(payload, {id, start});  // the size is the constant's
    return payload;
}

// The fence armed for one scope (`page_write_fence.hpp`).
class ArmedFence {
public:
    explicit ArmedFence(storage::PageWriteFence& fence) : fence_(fence) { fence_.Arm(); }
    ~ArmedFence() { fence_.Disarm(); }
    ArmedFence(const ArmedFence&) = delete;
    ArmedFence& operator=(const ArmedFence&) = delete;

private:
    storage::PageWriteFence& fence_;
};

// A backup's directory is new: absent, empty, or holding only the data file
// a snapshot put there (path D) and an empty `wal/` a failed STOP left.
Status CheckNewDirectory(const fs::path& root, const char* verb) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return Status::OK();
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        const std::string name = entry.path().filename().string();
        if (name == kBackupDataFileName) continue;
        if (name == kBackupWalDirName && fs::is_empty(entry.path(), ec)) continue;
        return Status::AlreadyExists(std::string(verb) + ": " + root.string() + " holds " + name +
                                     "; a backup goes into a new directory");
    }
    return Status::OK();
}

}  // namespace

const char* BackupControl::StateName(State state) noexcept {
    switch (state) {
        case State::kIdle: return "idle";
        case State::kOpen: return "open";
        case State::kStopping: return "stopping";
        case State::kFinishing: return "finishing";
    }
    return "?";
}

BackupControl::BackupControl(Wiring wiring) : wiring_(std::move(wiring)) {}

BackupControl::~BackupControl() {
    copy_cancel_.store(true, std::memory_order_relaxed);
    WaitIdle();
    std::lock_guard<std::mutex> lock(mu_);
    if (copy_fd_ >= 0) ::close(copy_fd_);
}

void BackupControl::WaitIdle() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(mu_);
        worker = std::move(worker_);
    }
    if (worker.joinable()) worker.join();
}

StatusOr<std::string> BackupControl::Start(const std::string& label, wal::WalManager& wal) {
    if (label.size() > kMaxLabelBytes) {
        return Status::InvalidArgument("BACKUP START: a label is at most " +
                                       std::to_string(kMaxLabelBytes) + " bytes");
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ != State::kIdle) {
        // BR-Q4 (a): one at a time, and the refusal names the open one.
        return Status::NotImplemented(
            "BACKUP START: backup " + std::to_string(backup_id_) + " ('" + label_ +
            "') is open from lsn " + std::to_string(start_lsn_) +
            "; one backup runs at a time - BACKUP STOP or BACKUP ABORT it first");
    }
    if (worker_.joinable()) worker_.join();  // the last backup's, already finished

    // **`D` and the pin in one hold** (census §3 item 4).
    const wal::Lsn d = wiring_.durable_redo_start();
    if (d == 0) {
        return Status::Unsupported("BACKUP START: no checkpoint anchor has been published, so "
                                   "there is no redo start to back up from");
    }
    const std::uint64_t id = DrawLineageId();
    const auto payload = MarkPayload(id, d);
    auto begun = wal.Append({wal::RecordType::kBackupBegin, wal::kNoTxnId, kInvalidPageId, 0},
                            payload);
    if (!begun.ok()) return begun.status();

    state_ = State::kOpen;
    backup_id_ = id;
    label_ = label;
    start_lsn_ = d;
    pin_ = d;
    start_capacity_ = wiring_.data_device->page_capacity();
    stop_lsn_ = 0;
    copy_dir_.clear();
    copy_error_.clear();
    pages_copied_.store(0, std::memory_order_relaxed);
    pages_verified_.store(0, std::memory_order_relaxed);
    log_bytes_copied_.store(0, std::memory_order_relaxed);
    Touch();
    if (wiring_.log != nullptr && wiring_.log->enabled(LogLevel::kInfo)) {
        wiring_.log->Info("backup", "backup " + std::to_string(id) + " started at lsn " +
                                        std::to_string(d) + " ('" + label + "')");
    }
    return "OK backup_id=" + std::to_string(id) + " start_lsn=" + std::to_string(d) +
           " start_segment=" + std::to_string(d / wiring_.segment_size) +
           " data_pages=" + std::to_string(start_capacity_);
}

StatusOr<std::string> BackupControl::Copy(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ != State::kOpen) return Status::InvalidArgument("BACKUP COPY: no backup is open");
    if (!copy_dir_.empty()) {
        return Status::InvalidArgument("BACKUP COPY: backup " + std::to_string(backup_id_) +
                                       " already copies its data file into " + copy_dir_);
    }
    const fs::path root(dir);
    std::error_code ec;
    if (fs::exists(root, ec) && !fs::is_empty(root, ec)) {
        return Status::AlreadyExists("BACKUP COPY: " + dir +
                                     " is not empty; a backup goes into a new directory");
    }
    fs::create_directories(root, ec);
    if (ec) return Status::IoError("BACKUP COPY: cannot create " + dir + ": " + ec.message());
    const std::string tmp = (root / kCopyTempName).string();
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        return Status::IoError("BACKUP COPY: create " + tmp + ": " + std::strerror(errno));
    }
    if (worker_.joinable()) worker_.join();
    copy_dir_ = dir;
    copy_fd_ = fd;
    copier_running_ = true;
    copy_cancel_.store(false, std::memory_order_relaxed);
    Touch();
    worker_ = std::thread([this] { RunCopy(); });
    return "OK backup_id=" + std::to_string(backup_id_) +
           " copying_pages=" + std::to_string(start_capacity_);
}

Status BackupControl::CopyPages(std::uint32_t first, std::uint32_t end, bool throttled) {
    // One buffer for every call: the copier and STOP never copy at once.
    std::vector<std::byte>& buffer = copy_buffer_;
    buffer.resize(static_cast<std::size_t>(kCopyChunkPages) * kPageSize);
    const auto began = std::chrono::steady_clock::now();
    std::uint64_t bytes = 0;
    for (std::uint32_t at = first; at < end;) {
        if (copy_cancel_.load(std::memory_order_relaxed)) {
            return Status::Cancelled("BACKUP COPY: cancelled");
        }
        const std::uint32_t n = std::min(kCopyChunkPages, end - at);
        const std::span<std::byte> chunk(buffer.data(), static_cast<std::size_t>(n) * kPageSize);
        if (Status s = wiring_.data_device->ReadPageRunFenced(at, n, chunk); !s.ok()) return s;
        if (Status s = PwriteAll(copy_fd_, chunk.data(), chunk.size(),
                                 static_cast<std::uint64_t>(at) * kPageSize, "the data copy");
            !s.ok()) {
            return s;
        }
        at += n;
        bytes += chunk.size();
        pages_copied_.fetch_add(n, std::memory_order_relaxed);
        if (throttled) {
            std::this_thread::sleep_until(
                began + std::chrono::microseconds(bytes * 1'000'000 / kCopyBytesPerSecond));
        }
    }
    return Status::OK();
}

void BackupControl::RunCopy() {
    std::uint32_t end = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        end = start_capacity_;
    }
    // Armed for the copy only: a write meeting it waits out a 1 MiB read at
    // most, and disarmed it costs a write three atomics.
    Status copied;
    {
        const ArmedFence armed(wiring_.data_device->write_fence());
        copied = CopyPages(0, end, /*throttled=*/true);
    }
    std::lock_guard<std::mutex> lock(mu_);
    copier_running_ = false;
    if (!copied.ok()) copy_error_ = copied.message();
    Touch();
}

StatusOr<std::string> BackupControl::Stop(const std::string& dir, wal::WalManager& wal,
                                          std::uint64_t now_unix) {
    const fs::path root(dir);
    bool copied = false;
    BackupManifest manifest;
    wal::Lsn stop = 0;
    std::uint64_t stop_segment = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (state_ != State::kOpen) {
            return Status::InvalidArgument(
                state_ == State::kIdle ? std::string("BACKUP STOP: no backup is open")
                                       : "BACKUP STOP: backup " + std::to_string(backup_id_) +
                                             " is already stopping");
        }
        if (copier_running_) {
            return Status::InvalidArgument(
                "BACKUP STOP: backup " + std::to_string(backup_id_) + "'s data copy has " +
                std::to_string(pages_copied_.load(std::memory_order_relaxed)) + " of " +
                std::to_string(start_capacity_) + " pages; stop it once the copy is done");
        }
        if (!copy_error_.empty()) {
            return Status::IoError("BACKUP STOP: backup " + std::to_string(backup_id_) +
                                   "'s data copy failed (" + copy_error_ +
                                   "); it cannot be a backup - BACKUP ABORT it");
        }
        copied = !copy_dir_.empty();
        std::error_code ec;
        if (copied && !fs::equivalent(root, copy_dir_, ec)) {
            return Status::InvalidArgument("BACKUP STOP: backup " + std::to_string(backup_id_) +
                                           " copied its data file into " + copy_dir_ +
                                           "; it stops there");
        }
        if (!copied) {
            if (Status s = CheckNewDirectory(root, "BACKUP STOP"); !s.ok()) return s;
        }
        state_ = State::kStopping;
        stop = stop_lsn_;
        stop_segment = stop_segment_;
        manifest.backup_id = backup_id_;
        manifest.label = label_;
        manifest.start_lsn = start_lsn_;
    }

    // The I/O, without `mu_`: no publish on another reactor waits on it.
    std::uint32_t capacity = 0;
    const auto fail = [&](const Status& s) -> Status {
        std::lock_guard<std::mutex> lock(mu_);
        state_ = State::kOpen;
        stop_lsn_ = stop;  // a BACKUP_END that landed is not appended twice
        stop_segment_ = stop_segment;
        if (stop != 0) stop_capacity_ = capacity;
        Touch();
        return s;
    };
    // The data tail the file grew since START, before the stop (§3 item 5),
    // and only before it: a retry whose BACKUP_END already landed copies no
    // page, since one copied now could carry a change past the stop.
    // A retry reuses the capacity its first attempt copied to: a map image
    // for a region created after the stop has no place in the copy.
    {
        std::lock_guard<std::mutex> lock(mu_);
        capacity = stop_capacity_ != 0 ? stop_capacity_ : wiring_.data_device->page_capacity();
    }
    if (copied && stop == 0) {
        const ArmedFence armed(wiring_.data_device->write_fence());
        capacity = wiring_.data_device->page_capacity();
        if (Status s = CopyPages(start_capacity_, capacity, /*throttled=*/false); !s.ok()) {
            return fail(s);
        }
    }
    if (stop == 0) {
        const auto payload = MarkPayload(manifest.backup_id, manifest.start_lsn);
        auto ended = wal.AppendAndSeal(
            {wal::RecordType::kBackupEnd, wal::kNoTxnId, kInvalidPageId, 0}, payload);
        if (!ended.ok()) return fail(ended.status());
        // The record's end, which the seal made the segment's end: every
        // record up to it is in the log the backup carries, none past it.
        stop = ended.value() + wal::EncodedRecordSize(wal::kBackupMarkPayloadSize);
        stop_segment = ended.value() / wiring_.segment_size;
    }
    if (Status s = wal.SyncAll(); !s.ok()) return fail(s);
    if (copied) {
        // The maps, after the stop (§3 item 1): every resident region's free
        // and headerless map as it stands in memory, written over what the
        // copier read - never to the source's device (`MapImages`). A retry
        // writes them again, which is safe: a map page carries no page_lsn.
        for (const storage::DevicePageStore::MapImage& image : wiring_.map_images()) {
            if (image.page_id >= capacity) continue;
            if (Status s = PwriteAll(copy_fd_, image.bytes.data(), kPageSize,
                                     static_cast<std::uint64_t>(image.page_id) * kPageSize,
                                     "the data copy's maps");
                !s.ok()) {
                return fail(s);
            }
        }
    }
    std::error_code ec;
    fs::create_directories(root / kBackupWalDirName, ec);
    if (ec) {
        return fail(Status::IoError("BACKUP STOP: cannot create " + dir + ": " + ec.message()));
    }

    // B when the server copied the data file; D when a snapshot supplied it -
    // into the backup directory, or kept outside it for the restore to name.
    manifest.path = copied ? 'B' : 'D';
    manifest.engine = kGitDescribe;
    manifest.superblock_version = wiring_.superblock_version;
    manifest.page_size = static_cast<std::uint32_t>(kPageSize);
    manifest.segment_size = wiring_.segment_size;
    manifest.lineage_id = wiring_.lineage_id;
    manifest.stop_lsn = stop;
    manifest.start_segment = manifest.start_lsn / wiring_.segment_size;
    manifest.stop_segment = stop_segment;
    manifest.created_unix = now_unix;

    std::lock_guard<std::mutex> lock(mu_);
    state_ = State::kFinishing;
    stop_lsn_ = stop;
    Touch();
    if (worker_.joinable()) worker_.join();  // the finished copier
    worker_ = std::thread([this, manifest, dir]() mutable { RunFinish(std::move(manifest), dir); });
    return "OK backup_id=" + std::to_string(manifest.backup_id) +
           " stop_lsn=" + std::to_string(stop) + " stop_segment=" + std::to_string(stop_segment) +
           " path=" + std::string(1, manifest.path) + " finishing=1";
}

void BackupControl::RunFinish(BackupManifest manifest, std::string dir) {
    const fs::path root(dir);
    const auto done = [&](const Status& s) {
        std::lock_guard<std::mutex> lock(mu_);
        if (s.ok()) {
            last_stop_lsn_ = manifest.stop_lsn;
            EndLocked("done", {});
        } else {
            EndLocked("failed", s.message());
        }
    };
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(mu_);
        fd = copy_fd_;
    }
    if (fd >= 0) {
        // The copy is whole once it is durable and has its name.
        if (::fsync(fd) != 0) {
            return done(Status::IoError(std::string("backup: fsync the data copy: ") +
                                        std::strerror(errno)));
        }
        if (::rename((root / kCopyTempName).c_str(), (root / kBackupDataFileName).c_str()) != 0) {
            return done(Status::IoError(std::string("backup: name the data copy: ") +
                                        std::strerror(errno)));
        }
        if (Status s = SyncDirectory(dir); !s.ok()) return done(s);
    }
    // **Every page by its own CRC32C** (`restore.hpp`): the copier's file,
    // or the data file a snapshot put here. A page that fails is named and
    // the backup fails - no manifest, so it is no backup.
    std::error_code ec;
    if (fs::exists(root / kBackupDataFileName, ec)) {
        DataFileCheck check;
        if (Status s = VerifyDataFilePages((root / kBackupDataFileName).string(), &check);
            !s.ok()) {
            return done(s);
        }
        pages_verified_.store(check.pages_checked, std::memory_order_relaxed);
    }
    for (std::uint64_t seg = manifest.start_segment; seg <= manifest.stop_segment; ++seg) {
        BackupFile file;
        file.name = std::string(kBackupWalDirName) + "/" + SegmentFileName(seg);
        const std::string from = (fs::path(wiring_.wal_dir) / SegmentFileName(seg)).string();
        if (Status s = CopyFileDurably(from, (root / file.name).string(), &file.size,
                                       &file.crc32c);
            !s.ok()) {
            return done(s.WithMessage("segment " + std::to_string(seg) + ": " + s.message()));
        }
        log_bytes_copied_.fetch_add(file.size, std::memory_order_relaxed);
        manifest.files.push_back(std::move(file));
    }
    if (fs::exists(root / kBackupDataFileName, ec)) {
        BackupFile file;
        file.name = kBackupDataFileName;
        if (Status s = ChecksumFile((root / file.name).string(), &file.size, &file.crc32c);
            !s.ok()) {
            return done(s);
        }
        manifest.files.push_back(std::move(file));
    }
    done(WriteManifest(dir, manifest));
}

void BackupControl::EndLocked(const char* outcome, const std::string& error) {
    last_id_ = backup_id_;
    last_outcome_ = outcome;
    last_error_ = error;
    const bool ok = std::string_view(outcome) == "done";
    (ok ? completed_ : failed_).fetch_add(1, std::memory_order_relaxed);
    if (copy_fd_ >= 0) {
        ::close(copy_fd_);
        copy_fd_ = -1;
        if (!ok) {
            std::error_code ec;
            fs::remove(fs::path(copy_dir_) / kCopyTempName, ec);
        }
    }
    pin_ = kNoPin;
    state_ = State::kIdle;
    stop_lsn_ = 0;
    stop_capacity_ = 0;
    copy_dir_.clear();
    if (wiring_.log != nullptr) {
        const std::string what = "backup " + std::to_string(backup_id_) + " " + outcome +
                                 (error.empty() ? std::string() : ": " + error);
        if (ok && wiring_.log->enabled(LogLevel::kInfo)) {
            wiring_.log->Info("backup", what);
        } else if (!ok && wiring_.log->enabled(LogLevel::kWarn)) {
            wiring_.log->Warn("backup", what);
        }
    }
}

StatusOr<std::string> BackupControl::Abort() {
    std::thread copier;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (state_ == State::kIdle) {
            return Status::InvalidArgument("BACKUP ABORT: no backup is open");
        }
        if (state_ != State::kOpen) {
            return Status::InvalidArgument("BACKUP ABORT: backup " + std::to_string(backup_id_) +
                                           " is past its stop and finishing; it ends by itself");
        }
        if (copier_running_) {
            copy_cancel_.store(true, std::memory_order_relaxed);
            copier = std::move(worker_);
        }
    }
    if (copier.joinable()) copier.join();
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ != State::kOpen) return Status::InvalidArgument("BACKUP ABORT: no backup is open");
    const std::uint64_t id = backup_id_;
    EndLocked("aborted", "BACKUP ABORT");
    return "OK backup_id=" + std::to_string(id) + " aborted";
}

void BackupControl::ExpireIfAbandoned() {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ != State::kOpen || copier_running_) return;
    const auto idle = std::chrono::steady_clock::now() - last_activity_;
    if (std::chrono::duration_cast<std::chrono::nanoseconds>(idle).count() <
        static_cast<std::int64_t>(kAbandonNs)) {
        return;
    }
    EndLocked("aborted", "abandoned: idle for " + std::to_string(kAbandonNs / 1'000'000'000) +
                             " s with nothing running");
}

void BackupControl::Recycle(wal::Lsn d, const std::function<void(wal::Lsn)>& recycle) {
    std::lock_guard<std::mutex> lock(mu_);
    recycle(std::min(d, pin_));
}

wal::Lsn BackupControl::ReclaimBound(wal::Lsn d) const {
    std::lock_guard<std::mutex> lock(mu_);
    return std::min(d, pin_);
}

bool BackupControl::active() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_ != State::kIdle;
}

std::string BackupControl::StatusLine() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::ostringstream os;
    os << "OK state=" << StateName(state_);
    if (state_ != State::kIdle) {
        os << " backup_id=" << backup_id_ << " start_lsn=" << start_lsn_
           << " copying=" << (copier_running_ ? 1 : 0)
           << " pages_copied=" << pages_copied_.load(std::memory_order_relaxed)
           << " data_pages=" << start_capacity_;
    }
    os << " last_id=" << last_id_ << " last_outcome=" << last_outcome_
       << " last_stop_lsn=" << last_stop_lsn_;
    const std::string& error = state_ != State::kIdle && !copy_error_.empty() ? copy_error_
                                                                               : last_error_;
    if (!error.empty()) os << " error=" << error;
    return os.str();
}

void BackupControl::AppendMeta(std::ostream& os) const {
    std::lock_guard<std::mutex> lock(mu_);
    os << " backup_state=" << StateName(state_);
    if (state_ != State::kIdle) {
        os << " backup_id=" << backup_id_ << " backup_pin=" << pin_
           << " backup_pages_copied=" << pages_copied_.load(std::memory_order_relaxed)
           << " backup_data_pages=" << start_capacity_;
    }
    os << " backup_log_bytes_copied=" << log_bytes_copied_.load(std::memory_order_relaxed)
       << " backup_fence_writes_waited=" << wiring_.data_device->write_fence().writes_waited()
       << " backup_pages_verified=" << pages_verified_.load(std::memory_order_relaxed)
       << " backups_completed=" << completed_.load(std::memory_order_relaxed)
       << " backups_failed=" << failed_.load(std::memory_order_relaxed);
}

}  // namespace kds::server
