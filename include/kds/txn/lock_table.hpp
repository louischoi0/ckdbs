#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "kds/base/latch.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/oid.hpp"
#include "kds/txn/holder_progress.hpp"


// The borrow model's **lock family** (AR2 §2) - AR0 D2's lock manager.
//
// Built in AO (M2, closed 2026-09-09) under
// `instructions/v3.0.0/workorder-ao-m2-lock-family.md`, which owns the
// rulings: AO-R1 (the name), AO-R2 (the table and its order), AO-R3 (no
// persisted bit), AO-R10 (the cap), AO-R11 (modes and units).
// `docs/spec/txn.md` §5 owns the contract.
//
// **Units** relation -> range -> {slice, tuple}, AR2-R3's chain; the page
// is not in it and belongs to the latch family
// (`storage/page_latch.hpp`). A **slice** is `(rel_oid, [lo, hi))` in key
// space and never by page (AR2-R6, ratified as E4): a gap is a key
// interval, and a fence over it must outlive the btree leaf division that
// moves keys between pages.
//
// **Modes** `IS`, `IX`, `S`, `X`; no `SIX` and no update mode (E1).
//
// ---- The engine's ask, and the wait ---------------------------------------
//
// **The engine asks with `TryAcquire`**, which never queues: a conflict is
// reported with the holder it lost to and a wake registration, and the
// statement parks outside every page-latched span (`DispatchAsync`), then
// re-runs. `Acquire`, the queueing ask, has no engine caller; the cells
// drive it.
//
// **The re-check after a park is mandatory, and the slot does not replace
// it** (AO-3's finding G). A wait breaks `manager.hpp`'s premise that
// nothing suspends between reading a tuple's header and overwriting it, so
// a woken waiter asks again; the slot only says "something changed, look",
// and a slot flipped for a grant another waiter took is a wasted poll,
// never a wrong answer. The registration is made **under the latch that
// observed the refusing holder** (`AcquireResult::wake`), so it lands
// before that holder's removal, whose release flips it, or the holder was
// never seen - no lost wake without AO-R4's disjunctive predicate.
//
// **Cross-core, a wake is write-then-kick** (AU-S2, AR0-6-R1). Whoever
// flips a waiter's slot - a decide's `WakeWaiters`, a withdrawal's
// `DequeueWaiter` - kicks the core it parked on through the wake registry
// (`sched/waker_table.hpp`, installed with `SetWakeRegistry`) **after**
// releasing the partition latch. A waiter on the decider's own core is not
// kicked; from a non-reactor thread `CurrentCore()` is 0 and a core-0
// waiter goes unkicked. None is installed at `cores = 1`. A skipped or lost
// kick costs one idle block (`max_idle_block_ms`), never liveness: the
// park is level-triggered and re-polled after every block.
//
// **The caller owes the ancestor chain and nothing here enforces it.** A
// relation `X` and a tuple `X` under it are both granted by this table,
// because AR2-R3's design catches the conflict at the relation entry *by
// the writer taking `IX` there*. A caller that borrows a tuple without the
// intention chain above it gets a wrong answer quietly
// (`TheTableDoesNotInferTheAncestorChain` pins the behaviour).
//
// ---- Concurrency (docs/rules/rules.md §3's lock-table row) ----------------
//
// **One table for the instance**, created by the expeditor and handed to
// every core (`server/core_runtime.hpp`'s `Config::locks`). This comment
// is the declaration `rules.md` §3 requires of any lock.
//
// **Partitioned 64 ways at every core count** (`kLockPartitions`; BP-S6,
// BP-Q15 (a)), keyed by `(rel_oid, unit kind, lo)` so a relation's own
// entry - touched by every writer's `IX` and every positioned reader's
// `IS` - hashes apart from the units beneath it. The unit kind is in the
// key because `Relation(r)`, `Tuple(r, 0)`, `Slice(r, 0, h)` and
// `Range(r, 0, h)` all carry `lo = 0`.
//
// **A `Latch*` per partition, null at `cores = 1`** (AO-R9,
// `base/latch.hpp`'s shape).
//
// **Acquisition order.** A partition latch is taken
//
//   - with **no park under a page latch** (BB-R4's amendment of AR2-R2;
//     `txn.md` §5 says why): taken under a page latch, never the reverse;
//   - with **no other partition latch** held. Every method here takes
//     exactly one at a time, including the two that walk every partition:
//     their guard is scoped to the loop body;
//   - **never under the WAL stream latch** or the visibility window latch;
//   - **released before any park.**
//
// Only the second is structural; the other three are discipline, with no
// assertion behind them.
//
// ---- The fence probe, in place of a persisted bit (AO-R3) -----------------
//
// The operator decided against a lock bit in the Keystone byte, so a
// writer cannot learn from a page that a fence covers its key. Instead a
// striped counter per relation records how many range- and slice-unit
// fences exist, and a writer reads it on the `IX` it takes anyway; only a
// nonzero reading costs the scan `FenceCoversKey` performs.
//
// **What the counter protocol establishes, and what it does not.** A
// thread that observes an entry observes the counter that was raised
// before it, because both are published under the same partition mutex.
// It does **not** order a writer that reads zero against a fence-taker
// that has not yet incremented: the two touch different keys in different
// partitions, and their relation-level `IX` and `IS` are compatible, so
// nothing serializes them.
//
// **Both sides therefore publish first and verify second** (AO-S6b, where
// the fence gets its first taker). A grant is written into its own
// partition under that partition's latch; the latch is dropped; and only
// then does the acquirer look at the other side - a fence-taker scanning
// for tuple holders inside its interval, a tuple-taker reading the
// counter and, if it is nonzero, scanning for a fence over its key. A
// verify that finds a conflict unwinds the grant it just published and
// answers as a conflict.
//
// **Why publishing first is what closes the race**, and the ordering is
// the whole argument: suppose a fence F and a write T of a key inside it
// both proceed. T's scan of fences did not see F, so it ran before F was
// published; F's scan of tuples did not see T, so it ran before T was
// published. Each side publishes before it scans, so
// `publish(T) < scan(T) < publish(F) < scan(F) < publish(T)` - a cycle,
// and therefore impossible. The partition latches supply the release and
// acquire edges that make "<" mean something across threads. Neither side
// may verify before it publishes, and neither may hold its partition
// latch across the scan - AO-R2's one-latch rule - which is why the
// verify sits outside the block that grants.
//
// **What that argument proves is "never both granted", not "never both
// refused".** Two overlapping asks that publish before either scans can
// each find the other and each unwind, so both are refused and neither
// holds anything. **What that costs since the lock became the wait, and
// since AY-S2's containment wake**: each side's scan registers a wake on
// the other's entry under the latch that saw it, before the other's unwind
// removes that holder - so each unwind flips the other's slot and neither
// wait outlives the pair. A waiter that holds anything draws its edge, the
// two edges close a cycle, and the second is refused naming deadlock
// (AO-R7); two that hold nothing re-run. No bias is built: one round of the
// pair is a victim or a retry, never a wait the fault net ends - though two
// autocommit asks that keep meeting this way can repeat it, bounded only by
// the net
// (`LockTableTest.TwoOverlappingAsksThatBothPublishFirstAreBothRefusedAndBothWoken`).
//
// ---- The wait-for graph and the victim (AO-R7, D12) -----------------------
//
// A wait is an edge `waiter -> holder`, and a cycle among them is a
// deadlock. The dispatcher records one for every park - the write-block
// wait and the lock wait (`command_dispatcher.cpp`, `NoteWaitFor`) - and
// never for a read holder, which never waits.
//
// **The graph is functional: one out-edge per waiter**, which makes the
// walk a unique chain and the cycle test decisive; a waiter re-registering
// replaces its edge. A refusal names **one** conflicting holder
// (`AcquireResult::blocking_txn`), so a waiter on a unit several hold in
// `S` draws an edge to one of them only, and a cycle through another is
// left to the fault net.
//
// **Detection happens when the edge is added, not on a cadence** - a
// departure from AO-R7's 100 ms core-0 walk. A cycle can only close at the
// instant a waiter adds the edge that closes it, and that waiter holds the
// graph latch and walks the chain itself, so AO-R7's victim rule - "it
// aborts the transaction whose edge closed it" - is exactly true. The
// table is the instance's and `wait_latch_` orders every core's
// registrations, so the walk sees every core's edges; no edgeless park is
// left for a periodic walk to catch (AT-S6, AT-S5f; history at cf889b63).
//
// ---- The cap (AO-R10, E2) -------------------------------------------------
//
// `max_locks_per_txn` = 65,536 entries per transaction - a transaction
// has one `Transaction` (AT-0 item 3, kept 2026-09-28). Reaching it
// refuses `ResourceExhausted`, non-retryable because a retry meets the same
// cap. **A cap refuses, it never truncates**, and there is no escalation
// (AR2-R4): escalation would convert fine borrows into a coarse one
// mid-flight, which is a wait the transaction did not ask for and a
// deadlock edge nobody drew. The one refusal the borrow model *adds* and
// keeps, because a cap cannot be waited out (AR2-A §3) - which is why it
// is refused **before** a conflict is queued: a transaction at its cap must
// not be left waiting for a grant that will be refused on arrival.

