#pragma once

#include <cstdint>
#include <map>
#include <string_view>

#include "kds/base/log.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/catalog/oid.hpp"
#include "kds/exec/range_eligible.hpp"
#include "kds/exec/assertion_check.hpp"
#include "kds/server/refusal_counters.hpp"
#include "kds/server/row_id_lease_service.hpp"
#include "kds/storage/device_page_store.hpp"
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
// The steps themselves are CC10's and are not restated here; what is
// local is which core performs each, and `OpenRangeOnSystemCore`'s body
// carries that against the code that does it.
//
// ---- The size, which is one quantity and not two --------------------
//
// `workplan-range-directory.md`'s **D6** (§11) takes range = **lease
// grant** on a mechanism rather than on a table: R4's tail-insert spreading is id-block-aligned and the
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
// row land without RD6. RD6 raises the default; RD9(b) sweeps it around
// `kRowIdLeasePerGrant` and the operator takes the final value on those
// numbers.

namespace kds::server {

// `range_size_ids = 0`: no range is ever opened, and the row-id lease
// asks for `kRowIdLeasePerGrant` as it always has. The off-switch and the
// size are one key for the reason the header gives.
//
// The *on* value has no constant here on purpose. RD9(b) sweeps it around
// `kRowIdLeasePerGrant`, and a named default with no reader is the same
// structurally-0 surface C3's counters were held back from landing as -
// RD6 introduces it with the caller that raises the default.
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
// line beside it fires once per pair and this carries the volume.
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

    // Returns whether this `(relation, gate)` pair is being seen for the
    // **first time**, which is what the per-event log line rides. A
    // permanently gated relation is every indexed one, so a line per lease
    // refill would be `log.hpp`'s synchronous `write()` once per block,
    // forever, on the insert-adjacent path; the counter carries the
    // volume. First-seen and not "the gate changed": it bounds the lines
    // at relations × gates for the life of the process, where a
    // change-based rule is unbounded under an index created and dropped in
    // a loop.
    bool Record(catalog::Oid rel_oid, exec::RangeGate gate) {
        const Key key{rel_oid, gate};
        const bool first = counts_.CountFor(key) == 0;
        counts_.Add(key);
        return first;
    }

    std::uint64_t CountFor(catalog::Oid rel_oid, exec::RangeGate gate) const {
        return counts_.CountFor(Key{rel_oid, gate});
    }

    std::uint64_t total() const noexcept { return counts_.total(); }

    const std::map<Key, std::uint64_t>& counts() const noexcept { return counts_.counts(); }

private:
    RefusalCounters<Key> counts_;
};

// The decline's per-event line, in one place because it was written four
// times with the same shape. `why` distinguishes the sites the gate name
// cannot: which core asked, and whether the answer came from the cached
// catalog, the durable `sys.assertions` row, or the namespace.
void LogRangeDecline(Logger* log, std::uint32_t core_id, catalog::Oid rel_oid,
                     exec::RangeGate gate, std::string_view why);

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
// The store is the **concrete** one, not the `PageStore` seam, and that is
// the flush: core 0 formats the head in its own frame and every core has
// its own store over the shared device, so `FlushPages`/`EvictClean` -
// which live only here - are what make the granted page readable by its
// new owner and unwritable by its old one (CC7's flush-then-grant).
StatusOr<PageId> OpenRangeOnSystemCore(catalog::Catalog& catalog,
                                       storage::DevicePageStore& store, wal::WalManager* wal,
                                       const exec::AssertionEnforcer& enforcer,
                                       catalog::Oid rel_oid, std::uint64_t lo,
                                       std::uint32_t owner_core, Logger* log);

}  // namespace kds::server
