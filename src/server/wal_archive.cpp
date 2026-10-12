#include "kds/server/wal_archive.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#include <csignal>
#include <cstring>

#include <spawn.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

#include "kds/server/backup_manifest.hpp"

namespace kds::server {
namespace {

namespace fs = std::filesystem;

constexpr auto kIdleWait = std::chrono::seconds(1);
constexpr auto kMaxBackoff = std::chrono::seconds(30);

// Byte-for-byte: the archive's promise is "never a different body under one
// name", which a checksum match only makes likely.
StatusOr<bool> SameBytes(const std::string& a, const std::string& b) {
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa.good() || !fb.good()) return Status::IoError("archive: cannot compare " + a + " and " + b);
    std::vector<char> ba(1 << 20), bb(1 << 20);
    for (;;) {
        fa.read(ba.data(), static_cast<std::streamsize>(ba.size()));
        fb.read(bb.data(), static_cast<std::streamsize>(bb.size()));
        const std::streamsize na = fa.gcount(), nb = fb.gcount();
        if (na != nb || !std::equal(ba.begin(), ba.begin() + na, bb.begin())) return false;
        if (na == 0) return true;
    }
}

std::string Substitute(std::string command, const std::string& path, const std::string& name) {
    std::string out;
    for (std::size_t i = 0; i < command.size(); ++i) {
        if (command[i] == '%' && i + 1 < command.size()) {
            if (command[i + 1] == 'p') {
                out += path;
                ++i;
                continue;
            }
            if (command[i + 1] == 'f') {
                out += name;
                ++i;
                continue;
            }
            if (command[i + 1] == '%') {
                out += '%';
                ++i;
                continue;
            }
        }
        out += command[i];
    }
    return out;
}

}  // namespace

std::string ArchiveName(std::uint64_t lineage, std::uint64_t segment) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "kds-%016llx-%llu.log",
                  static_cast<unsigned long long>(lineage), static_cast<unsigned long long>(segment));
    return buf;
}

WalArchiver::WalArchiver(Wiring wiring) : wiring_(std::move(wiring)), next_(wiring_.first_segment) {}

WalArchiver::~WalArchiver() { Stop(); }

void WalArchiver::Start() {
    if (wiring_.hold_from) wiring_.hold_from(next_.load(std::memory_order_acquire));
    std::lock_guard<std::mutex> lock(mu_);
    if (thread_.joinable()) return;
    stopping_ = false;
    thread_ = std::thread([this] { Run(); });
}

void WalArchiver::Stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
    }
    // A command still running is ended, so a stop never waits on it.
    if (const pid_t pid = child_.load(std::memory_order_acquire); pid > 0) ::kill(pid, SIGTERM);
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void WalArchiver::Kick() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        kicked_ = true;
    }
    wake_.notify_all();
}

void WalArchiver::Run() {
    auto backoff = std::chrono::seconds(1);
    for (;;) {
        bool failing = false;
        // Every segment below the durable LSN's is sealed and on the device.
        const std::uint64_t sealed_end = wiring_.durable_lsn() / wiring_.segment_size;
        while (next_.load(std::memory_order_relaxed) < sealed_end) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (stopping_) return;
            }
            const std::uint64_t segment = next_.load(std::memory_order_relaxed);
            if (Status s = ArchiveOne(segment); !s.ok()) {
                failures_.fetch_add(1, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    last_error_ = "segment " + std::to_string(segment) + ": " + s.message();
                }
                if (wiring_.log != nullptr && wiring_.log->enabled(LogLevel::kError)) {
                    wiring_.log->Error("archive", "archiving segment " + std::to_string(segment) +
                                                      " failed: " + s.message());
                }
                failing = true;
                break;
            }
            next_.store(segment + 1, std::memory_order_release);
            if (wiring_.hold_from) wiring_.hold_from(segment + 1);
            archived_.fetch_add(1, std::memory_order_relaxed);
            backoff = std::chrono::seconds(1);
            {
                std::lock_guard<std::mutex> lock(mu_);
                last_error_.clear();
            }
        }
        UpdateBlocking(failing);
        std::unique_lock<std::mutex> lock(mu_);
        if (stopping_) return;
        wake_.wait_for(lock, failing ? backoff : std::chrono::duration_cast<std::chrono::seconds>(kIdleWait),
                       [&] { return stopping_ || kicked_; });
        kicked_ = false;
        if (stopping_) return;
        if (failing) backoff = std::min(backoff * 2, std::chrono::duration_cast<std::chrono::seconds>(kMaxBackoff));
    }
}