namespace kds::sched {
class WakeRegistry;
}  // namespace kds::sched

namespace kds::txn {

// The nesting order is the enum's order, so an ancestor compares less than
// its descendants - for the first four.
//
// **`kIndexKey` sits outside that nesting** (BX-R3,
// `instructions/v3.2.0/workorder-bx-unique-index.md`): one value of one
// unique index, taken `X` by a writer of that value from its check to its
// append, so two writers of one key cannot both find it free. It is named
// under its **relation's** oid - so the writer's `IX` on the relation is its
// intention, as a tuple's is - and its interval is a hash of the index oid
// and the encoded key, which no fence and no tuple share: `Overlaps` never
// matches it. A hash collision costs a wait, never an answer; the check
// compares values.
enum class LockUnit : std::uint8_t {
    kRelation = 1,
    kRange = 2,
    kSlice = 3,
    kTuple = 4,
    kIndexKey = 5,
};

enum class LockMode : std::uint8_t {
    kIntentionShared = 1,
    kIntentionExclusive = 2,
    kShared = 3,
    kExclusive = 4,
};

inline constexpr std::size_t kLockModeCount = 4;

const char* LockUnitName(LockUnit unit) noexcept;
const char* LockModeName(LockMode mode) noexcept;

namespace detail {

constexpr std::size_t ModeIndex(LockMode m) noexcept {
    return static_cast<std::size_t>(m) - 1;
}

// **The compatibility matrix, as a table.** `IS` conflicts only with `X`;
// `IX` with `S` and `X`; `S` with `IX` and `X`; `X` with everything
// (AR2 §2).
//
// A table and not a chain of predicates, and the reason is a widening that
// has a name: adding `SIX` to the enum must not compile until somebody
// fills its row. An expression form falls through to its final `return`
// and answers a plausible default for the mode nobody considered, which is
// how a lock manager admits two writers and passes its tests.
inline constexpr bool kCompatible[kLockModeCount][kLockModeCount] = {
    //            IS     IX      S      X
    /* IS */ {true, true, true, false},
    /* IX */ {true, true, false, false},
    /* S  */ {true, false, true, false},
    /* X  */ {false, false, false, false},
};

// Does holding the row's mode make the column's mode a no-op for the same
// transaction? `X` covers all; `S` and `IX` each cover `IS`. `S` does not
// cover `IX` and `IX` does not cover `S` - v1 has no `SIX`, so a
// transaction needing both holds both entries, which is the honest
// accounting rather than a silent upgrade.
inline constexpr bool kCovers[kLockModeCount][kLockModeCount] = {
    //            IS     IX      S      X
    /* IS */ {true, false, false, false},
    /* IX */ {true, true, false, false},
    /* S  */ {true, false, true, false},
    /* X  */ {true, true, true, true},
};

}  // namespace detail

constexpr bool Compatible(LockMode held, LockMode want) noexcept {
    return detail::kCompatible[detail::ModeIndex(held)][detail::ModeIndex(want)];
}

constexpr bool Covers(LockMode held, LockMode want) noexcept {
    return detail::kCovers[detail::ModeIndex(held)][detail::ModeIndex(want)];
}

// A unit's identity. The interval is part of the identity; only `lo` is
// part of the hash, so two slices sharing a lower bound land in one
// partition and the chain tells them apart.
//
// `hi` is exclusive. A tuple is the unit-length interval `[tuple key, tuple key + 1)`,
// so one containment test serves tuples and slices alike.
struct LockKey {
    catalog::Oid rel_oid = 0;
    LockUnit unit = LockUnit::kRelation;
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;

