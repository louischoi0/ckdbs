#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"

// The one FULL_PAGE_IMAGE writer (review S1 of
// docs/workplan-rv3-catalog-recovery.md). Four sites used to hand-copy
// these lines - the dispatcher's structural changes, the catalog's
// chain-link edit, the built index tree, the assertion build - and the
// dispatcher's own comment named the failure mode: "the stamp is the half
// a hand-copied block loses". Redo gates on `page_lsn`, so an unstamped
// image replays a record whose effect the page already holds.
//
// Lives in storage rather than wal because the WAL layer owns no
// PageStore by design (wal.md: segments are not pages); storage already
// depends on the WAL through the write gate.

namespace kds::storage {

inline Status LogFullPageImage(wal::WalManager* wal, PageStore& store, std::uint64_t txn_id,
                               PageId page_id) {
    if (wal == nullptr) return Status::OK();

    auto bytes = store.Get(page_id);
    if (!bytes.ok()) return bytes.status();
    std::vector<std::byte> image(wal::kFullPageImagePayloadSize);
    if (auto n = wal::EncodeFullPageImage(
            image, std::span<const std::byte, kPageSize>(bytes.value().bytes()));
        !n.ok()) {
        return n.status();
    }
    auto fpi = wal->Append(wal::RecordSpec{wal::RecordType::kFullPageImage, txn_id, page_id},
                           image);
    if (!fpi.ok()) return fpi.status();
    return store.StampPageLsn(page_id, fpi.value());
}

}  // namespace kds::storage
