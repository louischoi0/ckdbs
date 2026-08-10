#include "kds/wal/redo.hpp"

#include <cstring>
#include <map>
#include <set>
#include <string>

#include "kds/storage/heap/heap_page.hpp"
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
        case RecordType::kTxnBegin:
        case RecordType::kTxnCommit:
        case RecordType::kTxnAbort:
        case RecordType::kCheckpointBegin:
        case RecordType::kCheckpointEnd:
        case RecordType::kPad:
            return true;
        default:
            return false;
    }
}

Status ApplyPageInit(std::span<std::byte, kPageSize> page, const DecodedRecord& record) {
    auto fields = DecodePageInit(record.payload);
    if (!fields.ok()) {
        return fields.status();
    }
    const auto type = static_cast<PageType>(fields.value().page_type);

    // A heap page (and a btree leaf, which is one) needs its min_key set at
    // format time and never again - invariant 2. Every other class formats
    // through the common header alone.
    if (type == PageType::kHeap || type == PageType::kBtreeLeaf) {
        auto view = heap::PageView::CreateEmptyAs(page, fields.value().min_key, type);
        if (!view.ok()) {
            return view.status();
        }
        return Status::OK();
    }
    if (type == PageType::kVarHeap) {
        return varheap::FormatPage(page);
    }
    storage::FormatPage(page, type);
    return Status::OK();
}