Status WalArchiver::ArchiveOne(std::uint64_t segment) {
    const std::string path = (fs::path(wiring_.wal_dir) / SegmentFileName(segment)).string();
    const std::string name = ArchiveName(wiring_.lineage_id, segment);
    if (!wiring_.settings.archive_dir.empty()) {
        const std::string target = (fs::path(wiring_.settings.archive_dir) / name).string();
        std::error_code ec;
        if (fs::exists(target, ec)) {
            auto same = SameBytes(path, target);
            if (!same.ok()) return same.status();
            if (!same.value()) {
                return Status::Corruption("the archive holds a different body under " + name +
                                          "; archiving stops rather than overwrite it");
            }
            return Status::OK();  // archived before a restart
        }
        std::uint64_t size = 0;
        std::uint32_t crc = 0;
        return CopyFileDurably(path, target, &size, &crc);
    }
    const std::string command = Substitute(wiring_.settings.archive_command, path, name);
    // **Not `std::system`**: it ignores SIGINT and SIGQUIT in the whole
    // process while the command runs, and the server hears its stop through
    // a blocked signalfd (`stop_signal.hpp`) that an ignored signal never
    // reaches. `posix_spawn` with an empty mask and default dispositions
    // leaves the server's signals alone and gives the child ordinary ones.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    const char* argv[] = {"/bin/sh", "-c", command.c_str(), nullptr};
    pid_t pid = 0;
    const int spawned = ::posix_spawn(&pid, "/bin/sh", nullptr, &attr,
                                      const_cast<char* const*>(argv), environ);
    posix_spawnattr_destroy(&attr);
    if (spawned != 0) {
        return Status::IoError("archive_command could not start: " +
                               std::string(std::strerror(spawned)));
    }
    child_.store(pid, std::memory_order_release);
    int rc = 0;
    while (::waitpid(pid, &rc, 0) < 0 && errno == EINTR) {
    }
    child_.store(0, std::memory_order_release);
    if (!WIFEXITED(rc) || WEXITSTATUS(rc) != 0) {
        return Status::IoError("archive_command exited " +
                               std::to_string(WIFEXITED(rc) ? WEXITSTATUS(rc) : -1) + ": " +
                               command);
    }
    return Status::OK();
}

void WalArchiver::UpdateBlocking(bool failing) {
    if (!failing) {
        blocking_.store(false, std::memory_order_release);
        return;
    }
    struct statvfs fs_stat {};
    if (::statvfs(wiring_.wal_dir.c_str(), &fs_stat) != 0) {
        blocking_.store(true, std::memory_order_release);  // unknown space: closed
        return;
    }
    const std::uint64_t free_bytes =
        static_cast<std::uint64_t>(fs_stat.f_bavail) * static_cast<std::uint64_t>(fs_stat.f_frsize);
    blocking_.store(free_bytes < kReserveSegments * wiring_.segment_size,
                    std::memory_order_release);
}

std::string WalArchiver::last_error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_error_;
}

void WalArchiver::AppendMeta(std::ostream& os) const {
    const std::uint64_t sealed_end = wiring_.durable_lsn() / wiring_.segment_size;
    const std::uint64_t next = next_.load(std::memory_order_acquire);
    os << " wal_archive_next_segment=" << next
       << " wal_archive_lag_segments=" << (sealed_end > next ? sealed_end - next : 0)
       << " wal_archived=" << archived_.load(std::memory_order_relaxed)
       << " wal_archive_failures=" << failures_.load(std::memory_order_relaxed)
       << " wal_archive_blocking=" << (blocking() ? 1 : 0);
    // Last in the block, and only while failing: the one free-text field,
    // its spaces written as `_` so the line stays `key=value` pairs.
    std::string error = last_error();
    if (!error.empty()) {
        std::replace(error.begin(), error.end(), ' ', '_');
        os << " wal_archive_last_error=" << error;
    }
}

}  // namespace kds::server
