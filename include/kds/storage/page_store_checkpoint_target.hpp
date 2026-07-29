#pragma once

#include <span>
#include <vector>

#include "kds/storage/device_page_store.hpp"
#include "kds/wal/checkpointer.hpp"

// Adapter: a DevicePageStore seen as the thing a checkpoint flushes
// (wal.md section 11-2). The sibling of BufferPoolCheckpointTarget, for
// the store the server actually runs on today - the buffer pool is not on
// the server's path yet, and the checkpointer must not care which it is.
//
// ---- What this target can and cannot promise ----------------------------
//
// DevicePageStore predates the WAL gate: it stamps no `page_lsn` and holds
// no `WalDurability` seam, so a flush through it is *not* ordered against
// the log. That is sound only while nothing logs page mutations - which is
// where the engine is (wal.md section 12 recovery does not exist, and no
// mutation path appends records). Every page therefore reports recLSN 0,
// which the checkpointer reads as "nothing to replay" rather than "replay
// from the head of the log" (see Checkpointer::Start).
//
// So what a checkpoint over this target buys today is the *flush*, not
// bounded recovery: dirty pages reach the platter on a cadence instead of
// only at SYNC and clean shutdown. When mutations start logging, this
// target has to grow the gate - or the server has to move onto BufferPool,
// which already has it - before the checkpoint's redo start means anything.
// Recording that here rather than in a commit message: the gap is not
// visible from the checkpointer's side.

namespace kds::storage {

class PageStoreCheckpointTarget final : public wal::CheckpointTarget {
public:
    explicit PageStoreCheckpointTarget(DevicePageStore& store) noexcept : store_(store) {}

    std::vector<wal::CheckpointDirtyPage> DirtyTable() const override {
        std::vector<wal::CheckpointDirtyPage> table;
        for (const PageId page_id : store_.DirtyPageIds()) {
            table.push_back(wal::CheckpointDirtyPage{page_id, /*rec_lsn=*/0});
        }
        return table;
    }

    Status FlushPages(std::span<const PageId> page_ids) override {
        return store_.FlushPages(page_ids);
    }

private:
    DevicePageStore& store_;
};

}  // namespace kds::storage
