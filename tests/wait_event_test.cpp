#include <gtest/gtest.h>

#include <set>
#include <string>

#include "kds/stats/wait_event.hpp"

// CJ's WE01 (`instructions/v3.2.0/workorder-cj-wait-events-ash.md` §2.4): the
// registry's shape. Its compile-time checks are `static_assert`s in the header;
// these pin what a build cannot say about the table's content.

namespace kds::stats {
namespace {

TEST(WaitEventRegistry, OnCpuIsIdZeroAndEveryIdIsItsIndex) {
    EXPECT_EQ(static_cast<int>(WaitEvent::kOnCpu), 0);
    for (std::size_t i = 0; i < kWaitEventCount; ++i) {
        EXPECT_EQ(static_cast<std::size_t>(kWaitEvents[i].id), i);
    }
}

TEST(WaitEventRegistry, TheV1EventsAreAllRegisteredByName) {
    const char* const names[] = {
        "commit.wal_flush",    "io.page_read",         "io.var_heap_read",
        "io.scan_ring_read",   "sysio.writeback",      "sysio.checkpoint",
        "sysio.wal_write",     "xcore.step_response",  "xcore.credit",
        "sched.run_queue",     "res.eviction_retry",   "net.send_backpressure",
        "app.lock_conflict",   "app.assertion_reservation", "idle.client",
        "idle.background"};
    for (const char* n : names) {
        EXPECT_NE(FindWaitEvent(n), WaitEvent::kCount) << n;
    }
    EXPECT_EQ(FindWaitEvent("io.nonsense"), WaitEvent::kCount);
    // The 16 of the spec, the reserved ON_CPU row and WE02..WE04's temporary
    // `sched.unclassified`.
    EXPECT_EQ(kWaitEventCount, 18u);
}

TEST(WaitEventRegistry, OnlyTheApplicationEventsAreCountersAndOnlyIdleLeavesDbTime) {
    std::set<std::string> counters;
    std::set<std::string> outside;
    for (const WaitEventDef& d : kWaitEvents) {
        if (d.kind == WaitKind::kCounter) counters.insert(std::string(d.name));
        if (!d.in_db_time) outside.insert(std::string(d.name));
    }
    EXPECT_EQ(counters, (std::set<std::string>{"app.lock_conflict", "app.assertion_reservation"}));
    EXPECT_EQ(outside, (std::set<std::string>{"idle.client", "idle.background"}));
}

TEST(WaitEventRegistry, EveryEventNamesItsClassAndAParameterOnlyWhereItHasOne) {
    for (const WaitEventDef& d : kWaitEvents) {
        EXPECT_FALSE(WaitClassName(d.wait_class).empty()) << d.name;
        // A later parameter is never described without the one before it.
        if (!d.p2.empty()) EXPECT_FALSE(d.p1.empty()) << d.name;
        if (!d.p3.empty()) EXPECT_FALSE(d.p2.empty()) << d.name;
    }
}

}  // namespace
}  // namespace kds::stats