    static constexpr LockKey Relation(catalog::Oid rel) noexcept {
        return LockKey{rel, LockUnit::kRelation, 0, 0};
    }
    static constexpr LockKey Range(catalog::Oid rel, std::uint64_t lo, std::uint64_t hi) noexcept {
        return LockKey{rel, LockUnit::kRange, lo, hi};
    }
    static constexpr LockKey Slice(catalog::Oid rel, std::uint64_t lo, std::uint64_t hi) noexcept {
        return LockKey{rel, LockUnit::kSlice, lo, hi};
    }
    static constexpr LockKey Tuple(catalog::Oid rel, std::uint64_t tuple_key) noexcept {
        return LockKey{rel, LockUnit::kTuple, tuple_key, tuple_key + 1};
    }
    // `key_hash` names the index and the encoded value together
    // (`exec::UniqueKeyUnit`); masked to 63 bits so `hi = lo + 1` cannot wrap.
    static constexpr LockKey IndexKey(catalog::Oid rel, std::uint64_t key_hash) noexcept {
        const std::uint64_t lo = key_hash & (~std::uint64_t{0} >> 1);
        return LockKey{rel, LockUnit::kIndexKey, lo, lo + 1};
    }

    friend constexpr bool operator==(const LockKey& a, const LockKey& b) noexcept {
        return a.rel_oid == b.rel_oid && a.unit == b.unit && a.lo == b.lo && a.hi == b.hi;
    }

    // Is this unit a **fence** - a borrow taken over a key interval rather
    // than over one row? Range and slice units are; a tuple's conflict is
    // found by looking its own key up, and a relation's by the intention
    // rule at the relation entry. AR2 §2 defines a gap lock as an `S` *or
    // `X`* fence, which is why the mode is not part of this test.
    constexpr bool IsFenceUnit() const noexcept {
        return unit == LockUnit::kRange || unit == LockUnit::kSlice;
    }

    constexpr bool ContainsKey(std::uint64_t tuple_key) const noexcept { return tuple_key >= lo && tuple_key < hi; }

