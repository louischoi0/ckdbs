#pragma once

#include <algorithm>
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

// The one FULL_PAGE_IMAGE writer: append the image, then stamp the page.
// One function so the stamp cannot be lost from a hand-copied block - redo
// gates on `page_lsn`, so an unstamped image replays a record whose effect
// the page already holds.
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
// split wrote imaged into a single record whose envelope names the first,
// and every page stamped with its LSN. One record is what makes the split
// replay whole or not at all - its CRC ends the log before a torn one -
// where a run of FULL_PAGE_IMAGEs could end between the parent and the
// link (BD-S1's `SortedLeafCrashTest.ALogCutInsideAnAppendSplit...`). The
// caller holds every page another writer could reach - the leaf and the
// parents it secured, `held` - so the images are the split as it finished.
//
// **No page is pinned twice.** A held page, and the anchor below, is imaged
// through the caller's hold; a page the split created, which nothing reaches
// but through a held one, is fetched, copied and unpinned before the next, so
// imaging adds at most one pin to the split's own and a deep split stays
// within the pool's per-pinner ceiling (`kPinCeiling`). Unpinned, a created
// page can be written back and evicted before its stamp; its disk copy is
// then this record's image, which redo replays over a lower `page_lsn`, so
// its stamp's `NotFound` is not a failure.
//
// **A split that grew a root passes the relation's anchor page too**, its
// root slot already moved and still held by the caller
// (`Catalog::StageAnchorRoot`): the root a mount reads is then replayed with
// the split or not at all, and never names a root the log did not keep or
// misses one it did. Used by the clustered tree and a secondary index alike.
//
// **The record fits the stream whole** (BD-R12's size rule): a split writes
// at most `kMaxStructuralChanges` pages (`index::kMaxIndexChanges` for an
// index, asserted where it is logged), one more for the anchor, and that is
// `wal::kMaxBtreeSplitPages`, which every stream's ring and segment floors
// hold (`stream.hpp`) - so the append is never refused for its size after
// the pages changed.
static_assert(kMaxStructuralChanges + 1 <= wal::kMaxBtreeSplitPages);
inline Status LogBtreeSplit(wal::WalManager* wal, PageStore& store, std::uint64_t txn_id,
                            std::span<const PageId> pages, std::span<const PageRef> held,
                            const PageRef* anchor = nullptr) {
    if (wal == nullptr) return Status::OK();
    if (pages.empty()) return Status::InvalidArgument("a BTREE_SPLIT names at least one page");

    std::vector<std::vector<std::byte>> created;
    std::vector<wal::BtreeSplitImage> images;
    created.reserve(pages.size());
    images.reserve(pages.size() + 1);
    for (const PageId page_id : pages) {
        const auto hold = std::ranges::find(held, page_id, &PageRef::page_id);
        if (hold != held.end()) {
            images.push_back({page_id, hold->bytes()});
            continue;
        }
        auto page = store.GetForRead(page_id);
        if (!page.ok()) return page.status();
        const auto bytes = page.value().bytes();
        created.emplace_back(bytes.begin(), bytes.end());
        images.push_back({page_id, created.back()});
    }
    if (anchor != nullptr) images.push_back({anchor->page_id(), anchor->bytes()});
    std::vector<std::byte> payload(wal::BtreeSplitSize(images.size()));
    if (auto n = wal::EncodeBtreeSplit(payload, images); !n.ok()) return n.status();
    auto rec =
        wal->Append(wal::RecordSpec{wal::RecordType::kBtreeSplit, txn_id, pages.front()}, payload);
    if (!rec.ok()) return rec.status();
    for (const wal::BtreeSplitImage& image : images) {
        Status s = store.StampPageLsn(image.page_id, rec.value());
        const bool copied = std::ranges::any_of(
            created, [&](const std::vector<std::byte>& c) { return c.data() == image.image.data(); });
        if (!s.ok() && !(copied && s.code() == StatusCode::kNotFound)) return s;
    }
    return Status::OK();
}

}  // namespace kds::storage
