#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "kds/base/latch.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/oid.hpp"

// The borrow model's **lock family** (AR2 §2) - AR0 D2's lock manager.
//
// Built for AO-S1 of `instructions/v3.0.0/workorder-ao-m2-lock-family.md`,
// which owns the rulings this implements: AO-R1 (named by its family, not
// "borrow" - that word is the genus covering this and the page latch),
// AO-R2 (the table and its order), AO-R3 (no persisted bit), AO-R10 (the
// cap) and AO-R11 (modes and units). `docs/rules/rules.md` §3 carries the
// declared-shared row; AO-S8 moves it to `docs/spec/txn.md` §5.
//
// **Units** relation -> range -> {slice, tuple}, AR2-R3's chain; the page
// is not in it and belongs to the latch family
// (`storage/page_latch.hpp`). A **slice** is `(rel_oid, [lo, hi))` in key
// space and never by page (AR2-R6, ratified as E4): a gap is a key
// interval, and a fence over it must outlive the btree leaf division that
// moves keys between pages.
//
// **Modes** `IS`, `IX`, `S`, `X`; no `SIX` and no update mode in v1 (E1).
//
// ---- What this stage is, and what it is not -------------------------------
//
// AO-S1 built **the pure core**: the table, its keys, modes,
// compatibility, the fence gate and the cap. **AO-S2 adds the wait** - a
// refused `Acquire` hands back a slot the waiter parks on with
// `sched::WaitUntil`, and a decide flips the slots of everyone queued on
// what it released. What is still absent is a *caller*: the wait-for graph
// is AO-S4a's, and the cutover that makes a statement take a borrow is
// AO-S3, so a writer still meets the first-updater-wins refusal at
// `src/txn/manager.cpp` today.
//
// **The re-check after a park is mandatory, and the slot does not replace
// it** (AO-3's finding G). `manager.hpp`'s premise - "nothing suspends
// between reading a tuple's header and overwriting it" - is exactly what a
// wait breaks, so a woken waiter must ask again rather than assume the
// grant it was woken for is still there. `Acquire` *is* that re-ask: the
// waiter calls it again, and the slot only says "something changed, look".
// A slot flipped for a grant another waiter took is therefore a wasted
// poll and never a wrong answer.
//
// **A departure from AO-R4, and why it is sound here.** AO-R4 specifies
// the predicate as a disjunction - "the slot flipped **or** the holder is
// decided" - with the waiter registering before its last read of the
// holder's state, because it assumes that last read is of the window or
// `IsInFlight`, outside any lock the registration takes. This
// implementation reads the holder's state from **the table's own holder
// list, in the same critical section as the registration and the slot's
// clear**, so "the registration precedes the last check" is true by
// construction rather than by discipline and the second disjunct is
// redundant. It is also tighter: "the holder is decided" would wake a
// waiter that still conflicts with a *second* holder the disjunct never
// named.
//
// **The premise dies at AO-S3.** There the re-check becomes
// `CheckWriteConflict` re-run - a read of the tuple header and the window,
// outside this latch - and the registration is no longer in the same
// critical section as the last state read. S3 must re-argue this rather
// than inherit it: either AO-R4's disjunction returns, or the borrow is
// taken before the header is read.
//
// **Cross-core, a wake is late rather than lost, until AO-S5.** A slot
// flip from a foreign thread is not one of the three things that end a
// sleeping reactor's idle block (`Scheduler::IdleTimeoutMs`: an fd, a
// timer, the ring waker), so a waiter whose reactor has gone idle learns
// of the flip when the block expires - `max_idle_block_ms`, 10 ms - not
// when it happens. Liveness is unaffected and latency is not:
// AO-R4's `kLockWake` ring message is what closes it, and AO-S5 owes it.
//
// Two consequences of having no caller yet, stated so they are not read as
// finished work:
//
//   - **the cap's refusal carries no wire detail here.**
//     `wire::ResourceDetail::kLockCap` is numbered and frozen in the golden
//     list, and the `Status` this returns is the one a session will map
//     through it - but the mapping happens where a session exists, which is
//     AO-S3;
//   - **the caller owes the ancestor chain and nothing here enforces it.**
//     A relation `X` and a tuple `X` under it are both granted by this
//     table, because AR2-R3's whole design is that the conflict is caught
//     at the relation entry *by the writer taking `IX` there*. A caller
//     that borrows a tuple without the intention chain above it gets a
//     wrong answer quietly. AO-S3 is where the callers appear and where
//     that obligation becomes checkable; `TheTableDoesNotInferTheAncestorChain`
//     pins the behaviour meanwhile.
//
// ---- Concurrency (docs/rules/rules.md §3's lock-table row) ----------------
//
// **One table for the instance**, to be owned by the expeditor and handed
// to every core the way `InstanceVisibility` is - the wiring AO-S3 adds to
// `server/core_runtime.hpp`'s `Config`, not present today. This comment is
// the declaration `rules.md` §3 requires of any lock.
//
// **Partitioned `64 x cores`** `[constant, AR0 D2's; re-measured in AO-S7]`,
// keyed by `(rel_oid, unit kind, lo)` so that a relation's own entry and
// the units beneath it hash apart: the relation entry is touched by every
// writer's `IX` and every positioned reader's `IS`, and sharing its
// partition with its tuples would serialize exactly the traffic the
// partitioning exists to spread. The unit kind is in the key because
// `Relation(r)`, `Tuple(r, 0)`, `Slice(r, 0, h)` and `Range(r, 0, h)` all
// carry `lo = 0` and would otherwise collide by construction rather than
// by chance.
//
// **A `Latch*` per partition, null at `cores = 1`** - `base/latch.hpp`'s
// shape, which keeps G2 a property of the code rather than of a build
// flag. A `std::mutex` and not a spin primitive: AR0-M2 records that the
// spin primitive was deleted at `7839a29`, and re-introducing one is a
// decision AO-S7's numbers can ask for and this stage does not take.
//
// **Acquisition order.** A partition latch is taken
//
//   - with **no page latch** held - AR2-R2's "a lock is acquired before the
//     page it protects is latched, and never under a page latch", so a lock
//     wait (a park, from AO-S2) can never park a latched page;
//   - with **no other partition latch** held. Every method here takes
//     exactly one at a time, including the two that walk every partition:
//     their guard is scoped to the loop body;
//   - **never under the WAL stream latch** or the visibility window latch;
//   - **released before any park.**
//
// **Nothing enforces those four.** They are prose here and structurally
// true only for the second; the census that would arm them is AO-S3's,
// where callers exist to violate them - the same shape AM-S1 needed for
// the page latch.
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
// nothing serializes them. Closing that needs a second side - the
// fence-taker scanning for conflicting descendants after it publishes -
// and that is AO-S6's, where the fence gets its first taker. Until then
// this is a gate on a probe, not a guarantee about a race.
//
// ---- The cap (AO-R10, E2) -------------------------------------------------
//
// `max_locks_per_txn` = 65,536 entries per transaction. Reaching it
// refuses `ResourceExhausted`, non-retryable because a retry meets the same
// cap. **A cap refuses, it never truncates**, and there is no escalation
// (AR2-R4): escalation would convert fine borrows into a coarse one
// mid-flight, which is a wait the transaction did not ask for and a
// deadlock edge nobody drew. The one refusal the borrow model *adds* and
// keeps, because a cap cannot be waited out (AR2-A §3) - which is why it
// is refused **before** a conflict is queued: a transaction at its cap must
// not be left waiting for a grant that will be refused on arrival.

