#include "kds/exec/varheap_sweep.hpp"

#include <set>
#include <utility>

#include "kds/catalog/well_known.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/tagged_cell.hpp"
#include "kds/storage/visit.hpp"
#include "kds/storage/varheap.hpp"
#include "kds/txn/varheap_release.hpp"

namespace kds::exec {
namespace {

// A spill pointer, as a key the referenced set can hold.
using SpillRef = std::pair<PageId, std::uint16_t>;

// Every var-heap slot the relation's **live** rows point at.
//
// Delete-marked rows count as live references here, deliberately: nothing
// retires a heap slot yet (`known-gaps.md`, reclamation), so a delete-marked
// row is still readable by an older snapshot and its spilled value must
// still resolve. Collecting a value a readable row points at would turn a
// leak into a wrong answer, which is the one direction this sweep must
// never err in.
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
                // present - so the whole sweep fails rather than freeing
                // bytes something may point at.
                if (!cell.ok()) return cell.status();
                if (cell.value().tag != storage::CellTag::kSpilled) continue;
                const varheap::VarHeapPtr ptr = varheap::DecodePtr(cell.value().varheap_ptr);
                out.insert({ptr.page_id, ptr.slot});
            }
            return storage::VisitControl::kContinue;
        });
}

// Releases every slot of the relation's var-heap chain that `referenced`
// does not name.
Status SweepChain(const catalog::TableAccess& access, storage::PageStore& store,
                  wal::WalManager* wal, const std::set<SpillRef>& referenced,
                  VarHeapSweepReport& report) {
    PageId page_id = access.varheap_page_id;
    for (std::uint32_t hops = 0; page_id != kInvalidPageId; ++hops) {
        if (Status s = storage::CheckPageWalkBudget(hops, access.varheap_page_id, "var-heap chain");
            !s.ok()) {
            return s;
        }
        auto bytes = store.GetForRead(page_id);
        if (!bytes.ok()) return bytes.status();
        const std::uint16_t slots = varheap::PageSlotCount(bytes.value().bytes());
        const PageId next = varheap::PageNextPageId(bytes.value().bytes());
        ++report.pages;

        for (std::uint16_t slot = 0; slot < slots; ++slot) {
            if (referenced.count({page_id, slot}) != 0) {
                ++report.retained;
                continue;
            }
            // **`kNoTxnId`, because no transaction owns this collection** -
            // `varheap_release.hpp`'s split, purge-drain arm. The release
            // is idempotent, so a crash mid-sweep replays it harmlessly and
            // the next mount finishes the job.
            auto released =
                txn::ReleaseVarHeapSlot(store, wal, wal::kNoTxnId, page_id, slot);
            if (!released.ok()) return released.status();
            // A slot that was already a tombstone reports `kReleased` too
            // (the release is idempotent and cannot tell), so this counts
            // *collections attempted*, not bytes recovered. Stated rather
            // than over-claimed: on a relation that has never rolled back,
            // this is the count of slots already dead.
            ++report.released;
        }
        page_id = next;
    }
    return Status::OK();
}

// The relations whose spills are logged under `kNoTxnId`, named rather than
// discovered: `exec::LogChainInsert`'s two callers are `sys.pattern_defs`
// and the assertion catalog, and adding a third caller must be a decision
// to add it here too rather than a silent widening of what gets swept.
constexpr catalog::Oid kUnownedSpillRelations[] = {
    catalog::kSysPatternDefsTable,
    catalog::kSysAssertionsTable,
};

}  // namespace

StatusOr<VarHeapSweepReport> SweepUnownedSpills(catalog::Catalog& catalog,
                                                storage::PageStore& store,
                                                wal::WalManager* wal) {
    VarHeapSweepReport report;
    for (catalog::Oid oid : kUnownedSpillRelations) {
        auto access = catalog.InitTableAccess(oid);
        if (!access.ok()) {
            // A catalog relation this build knows and this file does not
            // have is not a sweep failure - a fresh instance may not have
            // materialized every one. Skipped, not reported as damage.
            if (access.status().code() == StatusCode::kNotFound) continue;
            return access.status();
        }
        // Nothing has spilled into this relation yet, so there is no chain.
        if (access.value()->varheap_page_id == kInvalidPageId) continue;

        std::set<SpillRef> referenced;
        if (Status s = ReferencedSpills(*access.value(), store, referenced); !s.ok()) {
            return s.WithContext("var-heap sweep: collecting live references of relation oid " +
                                 std::to_string(oid));
        }
        if (Status s = SweepChain(*access.value(), store, wal, referenced, report); !s.ok()) {
            return s.WithContext("var-heap sweep: relation oid " + std::to_string(oid));
        }
    }
    return report;
}

}  // namespace kds::exec
