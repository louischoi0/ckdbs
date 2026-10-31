#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// **The wait-event registry** (`instructions/v3.2.0/workorder-cj-wait-events-ash.md`
// §2.4, CJ's WE01, decision WD2).
//
// A compile-time table: one entry per event, its name `class.event` the only
// public contract (WD2). The numeric `WaitEvent` is the table index, internal
// and not stable across versions; 0 is the reserved `ON_CPU`. There is no
// runtime registration.
//
// The table is checked at compile time: an id that is not its index, a name
// that is not `class.event`, a name whose class prefix is not its class's
// spelling, or a duplicate name fails the build (`static_assert`s below).

namespace kds::stats {

enum class WaitClass : std::uint8_t {
    kCpu = 0,  // the reserved ON_CPU row
    kCommit,
    kUserIo,
    kSystemIo,
    kCrossCore,
    kScheduler,
    kResource,
    kNetwork,
    kApplication,
    kIdle,
    kConcurrency,  // reserved: v1 has no site (core-owned structures)
    kCount,
};

// The class's name as the event names spell it before the dot.
constexpr std::string_view WaitClassPrefix(WaitClass c) noexcept {
    switch (c) {
        case WaitClass::kCpu: return "cpu";
        case WaitClass::kCommit: return "commit";
        case WaitClass::kUserIo: return "io";
        case WaitClass::kSystemIo: return "sysio";
        case WaitClass::kCrossCore: return "xcore";
        case WaitClass::kScheduler: return "sched";
        case WaitClass::kResource: return "res";
        case WaitClass::kNetwork: return "net";
        case WaitClass::kApplication: return "app";
        case WaitClass::kIdle: return "idle";
        case WaitClass::kConcurrency: return "conc";
        case WaitClass::kCount: break;
    }
    return {};
}

// The class's display name, as `sys.wait_event_names.class` prints it.
constexpr std::string_view WaitClassName(WaitClass c) noexcept {
    switch (c) {
        case WaitClass::kCpu: return "CPU";
        case WaitClass::kCommit: return "Commit";
        case WaitClass::kUserIo: return "User I/O";
        case WaitClass::kSystemIo: return "System I/O";
        case WaitClass::kCrossCore: return "CrossCore";
        case WaitClass::kScheduler: return "Scheduler";
        case WaitClass::kResource: return "Resource";
        case WaitClass::kNetwork: return "Network";
        case WaitClass::kApplication: return "Application";
        case WaitClass::kIdle: return "Idle";
        case WaitClass::kConcurrency: return "Concurrency";
        case WaitClass::kCount: break;
    }
    return {};
}

// `timed`: a `WaitScope` measures the span. `counter`: a fail-fast conflict
// that did not wait (WD10) - its count rises and its time is 0.
enum class WaitKind : std::uint8_t { kTimed, kCounter };

// The index into `kWaitEvents`. 0 is ON_CPU.
enum class WaitEvent : std::uint16_t {
    kOnCpu = 0,
    kCommitWalFlush,
    kIoPageRead,
    kIoVarHeapRead,
    kIoScanRingRead,
    kSysioWriteback,
    kSysioCheckpoint,
    kSysioWalWrite,
    kXcoreStepResponse,
    kXcoreCredit,
    kSchedRunQueue,
    kResEvictionRetry,
    kNetSendBackpressure,
    kAppLockConflict,
    kAppAssertionReservation,
    kIdleClient,
    kIdleBackground,
    // **Temporary** (CJ-S WE02..WE04): a suspension no stage has classified yet.
    // `CoroTask::Poll` books an unscoped park here and counts it as an I2
    // violation; WE04 deletes the event and with it the last such site.
    kSchedUnclassified,
    kCount,
};

inline constexpr std::size_t kWaitEventCount = static_cast<std::size_t>(WaitEvent::kCount);

struct WaitEventDef {
    WaitEvent id;
    std::string_view name;  // `class.event`
    WaitClass wait_class;
    WaitKind kind;
    // What p1..p3 mean, for `sys.wait_event_names`; empty when unused.
    std::string_view p1;
    std::string_view p2;
    std::string_view p3;
    // Whether the span counts toward DB time (I3): false only for `idle.*`.
    bool in_db_time;
};

inline constexpr std::array<WaitEventDef, kWaitEventCount> kWaitEvents = {{
    {WaitEvent::kOnCpu, "cpu.on_cpu", WaitClass::kCpu, WaitKind::kTimed, "", "", "", true},
    {WaitEvent::kCommitWalFlush, "commit.wal_flush", WaitClass::kCommit, WaitKind::kTimed,
     "target_lsn", "durability_class", "group_size", true},
    {WaitEvent::kIoPageRead, "io.page_read", WaitClass::kUserIo, WaitKind::kTimed, "page_id",
     "page_class", "core", true},
    {WaitEvent::kIoVarHeapRead, "io.var_heap_read", WaitClass::kUserIo, WaitKind::kTimed,
     "page_id", "rel_oid", "", true},
    {WaitEvent::kIoScanRingRead, "io.scan_ring_read", WaitClass::kUserIo, WaitKind::kTimed,
     "page_id", "rel_oid", "", true},
    {WaitEvent::kSysioWriteback, "sysio.writeback", WaitClass::kSystemIo, WaitKind::kTimed,
     "page_id", "bytes", "", true},
    {WaitEvent::kSysioCheckpoint, "sysio.checkpoint", WaitClass::kSystemIo, WaitKind::kTimed,
     "lsn", "bytes", "", true},
    {WaitEvent::kSysioWalWrite, "sysio.wal_write", WaitClass::kSystemIo, WaitKind::kTimed, "lsn",
     "bytes", "", true},
    {WaitEvent::kXcoreStepResponse, "xcore.step_response", WaitClass::kCrossCore,
     WaitKind::kTimed, "target_core", "step_id", "", true},
    {WaitEvent::kXcoreCredit, "xcore.credit", WaitClass::kCrossCore, WaitKind::kTimed,
     "target_core", "credits_left", "", true},
    {WaitEvent::kSchedRunQueue, "sched.run_queue", WaitClass::kScheduler, WaitKind::kTimed,
     "scheduling_group", "queue_length", "", true},
    {WaitEvent::kResEvictionRetry, "res.eviction_retry", WaitClass::kResource, WaitKind::kTimed,
     "retry_count", "pool_use", "", true},
    {WaitEvent::kNetSendBackpressure, "net.send_backpressure", WaitClass::kNetwork,
     WaitKind::kTimed, "session", "unsent_bytes", "", true},
    {WaitEvent::kAppLockConflict, "app.lock_conflict", WaitClass::kApplication,
     WaitKind::kCounter, "rel_oid", "pk", "holding_trx", true},
    {WaitEvent::kAppAssertionReservation, "app.assertion_reservation", WaitClass::kApplication,
     WaitKind::kCounter, "assertion_id", "group_key_hash", "", true},
    {WaitEvent::kIdleClient, "idle.client", WaitClass::kIdle, WaitKind::kTimed, "", "", "", false},
    {WaitEvent::kIdleBackground, "idle.background", WaitClass::kIdle, WaitKind::kTimed, "", "", "",
     false},
    {WaitEvent::kSchedUnclassified, "sched.unclassified", WaitClass::kScheduler, WaitKind::kTimed,
     "", "", "", true},
}};

constexpr const WaitEventDef& Def(WaitEvent e) noexcept {
    return kWaitEvents[static_cast<std::size_t>(e)];
}

// The event named `name`, or `kCount` when none is.
constexpr WaitEvent FindWaitEvent(std::string_view name) noexcept {
    for (const WaitEventDef& d : kWaitEvents) {
        if (d.name == name) return d.id;
    }
    return WaitEvent::kCount;
}

namespace detail {

constexpr bool RegistryIsIndexed() {
    for (std::size_t i = 0; i < kWaitEventCount; ++i) {
        if (static_cast<std::size_t>(kWaitEvents[i].id) != i) return false;
    }
    return true;
}

// `name` is `<class prefix>.<event>` with a non-empty event, for the entry's class.
constexpr bool NamesMatchClasses() {
    for (const WaitEventDef& d : kWaitEvents) {
        const std::string_view prefix = WaitClassPrefix(d.wait_class);
        if (prefix.empty() || d.name.size() <= prefix.size() + 1) return false;
        if (d.name.substr(0, prefix.size()) != prefix || d.name[prefix.size()] != '.') return false;
    }
    return true;
}

constexpr bool NamesAreUnique() {
    for (std::size_t i = 0; i < kWaitEventCount; ++i) {
        for (std::size_t j = i + 1; j < kWaitEventCount; ++j) {
            if (kWaitEvents[i].name == kWaitEvents[j].name) return false;
        }
    }
    return true;
}

// Only `idle.*` leaves DB time, and every idle event does (I3).
constexpr bool OnlyIdleLeavesDbTime() {
    for (const WaitEventDef& d : kWaitEvents) {
        if (d.in_db_time == (d.wait_class == WaitClass::kIdle)) return false;
    }
    return true;
}

}  // namespace detail

static_assert(detail::RegistryIsIndexed(), "a wait event's id must be its index in kWaitEvents");
static_assert(detail::NamesMatchClasses(), "a wait event's name must be `<class prefix>.<event>`");
static_assert(detail::NamesAreUnique(), "two wait events share a name");
static_assert(detail::OnlyIdleLeavesDbTime(), "DB time excludes exactly the idle class (I3)");
static_assert(kWaitEvents[0].id == WaitEvent::kOnCpu, "id 0 is the reserved ON_CPU");

}  // namespace kds::stats