Status ApplyHeapWrite(std::span<std::byte, kPageSize> page, const DecodedRecord& record) {
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
    // so this is idempotent without a special case - and rollback's
    // ClearDeleteMark is a *different* record, so redo never has to guess
    // which direction a mark record meant.
    return view.DeleteMark(fields.value().slot, fields.value().trx_id);
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

// UNDO_WRITE cannot be redone with today's payload, and this is a format
// gap rather than missing work here. See the block comment in redo.hpp and
// docs/workplan-wal-recovery.md RC03's finding.
//
// The on-page undo record (txn::UndoRecordFields, 28 bytes) carries
// `target_page_id`, `target_slot` and `type` - which is how the undo phase
// knows *which tuple* a before-image belongs to. `wal::UndoWritePayload`
// carries only `prior_trx_id`, `prior_undo_ptr`, `offset` and `image_len`,
// and `UndoLog::LogUndoWrite` passes the bare before-image as the record's
// bytes. So those three fields exist on the page and nowhere in the log.
//
// `txn.md` §3.5 says the payload "fits without amendment" and spells the
// mapping as `payload.image = record bytes [+16, +28 + image_len)` - the
// record's tail *including* those three fields. **The implementation does
// not do that**, and the difference is exactly the information recovery
// needs. Redoing this record as-is would rebuild an undo chain whose
// records point at page 0 slot 0, which is worse than not rebuilding it:
// the undo phase would roll back the wrong tuple rather than fail.
//
// So this refuses, loudly, and the mount fails whenever an UPDATE or
// DELETE is in the replay range. That is the honest state - the same
// discipline `physical_optimizer = on` follows by refusing at startup and
// naming its gates - and closing it is a decision about a persisted
// format, which this task does not get to take.
Status ApplyUndoWrite(std::span<std::byte, kPageSize> page, const DecodedRecord& record) {
    (void)page;
    (void)record;
    return Status::Unsupported(
        "redo of UNDO_WRITE is blocked on a format decision: the record carries the chain links "
        "and the before-image but not the undo record's target_page_id, target_slot or type, so "
        "the rebuilt chain would not name the tuple it restores. Either UndoLog::LogUndoWrite "
        "must log the record's tail as docs/txn.md section 3.5 specifies, or UndoWritePayload "
        "must gain the three fields (docs/workplan-wal-recovery.md RC03)");
}

Status ApplyVarHeapAppend(std::span<std::byte, kPageSize> page, const DecodedRecord& record) {
    auto decoded = DecodeVarHeapAppend(record.payload);
    if (!decoded.ok()) {
        return decoded.status();
    }
    return varheap::PageWriteAt(page, decoded.value().fields.slot, decoded.value().value);
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

}  // namespace

StatusOr<RedoStats> Redo(LogDevice& device, std::uint32_t core_id, storage::PageStore& store,
                         const AnalysisResult& analysis) {
    RedoStats stats;
    PoisonedPages poisoned;

    const auto visit = [&](const DecodedRecord& record) -> Status {
        ++stats.records;

        if (TouchesNoPage(record.type())) {
            ++stats.no_page;
            return Status::OK();
        }
        if (IsAssertionRecord(record.type())) {
            // RC07's, and blocked on feat-assertion.md §7's genesis
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
        const bool is_image = record.type() == RecordType::kFullPageImage;

        // A page the log names may not exist in the store at all - the
        // crash can have lost the allocation that created it. PAGE_INIT and
        // an FPI both describe a whole page, so either can create one;
        // anything else needs the page to be there already.
        std::span<std::byte, kPageSize> page;
        auto got = store.Get(page_id);
        if (got.ok()) {
            page = got.value();
        } else if (got.status().code() == StatusCode::kCorruption) {
            // Checksum failure. Held, not failed: an FPI later in the
            // stream is exactly what heals this (page.md §10).
            if (!is_image) {
                poisoned.insert(page_id);
                return Status::OK();
            }
            auto created = store.CreateAt(page_id);
            if (!created.ok()) {
                return created.status();
            }
            page = created.value();
            if (poisoned.erase(page_id) != 0) {
                ++stats.pages_healed;
            }
        } else if (record.type() == RecordType::kPageInit || is_image) {
            auto created = store.CreateAt(page_id);
            if (!created.ok()) {
                return created.status();
            }
            page = created.value();
            ++stats.pages_created;
        } else {
            return got.status();
        }

        // A page still poisoned is one no image has healed yet; its
        // ordinary records cannot be applied to bytes nobody trusts.
        if (poisoned.count(page_id) != 0 && !is_image) {
            return Status::OK();
        }

        // RV5, the whole of idempotence. An FPI is gated too: a page
        // already at or past this LSN does not need its image restored.
        if (storage::GetPageLsn(page) >= record.header.lsn) {
            ++stats.skipped_by_lsn;
            return Status::OK();
        }

        Status applied = Status::OK();
        switch (record.type()) {
            case RecordType::kFullPageImage: {
                auto image = DecodeFullPageImage(record.payload);
                if (!image.ok()) {
                    return image.status();
                }
                std::memcpy(page.data(), image.value().data(), kPageSize);
                ++stats.page_images;
                if (poisoned.erase(page_id) != 0) {
                    ++stats.pages_healed;
                }
                break;
            }
            case RecordType::kPageInit:
                applied = ApplyPageInit(page, record);
                break;
            case RecordType::kHeapInsert:
            case RecordType::kHeapOverwrite:
                applied = ApplyHeapWrite(page, record);
                break;
            case RecordType::kHeapDeleteMark:
                applied = ApplyDeleteMark(page, record);
                break;
            case RecordType::kSlotRetire:
                applied = ApplySlotRetire(page, record);
                break;
            case RecordType::kUndoWrite:
                applied = ApplyUndoWrite(page, record);
                break;
            case RecordType::kVarHeapAppend:
                applied = ApplyVarHeapAppend(page, record);
                break;
            case RecordType::kIndexInsert:
                applied = ApplyIndexInsert(page, record);
                break;
            default:
                // wal.md §5.2: an unknown type during replay is a hard
                // error, never skipped. ALLOC/FREE land here too - they are
                // assigned and emitted by nothing (page.md §5's
                // SpaceManager is unbuilt), so one in a stream means the
                // stream was written by something this build does not know.
                return Status::Corruption(
                    "redo: no applier for record type " +
                    std::string(RecordTypeName(record.type())) + " at lsn " +
                    std::to_string(record.header.lsn));
        }
        if (!applied.ok()) {
            return applied.WithContext("redo of " + std::string(RecordTypeName(record.type())) +
                                       " at lsn " + std::to_string(record.header.lsn) +
                                       " on page " + std::to_string(page_id));
        }

        // The stamp is what makes the next pass skip this record, and it
        // must follow the mutation: a stamp written first would mark the
        // page done for a change that then failed.
        storage::SetPageLsn(page, record.header.lsn);
        ++stats.applied;
        return Status::OK();
    };

    auto scan = ScanLog(device, core_id, analysis.redo_start_lsn, visit);
    if (!scan.ok()) {
        return scan.status();
    }

    if (!poisoned.empty()) {
        return Status::Corruption(
            "redo: page " + std::to_string(*poisoned.begin()) +
            " failed its checksum and no full page image restored it; the log cannot heal this "
            "page (docs/page.md §10)");
    }
    return stats;
}

}  // namespace kds::wal
