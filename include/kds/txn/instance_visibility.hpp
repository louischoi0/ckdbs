#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

#include "kds/base/latch.hpp"
#include "kds/server/superblock.hpp"

namespace kds::sched {
class WakeRegistry;
}

// The instance read view's shared half
// (`instructions/v3.0.0/workorder-an-read-view.md` AN-R1, AN-R8, AN-R9,
// AN-R12).
//
// ---- What this is, and why a per-core view is not enough ------------------
//
// Until AN-S2, `ReadView::Visible` decided visibility from a bound on
// transaction ids and `TransactionManager::ReadHorizon()` walked one core's
// live set. Both were sound only while a reader read its own core's
// versions, which held only because a peer reached another core's rows by
// shipping the statement. A shared buffer pool ends that, and the failure
// is a dirty read rather than an error.
//
// **A bound on trx ids cannot be repaired into an instance-wide one.** Ids
// are carved by each core in disjoint blocks of `kTrxIdBlockSize`
// (`trx_id.hpp`), so issue order across cores is not id order:
//
//   - a commit on a core holding a *higher* block is above a lower core's
//     cursor and reads as "not yet started" - invisible until that core
//     burns its own block and is carved one above (AN-3 E's H1);
//   - a transaction *begun after* a mint, out of a lower core's reserved
//     but unspent range, is below the mint's bound and in no in-flight set,
//     so it reads as committed before it has started (H2).
//
// So commit order is carried by the **commit record's LSN** instead - one
// stream, one latch, one total order (AR0 M0, AL-R1) - and this class is
// where that order is published and read. `docs/spec/txn.md` section 4.1
// owns the contract; this file owns the mechanism.
//
// ---- The three parts ------------------------------------------------------
//
// **The window** maps a committed transaction id to the LSN of its commit
// record. A reader with a snapshot LSN answers "was this committed before
// my snapshot" by one lookup.
//
// **The floor** is a trx id below which every transaction is resolved *and*
// no id will ever be issued again. Below it the window is not consulted and
// the answer is "committed": a loser's page changes are physically undone
// before the database is served, so a version still on a page whose writer
// is below the floor was a winner (`txn.md` section 4.1's existing
// load-bearing assumption).
//
// **The floor's second bound is the one that is easy to get wrong**, and it
// is the same fact H2 rests on. "Every transaction below F is resolved" is
// not maintainable on its own: a core holding an unspent range below F can
// issue into it at any time, and the floor would then answer "committed"
// for a writer that is live. So the floor is also bounded by the **minimum
// issue cursor across cores** - below that, no core can ever issue again.
// Once the floor reaches F, every cursor is at or above F and stays there,
// so nothing below F is ever issued and the floor may only rise.
//
// **A slot per core** publishes what the floor, the horizon and the
// snapshot ceiling are made of: this core's issue cursor, its oldest
// unresolved transaction, the oldest snapshot LSN it holds, and whether it
// has a commit between its append and its publication.
//
// ---- The snapshot ceiling, and the interval AN-Q3 names -------------------
//
// A commit's LSN is fixed under the append latch and its window entry
// becomes readable later, from the committing core, outside that latch.
// `ratification-an-commit-order.md` AN-Q3 names that interval as the one
// place this design can be implemented wrongly and pass every test: a
// snapshot minted at an LSN that *covers* a commit whose entry is not yet
// in the window would answer that commit invisible, then visible - a
// non-repeatable read inside one snapshot, and at `cores = 1` never
// reproducible, because one core publishes synchronously in LSN order.
//
// Two cores do not. Core A appends at 100, core B appends at 200 and
// publishes first; "the highest published LSN" is then 200 while 100 is
// still unpublished, and a snapshot minted at 200 flips on 100 when it
// lands. AN-R9 rules that the ceiling a snapshot takes must exclude every
// commit reserved and not yet published, and the shape here is **a marker
// per open commit, set before the append**: a core about to commit stores
// the ceiling it can see into its slot (`pending_commit_bound`), appends,
// publishes, and clears the marker. A core can hold several since BA-S7
// part 2 - a parked `strict` commit keeps its marker while the core runs
// others - and the slot then carries the lowest open one, which is the
// earliest set (`CoreVisibilitySlot::open_bounds`). Its commit's LSN is assigned after
// every commit that ceiling covers, so it is strictly above the marker -
// and the ceiling a mint reads is the published maximum capped by every
// marker, so no snapshot ever covers an unpublished commit. Set *before*
// the append rather than after, because the gap between an append
// returning and a marker being stored is exactly the gap AN-Q3 names, and
// a marker stored after it closes nothing. `SnapshotCeiling()` is that
// minimum; `CommitCeiling()` is the published maximum, which the mint must
// not read alone.
//
// The ceiling so defined is monotone over time: a marker is set to the
// ceiling as it stood, at or above every snapshot minted before it, and
// cleared only after its commit is published, so a later mint never reads
// below an earlier one.
//
// ---- The acknowledged-commit bound (BA-R1c) -------------------------------
//
// **A marker caps commits that are already acknowledged.** A marker is held
// from before its commit's append until after its publish, and under
// `strict` that span includes the `fdatasync`. Another core's commit can
// publish and be acknowledged inside it, and every snapshot minted until
// the marker lifts is capped below that commit - the committing session's
// own next statement included.
//
// **The session's own case is closed at the statement boundary.** A session
// records its last commit's LSN (`Session::acknowledged_commit_lsn`), and a
// statement whose bound sits above `SnapshotCeiling()` parks until it does
// not (`CommandDispatcher::DispatchAsync`). It cannot mint at the bound
// instead: that would cover a commit whose entry is not in yet, which is
// AN-Q3. The wait ends: every marker below the bound was set before the
// bound's commit was published, a marker set after reads a ceiling at or
// above it, and every marker lifts on every exit (`PendingCommit`). So the
// wait is at most the longest commit in flight when the bound's was
// published - one `strict` commit's sync, which on a peer can queue behind
// the one the writer is already running. And a covered bound stays
// covered - the ceiling is monotone - so the session consumes it at its
// first covered read, and only the first statement after a commit ever
// waits: a `BEGIN` or an autocommit statement, holding nothing.
//
// **The lift kicks the waiter's core.** A parked statement counts itself
// into its core's `ceiling_waiters`, then reads the ceiling; `EndCommit`
// clears its marker, then reads every core's count and kicks each non-zero
// one. Both sides are store-then-load on `seq_cst` atomics, the store-buffer
// shape the Concurrency note below closes with one total order: the waiter
// sees the lift, or the lift sees the waiter. The kick is the registry's
// best-effort one (`sched/waker_table.hpp`), a skipped kick costing one idle
// block.
//
// **Another session's commit is not covered.** A commit acknowledged to
// another session before this statement began stays invisible to it for up
// to one sync - the remainder BA-Q3 (c) names, snapshots that carry their
// in-flight set, which is its own order
// (`docs/inflight/bugs/a-strict-commits-marker-caps-every-cores-snapshot-across-its-sync.md`).
//
// ---- Reclamation, and the two windows it must not outrun -----------------
//
// `Reclaim()` drops a window entry and raises the floor past its
// transaction, after which every view answers that transaction committed
// by the floor's branch. That is sound only for a transaction every live
// **and every future** snapshot already sees, so the pass is bounded by
// three things read together under the window latch: the oldest published
// snapshot (`min_snapshot_lsn` over cores), every pending-commit marker
// (a future snapshot may be capped below a commit the marker's core has
// not published), and every *unpublished mint* - a view whose ceiling has
// been read and whose bound is not yet in its core's slot. The third is
// the one AN-R1's first argument missed: it claimed a pass running while a
// core's slot still read `kUnboundedBound` could only drop what a snapshot
// minted afterwards already covers, which is true of a snapshot minted
// *after* the pass and false of one minted before it and published after.
// So a mint **lowers its core's slot before it reads the ceiling**
// (`LowerSnapshotBound`), to a value at or below the snapshot it is about
// to take; the manager's next publication recomputes the slot exactly.
//
// ---- Concurrency ----------------------------------------------------------
//
// **The slots are lock-free and the window is latched.**
//
// A slot's fields are `std::atomic<std::uint64_t>`, written only by the
// owning core and read by every core. **`min_snapshot_lsn`, the ceiling
// and the pending-commit marker are sequentially consistent, not
// release/acquire**, and the reason is the shape of the two sides: a mint
// *stores* its slot then *loads* the ceiling, and a commit *stores* the
// ceiling then a pass *loads* the slots. That is the store-buffer pattern,
// under which release/acquire lets both loads read the old value - the
// pass sees no reader and drops an entry, the mint reads a ceiling below
// that entry's LSN, and the view then answers the entry committed by the
// floor at an LSN above its own snapshot. One total order over the four
// operations is what closes it, and `seq_cst` is that order. The cursor
// and the unresolved bound stay release/acquire: they only ever pull the
// floor down, and their one ordering rule is the pair's, below.
//
// `issue_cursor` and `oldest_unresolved` only ever pull the floor down, so
// a stale read of *either alone* is conservative in the ordinary sense.
// **The pair is not**, and this is the one ordering rule a publisher owes:
// a `Begin` raises the cursor and lowers the unresolved bound in one step,
// and a floor computed from the new cursor beside the old bound sits above
// the transaction that was just begun. So the bound that moves **down** is
// stored first, the bound that moves **up** second, and the release/acquire
// pair then makes a reader that sees the new cursor see the new bound with
// it. `PublishBounds` is that order made one call; the two single
// publishers stay public for the cells that model the interleaving.
//
// **The pending-commit marker is set under the window latch**, because a
// marker read outside it can be stale in a way no memory order repairs: a
// core reads the ceiling at 40, another publishes at 50 and runs a pass
// that sees no marker yet and drops that entry, and the first core then
// stores 40 - after which a mint caps at 40 and finds the 50 committed by
// the floor. Under the latch a marker and a pass are ordered: the pass
// sees the marker, or the marker's read of the ceiling sees the publish.
//
// The window is guarded by `Latch` (`base/latch.hpp`). **It is a leaf: no
// other latch is ever taken under it.** Acquisition order on the write
// side: taken with the WAL stream latch released, never under it - the
// same shape `wal/writer.hpp`'s wait mutex has, and the reason `Commit`
// publishes after `WalManager::Commit` has returned rather than inside the
// append (AN-R9). On the read side it is taken **under a page latch and
// under a page span**: `ReadView::Visible` runs where a tuple is decoded,
// and the one thing that makes that sound is that nothing holding this
// latch touches a page. Never held across a suspension point.
//
// ---- The in-flight tables (AX-S1) -----------------------------------------
//
// **Which transactions are running, instance-wide**
// (`instructions/v3.0.0/workorder-ax-inflight-publication.md`). A decided
// transaction is told apart from a running one by the window only when it
// committed: a loser is invisible by absence (AN-R2), so "no entry" is both
// "aborted" and "still running", and no bound on the slots above separates
// them either. A table per core does: an id is in its core's table exactly
// while `Transaction::active_` is true, and nothing else is published
// (AX-R2, the operator's "active bit only").
//
// **Single writer, and no latch.** Only the issuing core writes its table;
// every core reads it. A publish stores the id at `ids[count]` and then
// raises `count`; a retire swap-removes - the last entry is copied into the
// freed position, **then** the tail is cleared, **then** `count` is lowered -
// every store release, every load acquire.
//
// **A reader scans from `count - 1` down to 0, and the direction is the
// correctness argument, not a preference.** A swap-remove moves an id only
// *downward*, and the copy is ordered before the clear of the place it left.
// A descending reader that reads a place after its clear therefore sees the
// copy, which sits at a place it has not reached yet. An ascending reader can
// miss a live id: over `[A, B, X]` it reads A at 0, the writer retires A (X
// copied to 0, 2 cleared), and the reader then reads B at 1 and 0 at 2 - X
// was never absent and the scan never saw it. The work order's AX-R1 scanned
// `[0, count + 1)` ascending, which is that miss; `HoldsIn`'s cell drives it.
//
// **A stale answer errs one way only.** A reader can find an id its core has
// just retired - answering "in flight" for a transaction a moment decided,
// which every consumer re-asks or reads as the conservative answer - but it
// cannot miss an id whose publication happened before its question.
//
// A table is allocated when its core's manager attaches and freed with the
// instance, so an instance pays `kInFlightSlotsPerCore` ids per attached core
// and nothing for a slot that never attached - and a publication allocates
// nothing and cannot fail.

