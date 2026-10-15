#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "kds/base/latch.hpp"
#include "kds/catalog/oid.hpp"
#include "kds/sched/clock.hpp"
#include "kds/txn/lock_table.hpp"

// **`sys.lock_waits`' rows: every lock wait that parked, one row each**
// (`instructions/v3.0.0/workorder-bo-sys-statistics-show-sugar.md` §3.3,
// BO-S3). Memory-resident (BO-Q5 (b)): emptied by a restart and by a
// restore, never a catalog page.
//
// ---- Where the rows live, and the latch protocol ---------------------------
//
// One part per core, each a ring of `kRingCapacity` rows under a latch of
// its own (`LatchKind::kStatsRing`). The core whose statement waited is the
// only writer of its part, so its latch is uncontended but for a reader; a
// reader on any core takes each part's latch in turn and copies its rows
// (§3's "Where the rows live"), so no core reads another's words
// unguarded and no cross-core write is added to a wait. A part allocates its
// ring on its core's first wait: a core that never waits costs nothing.
//
// **A write never fails, delays or reorders the wait it describes.** A full
// ring overwrites its oldest row and counts it; a ring that cannot be
// allocated drops the row and counts that. Both counts are `sys.meta` fields
// (SV3). Nothing a row says changes a result: a wrong or lost row costs a
// reader's insight, never an answer.
//
// ---- What is a wait here ---------------------------------------------------
//
// A statement parked on a lock unit's wake slot (`AwaitWriteBlock`,
// `AwaitRelationLock`), from the park to its end, and a waiter refused at the
// edge it would have parked on because it closed a cycle (`deadlock`, waited
// 0). Not a wait here: a path that polls a holder's decide with no slot
// behind it (`known-gaps.md`, Locks), and a slot already flipped before the
// park - a re-run, not a wait.

namespace kds::stats {

enum class LockWaitOutcome : std::uint8_t { kGranted, kDeadlock, kFaultNet, kCancelled };

const char* LockWaitOutcomeName(LockWaitOutcome outcome) noexcept;

// **`unit`, `rel_oid` and `key` name the unit that refused** - the one the
// wait is registered on - and `mode` is what the waiter asked. Since AY-S2
// the two can differ: a row's `X` refused by a range fence is a `range` row
// whose `mode` is the row's `X` (`lock_table.hpp`, `AcquireResult::slot_key`).
struct LockWaitRecord {
    std::uint64_t seq = 0;         // the core's own count of rows, from 1
    std::int64_t started_us = 0;   // UTC epoch microseconds (a TIMESTAMP)
    std::uint64_t waited_us = 0;
    catalog::Oid rel_oid = 0;
    std::uint64_t key = 0;         // the unit's low bound; 0 for a relation
    std::uint64_t waiter_trx = 0;  // 0: a waiter holding no transaction
    std::uint64_t holder_trx = 0;
    std::uint32_t core = 0;
    txn::LockUnit unit = txn::LockUnit::kRelation;
    txn::LockMode mode = txn::LockMode::kExclusive;
    // `granted` means the slot flipped, not that the unit was granted: a
    // wake is not a grant (`lock_table.hpp`), and a re-run that parks again
    // writes a row of its own.
    LockWaitOutcome outcome = LockWaitOutcome::kGranted;
};
static_assert(sizeof(LockWaitRecord) == 64, "a ring of 4096 is 256 KiB per waiting core");

class LockWaitLog {
public:
    static constexpr std::size_t kRingCapacity = 4096;  // BO-Q7 (a), per core
    static constexpr std::uint32_t kParts = 64;         // `kMaxCores` (superblock.hpp)

    // Appends `row` to `row.core`'s part, stamping its `seq`.
    static void Record(LockWaitRecord row) noexcept;

    // Every part's rows, each copied under its part's latch: by core, then
    // oldest first.
    static std::vector<LockWaitRecord> Snapshot();

    struct Totals {
        std::uint64_t recorded = 0;     // rows written, overwritten ones included
        std::uint64_t overwritten = 0;  // rows a full ring dropped for a newer one
        std::uint64_t dropped = 0;      // rows no ring could take
    };
    static Totals Read() noexcept;

    // Empties every part; a test's alone.
    static void ResetForTest() noexcept;

private:
    struct Part {
        Latch latch{LatchKind::kStatsRing};
        std::vector<LockWaitRecord> ring;  // grows to `kRingCapacity`, then wraps
        std::size_t next = 0;              // the slot the next row takes once full
        std::uint64_t recorded = 0;
        std::uint64_t overwritten = 0;
        std::uint64_t dropped = 0;
    };
    static std::array<Part, kParts>& parts() noexcept;
};

// **One wait's row, written when the wait ends however it ends** - and as
// `cancelled` by the destructor when it never does, which is a coroutine
// destroyed at its suspend point (a session dropped mid-wait). It reaches
// the process-wide store and `clock`, so its contract is the scheduler's:
// the clock outlives every scheduler that can hold a waiting frame
// (`expeditor.hpp`; peers borrow it). A frame destroyed after `main`
// returns would meet a destroyed store - no scheduler is static, so none is.
class PendingLockWait {
public:
    // `clock` times the wait (monotonic); `started_us` is the wall instant.
    PendingLockWait(const sched::Clock* clock, LockWaitRecord row) noexcept;
    PendingLockWait(const PendingLockWait&) = delete;
    PendingLockWait& operator=(const PendingLockWait&) = delete;
    ~PendingLockWait() { End(LockWaitOutcome::kCancelled); }

    void End(LockWaitOutcome outcome) noexcept;

private:
    const sched::Clock* clock_;
    sched::MonoTimeNs start_ns_;
    LockWaitRecord row_;
    bool ended_ = false;
};

}  // namespace kds::stats