    // Do two units' key intervals meet? A relation unit carries the empty
    // interval `[0, 0)` and therefore overlaps nothing - which is right,
    // because a relation conflict is caught at the relation entry by the
    // intention rule and never needs a scan. An index-key unit's interval is
    // a hash, not tuple keys, so it overlaps nothing either: a fence over
    // `[0, 2^48)` must not meet a key unit whose hash falls inside it.
    constexpr bool Overlaps(const LockKey& other) const noexcept {
        return rel_oid == other.rel_oid && unit != LockUnit::kIndexKey &&
               other.unit != LockUnit::kIndexKey && lo < other.hi && other.lo < hi;
    }
};

// AO-R10's value and the default behind `max_locks_per_txn`
// (`Expeditor::Config`, `server/expeditor.hpp`), kept on 2026-09-28 (AT-0
// item 3). One name for the quantity: nothing else spells this number.
inline constexpr std::size_t kMaxLocksPerTxnDefault = 65536;

// AO-R2's partition count, fixed at every core count since BP-S6 (BP-Q15
// (a)): AO-S7 sized it per core under ownership, and above one core a tuple
// ask under a held fence walks every partition (`FenceCoversKey`), which at
// `64 x cores` cost +12 % of a statement's CPU at eight cores (BP-H10).
inline constexpr std::size_t kLockPartitions = 64;

// **The read borrow holds under an identity that is not a transaction's**
// (AO-S6e-b). AR2 §3's `SELECT` row scopes the read borrow to *the
// statement*, and a `SELECT` outside an explicit transaction has no
// transaction at all - `AutocommitSnapshot` mints a view owned by nobody -
// so there is no id to hold under and no `LockHoldings` to record in. The
// dispatcher mints one per read statement with this bit set.
//
// Free by construction: a trx id is 48 bits (`heap::kMaxTrxId`, invariant
// 12's `trx_id:48`), so nothing this table is handed as a transaction can
// collide with it. The table itself never reads the bit - a holder is an
// opaque id to it - and the two places that do are the refusal a waiter
// renders (a reader is not "transaction N") and the wait-for graph, which
// never sees one: a read borrow takes what is free and proceeds otherwise,
// so a reader is never the waiting half of an edge.
inline constexpr std::uint64_t kReadHolderBit = std::uint64_t{1} << 63;

inline constexpr bool IsReadHolder(std::uint64_t holder) noexcept {
    return (holder & kReadHolderBit) != 0;
}

// **The fault net, and it is never the normal end of a wait** (AO-R8,
// AR2-R10 as amended by AR2-A). A wait ends when the holder decides. This
// bound exists only for a fault - a detector that missed a cycle, or a
// holder that is stuck - and when it fires it aborts the *waiter* (R1
// never touches the holder) and is logged as the fault it is.
//
// **The net is 1 s** (D12): no holder waits on another core for anything,
// so the longest honest wait is a local statement's own work (the 11 s 2PC
// deadline retired at AT-S6; history at cf889b63). **A relation-`X` build
// is local work longer than that**, so the net measures a registered
// holder's silence rather than the wait's length (`holder_progress.hpp`):
// while the build advances, its waiters' bound restarts. `lock_wait_fault_net_ms`
// overrides it - `in_doubt_ceiling_ms` re-scoped (AT-0 item 7), whose old
// spelling is **refused at startup naming its successor**.
inline constexpr std::uint64_t kLockWaitFaultNetNs = 1ull * 1000 * 1000 * 1000;

// What a parked waiter polls. The table owns it and a decide flips it;
// the waiter holds a `shared_ptr` so the slot outlives the entry it was
// queued on - entries live in a `vector` that reallocates, so a raw
// pointer into one would dangle the moment another key hashed to the same
// partition.
//
// One bit and not a grant: the wake says "the unit changed, ask again",
// which is what keeps the re-check mandatory rather than optional.
struct LockWaitSlot {
    std::atomic<bool> ready{false};
};

// A wake registration: the slot to park on and the unit it is registered
// on, which may be another unit than the one asked for (`AcquireResult`).
// One value so a caller cannot keep the slot and drop it on the wrong key,
// which would leave the registration, and its entry, for good.
struct LockWake {
    LockKey key{};
    std::shared_ptr<LockWaitSlot> slot;
};

struct AcquireResult {
    bool granted = false;
    // Set only when `granted` is false: **one** conflicting holder, which
    // is the edge AO-S4a's wait-for graph draws. One and not all of them,
    // because a cycle needs one edge per waiter and the detector walks
    // transitively.
    std::uint64_t blocking_txn = 0;
    // The transaction already held this unit at a covering mode, so no
    // entry was added and no cap was spent. `granted` is true.
    bool already_held = false;
    // Set only when `granted` is false: what to park on
    // (`sched::WaitUntil` over `LockWaitReady(wake.slot)`), what a decide on
    // the blocking unit flips, and `wake.key`, the unit it is registered on
    // and what `DropWake` must be given.
    //
    // **The key is the blocking entry's, not always the asked one** (AY-S2,
    // the containment wake). A refusal found in the asked key's own entry is
    // registered there. A cross-unit refusal - a fence over a tuple, or a
    // tuple under a fence, found by AO-S6b's verify in another partition -
    // is registered on **the entry whose holder refused it**, inside the scan,
    // under the latch that observed that holder: the registration then lands
    // either before the holder's removal, whose release flips it, or after
    // it, in which case the scan never saw the holder and nothing refused.
    //
    // **Null on one refusal still, and a caller must test for it**: a
    // *queueing* `Acquire`'s cross-unit conflict, which is given no queue
    // position on another unit and no slot. No engine path calls `Acquire`.
    // `LockWaitReady` has no null branch by design, so parking on this
    // without the test is a crash.
    LockWake wake;
};

// The predicate a waiter parks on. Free rather than a member so the
// `std::function` a `WaitUntil` holds captures a `shared_ptr` and nothing
// else - it must not reach the table, because a predicate runs on the
// reactor once per iteration and taking a partition latch there would put
// the table's latch on the poll path of every parked task.
inline bool LockWaitReady(const std::shared_ptr<LockWaitSlot>& slot) noexcept {
    // **No null branch, deliberately.** A refusal always carries a slot, so
    // a null one is a broken contract - and answering `true` for it would
    // satisfy `WaitUntil::await_ready` on entry, which means the acquire
    // loop spins the reactor inside one `resume()` instead of parking. That
    // is the hang this stage already had to fix once; a crash at the bug is
    // the better failure.
    return slot->ready.load(std::memory_order_acquire);
}

// The per-transaction half of the borrow: the bounded list AO-R6 puts on
// `Transaction`. A transaction owns exactly one, so it is
// **move-only** - a copy would let two owners release the same borrows, and
// the second release would corrupt the fence counter.
//
// Core-local by construction (one transaction runs on one core), so it
// carries no synchronization.
class LockHoldings {
public:
    struct Held {
        LockKey key;
        LockMode mode;
    };