namespace kds::txn {

// "Constrains nothing": no snapshot held, no transaction live, no cursor
// published, no commit pending. The maximum rather than zero, because every
// one of the four is consumed by a `min`.
inline constexpr std::uint64_t kUnboundedBound = std::numeric_limits<std::uint64_t>::max();

// A window miss. `wal::kNoLsn` is 0 and no commit record ever sits at LSN 0,
// so the sentinel and the type's own null agree.
inline constexpr std::uint64_t kNoCommitLsn = 0;

// What one core publishes about itself. Written by its owner, read by all.
struct CoreVisibilitySlot {
    // The oldest snapshot LSN live on this core - over its live
    // transactions' views and its leased readers - or `kUnboundedBound`
    // with none. The instance horizon is the minimum of these.
    std::atomic<std::uint64_t> min_snapshot_lsn{kUnboundedBound};

    // `TrxIdSequence::peek()` - the next id this core would issue. Below
    // it, this core will never issue again. `kUnboundedBound` means the
    // core has not attached and constrains nothing.
    std::atomic<std::uint64_t> issue_cursor{kUnboundedBound};

    // The oldest transaction still running on this core, or
    // `kUnboundedBound` with none.
    std::atomic<std::uint64_t> oldest_unresolved{kUnboundedBound};