namespace kds::txn {

// The nesting order is the enum's order, so an ancestor compares less than
// its descendants.
enum class LockUnit : std::uint8_t {
    kRelation = 1,
    kRange = 2,
    kSlice = 3,
    kTuple = 4,
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
// `hi` is exclusive. A tuple is the unit-length interval `[pk, pk + 1)`,
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
    static constexpr LockKey Tuple(catalog::Oid rel, std::uint64_t pk) noexcept {
        return LockKey{rel, LockUnit::kTuple, pk, pk + 1};
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

    constexpr bool ContainsKey(std::uint64_t pk) const noexcept { return pk >= lo && pk < hi; }
};

// AO-R10's value. A constant and not a config key at this stage: E2 is
// where the operator marks it, on M2's opening.
inline constexpr std::size_t kMaxLocksPerTxnDefault = 65536;

// AO-R2's partition count, per core. `[constant]`, re-measured in AO-S7.
inline constexpr std::size_t kLockPartitionsPerCore = 64;

// **The fault net, and it is never the normal end of a wait** (AO-R8,
// AR2-R10 as amended by AR2-A). A wait ends when the holder decides. This
// bound exists only for a fault - a detector that missed a cycle, or a
// holder that is stuck - and when it fires it aborts the *waiter* (R1
// never touches the holder) and is logged as the fault it is.
//
// **Its value answers AO-3's finding J.** While 2PC is still in the tree
// an honest holder can sit inside a coordinator's phase deadline
// (`kTxnPhaseDeadlineNs`, 10 s, `server/txn_2pc_service.hpp`), so a net
// below that would fire on a holder doing nothing wrong. 11 s until M3
// retires 2PC, then D12's 1 s. Written out rather than derived from that
// header because `txn/` must not depend on `server/`; the derivation is
// this comment, and M3 moves both.
//
// Not a config key in M2: `in_doubt_ceiling_ms` already names a wait
// ceiling and has two 2PC users, so a second key would be a second name
// for one quantity (`CLAUDE.md`'s rule). M3 re-scopes and renames it.
inline constexpr std::uint64_t kLockWaitFaultNetNs = 11ull * 1000 * 1000 * 1000;

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
    // (`sched::WaitUntil` over `LockWaitReady(slot)`), and what a decide on
    // the blocking unit flips. Never null on a refusal, so a caller cannot
    // mistake "no slot" for "no wait needed".
    std::shared_ptr<LockWaitSlot> slot;
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
// `Transaction`, its own type so this stage can be built and tested before
// the manager is opened in AO-S2. A transaction owns exactly one, so it is
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
    // `core_count` sizes the partition array and decides whether latches
    // are constructed at all: at one core every `Latch*` is null and no
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

