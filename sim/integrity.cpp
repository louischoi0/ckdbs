#include "sim/integrity.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "kds/catalog/rows.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/btree/btree_page.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/tagged_cell.hpp"
#include "kds/storage/varheap.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/txn/undo_page.hpp"

namespace kds::sim {

const char* CheckKindName(CheckKind kind) {
    switch (kind) {
        case CheckKind::kPageHeader: return "page-header";
        case CheckKind::kCatalog: return "catalog";
        case CheckKind::kChainOrder: return "chain-order";
        case CheckKind::kBtreeStructure: return "btree-structure";
        case CheckKind::kKeystone: return "keystone";
        case CheckKind::kRowSize: return "row-size";
        case CheckKind::kTrxId: return "trx-id";
        case CheckKind::kUndoPtr: return "undo-ptr";
        case CheckKind::kVarHeap: return "var-heap";
    }
    return "unknown";
}

std::size_t IntegrityReport::CountOf(CheckKind kind) const {
    std::size_t n = 0;
    for (const Finding& f : findings) {
        if (f.kind == kind) ++n;
    }
    return n;
}

std::string IntegrityReport::Summary() const {
    if (ok()) {
        return "integrity clean: relations=" + std::to_string(relations_swept) +
               " pages=" + std::to_string(pages_swept) +
               " tuples=" + std::to_string(tuples_swept);
    }
    std::string out = "integrity: " + std::to_string(findings.size()) + " finding(s)";
    for (const Finding& f : findings) {
        out += "\n  [";
        out += CheckKindName(f.kind);
        out += "] ";
        if (f.page_id != kInvalidPageId) {
            out += "page " + std::to_string(f.page_id) + ": ";
        }
        out += f.detail;
    }
    return out;
}

namespace {

// The walk collects cross-page facts and resolves them after every span is
// released (the I15 R1 span discipline, applied to inspection).
struct PendingUndo {
    std::uint64_t ptr;
    std::uint64_t keystone_id;
};

struct PendingSpillCheck {
    std::uint64_t ptr_word;  // varheap::EncodePtr form, from the cell
    std::uint32_t declared_len;
    std::uint64_t keystone_id;
};

class Sweep {
public:
    Sweep(storage::PageStore& store, catalog::Catalog& catalog)
        : store_(store), catalog_(catalog) {}

    IntegrityReport Run() {
        SweepSuperblock();
        SweepCatalogAndRelations();
        return std::move(report_);
    }

    // The device-backed extra: every allocated page must be fetchable (a
    // cold fetch re-verifies the checksum) and, unless headerless, carry a
    // valid header.
    void SweepAllocatedPages(storage::DevicePageStore& store, storage::PageDevice& device) {
        const std::uint32_t capacity = device.page_capacity();
        for (PageId page_id = 0; page_id < capacity; ++page_id) {
            if (!store.IsAllocated(page_id)) continue;
            ++report_.pages_swept;
            auto page = store.GetForRead(page_id);
            if (!page.ok()) {
                Add(CheckKind::kPageHeader, page_id,
                    "allocated page unreadable: " + page.status().message());
                continue;
            }
            if (store.IsHeaderless(page_id)) continue;
            if (Status s = storage::ValidatePageHeader(page.value().bytes()); !s.ok()) {
                Add(CheckKind::kPageHeader, page_id, "header invalid: " + s.message());
            }
        }
    }

private:
    void Add(CheckKind kind, PageId page_id, std::string detail) {
        report_.findings.push_back(Finding{kind, page_id, std::move(detail)});
    }

    void SweepSuperblock() {
        auto page = store_.GetForRead(server::kSuperBlockPageId);
        if (!page.ok()) {
            Add(CheckKind::kPageHeader, server::kSuperBlockPageId,
                "superblock unreadable: " + page.status().message());
            return;
        }
        auto sb = server::SuperBlock::Decode(
            std::span<const std::byte, kPageSize>(page.value().bytes()));
        if (!sb.ok()) {
            Add(CheckKind::kPageHeader, server::kSuperBlockPageId,
                "superblock does not decode: " + sb.status().message());
            return;
        }
        next_trx_id_ = sb.value().next_trx_id();
    }