    // The ceiling this core saw as it began a commit, or `kUnboundedBound`
    // with no commit between its append and its publication. The commit's
    // LSN is above this, so a snapshot capped by it cannot cover the
    // commit (the header's "snapshot ceiling" note).
    std::atomic<std::uint64_t> pending_commit_bound{kUnboundedBound};

    // **The open markers behind it** (BA-S7 part 2): a `strict` commit that
    // parks before its publish holds its marker across the park, so more
    // than one commit per core can be between its append and its
    // publication. Each one's bound, in begin order - non-decreasing, since
    // the ceiling they are read from only rises - and `pending_commit_bound`
    // is the first. Written and read by this core's thread alone.
    std::vector<std::uint64_t> open_bounds;

    // Statements on this core parked until the ceiling covers their
    // session's last commit (BA-R1c; the header's "acknowledged-commit
    // bound" note). A marker's lift kicks this core while it is non-zero.
    std::atomic<std::uint32_t> ceiling_waiters{0};
};

// **The in-flight cap, per core** (AX-R4; AX-Q1 marked as proposed by the
// operator on 2026-09-28). 1,024 ids of 8 bytes is 8 KiB a core. Past it,
// `TransactionManager::Begin` refuses `ResourceExhausted` rather than admit
// a transaction it cannot publish: an unpublished one reads as "not in
// flight" on every other core while it runs, which is a wrong answer where
// the cap is a refusal.
inline constexpr std::uint32_t kInFlightSlotsPerCore = 1024;

// One core's running transactions (the header's "in-flight tables" note).
// The live ids are `ids[0, count)`; 0 is an empty place, and no transaction
// is ever issued id 0 (`wal::kNoTxnId`).
struct CoreInFlightTable {
    std::atomic<std::uint32_t> count{0};
    std::array<std::atomic<std::uint64_t>, kInFlightSlotsPerCore> ids{};
};

class InstanceVisibility {
public:
    InstanceVisibility() = default;
    InstanceVisibility(const InstanceVisibility&) = delete;
    InstanceVisibility& operator=(const InstanceVisibility&) = delete;

