#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/storage/insert_placement.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"
#include "kds/wal/stream.hpp"

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

// **A B+ tree split, as one record** (BTREE_SPLIT, BD-R12): every page a
// split's `InsertPlacement::changes()` names imaged into a single record whose
// envelope names the first,
// and every page stamped with its LSN. One record is what makes the split
// replay whole or not at all - its CRC ends the log before a torn one -
// where a run of FULL_PAGE_IMAGEs could end between the parent and the
// link (BD-S1's `SortedLeafCrashTest.ALogCutInsideAnAppendSplit...`). The caller holds every page, so the images are
// the split as it finished.
//
// **The record fits the stream whole** (BD-R12's size rule): a split writes
// at most `kMaxStructuralChanges` pages, and their images are under the
// default ring, the smaller of the two bounds `WalStream::Append` holds a
// record to (`WalManager::usable_payload_bytes`).
static_assert(wal::BtreeSplitSize(kMaxStructuralChanges) + wal::kRecordHeaderSize <=
              wal::kDefaultRingCapacity);
inline Status LogBtreeSplit(wal::WalManager* wal, PageStore& store, std::uint64_t txn_id,
                            std::span<const StructuralChange> pages) {
    if (wal == nullptr) return Status::OK();
    if (pages.empty()) return Status::InvalidArgument("a BTREE_SPLIT names at least one page");

    std::vector<PageRef> held;
    std::vector<wal::BtreeSplitImage> images;
    held.reserve(pages.size());
    images.reserve(pages.size());
    for (const StructuralChange& page : pages) {
        auto bytes = store.Get(page.page_id);
        if (!bytes.ok()) return bytes.status();
        held.push_back(std::move(bytes.value()));
        images.push_back({page.page_id, held.back().bytes()});
    }
    std::vector<std::byte> payload(wal::BtreeSplitSize(images.size()));
    if (auto n = wal::EncodeBtreeSplit(payload, images); !n.ok()) return n.status();
    auto rec =
        wal->Append(wal::RecordSpec{wal::RecordType::kBtreeSplit, txn_id, pages.front().page_id},
                    payload);
    if (!rec.ok()) return rec.status();
    held.clear();
    for (const StructuralChange& page : pages) {
        if (Status s = store.StampPageLsn(page.page_id, rec.value()); !s.ok()) return s;
    }
    return Status::OK();
}

}  // namespace kds::storage
