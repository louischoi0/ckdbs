#pragma once

#include <cstdint>
#include <map>
#include <string_view>

#include "kds/base/log.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/catalog/oid.hpp"
#include "kds/exec/range_eligible.hpp"
#include "kds/exec/assertion_check.hpp"
#include "kds/server/row_id_lease_service.hpp"
#include "kds/wal/manager.hpp"

// RD5 — where a second range comes from, and where a refusal to open one
// is read (`docs/spec/crosscore.md` CC9, CC10, §6a, §6b;
// `docs/inflight/in-progress/workplan-range-directory.md` RD5, §9b and
// §9e; work order `instructions/v2.5.0/range-directory.md` RB2).
//
// ---- CD2: where allocation runs, and why it is forced ----------------
//
// The workplan's §2b left the site open between the drain tick
// (`core_runtime.cpp`'s `MaybeRefillRowIds`, a `kSystem` task outside any
// statement's borrow) and the point of demand (`RowIdLeaseTable::Next`,
// inside a running INSERT). **It is the tick, and neither taste nor
// preference decides it - two independent facts each rule the other site
// out.**
//
//   1. **Only core 0 may write a catalog page** (M5), and `sys.ranges` is
//      a catalog relation. The core that discovers the demand is the
//      relation's owner, which is a peer; it cannot write the row, and
//      `Next()` is inside an INSERT that cannot await a round trip. The
//      demand site is therefore not merely undesirable, it is
//      unavailable.
//   2. **Publication is a version bump.** `Catalog::InsertRangeRow` ends
//      in `BumpVersion`, which drops the whole catalog cache - and since
//      RD3 that cache entry owns `TableAccess::ranges`, the storage a
//      resolved range set spans. At the point of demand the running
//      INSERT is holding a `const TableAccess*`; on the tick nothing is.
//      That is `key_order`'s first form exactly (catalog.cpp's note at
//      the flip), and it produced a wrong answer rather than a crash.
//
// **The failure mode is a refusal, which is the property the order asked
// the choice to be made on.** A declined gate, a failed re-check or a
// failed row write means *no range opens and the grant still goes out*:
// the relation stays one range, and the owner keeps inserting into the
// one chain exactly as before. Nothing half-opens, and no statement sees
// a boundary that is not durable. A carve that genuinely cannot be made
// already answers a zero-count grant, which the lease turns into a
// non-retryable refusal.
//
// So the sequence is the one CC10 states, with its steps landing on the
// two cores that can perform them:
//
//   owner core, on the tick   ask `RangeEligible` (authoritative here and
//                             nowhere else - §9c: the fifth gate's
//                             registry is core-local), then request a
//                             lease block *and* a range at its first id
//   core 0, grant handler     re-check what its own catalog can see
//                             (§9b's two admission windows), carve the
//                             block, format the new range's head page,
//                             log the handoff (CC10 step 2), write the
//                             directory rows in one transaction (step 3,
//                             durable before any grant), reply (step 4)
//   owner core, on the reply  admit the page, apply the lease; the
//                             version bump's broadcast is step 5
//
// ---- The size, which is one quantity and not two --------------------
//
// D6 (workplan §11) takes range = **lease grant** on a mechanism rather
// than on a table: R4's tail-insert spreading is id-block-aligned and the
// block is the grant, so a core inserting from its own lease stays inside
// one range by construction. That makes the range size and the grant size
// the same number, and this file spells it once - `range_size_ids`, the
// config key, defaulting to `kRangeSizeOff`. A second key for "how big is
// a range" would be a second name for a quantity the grant already
// expresses, and the two could then disagree at exactly the boundary D6
// exists to keep them agreeing at.
//
// **The default is off, and that is forced too.** RD6 is what makes a
// range its own chain; until it lands, a directory row would describe a
// partition no insert or read honours. So `range_size_ids = 0` means no
// range ever opens, every relation stays the one range CC8 says it starts
// as, and the engine behaves exactly as it did - which is what lets this
// row land without RD6. RD6 raises the default to
// `kDefaultRangeSizeIds`; RD9(b) sweeps it and the operator takes the
// final value on those numbers.