    // ---- What a core publishes about itself ------------------------------

    // This core's `TrxIdSequence::peek()`. A core must publish it before it
    // runs a transaction: until it does, the core constrains no floor, and
    // a floor raised in its absence would be a floor it could then issue
    // below. `PublishIssueCursor` therefore refuses to move a cursor
    // backwards, which is what a late attach would look like.
    void PublishIssueCursor(std::uint32_t core, std::uint64_t cursor) noexcept;

    // This core's oldest running transaction, `kUnboundedBound` with none.
    // **Published before the cursor** when both move - the Concurrency note
    // above says why.
    void PublishOldestUnresolved(std::uint32_t core, std::uint64_t trx_id) noexcept;

    // The two above in the order the contract requires, as one call. What
    // `TransactionManager` uses; the singles exist for the cells that
    // model the interleaving between them.
    void PublishBounds(std::uint32_t core, std::uint64_t oldest_unresolved,
                       std::uint64_t cursor) noexcept {
        PublishOldestUnresolved(core, oldest_unresolved);
        PublishIssueCursor(core, cursor);
    }

    // **Is any transaction unresolved anywhere in the instance** (AT-S7).
    // Derived from the same `oldest_unresolved` slots, kept as a count so
    // the question costs one load rather than a walk of every slot.
    //
    // Its one caller is the Cabin's banking rule (`cabin.md` §6a), which
    // needs "nothing can still commit below my snapshot" and used to get a
    // weaker fact: `ReadView::in_flight_at_mint`, stamped from the minting
    // core's own `live_` list. One store for the instance made that answer
    // the wrong question - a transaction in flight on *another* core writes
    // rows this view cannot see and will commit them the moment it ends -
    // so the question is the instance's now, and it is asked where the
    // answer is used rather than carried on the view.
    bool AnyUnresolved() const noexcept {
        return cores_with_unresolved_.load(std::memory_order_acquire) != 0;
    }

