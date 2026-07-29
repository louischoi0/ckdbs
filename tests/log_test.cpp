#include "kds/base/log.hpp"

#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

// The logger's contract: level filtering happens before formatting, a null
// sink is a legal destination, and a sink that fails never takes the caller
// down with it.

namespace kds {
namespace {

// A sink that can be told to fail, for the "logging must not be fatal" path.
class FlakySink final : public LogSink {
public:
    Status Write(std::string_view line) override {
        ++attempts;
        if (fail) return Status::IoError("scripted sink failure");
        lines.emplace_back(line);
        return Status::OK();
    }

    std::vector<std::string> lines;
    int attempts = 0;
    bool fail = false;
};

std::string TempPath(const std::string& name) {
    return "/tmp/kds_log_test_" + std::to_string(::getpid()) + "_" + name;
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

TEST(LogLevelTest, NamesRoundTripThroughParse) {
    for (LogLevel level : {LogLevel::kTrace, LogLevel::kDebug, LogLevel::kInfo, LogLevel::kWarn,
                           LogLevel::kError, LogLevel::kOff}) {
        auto parsed = ParseLogLevel(LogLevelName(level));
        ASSERT_TRUE(parsed.ok()) << LogLevelName(level);
        EXPECT_EQ(parsed.value(), level);
    }
}

TEST(LogLevelTest, ParseIsCaseInsensitiveAndAcceptsWarning) {
    EXPECT_EQ(ParseLogLevel("INFO").value(), LogLevel::kInfo);
    EXPECT_EQ(ParseLogLevel("Debug").value(), LogLevel::kDebug);
    EXPECT_EQ(ParseLogLevel("warning").value(), LogLevel::kWarn);
    EXPECT_EQ(ParseLogLevel("none").value(), LogLevel::kOff);
}

TEST(LogLevelTest, AnUnknownLevelIsNotFound) {
    auto parsed = ParseLogLevel("verbose");
    EXPECT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kNotFound);
}

TEST(LoggerTest, WritesTimestampLevelComponentAndMessage) {
    MemoryLogSink sink;
    ManualWallClock clock(1'700'000'000);
    Logger log(&sink, clock);

    log.Info("expeditor", "listening on 127.0.0.1:15432");

    ASSERT_EQ(sink.lines.size(), 1u);
    EXPECT_EQ(sink.lines[0], "1700000000 INFO [expeditor] listening on 127.0.0.1:15432");
    EXPECT_EQ(log.lines_written(), 1u);
}

TEST(LoggerTest, TheTimestampComesFromTheInjectedClock) {
    MemoryLogSink sink;
    ManualWallClock clock(100);
    Logger log(&sink, clock);

    log.Info("t", "first");
    clock.Advance(50);
    log.Info("t", "second");

    ASSERT_EQ(sink.lines.size(), 2u);
    EXPECT_EQ(sink.lines[0].substr(0, 3), "100");
    EXPECT_EQ(sink.lines[1].substr(0, 3), "150");
}

TEST(LoggerTest, MessagesBelowTheThresholdAreDropped) {
    MemoryLogSink sink;
    ManualWallClock clock;
    Logger log(&sink, clock, LogLevel::kWarn);

    log.Trace("t", "no");
    log.Debug("t", "no");
    log.Info("t", "no");
    log.Warn("t", "yes");
    log.Error("t", "yes");

    ASSERT_EQ(sink.lines.size(), 2u);
    EXPECT_NE(sink.lines[0].find("WARN"), std::string::npos);
    EXPECT_NE(sink.lines[1].find("ERROR"), std::string::npos);
}

TEST(LoggerTest, LevelOffSuppressesEverythingIncludingErrors) {
    MemoryLogSink sink;
    ManualWallClock clock;
    Logger log(&sink, clock, LogLevel::kOff);

    log.Error("t", "not even this");
    EXPECT_TRUE(sink.lines.empty());
    EXPECT_FALSE(log.enabled(LogLevel::kError));
}

TEST(LoggerTest, ANullSinkDiscardsWithoutCrashing) {
    ManualWallClock clock;
    Logger log(nullptr, clock, LogLevel::kTrace);

    log.Error("t", "goes nowhere");
    EXPECT_FALSE(log.enabled(LogLevel::kError));
    EXPECT_EQ(log.lines_written(), 0u);
    EXPECT_EQ(log.write_failures(), 0u);
}

TEST(LoggerTest, ASinkFailureIsCountedNotPropagated) {
    FlakySink sink;
    ManualWallClock clock;
    Logger log(&sink, clock);

    log.Info("t", "ok");
    sink.fail = true;
    log.Info("t", "lost");  // must not throw, abort, or otherwise escape
    sink.fail = false;
    log.Info("t", "ok again");

    EXPECT_EQ(log.lines_written(), 2u);
    EXPECT_EQ(log.write_failures(), 1u);
    EXPECT_EQ(sink.attempts, 3);
}

TEST(LoggerTest, SetMinLevelTakesEffectImmediately) {
    MemoryLogSink sink;
    ManualWallClock clock;
    Logger log(&sink, clock, LogLevel::kError);

    log.Info("t", "dropped");
    log.set_min_level(LogLevel::kInfo);
    log.Info("t", "kept");

    ASSERT_EQ(sink.lines.size(), 1u);
    EXPECT_NE(sink.lines[0].find("kept"), std::string::npos);
}

// ---- FileLogSink ---------------------------------------------------------

TEST(FileLogSinkTest, WritesOneNewlineTerminatedLinePerCall) {
    const std::string path = TempPath("basic.log");
    ::unlink(path.c_str());

    {
        auto sink = FileLogSink::Open(path);
        ASSERT_TRUE(sink.ok()) << sink.status().message();
        ManualWallClock clock(42);
        Logger log(sink.value().get(), clock);
        log.Info("a", "first");
        log.Warn("b", "second");
    }

    EXPECT_EQ(ReadFile(path), "42 INFO [a] first\n42 WARN [b] second\n");
    ::unlink(path.c_str());
}

TEST(FileLogSinkTest, ReopeningAppendsRatherThanTruncating) {
    const std::string path = TempPath("append.log");
    ::unlink(path.c_str());
    ManualWallClock clock(1);

    {
        auto sink = FileLogSink::Open(path);
        ASSERT_TRUE(sink.ok());
        Logger log(sink.value().get(), clock);
        log.Info("run", "first boot");
    }
    {
        // A restart must not erase why the last run died.
        auto sink = FileLogSink::Open(path);
        ASSERT_TRUE(sink.ok());
        Logger log(sink.value().get(), clock);
        log.Info("run", "second boot");
    }

    const std::string content = ReadFile(path);
    EXPECT_NE(content.find("first boot"), std::string::npos);
    EXPECT_NE(content.find("second boot"), std::string::npos);
    ::unlink(path.c_str());
}

TEST(FileLogSinkTest, MissingParentDirectoriesAreCreated) {
    const std::string dir = TempPath("nested");
    const std::string path = dir + "/deeper/still/kdb.log";

    auto sink = FileLogSink::Open(path);
    ASSERT_TRUE(sink.ok()) << sink.status().message();
    ASSERT_TRUE(sink.value()->Write("hello").ok());

    EXPECT_EQ(ReadFile(path), "hello\n");

    ::unlink(path.c_str());
    ::rmdir((dir + "/deeper/still").c_str());
    ::rmdir((dir + "/deeper").c_str());
    ::rmdir(dir.c_str());
}

TEST(FileLogSinkTest, AnEmptyPathIsRefused) {
    auto sink = FileLogSink::Open("");
    EXPECT_FALSE(sink.ok());
    EXPECT_EQ(sink.status().code(), StatusCode::kInvalidArgument);
}

TEST(FileLogSinkTest, AnUnopenablePathIsAnIoError) {
    // /dev/null/... cannot be a directory, so creating the parent fails.
    auto sink = FileLogSink::Open("/dev/null/nope/kdb.log");
    EXPECT_FALSE(sink.ok());
    EXPECT_EQ(sink.status().code(), StatusCode::kIoError);
}

}  // namespace
}  // namespace kds