    void SweepCatalogAndRelations() {
        auto tables = catalog_.ListTables();
        if (!tables.ok()) {
            Add(CheckKind::kCatalog, kInvalidPageId,
                "ListTables failed: " + tables.status().message());
            return;
        }
        std::unordered_set<std::uint64_t> oids;
        std::set<std::string, std::less<>> names;
        for (const catalog::SysObjectRow& row : tables.value()) {
            const std::string_view name = catalog::NameView(row.name);
            if (!oids.insert(row.oid).second) {
                Add(CheckKind::kCatalog, kInvalidPageId,
                    "duplicate oid " + std::to_string(row.oid) + " (" + std::string(name) + ")");
            }
            if (!names.emplace(name).second) {
                Add(CheckKind::kCatalog, kInvalidPageId,
                    "duplicate relation name '" + std::string(name) + "'");
            }
        }
        for (const catalog::SysObjectRow& row : tables.value()) {
            // Bootstrap relations are typed catalog codecs, not user
            // tuples, and carry no sys.columns rows — the oid range is
            // what distinguishes them, not the name.
            if (row.oid < catalog::kUserOidStart) continue;
            SweepRelation(row.oid, std::string(catalog::NameView(row.name)));
        }
    }

    void SweepRelation(std::uint64_t oid, const std::string& name) {
        auto access_or = catalog_.InitTableAccess(oid);
        if (!access_or.ok()) {
            Add(CheckKind::kCatalog, kInvalidPageId,
                "relation '" + name + "': InitTableAccess failed: " +
                    access_or.status().message());
            return;
        }
        const catalog::TableAccess& access = *access_or.value();
        ++report_.relations_swept;

        pending_undo_.clear();
        pending_spills_.clear();

        if (access.clustered_type == catalog::ClusteredType::kBtree) {
            SweepBtreeInternals(access, name);
        }
        SweepTuples(access, name);
        SweepVarHeap(access, name);
        ResolveUndo(name);
    }