    // ---- The in-flight tables (the header's note) -------------------------

    // Builds `core`'s table if it has none. By that core alone, before its
    // first `PublishInFlight` - `TransactionManager`'s constructor.
    void AttachInFlight(std::uint32_t core);

    // Whether `core`'s table has no place left for another id. The caller
    // asks before it issues one, so a refusal spends nothing.
    bool InFlightFull(std::uint32_t core) const noexcept;

    // `trx_id` is running on `core`. Called by that core alone, once
    // `Transaction::active_` is true and before anything the transaction
    // writes can be read elsewhere; the table must be attached and
    // `InFlightFull(core)` false. **No guard for the second**: `Begin`
    // asks and publishes in one synchronous call on the only core that
    // writes the count, and a publication that silently did nothing would
    // be the unpublished running transaction AX-R4 refuses to make.
    void PublishInFlight(std::uint32_t core, std::uint64_t trx_id) noexcept;

    // `trx_id` has decided on `core`. Called by that core alone, where
    // `active_` falls - after a commit's window entry is in, after an
    // abort's compensations - so a reader that finds it gone finds the
    // decision with it. Not in the table: a no-op.
    void RetireInFlight(std::uint32_t core, std::uint64_t trx_id) noexcept;

    // **Whether `trx_id` is running on any core.** `first_core`'s table is
    // read first, since a caller asks mostly about its own core's
    // transactions and at `cores = 1` that is the only table there is.
    bool InFlight(std::uint64_t trx_id, std::uint32_t first_core) const noexcept;

    // How many ids `core` has published and not retired; 0 for a core that
    // never published. What the cells read.
    std::uint32_t InFlightCount(std::uint32_t core) const noexcept;

    // `core`'s table, or null before its first publication.
    const CoreInFlightTable* in_flight_table(std::uint32_t core) const noexcept {
        return core < in_flight_.size() ? in_flight_[core].load(std::memory_order_acquire)
                                        : nullptr;
    }

    // **The scan, with a seam between places.** `before_place(i)` runs just
    // before place `i` is read; `InFlight` passes one that does nothing. The
    // seam is for the cell that retires an id between two reads of one scan
    // - the interleaving the header's direction argument is about, which two
    // threads reach only by luck.
    template <typename BeforePlace>
    static bool HoldsIn(const CoreInFlightTable& table, std::uint64_t trx_id,
                        BeforePlace&& before_place) {
        // Downward from the count the load returns (the header says why the
        // direction is the argument). The count is never above the cap: a
        // publication past it is refused before it is made.
        for (std::uint32_t i = table.count.load(std::memory_order_acquire); i-- > 0;) {
            before_place(i);
            if (table.ids[i].load(std::memory_order_acquire) == trx_id) return true;
        }
        return false;
    }

