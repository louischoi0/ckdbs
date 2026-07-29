#include "kds/base/log.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

namespace kds {

const char* LogLevelName(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::kTrace: return "TRACE";
        case LogLevel::kDebug: return "DEBUG";
        case LogLevel::kInfo: return "INFO";
        case LogLevel::kWarn: return "WARN";
        case LogLevel::kError: return "ERROR";
        case LogLevel::kOff: return "OFF";
    }
    return "?";
}

StatusOr<LogLevel> ParseLogLevel(std::string_view name) {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    if (lower == "trace") return LogLevel::kTrace;
    if (lower == "debug") return LogLevel::kDebug;
    if (lower == "info") return LogLevel::kInfo;
    if (lower == "warn" || lower == "warning") return LogLevel::kWarn;
    if (lower == "error") return LogLevel::kError;
    if (lower == "off" || lower == "none") return LogLevel::kOff;
    return Status::NotFound("unknown log level '" + std::string(name) + "'");
}

std::uint64_t SystemWallClock::NowUnixSeconds() const {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

namespace {

// Creates every missing directory component of `path`'s parent. A
// configured log_dir that does not exist yet should not be a startup
// failure - the operator asked for a location, not for a location that
// already exists.
Status EnsureParentDirectories(const std::string& path) {
    std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        return Status::OK();  // no directory part, or the root itself
    }

    const std::string dir = path.substr(0, slash);
    std::string partial;
    partial.reserve(dir.size());

    std::size_t start = 0;
    if (dir.front() == '/') {
        partial.push_back('/');
        start = 1;
    }

    while (start <= dir.size()) {
        std::size_t next = dir.find('/', start);
        if (next == std::string::npos) next = dir.size();
        if (next > start) {
            if (partial.size() > 1 || (partial.size() == 1 && partial[0] != '/')) {
                partial.push_back('/');
            }
            partial.append(dir, start, next - start);

            if (::mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST) {
                return Status::IoError("cannot create log directory '" + partial +
                                        "': " + std::strerror(errno));
            }
        }
        start = next + 1;
    }
    return Status::OK();
}

void AppendUint(std::string& out, std::uint64_t v) {
    char buf[24];
    int n = 0;
    if (v == 0) {
        buf[n++] = '0';
    } else {
        while (v > 0 && n < static_cast<int>(sizeof(buf))) {
            buf[n++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        }
    }
    while (n > 0) out.push_back(buf[--n]);
}

}  // namespace

StatusOr<std::unique_ptr<FileLogSink>> FileLogSink::Open(const std::string& path) {
    if (path.empty()) {
        return Status::InvalidArgument("log file path is empty");
    }
    if (Status s = EnsureParentDirectories(path); !s.ok()) return s;

    // O_APPEND, never O_TRUNC: a restart continues the log. Losing the
    // record of why the last run died, at the moment of starting the next
    // one, is precisely the wrong time to lose it.
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        return Status::IoError("cannot open log file '" + path + "': " + std::strerror(errno));
    }
    return std::unique_ptr<FileLogSink>(new FileLogSink(FileDescriptor(fd), path));
}

Status FileLogSink::Write(std::string_view line) {
    // One write() for the line and its newline together: two writes could
    // interleave with another process appending to the same file and split
    // a line in half.
    std::string buf;
    buf.reserve(line.size() + 1);
    buf.append(line);
    buf.push_back('\n');

    std::size_t written = 0;
    while (written < buf.size()) {
        ssize_t n = ::write(fd_.get(), buf.data() + written, buf.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return Status::IoError("log write failed: " + std::string(std::strerror(errno)));
        }
        written += static_cast<std::size_t>(n);
    }
    return Status::OK();
}

void Logger::Log(LogLevel level, std::string_view component, std::string_view message) {
    if (!enabled(level)) return;

    std::string line;
    line.reserve(component.size() + message.size() + 32);
    AppendUint(line, clock_.NowUnixSeconds());
    line.push_back(' ');
    line.append(LogLevelName(level));
    line.append(" [");
    line.append(component);
    line.append("] ");
    line.append(message);

    // A failed log line is counted, never propagated: the caller is
    // reporting something that already happened, and taking the server
    // down because the report could not be filed would be strictly worse
    // than losing the report.
    if (sink_->Write(line).ok()) {
        ++lines_written_;
    } else {
        ++write_failures_;
    }
}

}  // namespace kds
