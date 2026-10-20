#pragma once

#include <cstdint>
#include <vector>

#include "kds/catalog/oid.hpp"
#include "kds/sched/clock.hpp"
#include "kds/stats/per_core_ring.hpp"
#include "kds/txn/lock_table.hpp"

// **`sys.lock_waits`' rows: every lock wait that parked, one row each**
// (`instructions/v3.0.0/workorder-bo-sys-statistics-show-sugar.md` §3.3,
// BO-S3). Memory-resident (BO-Q5 (b)): emptied by a restart and by a
// restore, never a catalog page.
//
// ---- Where the rows live, and the latch protocol ---------------------------
//
// A ring of `kRingCapacity` rows per core, `PerCoreRing`'s protocol
// (`stats/per_core_ring.hpp`): the core whose statement waited is the only
// writer of its part, a reader copies each part under its latch, and a write
// never fails, delays or reorders the wait it describes. Nothing a row says
// changes a result: a wrong or lost row costs a reader's insight, never an
// answer.
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
    using Ring = PerCoreRing<LockWaitRecord, kRingCapacity>;

    // Appends `row` to `row.core`'s part, stamping its `seq`.
    static void Record(LockWaitRecord row) noexcept { ring().Record(row.core, row); }

    // Every part's rows: by core, then oldest first.
    static std::vector<LockWaitRecord> Snapshot() { return ring().Snapshot(); }

    using Totals = Ring::Totals;
    static Totals Read() noexcept { return ring().Read(); }

    // Empties every part; a test's alone.
    static void ResetForTest() noexcept { ring().Reset(); }

private:
    static Ring& ring() noexcept;
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