    // Wakes every waiter queued on `key` without releasing anything.
    // `Release` is its only engine caller; it is public because "a wake is
    // not a grant" cannot be asserted any other way - every `Release`
    // removes a holder, and the cell's whole point is a wake with none.
    void WakeWaiters(const LockKey& key);

    // Is a range- or slice-unit fence held over `pk` by anyone but `txn`,
    // in `S` or `X`? The probe AO-R3 puts in place of a persisted lock bit.
    // Tuple-unit conflicts are **not** its business - those are found by
    // looking the tuple's own key up - and neither are relation-unit
    // borrows, which the intention rule catches at the relation entry.
    bool FenceCoversKey(catalog::Oid rel, std::uint64_t pk, std::uint64_t txn) const;

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

    bool latched() const noexcept { return !latches_.empty(); }

private:
    // One tenancy. Holders and waiters are the same shape, so they are the
    // same type: a waiter is a tenant that has not been admitted.
    struct Tenant {
        std::uint64_t txn = 0;
        LockMode mode = LockMode::kIntentionShared;
        // A waiter's slot; null for a holder.
        std::shared_ptr<LockWaitSlot> slot;
    };

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

    std::vector<Partition> partitions_;
    std::vector<std::unique_ptr<Latch>> latches_;
    std::size_t max_locks_per_txn_;
    std::vector<std::atomic<std::uint64_t>> fence_counters_;
};

}  // namespace kds::txn