    // This core's oldest live snapshot LSN, `kUnboundedBound` with none.
    void PublishSnapshotBound(std::uint32_t core, std::uint64_t lsn) noexcept;

    // **Before a mint reads the ceiling**: lowers this core's slot to `lsn`
    // if it is above it, and never raises it. The header's "reclamation"
    // note says why a mint must be visible to a pass before it has a view
    // to publish; the manager's next `PublishSnapshotBound` recomputes the
    // slot exactly, which may raise it again.
    void LowerSnapshotBound(std::uint32_t core, std::uint64_t lsn) noexcept;

    // ---- The window -------------------------------------------------------

    // **Before the append** of a commit on `core`: caps the snapshot
    // ceiling below the LSN that append will be assigned, until
    // `EndCommit`. The header's "snapshot ceiling" note is the argument.
    // More than one commit per core can be open at once since BA-S7
    // part 2 - a parked `strict` commit holds its marker - so the slot
    // publishes the lowest open bound (`CoreVisibilitySlot::open_bounds`).
    // Returns the bound this commit registered, which its `EndCommit` names.
    // Takes the window latch (the header says why) and releases it before
    // returning, so nothing is held across the append that follows. Called
    // on `core`'s own thread, as `EndCommit` is.
    std::uint64_t BeginCommit(std::uint32_t core);

    // `BeginCommit` on construction, `EndCommit` on destruction: a marker
    // left set - by a throw between the two, say - would cap every mint on
    // the instance forever, and the manager's commit path has more than
    // one exit.
    class PendingCommit {
    public:
        PendingCommit(InstanceVisibility& visibility, std::uint32_t core)
            : visibility_(visibility), core_(core), bound_(visibility_.BeginCommit(core_)) {}
        ~PendingCommit() { visibility_.EndCommit(core_, bound_); }
        PendingCommit(const PendingCommit&) = delete;
        PendingCommit& operator=(const PendingCommit&) = delete;

    private:
        InstanceVisibility& visibility_;
        std::uint32_t core_;
        std::uint64_t bound_;
    };

    // Records that `trx_id` committed at `commit_lsn`. Called where the
    // transaction leaves the in-flight set, so the two records never
    // disagree (AN-R2, AN-R9). Reclaims opportunistically.
    //
    // `kNoCommitLsn` asks for **the next position in commit order**, and
    // is what an unlogged instance passes: with no stream there is no
    // record to take an LSN from, and the order the window carries is then
    // its own - one past the highest it has published. Not a second name
    // for the LSN (`CLAUDE.md`'s rule) but the quantity's only name where
    // the log does not exist; a logged instance never passes it. Returns
    // the LSN recorded.
    std::uint64_t PublishCommit(std::uint64_t trx_id, std::uint64_t commit_lsn);

    // After the publication - or after an append that failed - on `core`:
    // lifts the cap `BeginCommit` put on the ceiling, then kicks every core
    // with a statement parked on the ceiling (BA-R1c).
    void EndCommit(std::uint32_t core, std::uint64_t bound) noexcept;

    // **A statement on `core` waits for the ceiling, or stops waiting**
    // (BA-R1c; the header's "acknowledged-commit bound" note). Counted in,
    // before the waiter's first read of the ceiling, and out once it is
    // covered. The kick is through `wake`, installed once before any core
    // runs; none at `cores = 1`, where a session's own commit is published
    // before its next statement and no other core holds a marker.
    void EnterCeilingWait(std::uint32_t core) noexcept;
    void LeaveCeilingWait(std::uint32_t core) noexcept;
    void SetWakeRegistry(const sched::WakeRegistry* wake) noexcept { wake_ = wake; }

