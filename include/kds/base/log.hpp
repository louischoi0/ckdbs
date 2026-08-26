#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "kds/base/file_descriptor.hpp"
#include "kds/base/status.hpp"

// The server's diagnostic log: lifecycle events, failures, and the
// background work nobody is watching (checkpoints above all). One line per
// event, appended to a file, default `kdb.log`.
//
// ---- What this is not ---------------------------------------------------
//
// Not per-request tracing. `docs/inflight/in-progress/observability.md` proposes a separate span
// tracer for "where did those 4 ms go", and the two must not merge: a trace
// is high-volume, per-request, and off by default; a log is low-volume,
// per-event, and always on. Merging them produces a log nobody can read and
// a tracer too expensive to leave enabled.
//
// Not a channel for engine errors. A subsystem that cannot do its job
// returns a Status (rules.md section 1); it does not log and continue. What
// belongs here is the thing a Status has no caller to report to - a
// background task's failure, a startup decision, a shutdown - plus whatever
// a human needs to reconstruct what the process did.
//
// ---- Cost, and where it must not appear --------------------------------
//
// A write() per line, synchronous, no buffering thread. That is cheap
// enough for lifecycle events and far too expensive for anything holding a
// page latch or running inside a tuple loop. The level check happens before
// the message is formatted, so a suppressed Debug() costs one comparison
// and does not build its string - but the argument still evaluates, so do
// not call this with work in the arguments on a hot path.
//
// ---- Concurrency --------------------------------------------------------
//
// Core-local like everything else (rules.md section 3): one Logger, one
// reactor thread, no internal locking. The single write() per line means
// interleaving from separate processes appending to one file stays
// line-atomic in practice (O_APPEND), but two threads sharing one Logger is
// not supported and is not something this engine does.

namespace kds {

enum class LogLevel : std::uint8_t {
    kTrace = 0,
    kDebug = 1,
    kInfo = 2,
    kWarn = 3,
    kError = 4,
    // Not a level anything logs *at* - only a threshold, meaning "nothing
    // passes". Kept last so ordering comparisons keep working.
    kOff = 5,
};

const char* LogLevelName(LogLevel level) noexcept;

// Parses "trace"/"debug"/"info"/"warn"/"error"/"off", case-insensitively.
// "warning" is accepted for "warn" because people write it. NotFound for
// anything else - the caller decides whether that is fatal.
StatusOr<LogLevel> ParseLogLevel(std::string_view name);

// Wall-clock seconds since the Unix epoch. Separate from sched::Clock,
// which is monotonic: a monotonic reading is the right answer for measuring
// a duration and a useless one for stamping a log line, since it does not
// name an instant a human can find. Injectable for the same reason as every
// other clock here (rules.md section 4).
class WallClock {
public:
    virtual ~WallClock() = default;
    virtual std::uint64_t NowUnixSeconds() const = 0;
};

// The one implementation allowed to read a real clock.
class SystemWallClock final : public WallClock {
public:
    std::uint64_t NowUnixSeconds() const override;
};

// Deterministic counterpart for tests: time only moves when told to.
class ManualWallClock final : public WallClock {
public:
    explicit ManualWallClock(std::uint64_t now = 0) noexcept : now_(now) {}
    std::uint64_t NowUnixSeconds() const override { return now_; }
    void SetNow(std::uint64_t now) noexcept { now_ = now; }
    void Advance(std::uint64_t delta) noexcept { now_ += delta; }

private:
    std::uint64_t now_;
};

// Where formatted lines go. The seam exists so the whole logger is
// testable without a filesystem, and so a future syslog/stderr destination
// is a new implementation rather than a branch inside Logger.
class LogSink {
public:
    virtual ~LogSink() = default;
    // `line` has no trailing newline; the sink adds whatever its medium
    // needs. A sink that fails reports it - Logger counts the failure and
    // keeps going, since losing the log must never take the server down.
    virtual Status Write(std::string_view line) = 0;
};

// Appends to a file, creating it if absent, opened O_APPEND so a restart
// continues the same log rather than truncating it.
class FileLogSink final : public LogSink {
public:
    // Creates any missing parent directories of `path`, so a configured
    // log_dir that does not exist yet is not a startup failure.
    static StatusOr<std::unique_ptr<FileLogSink>> Open(const std::string& path);

    const std::string& path() const noexcept { return path_; }
    Status Write(std::string_view line) override;

private:
    FileLogSink(FileDescriptor fd, std::string path) noexcept
        : fd_(std::move(fd)), path_(std::move(path)) {}

    FileDescriptor fd_;
    std::string path_;
};

// Keeps lines in memory. For tests, and for a server told to log nowhere.
class MemoryLogSink final : public LogSink {
public:
    Status Write(std::string_view line) override {
        lines.emplace_back(line);
        return Status::OK();
    }

    std::vector<std::string> lines;
};

class Logger {
public:
    // `sink` and `clock` must outlive the logger. A null sink is legal and
    // means "discard": it keeps every call site working when logging is
    // switched off, without each one having to test for a logger.
    Logger(LogSink* sink, const WallClock& clock, LogLevel min_level = LogLevel::kInfo) noexcept
        : sink_(sink), clock_(clock), min_level_(min_level) {}

    LogLevel min_level() const noexcept { return min_level_; }
    void set_min_level(LogLevel level) noexcept { min_level_ = level; }

    // True when a message at this level would be written. Call sites that
    // would have to *build* something to log should test this first.
    bool enabled(LogLevel level) const noexcept {
        return sink_ != nullptr && level >= min_level_ && level < LogLevel::kOff;
    }

    // Lines written, and sink failures swallowed. A log that silently
    // stopped working is worse than no log, so the count is exposed rather
    // than only the successes.
    std::uint64_t lines_written() const noexcept { return lines_written_; }
    std::uint64_t write_failures() const noexcept { return write_failures_; }

    // Writes `<unix_seconds> <LEVEL> [component] message`. `component` is a
    // short subsystem tag ("expeditor", "checkpoint") rather than a file or
    // function: the useful question of a log line is which part of the
    // engine spoke, and __FILE__ answers a different one.
    void Log(LogLevel level, std::string_view component, std::string_view message);

    void Trace(std::string_view c, std::string_view m) { Log(LogLevel::kTrace, c, m); }
    void Debug(std::string_view c, std::string_view m) { Log(LogLevel::kDebug, c, m); }
    void Info(std::string_view c, std::string_view m) { Log(LogLevel::kInfo, c, m); }
    void Warn(std::string_view c, std::string_view m) { Log(LogLevel::kWarn, c, m); }
    void Error(std::string_view c, std::string_view m) { Log(LogLevel::kError, c, m); }

private:
    LogSink* sink_;
    const WallClock& clock_;
    LogLevel min_level_;
    std::uint64_t lines_written_ = 0;
    std::uint64_t write_failures_ = 0;
};

}  // namespace kds
