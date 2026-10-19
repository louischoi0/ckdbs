#include "kds/exec/catalog_spills.hpp"

#include <algorithm>

#include "kds/catalog/well_known.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/tagged_cell.hpp"
#include "kds/storage/varheap.hpp"
#include "kds/storage/visit.hpp"

namespace kds::exec {

Status ColumnSpills(const catalog::TableAccess& access, std::span<const std::byte> payload,
                    std::size_t col, std::vector<SpillRef>& out) {
    // **Only a `varchar` column is a tagged cell**, which is
    // `SchemaCanSpill`'s own rule and the reason it is the spill gate:
    // `char` is fixed by declaration and every numeric type by its
    // width, so neither can exceed its cell and neither carries a tag.
    // Column 0 is the Keystone word, a raw u64. Decoding any of them as
    // a cell reads a payload byte as a tag.
    if (col >= access.schema.columns.size()) return Status::OK();
    if (access.schema.columns[col].type_val != catalog::kTypeValVarchar) return Status::OK();
    const std::uint32_t offset = access.layout.offsets[col];
    if (offset >= payload.size()) return Status::OK();
    auto cell = storage::DecodeCell(payload.subspan(offset));
    // **A cell this build cannot read is not a licence to collect.** A
    // corrupt tag here means the row's references are unknown, and an
    // unknown reference must be treated as present - so the caller
    // fails rather than acting on a set that is missing one.
    if (!cell.ok()) return cell.status();
    if (cell.value().tag != storage::CellTag::kSpilled) return Status::OK();
    const varheap::VarHeapPtr ptr = varheap::DecodePtr(cell.value().varheap_ptr);
    out.emplace_back(ptr.page_id, ptr.slot);
    return Status::OK();
}

Status RowSpills(const catalog::TableAccess& access, std::span<const std::byte> payload,
                 std::vector<SpillRef>& out) {
    for (std::size_t col = 0; col < access.layout.offsets.size(); ++col) {
        if (Status s = ColumnSpills(access, payload, col, out); !s.ok()) return s;
    }
    return Status::OK();
}

Status ReferencedSpills(const catalog::TableAccess& access, storage::PageStore& store,
                        std::set<SpillRef>& out) {
    std::vector<SpillRef> row;
    return heap::ChainVisit(
        store, access.desc_page_id, storage::PageAccess::kRead,
        [&](PageId, heap::PageView& page, std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            auto tuple = page.ReadTuple(slot);
            if (!tuple.ok()) {
                // A dead or unreadable slot points at nothing. NotFound is
                // the ordinary "retired slot" answer and is not an error.
                if (tuple.status().code() == StatusCode::kNotFound) {
                    return storage::VisitControl::kContinue;
                }
                return tuple.status();
            }
            row.clear();
            if (Status s = RowSpills(access, tuple.value().payload, row); !s.ok()) return s;
            out.insert(row.begin(), row.end());
            return storage::VisitControl::kContinue;
        });
}

}  // namespace kds::exec
