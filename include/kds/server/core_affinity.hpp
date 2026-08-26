#pragma once

#include <cstdint>
#include <map>
#include <optional>
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
        ++counts_[Key{home_core, target_core, rel_oid}];
    }

    std::uint64_t CountFor(std::uint32_t home_core, std::uint32_t target_core,
                            catalog::Oid rel_oid) const {
        auto it = counts_.find(Key{home_core, target_core, rel_oid});
        return it == counts_.end() ? 0 : it->second;
    }

    std::uint64_t total() const noexcept {
        std::uint64_t n = 0;
        for (const auto& [key, count] : counts_) n += count;
        return n;
    }

    // Ordered, so a report of these is stable run to run - the same
    // determinism rule sched.md §8 applies to anything observable.
    const std::map<Key, std::uint64_t>& counts() const noexcept { return counts_; }

private:
    std::map<Key, std::uint64_t> counts_;
};

// The refusal a cross-core **write** gets.
//
// Retryable, and shaped like the first-updater-wins abort `docs/spec/txn.md`
// already defines, because it is the same thing from the client's side: the
// transaction cannot proceed and re-running it may work. A client that
// already retries on `TXN_CONFLICT` needs no new code.
Status CrossCoreWriteRefused(std::uint32_t home_core, std::uint32_t target_core,
                             std::string_view relation);

// The refusal a **read** spanning cores gets, until the pipeline exists.
//
// `Unsupported`, deliberately not retryable: retrying changes nothing, and
// telling a client to retry a statement that can never run here would be a
// lie that costs it a loop. The message names the relation and both cores,
// because the operator's next question is always "so where should it run?".
Status CrossCoreReadUnsupported(std::uint32_t this_core, std::uint32_t target_core,
                                std::string_view relation);

// The refusal every DDL verb gets on a non-system core
// (docs/inflight/in-progress/workplan-peer-writer.md PW4).
//
// A peer's catalog is read-only by construction (M5: the catalog pages
// have one writer, core 0), so a CREATE/ALTER/DROP dispatched there has no
// sound outcome. Since PW1c-5 the store's MayWrite is enforced for leased
// stores in **every** build, so an unguarded DDL would no longer corrupt -
// it would die mid-handler naming a page id. This refusal still earns its
// place for what that failure is not: it fires before any handler runs,
// names DDL and where DDL lives rather than a page, and leaves no
// half-executed handler state behind it.
//
// `Unsupported` and not retryable, like the read refusal: retrying on the
// same connection changes nothing. The message says where DDL does run,
// because the operator's next question is always the same one.
//
// This refusal is also load-bearing for §5d: the delete-mark purge's
// soundness argument assumes a peer takes no DDL, and this is what
// enforces it (command_dispatcher.cpp's purge gate cites it).
Status PeerDdlRefused(std::uint32_t this_core, std::string_view verb);

// The refusal a write to a relation **this core owns** gets while the
// core does not hold write rights over the relation's creation pages
// (docs/inflight/in-progress/workplan-peer-writer.md PW1c-7).
//
// Every grant is memory-resident, so a crash before the acquisition
// restamp, a restart, or a message lost to a full ring leaves a relation
// with an owner and no writer. The dispatcher records the demand where it
// finds it (CheckWriteAffinity's rights probe) and the drain tick asks the
// system core to re-deliver; retryable, because that request is what makes
// the retry succeed. Pages this core wrote itself need no re-delivery -
// their stamp claims them (device_page_store.hpp, MayWrite) - so this is
// reached only for creation pages core 0 formatted and this core never
// acquired.
Status RelationWriteRightsPending(std::uint32_t this_core, std::string_view relation);

// The relations a core found itself unable to write (PW1c-7): recorded by
// the dispatcher's rights probe where it refuses, drained one per request
// by CoreRuntime's drain tick (relation_grant_service.hpp is the ring
// half). Here rather than beside the ring functions because this is the
// dispatcher's whole dependency on the path - a sink - and pulling the
// scheduler and transport headers into command_dispatcher.hpp for it would
// tax every translation unit that includes the dispatcher. Unique per
// tick, so a client hammering one relation costs one request per cadence,
// not one per statement.
class RelationGrantDemand {
public:
    void Record(catalog::Oid table_oid);
    // The oldest pending relation, removed - the tick asks for one at a
    // time (one request in flight per core).
    std::optional<catalog::Oid> Pop();
    bool empty() const noexcept { return pending_.empty(); }

private:
    std::vector<catalog::Oid> pending_;
};

// The refusal a write gets on the owner of a relation whose index is being
// built there, or built and not yet published by core 0's commit
// (docs/inflight/in-progress/workplan-peer-writer.md §7c, PW1c-6b-2). Retryable: the window
// closes when core 0 says `done`, and the retry then writes - and it must
// close, because a row written inside it would be in nobody's index
// (index_build_service.hpp says why).
Status IndexBuildPending(std::uint32_t this_core, std::string_view relation);

// The index builds a core is running or has built and not yet heard `done`
// for (PW1c-6b-2). Opened, closed and expired by the index build service
// (the ring half); asked by the dispatcher's write gate. Here for
// RelationGrantDemand's reason: this is the dispatcher's whole dependency
// on the path. Times are `sched::MonoTimeNs`, spelled as the integer they
// are so this header pulls no scheduler header in.
class PendingIndexBuilds {
public:
    struct Entry {
        catalog::Oid table_oid;
        std::uint64_t index_oid;
        std::uint64_t opened_at_ns;
    };

    void Open(catalog::Oid table_oid, std::uint64_t index_oid, std::uint64_t now_ns);
    // Closes the window `index_oid` names; false when none was open.
    bool Close(std::uint64_t index_oid);
    bool Covers(catalog::Oid table_oid) const noexcept;
    // Closes and returns every window opened `ceiling_ns` or more ago.
    std::vector<Entry> Expire(std::uint64_t now_ns, std::uint64_t ceiling_ns);
    bool empty() const noexcept { return entries_.empty(); }
    std::size_t size() const noexcept { return entries_.size(); }
    // Open windows, oldest first. `SHOW META` reads their ages.
    const std::vector<Entry>& entries() const noexcept { return entries_; }

private:
    std::vector<Entry> entries_;
};

}  // namespace kds::server
