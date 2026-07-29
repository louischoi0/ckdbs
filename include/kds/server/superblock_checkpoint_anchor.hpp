#pragma once

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

namespace kds::server {

class SuperBlockCheckpointAnchor final : public wal::CheckpointAnchor {
public:
    // Both references must outlive the anchor, and `superblock` must be the
    // same instance the rest of the server mutates - the one bootstrap
    // produced - or a later Encode() from elsewhere would write back a copy
    // with no anchor in it.
    SuperBlockCheckpointAnchor(SuperBlock& superblock, storage::PageStore& store) noexcept
        : superblock_(superblock), store_(store) {}

    // Diagnostic log, null (discard) by default; `log` must outlive this.
    void SetLogger(Logger* log) noexcept { log_ = log; }

    Status Publish(const wal::CheckpointAnchorRecord& anchor) override;

    // Anchors published through this object since it was constructed. The
    // checkpoint-duration/cadence counterpart of WalStats; also what the
    // tests assert against.
    std::uint64_t publishes() const noexcept { return publishes_; }

private:
    SuperBlock& superblock_;
    storage::PageStore& store_;
    Logger* log_ = nullptr;
    std::uint64_t publishes_ = 0;
};

}  // namespace kds::server