    // **The window and the floor, answered together under one hold**
    // (AN-R12). `commit_lsn` is `kNoCommitLsn` when the window does not
    // hold `trx_id` - uncommitted, aborted, or reclaimed - and `floor` is
    // what separates the last from the first two.
    //
    // **Together is the whole point, and a separate `CommitLsnOf` used to
    // make it impossible.** `Reclaim()` erases entries below `reachable`
    // and raises the floor to it, both under this latch. A reader that took
    // the floor *before* a pass and looked the window up *after* it sees
    // `trx_id >= floor_old` (so the floor does not answer) and no entry (the
    // pass erased it), and concludes **not committed for a transaction
    // committed long ago** - a lost row from two reads straddling one pass.
    // The alternative fix was a rule that a window miss re-reads the floor,
    // which is sound by an argument about an ordering the code does not
    // state and survives only while nobody adds a second way to miss. This
    // removes the straddle instead of explaining that it is harmless.
    //
    // The invariant it buys, and the one the cell asserts: **an entry that
    // is absent is an entry below the floor**, for every transaction that
    // ever committed. Under one hold there is no third state.
    struct CommitLookup {
        std::uint64_t commit_lsn = kNoCommitLsn;
        std::uint64_t floor = 0;
    };
    CommitLookup LookupCommit(std::uint64_t trx_id) const;

    // ---- Derived ----------------------------------------------------------

    // **The ceiling a new snapshot takes** (AN-R9): the highest published
    // commit LSN, capped by every core's pending-commit marker. Every commit
    // at or below it is in the window, so a view minted here never changes
    // its answer for any transaction. Monotone over time (the header says
    // why), which is what lets a snapshot register after it is minted.
    std::uint64_t SnapshotCeiling() const noexcept;

    // The highest commit LSN this instance has *published* - raised inside
    // `PublishCommit`'s hold and *after* the entry, so a reader of this
    // value can find every entry it covers. **Not what a mint reads**: it
    // says nothing about commits whose LSN is fixed and whose entry is not
    // yet in, and `SnapshotCeiling` is the one that does. The cells read
    // it to tell the two apart.
    //
    // **The Cabin's banking gate reads it too** (`cabin.md` §6a), and for
    // the property the snapshot ceiling does not have: this is raised
    // before a committer drops out of `cores_with_unresolved_`, where the
    // ceiling's pending-commit cap is cleared *after*. A gate asking "has
    // anything committed above my snapshot" over the capped value has an
    // interval where the answer is no and the commit has happened; over
    // this one it has none. A mint must still not read it.
    std::uint64_t CommitCeiling() const noexcept { return commit_ceiling_.load(); }

    // The lowest LSN any future snapshot could be capped to by a commit in
    // flight, `kUnboundedBound` with none pending. What bounds a pass
    // beside the horizon; read under the window latch there.
    std::uint64_t PendingCommitBound() const noexcept;

    // Below this, a version still on a page was written by a winner.
    //
    // **Not the visibility decision's floor on its own** (AN-R12). This is
    // the atomic read, taken outside the window latch. `ReadView::Visible`
    // reads it first as a fast path that can only answer `true` - a floor
    // only rises - and decides the rest under `LookupCommit`'s hold.
    // Reclamation's own accounting, `SHOW META` and a cell asserting the
    // floor moved read it alone; pairing it with a separate window lookup
    // is the straddle that ruling exists to close.
    std::uint64_t Floor() const noexcept { return floor_.load(std::memory_order_acquire); }

    // The oldest snapshot LSN any core holds; `kUnboundedBound` with none.
    // `TransactionManager::ReadHorizon()` on every core answers this.
    std::uint64_t HorizonLsn() const noexcept;

    // How far the floor could rise if no reader held it back: the minimum
    // over attached cores of that core's cursor and its oldest unresolved
    // transaction. `kUnboundedBound` when no core has attached.
    //
    // **Also the in-flight tables' short-circuit** (AX-S2): no id below it
    // is running, and none will begin - an attached core's next id is at or
    // above its cursor, and a core not yet attached opens its first window
    // by a carve from the superblock's high-water, above every published
    // cursor - so a caller that takes it *once* and then meets ids over
    // time, a catalog scan meeting delete-marks, may answer every id below
    // it "not in flight" without asking `InFlight`. The oldest unresolved
    // id alone would not do: a transaction that begins after the read, on a
    // core whose window sits below it, would read as decided.
    std::uint64_t FloorCandidate() const noexcept;

