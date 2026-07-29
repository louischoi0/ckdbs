#pragma once

#include <span>
#include <vector>

#include "kds/storage/page_mgr/page_mgr.hpp"
#include "kds/wal/checkpointer.hpp"

// Adapter: a per-core BufferPool seen as the thing a checkpoint flushes
// (wal.md section 11-2, page.md section 8 - "checkpointer and background
// writer drive flushes through the same path").
//
// It lives on the storage side of the seam on purpose. The checkpointer
// must not depend on the buffer pool - it is WAL policy, testable against
// a scripted target - while the pool has no reason to know what a
// checkpoint is. This class is the only place the two names meet.

namespace kds::storage {

class BufferPoolCheckpointTarget final : public wal::CheckpointTarget {
public:
    explicit BufferPoolCheckpointTarget(BufferPool& pool) noexcept : pool_(pool) {}

    std::vector<wal::CheckpointDirtyPage> DirtyTable() const override {
        std::vector<wal::CheckpointDirtyPage> table;
        for (const BufferPool::DirtyPage& page : pool_.DirtyTable()) {
            table.push_back(wal::CheckpointDirtyPage{page.page_id, page.rec_lsn});
        }
        return table;
    }

    Status FlushPages(std::span<const PageId> page_ids) override {
        return pool_.FlushPages(page_ids);
    }

private:
    BufferPool& pool_;
};

}  // namespace kds::storage
