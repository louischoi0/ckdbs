#include "kds/exec/clean_dropped.hpp"

#include <algorithm>
#include <string>

#include "kds/exec/catalog_spills.hpp"
#include "kds/exec/wal_row_log.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/txn/varheap_release.hpp"

namespace kds::exec {

std::vector<std::uint16_t> DroppedColumns(const catalog::TableAccess& access) {
    std::vector<std::uint16_t> out;
    if (!access.layout.has_dropped()) return out;
    for (std::size_t i = 1; i < access.schema.columns.size(); ++i) {
        if (access.schema.columns[i].dropped()) out.push_back(static_cast<std::uint16_t>(i));
    }
    return out;
}

bool DroppedCellsClean(const catalog::TableAccess& access, std::span<const std::uint16_t> targets,
                       std::span<const std::byte> payload) {
    const catalog::RowLayout& layout = access.layout;
    // Invariant 13: a payload of another length is not interpreted; read as
    // dirty, so the clean's own check under the hold refuses it Corruption.
    if (payload.size() != layout.row_size) return false;
    for (const std::uint16_t col : targets) {
        if (layout.null_bits[col] != catalog::kNoNullBit &&
            !catalog::NullBitIsSet(payload, layout, col)) {
            return false;
        }
        const auto cell = payload.subspan(layout.offsets[col],
                                          layout.cell_ends[col] - layout.offsets[col]);
        if (std::any_of(cell.begin(), cell.end(), [](std::byte b) { return b != std::byte{0}; })) {
            return false;
        }
    }
    return true;
}

StatusOr<CleanResult> CleanDroppedCells(storage::PageStore& store, wal::WalManager* wal,
                                        const catalog::TableAccess& access,
                                        std::span<const std::uint16_t> targets, std::uint64_t id,
                                        FunctionRef<bool(std::uint64_t)> resolved) {
    auto at = btree::BtreeLookup(store, access.desc_page_id, id, storage::PageAccess::kWrite);
    if (!at.ok()) {
        if (at.status().code() == StatusCode::kNotFound) return CleanResult{};
        return at.status();
    }
    btree::Location& row = at.value();
    heap::PageView leaf(row.leaf.bytes());

    // Judged and copied under the hold, before anything is written, so a
    // cell that cannot be read refuses the key with the page untouched.
    std::vector<std::byte> image;
    std::vector<SpillRef> spills;
    std::uint64_t trx_id = 0;
    std::uint64_t undo_ptr = 0;
    {
        auto tuple = leaf.ReadTuple(row.slot);
        if (!tuple.ok()) return tuple.status();
        const std::span<const std::byte> payload = tuple.value().payload;
        if (payload.size() != access.layout.row_size) {
            return Status::Corruption("clean of tuple key " + std::to_string(id) +
                                      ": its payload is " + std::to_string(payload.size()) +
                                      " bytes for a relation whose row size is " +
                                      std::to_string(access.layout.row_size));
        }
        if (DroppedCellsClean(access, targets, payload)) {
            return CleanResult{CleanOutcome::kAlreadyClean};
        }
        trx_id = tuple.value().trx_id;
        if (!resolved(trx_id)) return CleanResult{CleanOutcome::kUnresolved, trx_id};
        undo_ptr = tuple.value().undo_ptr;
        for (const std::uint16_t col : targets) {
            if (Status s = ColumnSpills(access, payload, col, spills); !s.ok()) return s;
        }
        image.assign(payload.begin(), payload.end());
    }
    const catalog::RowLayout& layout = access.layout;
    for (const std::uint16_t col : targets) {
        std::fill(image.begin() + layout.offsets[col], image.begin() + layout.cell_ends[col],
                  std::byte{0});
        if (layout.null_bits[col] != catalog::kNoNullBit) catalog::SetNullBit(image, layout, col);
    }

    // The clean and its record, under the hold (AT-S21).
    if (Status s = leaf.OverwriteTuple(row.slot, image, trx_id, undo_ptr); !s.ok()) return s;
    if (Status s = LogHeapWrite(wal, store, wal::RecordType::kHeapOverwrite, wal::kNoTxnId,
                                row.page_id, row.slot, image, trx_id, undo_ptr);
        !s.ok()) {
        return s;
    }
    row.leaf.Release();  // the hold goes before any var-heap page is fetched

    // Distinct by construction (CD-S0 P2: no two cells of one version share
    // a spill); made so anyway, since a second release of one slot would
    // report a defect that is not there.
    std::sort(spills.begin(), spills.end());
    spills.erase(std::unique(spills.begin(), spills.end()), spills.end());
    for (const auto& [page_id, slot] : spills) {
        auto released = txn::ReleaseVarHeapSlot(store, wal, wal::kNoTxnId, page_id, slot);
        // **The row is clean already** - its record is logged - so a refusal
        // here says so: its spills from this one on leak, the gap a crash
        // between the two records leaves.
        if (!released.ok()) {
            return released.status().WithMessage(
                released.status().message() + "; tuple key " + std::to_string(id) +
                " is clean, and its spill at page " + std::to_string(page_id) + " slot " +
                std::to_string(slot) + " and any after it are not released");
        }
        // The cell named the slot an instant ago under the hold, and only a
        // reclaim of the row frees it, so its absence is a defect.
        if (released.value() == txn::ReleaseOutcome::kNothingToRelease) {
            return Status::Corruption("clean of tuple key " + std::to_string(id) +
                                      ": its dropped cell's spill at page " +
                                      std::to_string(page_id) + " slot " +
                                      std::to_string(slot) + " does not exist");
        }
    }
    return CleanResult{CleanOutcome::kCleaned, trx_id,
                       static_cast<std::uint32_t>(spills.size())};
}

}  // namespace kds::exec
