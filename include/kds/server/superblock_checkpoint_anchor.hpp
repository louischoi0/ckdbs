#pragma once

#include <array>
#include <bit>
#include <cstdint>

#include "kds/base/status.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/checkpointer.hpp"

// Adapter: the superblock's per-core anchor table seen as the thing a
// checkpoint publishes into (wal.md sections 8-3, 11-3, 14-3). This is the
// durable replacement for wal::InMemoryCheckpointAnchor, whose own comment
// says it is correct only for a process that never restarts.
//
// It lives on the server side of the seam for the same reason
// BufferPoolCheckpointTarget lives on the storage side: the checkpointer is
// WAL policy and must stay testable against a scripted anchor, while the
// superblock has no reason to know what a checkpoint is. This class is the
// only place the two names meet.
//
// ---- Why Publish() does I/O ---------------------------------------------
//
// An anchor that exists only in the in-memory SuperBlock is not an anchor:
// recovery reads the superblock *page*, so a checkpoint whose anchor never
// reached the platter leaves recovery replaying from the previous one -
// correct, but it silently gives back the bounded-RTO guarantee the
// checkpoint was run for. So Publish() encodes the superblock and syncs the
// store.
//
// The sync is whole-store, which is heavier than this one page needs. That
// is DevicePageStore's only durability point today (page_store.hpp), not a
// choice made here; it is also harmless, because the checkpoint has just
// flushed its dirty table through that same store anyway.
//
// ---- Ordering ------------------------------------------------------------
//
// The Checkpointer calls this only after CHECKPOINT_END is durable, which
// is what makes the anchor honest (wal.md section 8-3). Nothing in this
// class re-checks that - it cannot; it does not own the log - so the
// ordering contract stays where the sequence is, in Checkpointer::Complete().
//
// ---- The fold, under one stream (AR0 M0, work order AL's AL-R4) ---------
//
// Which slot an anchor lands in depends on what the volume's log *is*
// (`superblock.hpp`'s `log_topology`):
//
//   per-core streams   slot `core_id`, one anchor per stream. Unchanged.
//   one stream         **slot 0 alone**, holding the *minimum* redo start
//                      over every core's latest completed checkpoint.
//
// The minimum, because with one stream there is one place recovery starts
// and it must not be past any core's earliest still-needed record: core 2
// checkpointing at a high LSN says nothing about core 5's dirty pages, and
// starting there would skip records core 5 still needs replayed. The
// per-core numbers stay **in memory** here - the disk holds only the fold -
// which is why this object, not the superblock, owns the table.
//
// ---- The warm-up, and why it is not conservatism for its own sake -------
//
// No core has published at construction, and the table fills as cores
// checkpoint, so a minimum over it alone is a minimum over the cores that
// *happen to have published in this process lifetime*. Advancing on that would be
// wrong in a way no test of a settled system would show: at a fresh mount
// core 0 checkpoints first, core 5 has not yet, and slot 0 would move to
// core 0's redo start - past records core 5's still-dirty pages need. The
// next crash would replay from there and lose them.
//
// So **the anchor does not advance until every core has published at least
// once**; until then the mount-time anchor is rewritten unchanged. That
// bound is sound because recovery replayed from it, so every page any core
// holds dirty was either redone from at-or-after it or dirtied later - no
// core's earliest needed record can precede it.
//
// **The warm-up is load-bearing for the prepare floor too, not only for
// redo.** A peer holding a live `TXN_PREPARE` that has not checkpointed in
// this run contributes nothing to the minimum, so without the warm-up slot
// 0 could advance past its prepare record - `checkpointer.cpp`'s D4 hazard,
// reached silently. Anyone optimising the warm-up away must answer that
// case, not just the dirty-page one.
//
// The cost is the slowest core's first checkpoint - **and it is unbounded
// where `checkpoint_interval_ns` is 0**, because then no core's cadence
// tick is armed and the warm-up ends only at shutdown. A configuration
// that turns checkpointing off keeps the anchor it mounted with, which is
// the same bargain turning checkpointing off already makes.
//
// Buffer pools are per core in M0 (`page.md` section 6), so a dirty table
// is a per-core fact and each core still runs its own fuzzy checkpoint. A
// single gathered checkpoint is M1's, when the pools merge; then this fold
// has one input and becomes an identity.

namespace kds::server {

class SuperBlockCheckpointAnchor final : public wal::CheckpointAnchor {
public:
    // Both references must outlive the anchor, and `superblock` must be the
    // same instance the rest of the server mutates - the one bootstrap
    // produced - or a later Encode() from elsewhere would write back a copy
    // with no anchor in it.
    SuperBlockCheckpointAnchor(SuperBlock& superblock, storage::PageStore& store) noexcept
        : superblock_(superblock), store_(store), mount_anchor_(MountAnchorOf(superblock)) {}

    // Diagnostic log, null (discard) by default; `log` must outlive this.
    void SetLogger(Logger* log) noexcept { log_ = log; }

    Status Publish(const wal::CheckpointAnchorRecord& anchor) override;

    // Anchors published through this object since it was constructed. The
    // checkpoint-duration/cadence counterpart of WalStats; also what the
    // tests assert against.
    std::uint64_t publishes() const noexcept { return publishes_; }

    // How many distinct cores have published into the fold. The warm-up is
    // over when this reaches the volume's core count; 0 under per-core
    // streams, where nothing folds.
    std::size_t folded_cores() const noexcept {
        return static_cast<std::size_t>(std::popcount(published_));
    }

private:
    // The fold: the anchor slot 0 should hold, given everything published
    // so far. Minimum `redo_start_lsn`, and the rest of the fields come
    // from whichever core supplied that minimum, so the four numbers stay
    // one core's consistent set rather than a mix. Returns the mount-time
    // anchor while any core is still to publish (the warm-up above).
    wal::CheckpointAnchorRecord FoldedAnchor() const noexcept;

    // The floor the warm-up holds to: the lowest redo start over the anchor
    // slots the mount found **that were ever published into**. For a volume
    // born single-stream that is slot 0 alone, since nothing else is ever
    // written. It differs only for a volume converted in place, where a
    // peer's old slot can sit below core 0's and a floor above it would
    // reintroduce the hazard the warm-up closes. Nothing converts in place
    // today (AL-R3); this costs one loop over 64 entries, once.
    //
    // **Skipping the never-published slots is not a detail.** A zero
    // `redo_start_lsn` means "this core has never checkpointed"
    // (`superblock.hpp`), not "an anchor at 0" - and under one stream slots
    // 1..63 hold exactly that forever. Treating them as candidates makes
    // the minimum 0 on every mount, which both pins the warm-up at
    // replay-the-whole-log and writes that zero over the real anchor.
    static wal::CheckpointAnchorRecord MountAnchorOf(const SuperBlock& superblock) noexcept;

    SuperBlock& superblock_;
    storage::PageStore& store_;
    Logger* log_ = nullptr;
    std::uint64_t publishes_ = 0;
    wal::CheckpointAnchorRecord mount_anchor_{};

    // Indexed by core id, with a bit per core that has published. An array
    // and a mask rather than a map, so that "every core has published" is
    // `popcount(published_) == core_count` - a fact about *which* cores,
    // where counting a map's entries would answer the same only if every
    // key were in range, which nothing upstream guarantees.
    std::array<wal::CheckpointAnchorRecord, kMaxWalCores> per_core_{};
    std::uint64_t published_ = 0;
    static_assert(kMaxWalCores == 64, "the published mask is one bit per core");
};

}  // namespace kds::server