    LockHoldings() = default;
    LockHoldings(const LockHoldings&) = delete;
    LockHoldings& operator=(const LockHoldings&) = delete;
    LockHoldings(LockHoldings&&) = default;
    LockHoldings& operator=(LockHoldings&&) = default;

    std::size_t size() const noexcept { return held_.size(); }
    bool empty() const noexcept { return held_.empty(); }
    // Whether `key` is held at any mode - the question a caller asks before
    // an ask whose *first* grant means something (AT-S5e: a writer's first
    // relation intention, and a DDL's own-transaction test).
    bool Holds(const LockKey& key) const noexcept {
        for (const Held& h : held_) {
            if (h.key == key) return true;
        }
        return false;
    }
    // Whether `key` is held at `mode` - a rename asks it of its own
    // transaction's relation `X`, which already fences what it needs
    // (`alter.md` AL4a).
    bool HoldsAt(const LockKey& key, LockMode mode) const noexcept {
        for (const Held& h : held_) {
            if (h.key == key && h.mode == mode) return true;
        }
        return false;
    }

    // The unit this transaction is queued on, if any. **At most one**: a
    // transaction is one thread of control and can be blocked on exactly
    // one borrow at a time. That is what makes the queue unwindable
    // without a second index - `Release` dequeues this one record.
    const std::optional<LockKey>& waiting_on() const noexcept { return waiting_; }

private:
    friend class LockTable;
    std::vector<Held> held_;
    std::optional<LockKey> waiting_;
};

class LockTable {
public:
    // `core_count` decides whether latches are constructed at all (the
    // partition count is fixed, `kLockPartitions`): at one core every `Latch*` is null and no
    // `std::mutex` exists (AO-R9 - what compiles out is the
    // synchronization, never the wait). Zero cores is a refusal, not a
    // silent reinterpretation.
    static StatusOr<std::unique_ptr<LockTable>> Create(
        std::uint32_t core_count, std::size_t max_locks_per_txn = kMaxLocksPerTxnDefault);

    LockTable(const LockTable&) = delete;
    LockTable& operator=(const LockTable&) = delete;

    // Takes `key` in `mode` for `txn`, recording it in `holdings`.
    //
    // `ResourceExhausted` when the transaction is at its cap - checked
    // after the covering test (a re-borrow spends nothing) and **before**
    // the conflict test, so a capped transaction is refused rather than
    // queued behind a grant that would refuse it anyway.
    //
    // A conflict is an answer, not a park: `granted` is false,
    // `blocking_txn` names a holder, and the request is queued on the
    // entry and recorded in `holdings` so it can be withdrawn.
    StatusOr<AcquireResult> Acquire(std::uint64_t txn, const LockKey& key, LockMode mode,
                                    LockHoldings& holdings);

