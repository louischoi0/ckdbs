#include "kds/exec/catalog_spills.hpp"

#include <algorithm>

#include "kds/catalog/well_known.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/tagged_cell.hpp"
#include "kds/storage/varheap.hpp"
#include "kds/storage/visit.hpp"

namespace kds::exec {

Status ReferencedSpills(const catalog::TableAccess& access, storage::PageStore& store,
                        std::set<SpillRef>& out) {
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
            const std::span<const std::byte> payload = tuple.value().payload;
            for (std::size_t col = 0; col < access.layout.offsets.size(); ++col) {
                // **Only a `varchar` column is a tagged cell**, which is
                // `SchemaCanSpill`'s own rule and the reason it is the
                // spill gate: `char` is fixed by declaration and every
                // numeric type by its width, so neither can exceed its cell
                // and neither carries a tag. Column 0 is the Keystone word,
                // a raw u64. Decoding any of them as a cell reads a payload
                // byte as a tag - which is what the first form of this did,
                // and it failed the sweep on tag 21 of an integer.
                if (col >= access.schema.columns.size()) continue;
                if (access.schema.columns[col].type_val != catalog::kTypeValVarchar) continue;
                const std::uint32_t offset = access.layout.offsets[col];
                if (offset >= payload.size()) continue;
                auto cell = storage::DecodeCell(payload.subspan(offset));
                // **A cell this build cannot read is not a licence to
                // collect.** A corrupt tag here means the row's references
                // are unknown, and an unknown reference must be treated as
                // present - so the whole walk fails rather than reporting a
                // set that is missing one.
                if (!cell.ok()) return cell.status();
                if (cell.value().tag != storage::CellTag::kSpilled) continue;
                const varheap::VarHeapPtr ptr = varheap::DecodePtr(cell.value().varheap_ptr);
                out.insert({ptr.page_id, ptr.slot});
            }
            return storage::VisitControl::kContinue;
        });
}

}  // namespace kds::exec
