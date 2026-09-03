#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>

#include "kds/base/latch.hpp"
#include "kds/server/superblock.hpp"

// The instance read view's shared half
// (`instructions/v3.0.0/workorder-an-read-view.md` AN-R1, AN-R8, AN-R9).
//
// ---- What this is, and why a per-core view is not enough ------------------
//
// `ReadView::Visible` decides visibility from a bound on transaction ids,
// and `TransactionManager::ReadHorizon()` walks one core's live set. Both
// are sound only while a reader reads its own core's versions, which holds
// today only because a peer reaches another core's rows by shipping the
// statement. A shared buffer pool ends that, and the failure is a dirty
// read rather than an error.
//
// **A bound on trx ids cannot be repaired into an instance-wide one.** Ids
// are leased to each core in disjoint blocks of `kTrxIdBlockSize`
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
// where that order is published. `docs/spec/txn.md` section 4.1 owns the
// contract; this file owns the mechanism.
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
// **A slot per core** publishes what the floor and the horizon are made of:
// this core's issue cursor, its oldest unresolved transaction, and (from
// AN-S2) the oldest snapshot LSN it holds.
//
// ---- Concurrency ----------------------------------------------------------
//
// **The slots are lock-free and the window is latched.**
//
// A slot's three fields are `std::atomic<std::uint64_t>`, written only by
// the owning core and read by every core, release/acquire. The argument is
// *not* that a stale read is conservative: `min_snapshot_lsn` is not
// monotone - `kUnbounded` is its maximum, meaning "no snapshot", and taking
// a snapshot lowers it - so a stale read of it is too *high*. What makes
// the plain atomic sound is that a newly minted snapshot takes the current
// published ceiling, and the ceiling is at or above every live snapshot, so
// a reclamation that ran while this core's slot still read `kUnbounded` can
// only have dropped entries at or below a bound the new snapshot is itself
// at or above. **A snapshot that is *adopted* rather than minted breaks
// that argument** and must publish before its first read - the cross-owner
// case, AN-S3.
//
// `issue_cursor` and `oldest_unresolved` only ever pull the floor down, so
// a stale read of *either alone* is conservative in the ordinary sense.
// **The pair is not**, and this is the one ordering rule a publisher owes:
// a `Begin` raises the cursor and lowers the unresolved bound in one step,
// and a floor computed from the new cursor beside the old bound sits above
// the transaction that was just begun. So the bound that moves **down** is
// stored first, the bound that moves **up** second, and the release/acquire
// pair then makes a reader that sees the new cursor see the new bound with
// it (`TransactionManager::PublishCoreBounds`).
//
// The window is guarded by `Latch` (`base/latch.hpp`). **Acquisition order:
// taken with the WAL stream latch released, never under it** - the same
// shape `wal/writer.hpp`'s wait mutex has, and the reason `Commit`
// publishes after `WalManager::Commit` has returned rather than inside the
// append (AN-R9). It is taken while holding no other latch and never across
// a suspension point.

namespace kds::txn {

// "Constrains nothing": no snapshot held, no transaction live, no cursor
// published. The maximum rather than zero, because every one of the three
// is consumed by a `min`.
inline constexpr std::uint64_t kUnboundedBound = std::numeric_limits<std::uint64_t>::max();

// A window miss. `wal::kNoLsn` is 0 and no commit record ever sits at LSN 0,
// so the sentinel and the type's own null agree.
inline constexpr std::uint64_t kNoCommitLsn = 0;

// What one core publishes about itself. Written by its owner, read by all.
struct CoreVisibilitySlot {
    // The oldest snapshot LSN live on this core. **Published from AN-S2
    // onwards**: at AN-S1 a read view is still a bound on trx ids and has
    // no LSN to publish, so this stays `kUnboundedBound` and reclamation is
    // unconstrained by readers - which is correct while nothing reads the
    // window.
    std::atomic<std::uint64_t> min_snapshot_lsn{kUnboundedBound};

    // `TrxIdSequence::peek()` - the next id this core would issue. Below
    // it, this core will never issue again. `kUnboundedBound` means the
    // core has not attached and constrains nothing.
    std::atomic<std::uint64_t> issue_cursor{kUnboundedBound};

    // The oldest transaction still running on this core, or
    // `kUnboundedBound` with none.
    std::atomic<std::uint64_t> oldest_unresolved{kUnboundedBound};
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

    // This core's oldest live snapshot LSN, `kUnboundedBound` with none.
    // AN-S2's; nothing calls it at AN-S1.
    void PublishSnapshotBound(std::uint32_t core, std::uint64_t lsn) noexcept;

    // ---- The window -------------------------------------------------------

    // Records that `trx_id` committed at `commit_lsn`. Called where the
    // transaction leaves the in-flight set, so the two records never
    // disagree (AN-R2, AN-R9). Reclaims opportunistically.
    void PublishCommit(std::uint64_t trx_id, std::uint64_t commit_lsn);

    // The commit LSN of `trx_id`, or `kNoCommitLsn` when the window does
    // not hold it - which means uncommitted, aborted, or reclaimed. The
    // caller separates the last from the first two by the floor, never by
    // this answer alone.
    std::uint64_t CommitLsnOf(std::uint64_t trx_id) const;

    // ---- Derived ----------------------------------------------------------

    // Below this, a version still on a page was written by a winner.
    std::uint64_t Floor() const noexcept { return floor_.load(std::memory_order_acquire); }

    // The oldest snapshot LSN any core holds; `kUnboundedBound` with none.
    std::uint64_t HorizonLsn() const noexcept;

    // How far the floor could rise if no reader held it back: the minimum
    // over attached cores of that core's cursor and its oldest unresolved
    // transaction. `kUnboundedBound` when no core has attached.
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

    // Raises the floor as far as the candidate and the horizon allow, and
    // drops every window entry it passes. Returns entries dropped.
    //
    // An entry may be dropped only when its commit is at or below every
    // live snapshot: an entry above the horizon holds the floor at its own
    // id, because a snapshot below its commit must still be told this
    // writer had not committed. That is why the pass lowers the candidate
    // rather than simply skipping the entry.
    std::size_t Reclaim();

    std::size_t window_size() const;

    const CoreVisibilitySlot& slot(std::uint32_t core) const noexcept { return slots_[core]; }

private:
    std::array<CoreVisibilitySlot, server::kMaxWalCores> slots_{};

    mutable Latch window_latch_;
    std::unordered_map<std::uint64_t, std::uint64_t> window_;

    // Amortises `Reclaim`'s O(window) pass: it runs when the window reaches
    // this size, and the threshold is then set from what the pass left, so
    // a floor that cannot advance backs off instead of rescanning per
    // commit.
    std::size_t reclaim_at_ = kReclaimFloor;

    std::atomic<std::uint64_t> floor_{0};

    // Small enough that the window stays a few tens of KiB between passes,
    // large enough that the pass is amortised over many commits. Not
    // measured; AN-S5 is where it becomes a number.
    static constexpr std::size_t kReclaimFloor = 1024;
};

}  // namespace kds::txn