    // **The non-queueing acquire** (AO-S6a). Grants and records exactly as
    // `Acquire` does when the unit is free, and on a conflict returns
    // `false` with no waiter queued and `holdings.waiting_` not moved - only
    // a wake registration left, when `wake` asks for one. The cap still
    // refuses, because a cap is not a conflict.
    //
    // **Why the engine's acquire is this one and not `Acquire`.** The two
    // write sites run inside a page-latched span (`BtreeVisitLeafPage`'s,
    // for the first of them), which is no place to park - so the borrow is
    // taken where it is free and a conflict is *reported* rather than
    // waited on here. AO-S3b's shape is what carries the wait: the walk
    // stops holding nothing, and the park happens on the statement, in
    // `DispatchAsync`, outside every span. That is why `blocker` exists.
    //
    // **`blocker`, when given, receives the transaction this ask lost to**
    // (AO-S6c-b) - what a fence-blocked writer waits on, since a tuple
    // header names only the row's own writer. Untouched on a grant and by
    // the cap's refusal, which is not a conflict and names nobody.
    //
    // **`wake`, when given, receives a slot to park on** (AO-S6e-b). The
    // ask stays non-queueing - no queue position, nothing recorded in
    // `holdings.waiting_`, and the grant this waiter eventually gets is one
    // it asks for again - but a registration is left on the entry so the
    // holder's release flips the slot and kicks the core that asked. It is
    // what a **statement** waits on: the release wakes it from whichever
    // core releases, where a poll of `TransactionManager::IsInFlight` - the
    // instance's since AX-S1 - is re-read only when the waiter's reactor
    // next runs, and a read borrow's holder is no transaction it knows.
    // Untouched on a grant and by the cap.
    //
    // **Every refusal carries one since AY-S2**, a cross-unit one included:
    // it is registered on the entry whose holder refused the ask, which
    // `wake->key` names and which need not be `key` (`AcquireResult::wake`
    // says why the registration cannot be lost).
    //
    // The registration is the caller's to remove - `DropWake(wake->key,
    // wake->slot)` - because the statement that took it is torn down and
    // re-run between the ask and the grant, so no scope here outlives it.
    StatusOr<bool> TryAcquire(std::uint64_t txn, const LockKey& key, LockMode mode,
                              LockHoldings& holdings, std::uint64_t* blocker = nullptr,
                              LockWake* wake = nullptr);

    // Releases **one** of `holdings`' borrows, waking whoever was queued on
    // it, and drops its ledger record so the cap and `Release` see it as
    // never taken.
    //
    // Its callers give back one borrow before the decide: the verify below
    // unwinds a grant it just published; a positioned reader's borrow
    // **moves** (AO-S6e-b) - it takes the slice it is entering before letting
    // go of the one it is leaving, so the position is never unheld between
    // two pages; and a child-row wait gives back a grant it did not need.
    void ReleaseOne(std::uint64_t txn, const LockKey& key, LockMode mode,
                    LockHoldings& holdings);

    // Releases every one of `holdings`' borrows `give_back` selects, in one
    // pass over the ledger: O(ledger) however many go, where a `ReleaseOne`
    // per borrow scans and erases from the middle each time. A failed
    // foreign-key statement gives back its absent parents' `S` this way
    // (AZ-S5, AZ-R5 as amended 2026-10-02).
    template <typename Select>
    void ReleaseIf(std::uint64_t txn, LockHoldings& holdings, Select&& give_back) {
        auto& held = holdings.held_;
        held.erase(std::remove_if(held.begin(), held.end(),
                                  [&](const LockHoldings::Held& h) {
                                      if (!give_back(h)) return false;
                                      ReleaseHeld(txn, h);
                                      return true;
                                  }),
                   held.end());
    }

    // Removes the wake registration `TryAcquire` handed back, by the slot's
    // own identity: the waiter it names is not addressable by transaction
    // id, because a statement's re-run asks under a new one. A slot this
    // table no longer knows about - the entry went with its last holder -
    // is not an error and does nothing.
    void DropWake(const LockKey& key, const std::shared_ptr<LockWaitSlot>& slot);

    // Releases every borrow `holdings` records, **wakes everyone queued on
    // the units it let go**, and withdraws its own pending wait, then
    // empties it.
    //
    // The last act of a decide, **after** visibility is published (AO-R6):
    // a waiter woken here re-checks immediately, and if it ran before the
    // commit was published it would read the holder as still in flight and
    // park again on a slot nobody will flip a second time. Order matters
    // for liveness, not only for tidiness.
    void Release(std::uint64_t txn, LockHoldings& holdings);

    // Records that `waiter` is about to park for `holder`, and answers
    // **whether that edge closes a cycle**. When it does, no edge is
    // recorded and the caller must not park: it is the victim (AO-R7), and
    // aborting it is what breaks the cycle.
    //
    // Idempotent per waiter: a transaction is one thread of control and
    // waits for one thing at a time, so a second call replaces the first.
    bool NoteWaitFor(std::uint64_t waiter, std::uint64_t holder);

    // Drops `waiter`'s edge. Called when the wait ends, however it ends -
    // granted, refused, or the transaction decided.
    void ClearWaitFor(std::uint64_t waiter);

    // Edges currently held. Zero when nothing is waiting, which is what a
    // cell asserts after every wait in it has ended.
    std::size_t WaitEdgeCount() const;

    // The holders that report progress to the fault net (`holder_progress.hpp`):
    // a relation-`X` build registers its transaction here, and a waiter whose
    // net expires asks it before refusing.
    HolderProgress& progress() noexcept { return progress_; }

    // Wakes every waiter queued on `key` without releasing anything.
    // `Release` is its only engine caller; it is public because "a wake is
    // not a grant" cannot be asserted any other way - every `Release`
    // removes a holder, and the cell's whole point is a wake with none.
    void WakeWaiters(const LockKey& key);