    // Walks the tuple-bearing pages (heap chain, or btree leaves — one
    // PageView reads both) checking invariants 2/3/5/7/13 and collecting
    // the cross-page facts.
    void SweepTuples(const catalog::TableAccess& access, const std::string& name) {
        PageId current_page = kInvalidPageId;
        std::uint64_t current_min_key = 0;
        std::uint64_t prev_pages_max_id = 0;
        bool have_prev_pages = false;
        std::uint64_t current_max_id = 0;
        bool current_has_tuples = false;

        auto on_new_page = [&](PageId page_id, heap::PageView& page) {
            if (current_page != kInvalidPageId && current_has_tuples) {
                prev_pages_max_id = std::max(prev_pages_max_id, current_max_id);
                have_prev_pages = true;
            }
            current_page = page_id;
            current_min_key = page.min_key();
            current_max_id = 0;
            current_has_tuples = false;
            ++report_.pages_swept;
            if (have_prev_pages && prev_pages_max_id >= current_min_key) {
                Add(CheckKind::kChainOrder, page_id,
                    "relation '" + name + "': min_key " + std::to_string(current_min_key) +
                        " does not exceed a predecessor page's max id " +
                        std::to_string(prev_pages_max_id));
            }
        };

        auto visitor = [&](PageId page_id, heap::PageView& page,
                           std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            if (page_id != current_page) on_new_page(page_id, page);
            auto tuple = page.ReadTuple(slot);
            if (!tuple.ok()) {
                // NotFound is a dead or retired slot — normal. Anything
                // else is a slot that stopped decoding, which is a finding,
                // not a skip.
                if (tuple.status().code() != StatusCode::kNotFound) {
                    Add(CheckKind::kRowSize, page_id,
                        "relation '" + name + "' slot " + std::to_string(slot) +
                            " does not decode: " + tuple.status().message());
                }
                return storage::VisitControl::kContinue;
            }
            ++report_.tuples_swept;
            current_has_tuples = true;

            const std::span<const std::byte> payload = tuple.value().payload;
            if (payload.size() != access.layout.row_size) {
                Add(CheckKind::kRowSize, page_id,
                    "relation '" + name + "' slot " + std::to_string(slot) + ": payload " +
                        std::to_string(payload.size()) + " bytes, schema constant is " +
                        std::to_string(access.layout.row_size));
                return storage::VisitControl::kContinue;
            }

            auto id_or = KeystoneIdOfPayload(payload);
            if (!id_or.ok()) {
                Add(CheckKind::kKeystone, page_id,
                    "relation '" + name + "' slot " + std::to_string(slot) + ": " +
                        id_or.status().message());
                return storage::VisitControl::kContinue;
            }
            const std::uint64_t id = id_or.value();
            current_max_id = std::max(current_max_id, id);

            std::uint64_t word = 0;
            std::memcpy(&word, payload.data(), sizeof word);
            const Keystone keystone = Keystone::Decode(word);
            if (keystone.reserved != 0) {
                Add(CheckKind::kKeystone, page_id,
                    "relation '" + name + "' id " + std::to_string(id) +
                        ": reserved bits nonzero (" + std::to_string(keystone.reserved) + ")");
            }
            if (id > kMaxKeystoneId) {
                Add(CheckKind::kKeystone, page_id,
                    "relation '" + name + "' slot " + std::to_string(slot) +
                        ": id above 2^40-1");
            }
            if (id < current_min_key) {
                Add(CheckKind::kChainOrder, page_id,
                    "relation '" + name + "' id " + std::to_string(id) +
                        " below the page's min_key " + std::to_string(current_min_key));
            }

            const std::uint64_t trx_id = tuple.value().trx_id;
            if (next_trx_id_ != 0 && trx_id >= next_trx_id_) {
                Add(CheckKind::kTrxId, page_id,
                    "relation '" + name + "' id " + std::to_string(id) + ": trx_id " +
                        std::to_string(trx_id) + " was never issued (next_trx_id " +
                        std::to_string(next_trx_id_) + ")");
            }
            if (tuple.value().undo_ptr != txn::kNoUndoPtr) {
                pending_undo_.push_back(PendingUndo{tuple.value().undo_ptr, id});
            }

            CollectCells(access, name, page_id, id, payload);
            return storage::VisitControl::kContinue;
        };

        Status walked =
            access.clustered_type == catalog::ClusteredType::kBtree
                ? btree::BtreeVisit(store_, access.desc_page_id,
                                             storage::PageAccess::kRead, visitor)
                : heap::ChainVisit(store_, access.desc_page_id,
                                            storage::PageAccess::kRead, visitor);
        if (!walked.ok()) {
            Add(CheckKind::kChainOrder, access.desc_page_id,
                "relation '" + name + "': walk failed: " + walked.message());
        }
    }

    // Tagged cells (invariant 13's other half): every varchar cell must
    // decode, and a spilled one is queued for post-walk resolution.
    void CollectCells(const catalog::TableAccess& access, const std::string& name,
                      PageId page_id, std::uint64_t id, std::span<const std::byte> payload) {
        const catalog::Schema& schema = access.schema;
        for (std::size_t i = 1; i < schema.columns.size(); ++i) {
            if (schema.columns[i].type_val != catalog::kTypeValVarchar) continue;
            const std::uint32_t offset = access.layout.offsets[i];
            const std::uint32_t width = access.layout.inline_cell_width;
            auto cell = storage::DecodeCell(payload.subspan(offset, width));
            if (!cell.ok()) {
                Add(CheckKind::kVarHeap, page_id,
                    "relation '" + name + "' id " + std::to_string(id) + " column '" +
                        std::string(catalog::NameView(schema.columns[i].name)) +
                        "': cell does not decode: " + cell.status().message());
                continue;
            }
            if (cell.value().tag == storage::CellTag::kSpilled) {
                pending_spills_.push_back(
                    PendingSpillCheck{cell.value().varheap_ptr, cell.value().len, id});
            }
        }
    }

