#pragma once

#ifndef NDEBUG
#include <cstdio>
#include <cstdlib>
#endif

// **The debug half of "statistics queue no statement"** (`docs/rules/rules.md`
// §3a; DL-S5, the work order's DL-Q8). A statistics record - a count, a
// sample, a note taken on a statement's path - opens a
// `StatisticsRecordScope`; while one is open on a thread, the two things a
// record must never do abort the process, naming the record and what it
// tried:
//
//   - **a blocking latch acquisition** (`Latch::lock`, the `LatchGuard` that
//     waits): a record takes its own core's state, and a latch only by the
//     try form;
//   - **a page fetch or creation** (`PageStore`'s pinned accessors): a
//     record takes no page hold.
//
// So the next statistic that would queue a statement is an abort in the
// first test that reaches it, in every debug build - the whole suite runs
// with the scopes armed - and not a number found at a milestone's close
// (`sys.access_stats`' page 11, DA).
//
// **Debug only**: a release build compiles the scope and the check to
// nothing. A thread-local pointer, so a scope costs two stores and a check
// one load; scopes nest, and a nested one names the innermost.

namespace kds {

#ifndef NDEBUG

namespace detail {
// The record the calling thread is inside, or null.
inline thread_local const char* g_statistics_record = nullptr;
}  // namespace detail

// Aborts where the calling thread is inside a statistics record.
inline void AssertNoStatisticsRecord(const char* doing) noexcept {
    if (detail::g_statistics_record == nullptr) return;
    std::fprintf(stderr,
                 "kds: %s inside a statistics record (%s): statistics queue no statement "
                 "(docs/rules/rules.md section 3a)\n",
                 doing, detail::g_statistics_record);
    std::abort();
}

class StatisticsRecordScope {
public:
    // `what` names the record in the abort's message; static storage.
    explicit StatisticsRecordScope(const char* what) noexcept
        : outer_(detail::g_statistics_record) {
        detail::g_statistics_record = what;
    }
    ~StatisticsRecordScope() { detail::g_statistics_record = outer_; }
    StatisticsRecordScope(const StatisticsRecordScope&) = delete;
    StatisticsRecordScope& operator=(const StatisticsRecordScope&) = delete;

private:
    const char* outer_;
};

#else

inline void AssertNoStatisticsRecord(const char*) noexcept {}

class StatisticsRecordScope {
public:
    explicit StatisticsRecordScope(const char*) noexcept {}
    StatisticsRecordScope(const StatisticsRecordScope&) = delete;
    StatisticsRecordScope& operator=(const StatisticsRecordScope&) = delete;
};

#endif

}  // namespace kds
