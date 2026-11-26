#include "kds/server/wal_archive.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
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
// How often a bounded command's exit is asked for. The archiver's own
// thread sleeps between asks; a command's exit is read at most this late.
constexpr auto kChildPoll = std::chrono::milliseconds(20);
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
    // A command's exit is read by `waitpid`, which answers `ECHILD` while
    // SIGCHLD is ignored - and an ignored disposition is inherited across
    // `execve`, so a launcher that left it so would make every archive
    // attempt fail (G42's refusal, in `ArchiveOne`). Only that disposition
    // is replaced, by the default, which discards the signal too: a handler
    // the process installed is its own and is left alone.
    if (!wiring_.settings.archive_command.empty()) {
        struct sigaction current {};
        if (::sigaction(SIGCHLD, nullptr, &current) == 0 &&
            (current.sa_handler == SIG_IGN || (current.sa_flags & SA_NOCLDWAIT) != 0)) {
            ::signal(SIGCHLD, SIG_DFL);
        }
    }
    if (wiring_.hold_from) wiring_.hold_from(next_.load(std::memory_order_acquire));
    std::lock_guard<std::mutex> lock(mu_);
    if (thread_.joinable()) return;
    stopping_ = false;
    stop_requested_.store(false, std::memory_order_release);
    thread_ = std::thread([this] { Run(); });
}

void WalArchiver::Stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
    }
    // Read by a bounded command's wait, which then gives the command the
    // grace and no more: a stop is not held for the rest of the bound.
    stop_requested_.store(true, std::memory_order_release);
    // A command still running is ended, so a stop never waits on it. A
    // bounded command's entry is its group, negated (`child_`).
    if (const pid_t pid = child_.load(std::memory_order_acquire); pid != 0) ::kill(pid, SIGTERM);
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
    // **A bounded command leads its own process group** (DS-S2): the bound
    // signals the group, since ending `/bin/sh` alone would leave whatever
    // it started holding the segment's upload - and the next attempt would
    // run beside it. An unbounded command is spawned as it always was.
    const auto bound = std::chrono::seconds(wiring_.settings.command_timeout_s);
    const bool bounded = bound.count() > 0;
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
    if (bounded) {
        posix_spawnattr_setpgroup(&attr, 0);
        flags |= POSIX_SPAWN_SETPGROUP;
    }
    posix_spawnattr_setflags(&attr, flags);
    const char* argv[] = {"/bin/sh", "-c", command.c_str(), nullptr};
    pid_t pid = 0;
    const int spawned = ::posix_spawn(&pid, "/bin/sh", nullptr, &attr,
                                      const_cast<char* const*>(argv), environ);
    posix_spawnattr_destroy(&attr);
    if (spawned != 0) {
        return Status::IoError("archive_command could not start: " +
                               std::string(std::strerror(spawned)));
    }
    const pid_t target = bounded ? -pid : pid;
    child_.store(target, std::memory_order_release);
    int rc = 0;
    pid_t reaped = 0;
    int wait_errno = 0;
    bool timed_out = false;  // the bound fell
    bool ended = false;      // the command was signalled: by the bound, or by a stop
    if (!bounded) {
        while ((reaped = ::waitpid(pid, &rc, 0)) < 0 && errno == EINTR) {
        }
        wait_errno = errno;
    } else {
        // Asked without waiting, so the bound - and a stop - can be read
        // between asks. `SIGTERM` when either falls, `SIGKILL` a grace
        // later, and the loop ends only at the command's exit: a killed
        // process always exits.
        //
        // **The exit is looked at before it is reaped** (`WNOWAIT`): while
        // the shell is an unreaped zombie it still holds its group's id, so
        // the signal that clears what it left behind cannot reach a group
        // that took the id over. Reaped first, the id would be free.
        const auto started = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point signalled{};
        bool killed = false, exited = false;
        for (;;) {
            siginfo_t info {};
            const int asked = ::waitid(P_PID, static_cast<id_t>(pid), &info,
                                       WEXITED | WNOHANG | WNOWAIT);
            if (asked == 0 && info.si_pid == pid) {
                exited = true;
                break;
            }
            if (asked < 0 && errno != EINTR) {
                wait_errno = errno;  // ECHILD: the kernel reaped it (G42)
                reaped = -1;
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            if (!ended) {
                timed_out = now - started >= bound;
                if (timed_out || stop_requested_.load(std::memory_order_acquire)) {
                    ended = true;
                    signalled = now;
                    ::kill(target, SIGTERM);
                }
            } else if (!killed && now - signalled >= kCommandKillGrace) {
                killed = true;
                ::kill(target, SIGKILL);
            }
            std::this_thread::sleep_for(kChildPoll);
        }
        if (exited) {
            // Whatever the shell left in its group goes with it, so a retry
            // never runs beside a survivor of the attempt before.
            if (ended) ::kill(target, SIGKILL);
            while ((reaped = ::waitpid(pid, &rc, 0)) < 0 && errno == EINTR) {
            }
            wait_errno = errno;
        }
    }
    child_.store(0, std::memory_order_release);
    // **A command that was signalled archived nothing, whatever it exited
    // with**: an exit 0 from a handler that cleaned up and left is not the
    // command's word that the segment is durable. The one honest exit 0
    // this refuses is a command that finished in the instant the bound
    // fell; it costs a retry, which an idempotent command absorbs.
    if (ended) {
        if (!timed_out) {
            return Status::IoError("archive_command was ended by the archiver's stop: " +
                                   command);
        }
        return Status::IoError("archive_command ran past archive_command_timeout_s = " +
                               std::to_string(wiring_.settings.command_timeout_s) +
                               " and was ended: " + command);
    }
    // **A wait that failed is not an exit 0** (the audit's G42): `rc` is
    // still the 0 it was initialised to, which reads as a clean exit, and
    // the segment would count as archived and be recycled on the word of a
    // command whose outcome nobody learned. The reachable case is `ECHILD`
    // while SIGCHLD is ignored: the kernel reaps the child itself.
    if (reaped != pid) {
        return Status::IoError("archive_command: its exit could not be read (waitpid: " +
                               std::string(std::strerror(wait_errno)) + "): " + command);
    }
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
