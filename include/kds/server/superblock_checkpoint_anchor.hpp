#pragma once

#include <cstdint>
#include <map>

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
// The map is empty at construction and fills as cores checkpoint, so a
// minimum taken over it alone is a minimum over the cores that *happen to
// have published in this process lifetime*. Advancing on that would be
// wrong in a way no test of a settled system would show: at a fresh mount
// core 0 checkpoints first, core 5 has not yet, and slot 0 would move to
// core 0's redo start - past records core 5's still-dirty pages need. The
// next crash would replay from there and lose them.
//
// So **the anchor does not advance until every core has published at least
// once**; until then the mount-time anchor is rewritten unchanged. That
// bound is sound because recovery replayed from it, so every page any core
// holds dirty was either redone from at-or-after it or dirtied later - no
// core's earliest needed record can precede it. The cost is one
// checkpoint's worth of warm-up per mount, and the alternative is a lost
// page.
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

    // The latest anchor each core published, as this object has seen them -
    // the fold's input, empty under per-core streams because nothing folds
    // there. Exposed for the tests and for a later `SHOW META` block; a
    // core that has not checkpointed since this object was built is absent.
    const std::map<std::uint32_t, wal::CheckpointAnchorRecord>& per_core() const noexcept {
        return per_core_;
    }

private:
    // The fold: the anchor slot 0 should hold, given everything published
    // so far. Minimum `redo_start_lsn`, and the rest of the fields come
    // from whichever core supplied that minimum, so the four numbers stay
    // one core's consistent set rather than a mix. Returns the mount-time
    // anchor while any core is still to publish (the warm-up above).
    wal::CheckpointAnchorRecord FoldedAnchor() const noexcept;

    // Slot 0 as the mount found it - the floor the warm-up holds to.
    static wal::CheckpointAnchorRecord MountAnchorOf(const SuperBlock& superblock) noexcept;

    SuperBlock& superblock_;
    storage::PageStore& store_;
    Logger* log_ = nullptr;
    std::uint64_t publishes_ = 0;
    wal::CheckpointAnchorRecord mount_anchor_{};

    // Ordered, so the fold and the log line are deterministic when two
    // cores tie on the minimum.
    std::map<std::uint32_t, wal::CheckpointAnchorRecord> per_core_;
};

}  // namespace kds::server