    // Internal-node structure: entries ascend, and each separator equals
    // its child's min_key — the documented equality, checked literally.
    void SweepBtreeInternals(const catalog::TableAccess& access, const std::string& name) {
        SweepBtreeNode(access.desc_page_id, name, /*depth=*/0);
    }

    void SweepBtreeNode(PageId page_id, const std::string& name, int depth) {
        if (depth > 8) {
            Add(CheckKind::kBtreeStructure, page_id,
                "relation '" + name + "': tree deeper than 8 levels, cycle suspected");
            return;
        }
        auto page = store_.GetForRead(page_id);
        if (!page.ok()) {
            Add(CheckKind::kBtreeStructure, page_id,
                "relation '" + name + "': node unreadable: " + page.status().message());
            return;
        }
        const std::uint8_t raw_type = storage::RawPageType(page.value().bytes());
        if (raw_type == static_cast<std::uint8_t>(PageType::kBtreeLeaf)) return;
        if (raw_type != static_cast<std::uint8_t>(PageType::kBtreeInternal)) {
            Add(CheckKind::kBtreeStructure, page_id,
                "relation '" + name + "': node has page type " + std::to_string(raw_type));
            return;
        }

        btree::InternalView node(page.value().bytes());
        const std::uint16_t entries = node.entry_count();
        struct ChildExpectation {
            PageId child;
            std::uint64_t sep_key;
            bool check_min_key;
        };
        std::vector<ChildExpectation> children;
        children.push_back({node.leftmost_child(), 0, false});
        std::uint64_t prev_sep = 0;
        bool have_prev = false;
        for (std::uint16_t i = 0; i < entries; ++i) {
            auto entry = node.Entry(i);
            if (!entry.ok()) {
                Add(CheckKind::kBtreeStructure, page_id,
                    "relation '" + name + "': entry " + std::to_string(i) +
                        " unreadable: " + entry.status().message());
                return;
            }
            if (have_prev && entry.value().sep_key <= prev_sep) {
                Add(CheckKind::kBtreeStructure, page_id,
                    "relation '" + name + "': separators not ascending at entry " +
                        std::to_string(i));
            }
            prev_sep = entry.value().sep_key;
            have_prev = true;
            children.push_back({entry.value().child, entry.value().sep_key, true});
        }
        // The parent's span is done with; descend with it released.
        for (const ChildExpectation& expect : children) {
            if (expect.check_min_key) {
                auto child = store_.GetForRead(expect.child);
                if (!child.ok()) {
                    Add(CheckKind::kBtreeStructure, expect.child,
                        "relation '" + name + "': child unreadable: " +
                            child.status().message());
                    continue;
                }
                heap::PageView view(child.value().bytes());
                if (view.min_key() != expect.sep_key) {
                    Add(CheckKind::kBtreeStructure, expect.child,
                        "relation '" + name + "': separator " +
                            std::to_string(expect.sep_key) + " != child min_key " +
                            std::to_string(view.min_key()));
                }
            }
            SweepBtreeNode(expect.child, name, depth + 1);
        }
    }

