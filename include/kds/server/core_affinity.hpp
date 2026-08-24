#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/oid.hpp"

// Which core may run a statement, and what happens when the answer is "not
// this one" (docs/crosscore.md CC3 and §6, workplan-crosscore.md P4).
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
//   (`docs/sched.md` §3 and §10, CLAUDE.md), and rewriting the executor into
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
// Retryable, and shaped like the first-updater-wins abort `docs/txn.md`
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
// (docs/workplan-peer-writer.md PW4).
//
// A peer's catalog is read-only by construction (M5: the catalog pages
// have one writer, core 0), so a CREATE/ALTER/DROP dispatched there has no
// sound outcome. In a **Debug** build it reaches `MayWrite` and fails
// naming a page id - the exact failure this header's read refusal exists
// to prevent, one statement class over. In a **release** build there is no
// such check at all (`DevicePageStore::ResidentBytes` guards it with
// `#ifndef NDEBUG`), so the statement would dirty a catalog page this core
// does not own and become a second writer of a single-writer page.
// Refused at dispatch instead, before any handler runs.
//
// `Unsupported` and not retryable, like the read refusal: retrying on the
// same connection changes nothing. The message says where DDL does run,
// because the operator's next question is always the same one.
//
// This refusal is also load-bearing for §5d: the delete-mark purge's
// soundness argument assumes a peer takes no DDL, and this is what
// enforces it (command_dispatcher.cpp's purge gate cites it).
Status PeerDdlRefused(std::uint32_t this_core, std::string_view verb);

// The PW1c interim guard (workplan-peer-writer.md §8): a peer refuses
// every DML write by name until the write handoff lands. A rotated
// relation's creation pages are formatted and logged by core 0, so a peer
// write to them is a cross-stream transition the ratified PL contract
// forbids without a handoff — and the store's MayWrite enforcement is
// Debug-only, so without this dispatch guard a release-build peer INSERT
// silently dirtied core 0's page (docs/known-gaps.md's two-writer route).
// Removed by PW1c-5 when exact-page write grants exist.
Status PeerWriteRefused(std::uint32_t this_core, std::string_view verb);

}  // namespace kds::server