    // **The instance's wake registry** (AU-S2), through which a slot flip
    // reaches a waiter parked on another core. Installed once, on the
    // startup thread, before any reactor can wait; null - the default and
    // the whole single-core story - means no waiter is ever kicked, which
    // is right where every waiter shares the decide's reactor. `registry`
    // must outlive this table.
    void SetWakeRegistry(const sched::WakeRegistry* registry) noexcept { wake_ = registry; }
    const sched::WakeRegistry* wake_registry() const noexcept { return wake_; }

    // Is a range- or slice-unit fence held over `tuple_key` by anyone but `txn`,
    // in `S` or `X`? The probe AO-R3 puts in place of a persisted lock bit.
    // Tuple-unit conflicts are **not** its business - those are found by
    // looking the tuple's own key up - and neither are relation-unit
    // borrows, which the intention rule catches at the relation entry.
    // `want` is the mode the asker is taking, and the compatibility table
    // decides against it - so an `S` fence covers an `X` write of a key it
    // spans and admits an `S` read of the same key. It defaults to `X`
    // ("is this key fenced at all").
    //
    // `holder`, when given, receives the covering transaction's id, which
    // is what a refused caller reports as its blocker.
    //
    // `wake`, when given, is registered on the covering fence's entry under
    // the latch that observed its holder, and filled in (AY-S2); the caller
    // owns it as it owns `TryAcquire`'s.
    bool FenceCoversKey(catalog::Oid rel, std::uint64_t tuple_key, std::uint64_t txn,
                        LockMode want = LockMode::kExclusive,
                        std::uint64_t* holder = nullptr, LockWake* wake = nullptr);

    // The fence's **other side** (AO-S6b): does any key interval this
    // relation holds meet `fence`, in a mode `fence`'s own mode conflicts
    // with, held by anyone but `txn`? A fence spans keys whose entries live
    // in other partitions, so its own partition's conflict test cannot see
    // them; this is the scan that does. `holder` receives the conflicting
    // transaction's id.
    //
    // **Every interval-carrying unit, not only tuples.** A tuple is the
    // unit-length interval `[tuple key, tuple key + 1)`, so one overlap test serves both
    // - and it has to, because two *fences* over one relation are the
    // ordinary case the moment a write declares its unit from a predicate
    // (AO-0 item 14): `WHERE id > 1` and `WHERE id BETWEEN 2 AND 3` derive
    // different ranges that meet at row 2, and an exact-key test grants
    // both. That would make a coarse declaration **weaker** in detection
    // than the per-row borrow it replaces, which is the one thing it must
    // not be. A relation-unit holder is still not looked at: it carries the
    // empty interval and is caught at the relation entry.
    //
    // **An intention-mode holder is not looked at either** (AO-S6e-b), the
    // rule `FenceCoversKey` already applies from the other side: a slice
    // `IS` is a positioned reader's declaration of where it is, not a claim
    // on the keys, so a fence-taker passes it and the two meet - if they
    // meet at all - at the relation entry.
    //
    // `wake` as `FenceCoversKey`'s: registered on the overlapping entry.
    bool ConflictingOverlap(const LockKey& fence, LockMode mode, std::uint64_t txn,
                            std::uint64_t* holder, LockWake* wake = nullptr);

    // **A seam between the grant's publication and its verify** (AY-S2),
    // for the one interleaving no single thread produces: two overlapping
    // asks that both publish before either scans. Called with `kPublished`
    // after the grant is written and its latch dropped, and with `kScanned`
    // after the verify's scan and before any unwind, on the asking thread.
    // Null in every production assembly; set only while no ask is in
    // flight, since the hook is read without a latch.
    enum class VerifyPoint : std::uint8_t { kPublished, kScanned };
    void SetVerifyHookForTest(std::function<void(std::uint64_t txn, VerifyPoint)> hook) {
        verify_hook_ = std::move(hook);
    }

    // How many range- and slice-unit fences exist for `rel`. The gate a
    // writer reads on the `IX` it takes anyway; only nonzero costs the
    // scan. **Striped and hash-keyed**, so two relations may share a
    // counter: a collision is a false positive that costs one scan and
    // never an answer.
    std::uint64_t RelationFenceCount(catalog::Oid rel) const;

    // Requests queued on `key`. A decide probes a trail entry's tuple key
    // only when this is nonzero (AO-R3); AO-S4a reads the queue itself.
    std::size_t WaiterCount(const LockKey& key) const;

    // Live entries table-wide. An entry exists only while somebody holds
    // or waits for its unit, so this returns to zero once every
    // transaction has released - including the transactions that only ever
    // waited.
    std::size_t EntryCount() const;

    std::size_t partition_count() const noexcept { return partitions_.size(); }

    // The partition `key` falls in. Public because the property AO-R2's
    // key exists for is asserted by the cells.
    std::size_t PartitionOf(const LockKey& key) const noexcept;

    // The latch serializing `key`'s partition, or null at `cores = 1`.
    // Public for the same reason: "two partitions never serialize" is a
    // statement about these being distinct objects.
    const Latch* LatchOf(const LockKey& key) const noexcept;
    // The wait-for graph's latch, null at `cores = 1`: what BA-S2's cell
    // holds to make an edge count wait under its kind.
    const Latch* WaitForLatchForTest() const noexcept { return wait_latch_.get(); }