    // How many cores have published a cursor.
    std::size_t attached_cores() const noexcept;

    // **Whether `core` is what the floor is waiting on** (AN-R13): its
    // cursor is the candidate, and at least one other core has attached.
    //
    // The comparison is against `FloorCandidate()` rather than against the
    // other cursors, so a *busy* core whose oldest unresolved transaction
    // sits below this cursor answers false here - that core is the one
    // holding the floor, and it will let go when its transaction ends. Only
    // a core whose own cursor is the binding term can unpin anything by
    // burning it.
    //
    // False at one attached core, always: the floor then tracks that core's
    // own cursor, which rises with its own work, so the window drains and
    // there is nothing to burn for. That is the shipped `cores = 1` case.
    bool PinsFloor(std::uint32_t core) const noexcept;

    // Raises the floor as far as the candidate and the readers allow, and
    // drops every window entry it passes. Returns entries dropped.
    //
    // An entry may be dropped only when its commit is at or below every
    // live and future snapshot - the horizon, the pending-commit markers
    // and the unpublished mints, read together under the hold (the
    // header's "reclamation" note): an entry above that bound holds the
    // floor at its own id, because a snapshot below its commit must still
    // be told this writer had not committed. That is why the pass lowers
    // the candidate rather than simply skipping the entry.
    std::size_t Reclaim();

    std::size_t window_size() const;
    // The window latch: what BA-S2's cell holds to make a reader wait under
    // its kind (`base/contention.hpp`).
    Latch& WindowLatchForTest() const noexcept { return window_latch_; }

    const CoreVisibilitySlot& slot(std::uint32_t core) const noexcept { return slots_[core]; }

private:
    void NoteSlot(std::uint32_t core) noexcept;

    std::array<CoreVisibilitySlot, server::kMaxWalCores> slots_{};

    // The in-flight tables: owned here, each written by its core alone,
    // which is also the only writer of its pointer - so the owning array is
    // touched by one thread per index and never read by another, and a
    // reader reaches a table only through `in_flight_`.
    std::array<std::unique_ptr<CoreInFlightTable>, server::kMaxWalCores> in_flight_owned_{};
    std::array<std::atomic<const CoreInFlightTable*>, server::kMaxWalCores> in_flight_{};

    // How many cores report an unresolved transaction, maintained by
    // `PublishOldestUnresolved` alone: only the owning core writes a slot,
    // so the transition it observes is its own and the count cannot race
    // itself. `AnyUnresolved` is the reader.
    std::atomic<std::uint32_t> cores_with_unresolved_{0};

    // One past the highest core that has published anything. Every walk
    // over the slots stops here rather than at `kMaxWalCores`, so the
    // shipped `cores = 1` pays one slot per mint and not sixty-four.
    // Monotone: a core never detaches.
    std::atomic<std::uint32_t> slots_in_use_{0};

    mutable Latch window_latch_{LatchKind::kWindow};
    std::unordered_map<std::uint64_t, std::uint64_t> window_;

    // Amortises `Reclaim`'s O(window) pass: it runs when the window reaches
    // this size, and the threshold is then set from what the pass left, so
    // a floor that cannot advance backs off instead of rescanning per
    // commit.
    std::size_t reclaim_at_ = kReclaimFloor;

    std::atomic<std::uint64_t> floor_{0};
    // `CommitCeiling()`. Monotone: a commit LSN is assigned from one
    // stream under one latch, so a later publication never carries an
    // earlier LSN - but the max is taken rather than assumed, because
    // "assigned in order" and "published in order" are the two things
    // AN-Q3 says not to conflate. Written under the window latch only.
    std::atomic<std::uint64_t> commit_ceiling_{0};

    // Borrowed; null at `cores = 1` and before `SetWakeRegistry`.
    const sched::WakeRegistry* wake_ = nullptr;

    // Small enough that the window stays a few tens of KiB between passes,
    // large enough that the pass is amortised over many commits. Not
    // measured; AN-S5 is where it becomes a number.
    static constexpr std::size_t kReclaimFloor = 1024;
};

}  // namespace kds::txn
