#pragma once

#include <cstdint>
#include <map>

#include "kds/server/refusal_counters.hpp"
#include <string>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/oid.hpp"

// Which core may run a statement, and what happens when the answer is "not
// this one" (docs/spec/crosscore.md CC3 and §6, workplan-crosscore.md P4).
//
// ---- What this is, and what it deliberately is not ----------------------
//
// It is the **restriction** half of cross-core execution: the rules that say
// a statement cannot run here. It is not the pipeline. `crosscore.md` §2's
// step pipeline - ship each step to the core owning its relation and stream
// rows back - is not built and **cannot be built** against the engine as it
// stands, for a reason worth recording where somebody will look for it:
//
//   `CommandDispatcher::Dispatch()` returns a finished reply synchronously,
//   `TcpServer` calls it inline from a read handler, and `ChainRunner` walks
//   a step chain start to finish with no suspension point anywhere in it.
//   A pipeline is an asynchronous dataflow - the session core sends
//   `STEP_OPEN` and must then *wait* for batches - so building one means
//   making the whole statement path suspendable. Task representation
//   (callbacks vs coroutines vs fibers) is an explicitly open decision
//   (`docs/spec/sched.md` §3 and §10, CLAUDE.md), and rewriting the executor into
//   a state machine would settle it by precedent, at the largest possible
//   scale, without anybody deciding it.
//
// So until that decision lands, a statement that spans cores is **refused
// with an exact reason** rather than mis-executed. That is strictly better
// than what preceded it: without this check the same statement reached the
// page store and failed with "core 1 may not fault page 129", which names a
// page id to a client that has never heard of pages.
//
// ---- The write restriction is decided, not deferred ---------------------
//
// CC3 is settled: **v1 is read-only cross-core.** A transaction's writes
// bind to one home core, and a write to another core's relation is a
// retryable error. That is not a placeholder for the pipeline - it survives
// the pipeline, because it is what keeps commit single-stream. Guideline 3
// spells out why: LSNs are stream-local and are never compared across cores,
// so a transaction whose writes landed in two streams could not be recovered
// as one. Lifting it needs 2PC, which is `[OPEN]`.

namespace kds::server {

// Counts refused cross-core writes by `(home core, target core, relation)` -
// `crosscore.md` §6's "input the future placement/2PC decision will be made
// from".
//
// **Metrics, not stored state** (§6 says so in as many words): it lives in
// memory, it is per core, and nothing reads it back to make a decision. What
// it answers is the question 2PC's design will open with - *does this
// workload actually want cross-core writes, and for which relations?* - and
// a counter that only starts when somebody remembers to enable it cannot
// answer that.
//
// ---- Two eras, one meaning (2026-08-26, SS4) ---------------------------
//
// Statement shipping converts most of what this used to count into work:
// an autocommit single-relation write is now carried to its owner and run
// there, and never reaches either `Record` call. **The semantics are
// deliberately unchanged anyway**, so the series spans the change:
//
//   - *before shipping* it counted the whole demand - every write a
//     wrong-core client issued;
//   - *after* it counts the **residue** - the writes shipping does not
//     convert, which is a statement inside an explicit transaction and a
//     statement spanning two owners.
//
// That residue is the better evidence base, not a worse one: it is exactly
// the population 2PC would address, with the population a routing layer
// already handles taken out of it. What shipping converts is counted
// separately by `ShippedStatementExecutor` and `StatementShipClient`
// (`SHOW META`'s `shipped_*` fields). A reading of this counter must say
// which era it was taken in; the field name does not.
class CrossCoreWriteCounters {
public:
    struct Key {
        std::uint32_t home_core;
        std::uint32_t target_core;
        catalog::Oid rel_oid;

        bool operator<(const Key& other) const noexcept {
            if (home_core != other.home_core) return home_core < other.home_core;
            if (target_core != other.target_core) return target_core < other.target_core;
            return rel_oid < other.rel_oid;
        }
    };

    void Record(std::uint32_t home_core, std::uint32_t target_core, catalog::Oid rel_oid) {
        counts_.Add(Key{home_core, target_core, rel_oid});
    }

    std::uint64_t CountFor(std::uint32_t home_core, std::uint32_t target_core,
                            catalog::Oid rel_oid) const {
        return counts_.CountFor(Key{home_core, target_core, rel_oid});
    }

    std::uint64_t total() const noexcept { return counts_.total(); }

    const std::map<Key, std::uint64_t>& counts() const noexcept { return counts_.counts(); }

private:
    // The tally, ordering and total live in `refusal_counters.hpp`; what
    // stays here is the key this counter is keyed by and what its numbers
    // mean. RD5's decline counters are the second instance.
    RefusalCounters<Key> counts_;
};


// The refusal a **read** spanning cores gets when the step pipeline cannot
// take it (R4-R/RS0). The pipeline itself is built and lives on every core
// since RR2 - what this refusal reports is a *shape* outside the class
// `HandleSelect`'s fan-in route serves, which the message spells out.
//
// `Unsupported`, deliberately not retryable: retrying changes nothing, and
// telling a client to retry a statement that can never run here would be a
// lie that costs it a loop. The message names the relation and both cores,
// because the operator's next question is always "so where should it run?".
Status CrossCoreReadNotImplemented(std::uint32_t this_core, std::uint32_t target_core,
                                std::string_view relation);


// `IndexBuildPending` and `PendingIndexBuilds` stood here until AT-S5e:
// the refusal and the window a write met while an index of its relation
// was being built by the relation's owner (PW1c-6b-2), turned into a wait
// by AO-S6e-a. `CREATE INDEX` takes the relation `X` a writer's `IX`
// waits on since AT-S5e, on every core, and both went with the window.

}  // namespace kds::server