namespace kds::server {

// A range is one row-id lease grant (D6), so the default size is the
// grant's - `kRowIdLeasePerGrant`, itself the measured K-M2 floor below
// which the durable `next_id` bump stops amortizing. Derived, not chosen:
// naming a separate number here is what §2a's "never a literal at a call
// site" forbids, one level up.
inline constexpr std::uint64_t kDefaultRangeSizeIds = kRowIdLeasePerGrant;

// `range_size_ids = 0`: no range is ever opened, and the row-id lease
// asks for `kRowIdLeasePerGrant` as it always has. The off-switch and the
// size are one key for the reason the header gives.
inline constexpr std::uint64_t kRangeSizeOff = 0;

// Counts declined range openings by `(relation, gate)` — C3's decision
// (workplan §9e), in `crosscore.md` §6's refusal-counter form.
//
// **Metrics, not stored state**, and the distinction is the whole reason
// this is a counter rather than a `SHOW` field: eligibility is not
// cacheable state. The fifth gate's fact lives in the owner core's
// registry and moves with no version bump, so a displayed "is this
// relation splittable" bit is stale by construction (§9e). A decision
// *taken*, recorded where and when it was authoritative, is truthful
// where a state field cannot be.
//
// The reading it exists for is aggregate: **which gate declines how often
// on which relation is the evidence for which owning decision to lift
// first** — the index (`index.md` §13), the Cabin (`cabin.md` §11), the
// var-heap partition, FK placement, or assertion placement. An
// unaggregated log line cannot be that input, which is why the per-event
// line beside it is bounded to transitions and this carries the volume.
//
// Written on the **owner core**, which is the only core whose answer is
// authoritative (§9c), and read per core through `SHOW META`.
class RangeSplitDeclineCounters {
public:
    struct Key {
        catalog::Oid rel_oid;
        exec::RangeGate gate;

        bool operator<(const Key& other) const noexcept {
            if (rel_oid != other.rel_oid) return rel_oid < other.rel_oid;
            return gate < other.gate;
        }
    };

    // Returns whether this is a **transition** - the first decline for the
    // relation, or a change of the gate that declines it. The per-event log
    // line rides that answer rather than the call, because a permanently
    // gated relation (any indexed one) would otherwise pay `log.hpp`'s
    // synchronous `write()` once per lease refill, forever, on the
    // insert-adjacent path. The counter carries the per-ask volume.
    bool Record(catalog::Oid rel_oid, exec::RangeGate gate) {
        ++counts_[Key{rel_oid, gate}];
        auto it = last_gate_.find(rel_oid);
        if (it == last_gate_.end()) {
            last_gate_.emplace(rel_oid, gate);
            return true;
        }
        if (it->second == gate) return false;
        it->second = gate;
        return true;
    }

    std::uint64_t CountFor(catalog::Oid rel_oid, exec::RangeGate gate) const {
        auto it = counts_.find(Key{rel_oid, gate});
        return it == counts_.end() ? 0 : it->second;
    }

    std::uint64_t total() const noexcept {
        std::uint64_t n = 0;
        for (const auto& [key, count] : counts_) n += count;
        return n;
    }

    // Ordered, so a report of these is stable run to run - sched.md §8's
    // determinism rule for anything observable.
    const std::map<Key, std::uint64_t>& counts() const noexcept { return counts_; }

private:
    std::map<Key, std::uint64_t> counts_;
    // The last gate named for a relation, for `Record`'s transition
    // answer. Separate from the key above because the question is "what
    // does this relation decline on *now*", which the per-gate counts
    // cannot answer without knowing which was most recent.
    std::map<catalog::Oid, exec::RangeGate> last_gate_;
};

// Core 0's half: the re-check `crosscore.md` CC10 step 3 requires, then
// `Catalog::OpenRange`, then the durable handoff of the new head page to
// `owner_core` (step 2's record, ordered last so one `EnsureDurable`
// covers the rows it follows).
//
// Returns the head page, or **`kInvalidPageId` with an OK status when the
// re-check declined** — a decline is an answer, not a failure, and the
// caller replies with the ids and no range. An error is reserved for a
// catalog or device failure, where the caller replies with the ids and no
// range too: nothing half-opens either way.
//
// **Why core 0 re-checks at all when the owner already asked** (§9b's two
// admission windows): an index build's catalog half or an assertion can
// land between the owner's ask and this row. Both are core-0 catalog
// writes and so is this one, so core 0's single stream is the
// serialization point — whichever lands second loses. This is that
// re-check for the order "auxiliary first"; the converse gates (an
// auxiliary DDL declining on an already-split relation) are the other
// order, and the pair is what closes the race rather than either alone.
//
// The **fifth gate is asked here against the durable rows, not the
// registry**, and the difference is the point: core 0's
// `AssertionEnforcer` holds nothing for a peer-owned relation
// (`mount_recovery.cpp` counts it foreign and adopts neither record), so
// asking it would answer "eligible" for exactly the relation whose
// assertion should decline it. `sys.assertions` is authoritative on core
// 0 by construction.
StatusOr<PageId> OpenRangeOnSystemCore(catalog::Catalog& catalog, storage::PageStore& store,
                                       wal::WalManager* wal,
                                       const exec::AssertionEnforcer& enforcer,
                                       catalog::Oid rel_oid, std::uint64_t lo,
                                       std::uint32_t owner_core, Logger* log);

}  // namespace kds::server
