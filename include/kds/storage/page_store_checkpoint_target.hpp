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
// ---- What this target promises ------------------------------------------
//
// The gate and the dirty table are both the store's (since
// INSERT started logging): DevicePageStore::SetWalGate() orders every one
// of its write paths against the log, and DirtyPagesWithRecLsn() carries a
// real per-frame recLSN, so this adapter is a straight projection of both
// and holds no policy of its own.
//
// A page reports recLSN 0 when nothing *logged* dirtied it - catalog rows
// and bootstrap, which still write outside the log. The checkpointer reads
// that as "nothing to replay for this page" and leaves it out of the redo
// start, which is accurate for those paths and would be a lie for a logged
// one. That is why every logged mutation must call StampPageLsn(); the
// obligation is on the mutation path, and it is not checkable from here.

namespace kds::storage {

class PageStoreCheckpointTarget final : public wal::CheckpointTarget {
public:
    explicit PageStoreCheckpointTarget(DevicePageStore& store) noexcept : store_(store) {}

    std::vector<wal::CheckpointDirtyPage> DirtyTable() const override {
        std::vector<wal::CheckpointDirtyPage> table;
        for (const auto& [page_id, rec_lsn] : store_.DirtyPagesWithRecLsn()) {
            table.push_back(wal::CheckpointDirtyPage{page_id, rec_lsn});
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