    // The relation's var-heap chain, then the collected spills against it.
    void SweepVarHeap(const catalog::TableAccess& access, const std::string& name) {
        std::unordered_set<PageId> chain_pages;
        if (access.varheap_page_id != kInvalidPageId) {
            PageId at = access.varheap_page_id;
            while (at != kInvalidPageId) {
                if (!chain_pages.insert(at).second) {
                    Add(CheckKind::kVarHeap, at,
                        "relation '" + name + "': var-heap chain cycles");
                    break;
                }
                auto page = store_.GetForRead(at);
                if (!page.ok()) {
                    Add(CheckKind::kVarHeap, at,
                        "relation '" + name + "': var-heap page unreadable: " +
                            page.status().message());
                    break;
                }
                ++report_.pages_swept;
                if (storage::RawPageType(page.value().bytes()) !=
                    static_cast<std::uint8_t>(PageType::kVarHeap)) {
                    Add(CheckKind::kVarHeap, at,
                        "relation '" + name + "': chain page is not kVarHeap");
                }
                at = varheap::PageNextPageId(page.value().bytes());
            }
        }

        for (const PendingSpillCheck& spill : pending_spills_) {
            const varheap::VarHeapPtr ptr =
                varheap::DecodePtr(spill.ptr_word);
            if (!chain_pages.count(ptr.page_id)) {
                Add(CheckKind::kVarHeap, ptr.page_id,
                    "relation '" + name + "' id " + std::to_string(spill.keystone_id) +
                        ": spilled cell points outside the relation's own chain");
                continue;
            }
            storage::PageRef value_pin;
            auto bytes = varheap::Fetch(store_, ptr, value_pin);
            if (!bytes.ok()) {
                Add(CheckKind::kVarHeap, ptr.page_id,
                    "relation '" + name + "' id " + std::to_string(spill.keystone_id) +
                        ": spilled cell does not resolve: " + bytes.status().message());
                continue;
            }
            if (bytes.value().size() != spill.declared_len) {
                Add(CheckKind::kVarHeap, ptr.page_id,
                    "relation '" + name + "' id " + std::to_string(spill.keystone_id) +
                        ": spilled length " + std::to_string(bytes.value().size()) +
                        " != declared " + std::to_string(spill.declared_len));
            }
        }
    }

    void ResolveUndo(const std::string& name) {
        for (const PendingUndo& pending : pending_undo_) {
            if (Status s = txn::UndoPtrIsPlausible(pending.ptr); !s.ok()) {
                Add(CheckKind::kUndoPtr, txn::UndoPtrPageId(pending.ptr),
                    "relation '" + name + "' id " + std::to_string(pending.keystone_id) +
                        ": " + s.message());
                continue;
            }
            const PageId undo_page = txn::UndoPtrPageId(pending.ptr);
            auto page = store_.GetForRead(undo_page);
            if (!page.ok()) {
                Add(CheckKind::kUndoPtr, undo_page,
                    "relation '" + name + "' id " + std::to_string(pending.keystone_id) +
                        ": undo page unreadable: " + page.status().message());
                continue;
            }
            if (storage::RawPageType(page.value().bytes()) !=
                static_cast<std::uint8_t>(PageType::kUndo)) {
                Add(CheckKind::kUndoPtr, undo_page,
                    "relation '" + name + "' id " + std::to_string(pending.keystone_id) +
                        ": undo_ptr points at a non-undo page");
                continue;
            }
            // **The chain walk was retired by the undo purge**
            // (docs/inflight/in-progress/workplan-undo-purge.md, review finding 2). A settled
            // page recycles, so a committed tuple's undo_ptr may point
            // into bytes that now belong to newer records - legal on a
            // healthy database, and exactly the pointer no production
            // reader dereferences (the writer is visible, so every walk
            // stops at the tuple). Walking it here validated somebody
            // else's chain and could neither fail honestly nor pass
            // meaningfully. The checks above survive because they stay
            // true under reuse: the pointer must be plausible and must
            // name a kUndo page, recycled or not. A per-page reuse
            // generation would let this walk return - it is paired with
            // the byte-cap retention D1 declined, not buildable alone.
        }
    }

    storage::PageStore& store_;
    catalog::Catalog& catalog_;
    IntegrityReport report_;
    std::uint64_t next_trx_id_ = 0;
    std::vector<PendingUndo> pending_undo_;
    std::vector<PendingSpillCheck> pending_spills_;
};

}  // namespace

IntegrityReport CheckInstance(storage::PageStore& store, catalog::Catalog& catalog) {
    return Sweep(store, catalog).Run();
}

IntegrityReport CheckInstance(storage::DevicePageStore& store, storage::PageDevice& device,
                              catalog::Catalog& catalog) {
    Sweep sweep(store, catalog);
    // The allocated-page pass first: a page the catalog walk is about to
    // traverse gets its header finding attributed here, by category, before
    // the walk turns it into a less specific failure.
    sweep.SweepAllocatedPages(store, device);
    return sweep.Run();
}

}  // namespace kds::sim