    bool latched() const noexcept { return !latches_.empty(); }

private:
    // `Acquire` and `TryAcquire` are one body; `queue_on_conflict` is the
    // only difference between them, and it gates two things rather than
    // one - whether a conflict queues, and whether the preamble withdraws
    // a wait this transaction already holds on another unit. A
    // non-queueing ask must not withdraw one: it puts no wait in the place
    // of the one it would drop, so a transaction asking opportunistically
    // for a second unit would silently give up the queue position it is
    // still parked on. **Not a lost wakeup** - `DequeueWaiter` flips the
    // slot on the way out, exactly so a still-parked waiter is re-entered
    // and re-asks - but a live wait turned into a re-ask nobody ordered,
    // which is a loss of place and, at the back of a busy unit, of
    // liveness. Withdrawing belongs to the queueing ask, because only it
    // replaces what it takes.
    //
    // `register_wake` is the third thing it gates, and only for a
    // non-queueing ask: the waiter record is written and its slot handed
    // back, while `holdings.waiting_` stays empty. That combination is what
    // AO-S6e-b's statement wait needs - a wake with no queue position -
    // and it is why the flag is separate from `queue_on_conflict` rather
    // than derived from it.
    StatusOr<AcquireResult> AcquireInner(std::uint64_t txn, const LockKey& key, LockMode mode,
                                         LockHoldings& holdings, bool queue_on_conflict,
                                         bool register_wake = false);

    // One tenancy. Holders and waiters are the same shape, so they are the
    // same type: a waiter is a tenant that has not been admitted.
    struct Tenant {
        std::uint64_t txn = 0;
        LockMode mode = LockMode::kIntentionShared;
        // A waiter's slot; null for a holder.
        std::shared_ptr<LockWaitSlot> slot;
        // The core a waiter parks on - `CurrentCore()` at the `Acquire` that
        // queued it, which is the reactor its `WaitUntil` polls on - so a
        // flip from another core knows whom to kick (AU-S2). Meaningless
        // for a holder.
        std::uint32_t core = 0;
    };

    // Flips a waiter's slot and, if the waiter parks on another core,
    // records that core in `kick`; the caller kicks after the partition
    // latch is released, which keeps a syscall out of the section.
    static void FlipSlot(Tenant& w, std::vector<std::uint32_t>& kick);
    void KickAll(const std::vector<std::uint32_t>& cores) const noexcept;

    struct Entry {
        LockKey key;
        std::vector<Tenant> holders;
        std::vector<Tenant> waiters;
    };

    struct Partition {
        Latch* latch = nullptr;  // Null at `cores = 1`; owned by `latches_`.
        std::vector<Entry> entries;
    };

    LockTable(std::uint32_t core_count, std::size_t max_locks_per_txn);

    Partition& PartitionFor(const LockKey& key) noexcept;
    const Partition& PartitionFor(const LockKey& key) const noexcept;
    std::atomic<std::uint64_t>& FenceCounterFor(catalog::Oid rel) noexcept;
    const std::atomic<std::uint64_t>& FenceCounterFor(catalog::Oid rel) const noexcept;

    // Drops `txn`'s queued request on `key`, erasing the entry if that
    // leaves it empty. One partition, taken and released here.
    void DequeueWaiter(std::uint64_t txn, const LockKey& key);

    // Removes one holder record from the table: the body `Release` runs
    // per held unit, factored so the verify's unwind runs exactly the same
    // steps in the same order - erase the holder, erase an entry nobody is
    // left on, lower the fence counter only if a holder actually went, and
    // wake whoever was queued. A second copy of that order is how a fence
    // counter drifts.
    void ReleaseHeld(std::uint64_t txn, const LockHoldings::Held& held);

    // Leaves a waiter record for `txn` on `e`, whose partition latch the
    // caller holds - the latch under which it saw the holder that refused
    // `txn` (AY-S2) - and returns it. The record is valid until that latch
    // is released.
    Tenant& RegisterWake(Entry& e, std::uint64_t txn, LockMode mode);


    // The wait-for graph, `waiter -> holder`. One entry per waiting
    // transaction, so it is bounded by the number of live transactions and
    // needs no eviction. Its own latch rather than a partition's: an edge
    // is not a property of any one unit, and taking a partition latch for
    // it would put two of them in one operation's path, which AO-R2's
    // order forbids.
    std::unique_ptr<Latch> wait_latch_;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> wait_edges_;
    // Edges live, maintained under `wait_latch_` and read without it by
    // `ClearWaitFor`, which every decide calls: zero means no latch is
    // taken on the commit path (the `.cpp` says why that read is sound).
    std::atomic<std::size_t> wait_edge_count_{0};
    HolderProgress progress_;

    std::vector<Partition> partitions_;
    std::vector<std::unique_ptr<Latch>> latches_;
    std::size_t max_locks_per_txn_;
    std::vector<std::atomic<std::uint64_t>> fence_counters_;
    // Borrowed; null at `cores = 1` and before `SetWakeRegistry`.
    const sched::WakeRegistry* wake_ = nullptr;
    std::function<void(std::uint64_t, VerifyPoint)> verify_hook_;
};

}  // namespace kds::txn
