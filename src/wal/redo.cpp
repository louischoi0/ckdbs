#include "kds/wal/redo.hpp"

#include <cstring>
#include <optional>
#include <map>
#include <set>
#include <string>

#include "kds/base/function_ref.hpp"
#include "kds/storage/anchor_page.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/cabin_bound_page.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/index/index_page.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/varheap.hpp"
#include "kds/txn/undo_page.hpp"
#include "kds/wal/log_scanner.hpp"
#include "kds/wal/payload.hpp"

namespace kds::wal {
namespace {

// Pages the store could not load because their checksum failed. Their
// records are skipped until an FPI restores them; one still here when the
// scan ends is the failure the FPI was supposed to prevent.
using PoisonedPages = std::set<PageId>;

bool IsAssertionRecord(RecordType type) noexcept {
    switch (type) {
        case RecordType::kAssertReserve:
        case RecordType::kAssertCommit:
        case RecordType::kAssertRollback:
        case RecordType::kAssertBuild:
        case RecordType::kAssertDrop:
            return true;
        default:
            return false;
    }
}

// A record with no page target changes no page: transaction boundaries and
// checkpoint control. Analysis read them; redo has nothing to apply.
bool TouchesNoPage(RecordType type) noexcept {
    switch (type) {
        // PAGE_HANDOFF names a page in its envelope but describes no
        // mutation of it - it is an ownership fact - so there is nothing to
        // apply and redo must not fault the page to discover that. Analysis
        // is its only consumer, and since AM-S4(d) only for `max_page_id`
        // (PW1c-2, `analysis.cpp`).
        case RecordType::kPageHandoff:
        case RecordType::kTxnBegin:
        case RecordType::kTxnCommit:
        case RecordType::kTxnAbort:
        // TXN_PREPARE is a transaction boundary like the three above it
        // (R6-3): it names no page, changes none, and says only that this
        // stream's own records for the transaction are durable. Its reader
        // is analysis, at R6-4.
        case RecordType::kTxnPrepare:
        case RecordType::kCheckpointBegin:
        case RecordType::kCheckpointEnd:
        case RecordType::kPad:
            return true;
        default:
            return false;
    }
}

// Formats the page the record describes, then gives it the stamp of the
// core that logged the record - **not** of the core replaying it. This is
// the only place redo writes a stamp at all, the apply path's restamp
// having gone with per-core streams (AM-S4(d)), and the logging core is the
// only right answer: the stamp is what `device_page_store` reads at the
// next fault to decide who may write the page, so stamping the recovering
// core would hand it one it does not own.
Status ApplyPageInit(std::span<std::byte, kPageSize> page, const DecodedRecord& record) {
    auto fields = DecodePageInit(record.payload);
    if (!fields.ok()) {
        return fields.status();
    }
    const auto type = static_cast<PageType>(fields.value().page_type);

    // A heap page (and a btree leaf, which is one) needs its min_key set at
    // format time and never again - invariant 2. Every other class formats
    // through the common header alone.
    // The owner rides the record (page.md §2a): a 12-byte pre-§2a record
    // decodes as owner 0, so replaying old log leaves old pages exactly as
    // unattributed as the build that wrote them did.
    const std::uint64_t owner_oid = fields.value().owner_oid;
    // Every format below clears the header's flags word, which is where the
    // stamp lives, so the stamp is written after whichever one ran.
    const std::uint16_t stamp = storage::StreamStampFor(LoggingCoreOf(record.header.flags));
    if (type == PageType::kHeap || type == PageType::kBtreeLeaf) {
        auto view = heap::PageView::CreateEmptyAs(page, fields.value().min_key, type, owner_oid);
        if (!view.ok()) {
            return view.status();
        }
        storage::SetPageStreamStamp(page, stamp);
        return Status::OK();
    }
    if (type == PageType::kVarHeap) {
        if (Status s = varheap::FormatPage(page, owner_oid); !s.ok()) {
            return s;
        }
        storage::SetPageStreamStamp(page, stamp);
        return Status::OK();
    }
    if (type == PageType::kCabinBound) {
        // Not the generic arm below: BoundCabinPage::Format writes a body -
        // entry_count 0 and next_page_id = kInvalidPageId - and a zeroed
        // body reads next_page_id as page 0, so AdoptChain on a redone root
        // walked into the superblock and the assertion could not be revived
        // (found when the sys.assertions row first survived a crash;
        // pre-existing since the chain's PAGE_INIT was first logged).
        // The record's owner_oid is deliberately ignored: Format stamps 0,
        // Grow logs 0, and honoring a nonzero one here would be the
        // divergence, not the fidelity.
        if (Status s = storage::cabin::BoundCabinPage::Format(page); !s.ok()) {
            return s;
        }
        storage::SetPageStreamStamp(page, stamp);
        return Status::OK();
    }
    storage::FormatPage(page, type, /*flags=*/0, owner_oid);
    storage::SetPageStreamStamp(page, stamp);
    return Status::OK();
}

Status ApplyHeapWrite(std::span<std::byte, kPageSize> page, const DecodedRecord& record) {
    // A B+ tree leaf's rows arrive as BTREE_INSERT (BD-R3 E2): a HEAP_INSERT
    // naming one would replay a mid-leaf row over its neighbour through the
    // overwrite arm below, so it is not this page's record.
    if (record.type() == RecordType::kHeapInsert &&
        storage::RawPageType(page) == static_cast<std::uint8_t>(PageType::kBtreeLeaf)) {
        return Status::Corruption("HEAP_INSERT names a B+ tree leaf, whose rows are BTREE_INSERTs");
    }
    auto decoded = DecodeHeapWrite(record.payload);
    if (!decoded.ok()) {
        return decoded.status();
    }
    heap::PageView view(page);
    return view.RedoWriteTuple(decoded.value().fields.slot, decoded.value().tuple,
                               decoded.value().fields.trx_id, decoded.value().fields.undo_ptr);
}

Status ApplyDeleteMark(std::span<std::byte, kPageSize> page, const DecodedRecord& record) {
    auto fields = DecodeHeapDeleteMark(record.payload);
    if (!fields.ok()) {
        return fields.status();
    }
    heap::PageView view(page);
    // DeleteMark re-stamps rather than failing on an already-marked slot,
    // so this is idempotent without a special case - and rollback's clear
    // is HEAP_DELETE_UNMARK, a *different* record, so redo never has to
    // guess which direction a mark record meant.
    //
    // **That was not true until 2026-08-11 and this comment asserted it
    // anyway.** `TransactionManager::Compensate` logged kHeapDeleteMark to
    // clear a mark, so redo replayed a rollback's compensation as a second
    // delete - and a transaction that delete-marked a row and then aborted
    // came back from recovery with the row still deleted. RC05 added the
    // record type the comment had already assumed.
    return view.DeleteMark(fields.value().slot, fields.value().trx_id);
}

Status ApplyDeleteUnmark(std::span<std::byte, kPageSize> page, const DecodedRecord& record) {
    auto fields = DecodeHeapDeleteUnmark(record.payload);
    if (!fields.ok()) {
        return fields.status();
    }
    heap::PageView view(page);
    // Idempotent for the same reason its counterpart is: clearing a mark
    // that is already clear re-stamps the same header and moves no other
    // byte.
    return view.ClearDeleteMark(fields.value().slot, fields.value().trx_id,
                                fields.value().undo_ptr);
}

Status ApplySlotRetire(std::span<std::byte, kPageSize> page, const DecodedRecord& record) {
    auto fields = DecodeSlotRetire(record.payload);
    if (!fields.ok()) {
        return fields.status();
    }
    heap::PageView view(page);
    Status s = view.RetireSlot(fields.value().slot);
    // Already dead is the re-application case, not a failure: RetireSlot
    // answers NotFound for both "out of range" and "already dead", and
    // only the first is a real problem here. The slot count tells them
    // apart.
    if (s.code() == StatusCode::kNotFound && fields.value().slot < view.slot_count()) {
        return Status::OK();
    }
    return s;
}

// The record carries the undo record's **tail** - its bytes from
// `target_page_id` onward - plus the two chain links as payload fields
// (docs/spec/txn.md §3.5). Reassembling those two halves is the whole applier,
// and it is only possible because `LogUndoWrite` was corrected on
// 2026-08-10 to log the tail: before that the three fields naming *which
// tuple* a before-image belongs to lived on the page and nowhere in the
// log, and a chain rebuilt from it would have named page 0 slot 0.
Status ApplyUndoWrite(std::span<std::byte, kPageSize> page, const DecodedRecord& record) {
    auto decoded = DecodeUndoWrite(record.payload);
    if (!decoded.ok()) {
        return decoded.status();
    }
    auto tail = txn::DecodeUndoRecordTail(decoded.value().tail);
    if (!tail.ok()) {
        return tail.status();
    }

    // The links are the payload's; everything else is the tail's. One
    // assembly point, so neither half can be read from the wrong place.
    txn::UndoRecordFields fields = tail.value().fields;
    fields.prior_trx_id = decoded.value().fields.prior_trx_id;
    fields.prior_undo_ptr = decoded.value().fields.prior_undo_ptr;

    return txn::UndoPageWriteAt(page, decoded.value().fields.offset, fields, tail.value().image);
}

Status ApplyVarHeapAppend(std::span<std::byte, kPageSize> page, const DecodedRecord& record) {
    auto decoded = DecodeVarHeapAppend(record.payload);
    if (!decoded.ok()) {
        return decoded.status();
    }
    return varheap::PageWriteAt(page, decoded.value().fields.slot, decoded.value().value);
}

Status ApplyVarHeapRelease(std::span<std::byte, kPageSize> page, const DecodedRecord& record) {
    auto decoded = DecodeVarHeapRelease(record.payload);
    if (!decoded.ok()) {
        return decoded.status();
    }
    // No re-application test, because the applier *is* one (PageRelease is
    // idempotent). Every other applier here needs one because it writes
    // bytes that differ from what a second application would write.
    return varheap::PageRelease(page, decoded.value().slot);
}

Status ApplyIndexInsert(std::span<std::byte, kPageSize> page, const DecodedRecord& record) {
    auto decoded = DecodeIndexInsert(record.payload);
    if (!decoded.ok()) {
        return decoded.status();
    }
    index::IndexLeafView leaf(page);

    // An index entry's position is a function of its bytes (entries sort by
    // (key, pk), IX4b), so re-inserting reproduces the same slot rather
    // than appending a duplicate. The re-application check is therefore a
    // comparison against what is already there.
    const std::uint16_t slot = decoded.value().fields.slot;
    if (slot < leaf.entry_count()) {
        auto existing = leaf.Entry(slot);
        if (!existing.ok()) {
            return existing.status();
        }
        if (existing.value().size() == decoded.value().entry.size() &&
            std::memcmp(existing.value().data(), decoded.value().entry.data(),
                        existing.value().size()) == 0) {
            return Status::OK();
        }
    }

    auto landed = leaf.InsertEntry(decoded.value().entry);
    if (!landed.ok()) {
        return landed.status();
    }
    if (landed.value() != slot) {
        // The sort order put it somewhere the writer did not - so the leaf
        // is not in the state the record was written against.
        return Status::Corruption("redo placed an index entry at slot " +
                                  std::to_string(landed.value()) + ", but the record names " +
                                  std::to_string(slot));
    }
    return Status::OK();
}

// The siblings' stance (the 3f07eda review's C2): a record that is not this
// page's is refused, never applied - a heap page's body+4 read as an entry
// count is exactly the forged bound the anchor accessors refuse.
Status ApplyAnchorUpdate(std::span<std::byte, kPageSize> page, const DecodedRecord& record) {
    if (storage::RawPageType(page) != static_cast<std::uint8_t>(PageType::kAnchor)) {
        return Status::Corruption("ANCHOR_UPDATE names page type " +
                                  std::to_string(storage::RawPageType(page)) + ", not an anchor");
    }
    auto decoded = DecodeAnchorUpdate(record.payload);
    if (!decoded.ok()) {
        return decoded.status();
    }
    if (decoded.value().index_oid == 0) {
        storage::SetAnchorClusteredRoot(page, decoded.value().root);
        return Status::OK();
    }
    // A full or forged table here means the log and the page disagree -
    // Corruption whatever the accessor's own spelling (its
    // ResourceExhausted is a live-path answer, not a replay one).
    if (Status s = storage::SetAnchorIndexRoot(page, decoded.value().index_oid,
                                               decoded.value().root);
        !s.ok()) {
        return Status::Corruption(s.message());
    }
    return Status::OK();
}

// **A row placed where its key sorts** (BTREE_INSERT, BD-R3 E2). Strict:
// redo reaches this only for a page below the record's LSN, so the leaf is
// the one the record was written against, and the slot the record names is
// the one the live placement's search answered - `btree::SearchLeaf` on the
// same leaf. Anything else is a record that is not this page's, and is
// refused rather than placed.
Status ApplyBtreeInsert(std::span<std::byte, kPageSize> page, const DecodedRecord& record) {
    if (storage::RawPageType(page) != static_cast<std::uint8_t>(PageType::kBtreeLeaf)) {
        return Status::Corruption("BTREE_INSERT names page type " +
                                  std::to_string(storage::RawPageType(page)) +
                                  ", not a B+ tree leaf");
    }
    auto decoded = DecodeHeapWrite(record.payload);
    if (!decoded.ok()) return decoded.status();
    auto key = KeystoneIdOfPayload(decoded.value().tuple);
    if (!key.ok()) return key.status();
    heap::PageView view(page);
    auto at = btree::SearchLeaf(view, key.value());
    if (!at.ok()) return at.status();
    const std::uint16_t slot = decoded.value().fields.slot;
    if (at.value().present || at.value().at != slot) {
        return Status::Corruption("BTREE_INSERT of key " + std::to_string(key.value()) +
                                  " names slot " + std::to_string(slot) + ", where the leaf " +
                                  (at.value().present ? std::string("already holds the key")
                                                      : "sorts it at slot " +
                                                            std::to_string(at.value().at)));
    }
    auto placed = view.InsertTupleAt(slot, decoded.value().tuple, decoded.value().fields.trx_id,
                                     decoded.value().fields.undo_ptr);
    return placed.ok() ? Status::OK() : placed.status();
}

// The applier for a record that changes the one page its envelope names,
// or null for a type redo does not know (wal.md §5.2: a hard error, never
// skipped). FULL_PAGE_IMAGE and BTREE_SPLIT are whole-page records and are
// handled by the visitor itself.
using Applier = Status (*)(std::span<std::byte, kPageSize>, const DecodedRecord&);
Applier ApplierFor(RecordType type) noexcept {
    switch (type) {
        case RecordType::kPageInit: return ApplyPageInit;
        case RecordType::kHeapInsert:
        case RecordType::kHeapOverwrite: return ApplyHeapWrite;
        case RecordType::kBtreeInsert: return ApplyBtreeInsert;
        case RecordType::kHeapDeleteMark: return ApplyDeleteMark;
        case RecordType::kHeapDeleteUnmark: return ApplyDeleteUnmark;
        case RecordType::kSlotRetire: return ApplySlotRetire;
        case RecordType::kUndoWrite: return ApplyUndoWrite;
        case RecordType::kVarHeapAppend: return ApplyVarHeapAppend;
        case RecordType::kVarHeapRelease: return ApplyVarHeapRelease;
        case RecordType::kIndexInsert: return ApplyIndexInsert;
        case RecordType::kAnchorUpdate: return ApplyAnchorUpdate;
        default: return nullptr;
    }
}

}  // namespace

StatusOr<RedoStats> Redo(LogDevice& device, std::uint32_t core_id, storage::PageStore& store,
                         const AnalysisResult& analysis) {
    RedoStats stats;
    PoisonedPages poisoned;

    // One page's share of one record: the dirty filter, the load (or the
    // creation a whole-page record may make), the poison and `page_lsn`
    // gates, `apply`, and the stamp. A record names one page in its
    // envelope, except BTREE_SPLIT, whose payload names every page it
    // writes and which runs this once per page (BD-R12).
    const auto apply_page =
        [&](const DecodedRecord& record, PageId page_id, bool is_image,
            FunctionRef<Status(std::span<std::byte, kPageSize>)> apply) -> Status {

        // PW1c-2's filter: a record for a page analysis holds no dirty
        // entry for - or one below that entry's recLSN - describes state
        // already in the durable image, and is skipped **before the load**,
        // because a page recovery owes nothing to must not even be faulted.
        // What reaches it is the checkpoint-seeded entry: a page dirtied,
        // written back and re-dirtied carries a recLSN above its flushed
        // records, while the redo start is the minimum over every page and
        // lies below them. The departed-page case this also served is gone
        // with the topology - a handoff removes no entry now (AM-S4(d),
        // `analysis.cpp`) - and a first-wins entry is never above its own
        // page's records, so for the rest this skips nothing the RV5 gate
        // would have applied. The no-entry disjunct is unreachable from any
        // result `Analyze` produces now - every page-touching record
        // enters its page (`note_dirty`) - and is kept for the hand-built results this
        // function's tests pass it.
        const auto dirty = analysis.dirty_pages.find(page_id);
        if (dirty == analysis.dirty_pages.end() || record.header.lsn < dirty->second) {
            ++stats.skipped_not_dirty;
            return Status::OK();
        }

        // A page the log names may not exist in the store at all - the
        // crash can have lost the allocation that created it. PAGE_INIT and
        // an image both describe a whole page, so either can create one;
        // anything else needs the page to be there already.
        // Dynamic extent while the branches below decide *which* page this
        // is, narrowed to the fixed extent once past them. A
        // `span<byte, kPageSize>` cannot be declared here and assigned
        // there: a fixed-extent span has no default constructor, and the
        // store hands back dynamic spans besides.
        std::span<std::byte> page_bytes;
        // The pin outlives the branch that took it. A `PageRef` local
        // declared inside one of the `CreateAt` arms below would unpin at
        // that arm's closing brace, leaving `page_bytes` pointing into a
        // frame nothing holds - the exact lifetime the migration exists to
        // make visible (workplan-pageref.md §3, Shape C).
        storage::PageRef created_ref;
        auto got = store.Get(page_id);
        if (got.ok()) {
            page_bytes = got.value().bytes();
        } else if (got.status().code() == StatusCode::kCorruption) {
            // Checksum failure. Held, not failed: an image later in the
            // stream is exactly what heals this (page.md §10).
            if (!is_image) {
                poisoned.insert(page_id);
                return Status::OK();
            }
            auto created = store.CreateAt(page_id);
            if (!created.ok()) {
                return created.status();
            }
            created_ref = std::move(created.value());
            page_bytes = created_ref.bytes();
            if (poisoned.erase(page_id) != 0) {
                ++stats.pages_healed;
            }
        } else if (record.type() == RecordType::kPageInit || is_image) {
            auto created = store.CreateAt(page_id);
            if (!created.ok()) {
                return created.status();
            }
            created_ref = std::move(created.value());
            page_bytes = created_ref.bytes();
            ++stats.pages_created;
        } else {
            // Named, because "page id not found" alone says nothing about
            // which record described a page nobody has. Every logged
            // creation's first record is a PAGE_INIT or a page image (BF-S1's
            // Census A, CREATE TABLE's three pages included), so this is
            // reached by a page whose bit a reclaim cleared while the log
            // still named it - which BF-R4's gate exists to rule out - or by
            // damage.
            return got.status().WithContext(
                std::string("redo: ") + RecordTypeName(record.type()) + " at lsn " +
                std::to_string(record.header.lsn) + " names page " + std::to_string(page_id) +
                ", which the store does not hold and no PAGE_INIT or full page image in the "
                "replay range creates");
        }

        // Every path that reaches here assigned a whole page.
        const std::span<std::byte, kPageSize> page(page_bytes.data(), kPageSize);

        // A page still poisoned is one no image has healed yet; its
        // ordinary records cannot be applied to bytes nobody trusts.
        if (poisoned.count(page_id) != 0 && !is_image) {
            return Status::OK();
        }

        // No foreign-stamp check here. PL §9 rules 5-6 (PW1c-3) made a
        // reachable foreign stamp Corruption because it meant "this page
        // crossed into my stream without a logged handoff"; with one stream
        // for the instance (AM-S4(d)) there is no other stream to have
        // crossed from - every core's records are in this log by
        // construction, so a page stamped by core 2 met in core 0's mount
        // pass is core 2 owning its page and not evidence of anything lost.
        // The stamp survives as a claim (`device_page_store`'s
        // claim-at-fault), which is why redo leaves it alone.
        //
        // RV5, the whole of idempotence. An image is gated too: a page
        // already at or past this LSN does not need its image restored.
        if (storage::GetPageLsn(page) >= record.header.lsn) {
            ++stats.skipped_by_lsn;
            return Status::OK();
        }

        if (Status applied = apply(page); !applied.ok()) {
            return applied.WithContext("redo of " + std::string(RecordTypeName(record.type())) +
                                       " at lsn " + std::to_string(record.header.lsn) +
                                       " on page " + std::to_string(page_id));
        }
        if (is_image) {
            ++stats.page_images;
            if (poisoned.erase(page_id) != 0) {
                ++stats.pages_healed;
            }
        }

        // The page_lsn is what makes the next pass skip this record, and it
        // must follow the mutation: a stamp written first would mark the
        // page done for a change that then failed.
        storage::SetPageLsn(page, record.header.lsn);
        // No restamp rides it. PL §9 rule 4 had every applied page take the
        // replaying stream's stamp; with one stream (AM-S4(d)) the
        // recovering core is not the core that wrote the record, so that
        // would label core 2's pages core 0's. The stamp decides nothing
        // since AW-S1b took `device_page_store`'s claim-at-fault
        // (`page_header.hpp`); it stays truthful about which core last
        // wrote the page. `ApplyPageInit` is the one path that still writes
        // a stamp, from `LoggingCoreOf(record.header.flags)` - the record's
        // own core, not the recovering core. An image writes none: its memcpy
        // restores whatever stamp the captured image carried.
        ++stats.applied;
        return Status::OK();
    };

    const auto copy_image = [](std::span<const std::byte> image) {
        return [image](std::span<std::byte, kPageSize> page) {
            std::memcpy(page.data(), image.data(), kPageSize);
            return Status::OK();
        };
    };

    const auto visit = [&](const DecodedRecord& record) -> Status {
        ++stats.records;

        if (TouchesNoPage(record.type())) {
            ++stats.no_page;
            return Status::OK();
        }
        if (IsAssertionRecord(record.type())) {
            // RC07's, and blocked on assertion.md §7's genesis
            // decision. Counted rather than dropped so the deferral shows
            // up in the report instead of being assumed.
            ++stats.deferred_assertions;
            return Status::OK();
        }
        if (record.header.page_id == kInvalidPageId) {
            ++stats.no_page;
            return Status::OK();
        }

        const PageId page_id = record.header.page_id;
        if (record.type() == RecordType::kFullPageImage) {
            auto image = DecodeFullPageImage(record.payload);
            if (!image.ok()) {
                return image.status();
            }
            return apply_page(record, page_id, /*is_image=*/true, copy_image(image.value()));
        }
        if (record.type() == RecordType::kBtreeSplit) {
            // Whole or not at all is the record's CRC's to give (a torn
            // record ends the log before it); here each page it names is
            // applied through its own gates, as if it were an image of its
            // own.
            auto images = DecodeBtreeSplit(record.payload);
            if (!images.ok()) {
                return images.status();
            }
            for (const BtreeSplitImage& image : images.value()) {
                if (Status s = apply_page(record, image.page_id, /*is_image=*/true,
                                          copy_image(image.image));
                    !s.ok()) {
                    return s;
                }
            }
            return Status::OK();
        }
        const Applier applier = ApplierFor(record.type());
        if (applier == nullptr) {
            // wal.md §5.2: an unknown type during replay is a hard error,
            // never skipped. ALLOC/FREE land here too - both are reserved and
            // emitted by nothing, a page being freed without a record (BF-R4)
            // - so one in a stream means the stream was written by something
            // this build does not know.
            return Status::Corruption("redo: no applier for record type " +
                                      std::string(RecordTypeName(record.type())) + " at lsn " +
                                      std::to_string(record.header.lsn));
        }
        return apply_page(record, page_id, /*is_image=*/false,
                          [&](std::span<std::byte, kPageSize> page) {
                              return applier(page, record);
                          });
    };

    auto scan = ScanLog(device, core_id, analysis.redo_start_lsn, visit);
    if (!scan.ok()) {
        return scan.status();
    }

    if (!poisoned.empty()) {
        return Status::Corruption(
            "redo: page " + std::to_string(*poisoned.begin()) +
            " failed its checksum and no full page image restored it; the log cannot heal this "
            "page (docs/spec/page.md §10)");
    }
    return stats;
}

}  // namespace kds::wal
