#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "kds/sched/wait_state.hpp"
#include "kds/stats/per_core_ring.hpp"

// **How transactions end, and how long explicit ones live**
// (`instructions/v3.2.0/workorder-df-statistics-coverage.md` §2 part B,
// DF-Q13 (a)): `sys.meta`'s `txn` counts and `sys.txn_histogram`'s rows.
// Memory-resident (DF-R1): emptied by a restart and by a restore. No switch
// (DF-R9).
//
// ---- What is counted --------------------------------------------------------
//
// Six ends of a read-write transaction, each counted where its cause is
// known, so the six partition the transactions a core decided:
//
//   kCommit           the commit is published (`TransactionManager::
//                     FinishCommitBody`): autocommit and explicit alike, a
//                     deferred `strict` commit once its record is durable.
//   kRollback         the client's `ROLLBACK`, and a load stream's abort.
//   kCeilingRollback  the listener's sweep, at `txn_lifetime_ceiling_ms`.
//   kCloseRollback    a connection that went away with a transaction open,
//                     and a load listener detaching with a load in progress.
//   kAbort            the engine rolled it back because a statement or a
//                     commit failed: an autocommit statement's, an autocommit
//                     or explicit `COMMIT`'s, a deferred `strict` commit's
//                     whose sync never came, a load stream's failed chunk or
//                     a frame not legal inside it.
//   kWaitAbort        an autocommit write gave its transaction up before it
//                     had written a row, to wait - for a row's writer, for a
//                     relation's lock - or to run again after a stale
//                     descent. Counted when it is given up, whether the
//                     statement then runs again or is refused: one client
//                     statement can make several.
//
// **Not counted**: a `READ ONLY` transaction, which decides nothing (its
// `COMMIT` and its `ROLLBACK` drop a lease); the lock-holder transaction a
// declaration's build takes and always rolls back
// (`CommandDispatcher::DdlBuildLock`); a mount's losers (`sys.meta`'s
// `recovery_rolled_back`).
//
// The life of an explicit transaction is the head of its `BEGIN` to the
// instant its end is handled - a deferred `strict` commit's wait for its
// sync is after that - in `sys.wait_histogram`'s buckets, a `READ ONLY`
// transaction's apart from a read-write one's. An autocommit statement's
// transaction is not in it: `sys.statement_histogram` holds its statement.
//
// ---- Where the counts live, and the protocol --------------------------------
//
// A fixed part per core, no latch and no allocation, as `RefusalStats`: the
// core that decided is the only writer of its part, with relaxed
// single-writer words (`sched::RelaxedU64`), and a reader on any core reads
// them unguarded. **One writer a part is the caller's premise**: two threads
// recording as one core lose counts and nothing else. Nothing in the engine
// reads them (DF-R3).

namespace kds::stats {

enum class TxnEnd : std::uint8_t {
    kCommit = 0,
    kRollback,
    kCeilingRollback,
    kCloseRollback,
    kAbort,
    kWaitAbort,
};
inline constexpr std::size_t kTxnEndCount = static_cast<std::size_t>(TxnEnd::kWaitAbort) + 1;

// The field `sys.meta` shows the count under.
const char* TxnEndFieldName(TxnEnd end) noexcept;

struct TxnLifeBucket {
    std::uint32_t core = 0;
    bool read_only = false;
    std::uint32_t bucket = 0;  // `sched::WaitBucketOf`'s
    std::uint64_t count = 0;
};

class TxnStats {
public:
    // One transaction of `core` ended this way.
    static void Count(std::uint32_t core, TxnEnd end) noexcept;

    // One explicit transaction of `core` ended `life_ns` after it began.
    static void RecordLife(std::uint32_t core, bool read_only, std::uint64_t life_ns) noexcept;

    // `core`'s six counts, indexed by `TxnEnd`.
    static std::array<std::uint64_t, kTxnEndCount> CountsOf(std::uint32_t core) noexcept;

    // Every non-empty bucket, ordered by (core, read-write first, bucket).
    static std::vector<TxnLifeBucket> SnapshotLives();

    // Zeroes every part; a test's alone.
    static void ResetForTest() noexcept;

private:
    struct Part {
        std::array<sched::RelaxedU64, kTxnEndCount> ends;
        // [0] read-write, [1] `READ ONLY`.
        std::array<std::array<sched::RelaxedU64, sched::kWaitHistogramBuckets>, 2> lives;
    };
    static std::array<Part, kStatsParts>& parts() noexcept;
};

}  // namespace kds::stats
