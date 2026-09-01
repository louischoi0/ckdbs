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
// config key, whose default is `kRangeSizeIdsDefault` since DA1 and whose
// off-switch is `kRangeSizeOff`. A second key for "how big is
// a range" would be a second name for a quantity the grant already
// expresses, and the two could then disagree at exactly the boundary D6
// exists to keep them agreeing at.
//
// **The default was off, and it was forced off; DA1 turned it on**
// (2026-08-31, `instructions/v2.7.0/ratification-da.md`). The forcing was
// RD6's: a range is its own chain only since RD6, and until that landed a
// directory row would have described a partition no insert or read
// honoured. RD6 landed with R3, R4 supplied the producer that makes a peer
// ask for a range at all, and R4-R/RS gave a spread relation a read
// surface from every core - so the reason is spent, and RD9(b)'s sweep
// (`bench/v2.6.0/results-k-sweep-and-read-ceiling-v2.4.0-52-g5b37fec.md`
// §6d) is the numbers the operator took the value on.
//
// `range_size_ids = 0` is still the off-switch and still means no range
// ever opens and every relation stays the one range CC8 says it starts as.
// What changed is which value ships.

namespace kds::server {

// `range_size_ids = 0`: no range is ever opened, and the row-id lease
// asks for `kRowIdLeasePerGrant` as it always has. The off-switch and the
// size are one key for the reason the header gives.
inline constexpr std::uint64_t kRangeSizeOff = 0;

// **D6's value, ratified as DA1** and shipped as the default. The sweep it
// rests on is RD9(b)'s, re-run at k = 4 with `placement = creating` and
// both durability arms in
// `bench/v2.6.0/results-k-sweep-and-read-ceiling-v2.4.0-52-g5b37fec.md`
// §6d. What the value buys and gives up, in that sweep's own numbers:
// 65,536 gives up **9% of the group arm's gain** against 4,096's optimum
// (1.339 against 1.470) and takes **16x the read ceiling**; the relaxed arm
// **prefers** it (1.031, the sweep's high), because that arm's cost is
// refill rate and a block this size is not spent fast enough to show it.
// Below 4,096 is a loss on every axis - 0.434x relaxed at 256 - so the
// value is taken from the top of the swept range rather than its middle.
//
// **This is the row-id lease grant too**, because D6 makes range size and
// grant size one quantity and `core_runtime.cpp`'s refill spends this key
// wherever it would otherwise spend `kRowIdLeasePerGrant`. So
// `kRowIdLeasePerGrant`'s 4,096 is not a competing value for the same
// question: K-M2 established it as a **floor** below which the durable
// bump stops amortizing, and 65,536 clears it.
//
// **The burn is the other side of the number, and its dominant term is not
// the mount.** DA1's own text says the cost is *"up to 65,535 ids per
// (relation, core, mount)"*; that is the idle-core and restart remainder,
// and it is the smaller half. The steady-state term is the **refill**:
// `MaybeRefillRowIds` asks at `remaining <= window / 4`, so a quarter of
// the block is still in hand when the ask goes out, and on a contended
// relation another core has carved in between - so `RowIdLeaseTable::Grant`
// takes its **replace** arm and that quarter is burnt. **Roughly a quarter
// of every block a contended relation carves, forever**, which the sweep's
// own numbers confirm: 56,018 burnt against 234,776 rows at k = 4 is 23.9%,
// where the mount-remainder model predicts 4 x 4,095 = 16,380.
//
// **The ratio is size-invariant, which is why DA1 does not make it worse.**
// At 65,536 each burn event is 16x larger and 16x rarer; a contended
// relation spends ~25% of its issued ids either way, and an uncontended one
// extends contiguously and burns nothing. Against a 40-bit space that is
// affordable at both sizes, and it is a fraction of ids *issued* rather
// than a per-mount constant.
//
// **What this value does not arm on its own.** A single-core instance opens
// no range at any size, and the reason is structural rather than the
// suppression below: `expeditor.cpp` gates the whole peer fan-out - the
// ring, every `CoreRuntime`, and the row-id grant handler - on `cores > 1`,
// so at the `cores` default nothing holds a lease and `OpenRangeOnSystemCore`
// has no caller at all. This value then sizes nothing but a lease block that
// is never asked for. The suppression is what bounds the *contended* case
// (§6's HK4): a relation only one peer ever writes settles at two ranges.
//
// **This is a size, and since the 2026-08-31 operator amendment it is no
// longer a default.** The amendment makes insert spreading a per-relation
// option the user decides, default **off**, so `Expeditor::Config` ships
// `kRangeSizeOff` and this value is what a range measures **once
// something has asked for one**. The name is kept rather than a second one
// minted: DA1's sweep is still the derivation of the number, and the
// number is still the lease grant (D6's "range = grant"). What moved is
// which layer answers *whether*, not *how big* - and until the relation's
// own flag exists (`expeditor.hpp` names its two gates: a `SysTableRow`
// that has no spare byte, and V11's unbuilt `WITH (...)`), the answer to
// *whether* is "no relation has asked".
inline constexpr std::uint64_t kRangeSizeIdsDefault = 65536;

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
//
// `cabins` and `discards` are SB1's: the Observational discard runs inside
// this call, after the gates and **before** the rows that publish the
// boundary, so no path exists on which a grant precedes it. Both nullable,
// and a caller that passes neither opens the range and drops nothing —
// which is every fixture and every instance running `cabins = off`.
StatusOr<PageId> OpenRangeOnSystemCore(catalog::Catalog& catalog,
                                       storage::DevicePageStore& store, wal::WalManager* wal,
                                       const exec::AssertionEnforcer& enforcer,
                                       catalog::Oid rel_oid, std::uint64_t lo,
                                       std::uint32_t owner_core, Logger* log,
                                       stats::CabinStore* cabins = nullptr,
                                       CabinSplitDiscardCounters* discards = nullptr);

}  // namespace kds::server
