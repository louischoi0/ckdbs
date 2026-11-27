#include "kds/server/volume_census.hpp"

#include <algorithm>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>

#include "kds/catalog/catalog.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/exec/assertion_catalog.hpp"
#include "kds/server/page_reclaim.hpp"
#include "kds/server/superblock.hpp"
#include "kds/stats/waystone.hpp"
#include "kds/stats/waystone_dir.hpp"
#include "kds/storage/cabin_bound_page.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/free_map.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/varheap.hpp"

namespace kds::server {

namespace {

const char* PageClassName(std::uint8_t type) {
    switch (static_cast<PageType>(type)) {
        case PageType::kInvalid: return "unformatted";
        case PageType::kHeap: return "heap";
        case PageType::kBtreeInternal: return "btree_internal";
        case PageType::kBtreeLeaf: return "btree_leaf";
        case PageType::kUndo: return "undo";
        case PageType::kCatalog: return "catalog";
        case PageType::kSuperBlock: return "superblock";
        case PageType::kFreeMap: return "free_map";
        case PageType::kHeaderlessMap: return "headerless_map";
        case PageType::kWaystone: return "waystone";
        case PageType::kVarHeap: return "varheap";
        case PageType::kIndexInternal: return "index_internal";
        case PageType::kIndexLeaf: return "index_leaf";
        case PageType::kCabinBound: return "cabin_bound";
        case PageType::kAnchor: return "anchor";
        case PageType::kHeapData: return "heap_data";
    }
    return "unknown";
}

// One census run: the marks, and the report being filled.
class Marker {
public:
    Marker(storage::DevicePageStore& store, std::uint32_t limit, VolumeCensus& out)
        : store_(store), limit_(limit), out_(out) {}

    // Marks `page` reached from `root`; false, with a refusal, on a page
    // reached twice (a cycle, or two roots sharing a page).
    bool Mark(PageId page, const std::string& root) {
        if (!reached_.insert(page).second) {
            Refuse("page " + std::to_string(page) + " is reached twice (last from " + root + ")");
            return false;
        }
        return true;
    }

    bool Reached(PageId page) const { return reached_.count(page) != 0; }

    void Refuse(std::string why) {
        out_.complete = false;
        out_.refusals.push_back(std::move(why));
    }

    void Root(const char* kind) { ++out_.roots_walked[kind]; }

    // Reads `page` for a walk from `root`, which expects one of `types`.
    // Null, with a refusal, when the page cannot be read or is of another
    // class: a link the walk cannot follow is a root it did not walk whole.
    StatusOr<storage::PageRef> Read(PageId page, const std::string& root,
                                    std::initializer_list<PageType> types) {
        if (page >= limit_) {
            Refuse(root + " links page " + std::to_string(page) + ", past the census limit");
            return Status::OutOfRange("past the limit");
        }
        auto ref = store_.GetForRead(page);
        if (!ref.ok()) {
            Refuse(root + " links page " + std::to_string(page) +
                   ", which does not read: " + ref.status().message());
            return ref.status();
        }
        const std::uint8_t type = storage::RawPageType(ref.value().bytes());
        const bool expected = std::any_of(types.begin(), types.end(), [&](PageType t) {
            return static_cast<std::uint8_t>(t) == type;
        });
        if (!expected) {
            Refuse(root + " links page " + std::to_string(page) + " of class " +
                   PageClassName(type));
            return Status::Corruption("wrong class");
        }
        return ref;
    }

private:
    storage::DevicePageStore& store_;
    std::uint32_t limit_;
    VolumeCensus& out_;
    std::unordered_set<PageId> reached_;
};

// A chain of headered pages linked by `next(bytes)`, every page of `type`.
template <typename Next>
void WalkChain(Marker& marker, PageId head, const std::string& root, PageType type, Next next) {
    for (PageId page = head; page != kInvalidPageId;) {
        auto ref = marker.Read(page, root, {type});
        if (!ref.ok()) return;
        if (!marker.Mark(page, root)) return;
        page = next(ref.value().bytes());
    }
}

void WalkCatalogChains(Marker& marker, catalog::Catalog& catalog) {
    for (const catalog::CatalogRoot& entry : catalog::kCatalogRoots) {
        const std::string root = "catalog chain oid=" + std::to_string(entry.oid);
        marker.Root("catalog");
        WalkChain(marker, entry.page, root, PageType::kHeap, [](std::span<std::byte, kPageSize> b) {
            return heap::PageView(b).next_page_id();
        });
        // Every catalog root has a `sys.tables` row (`Catalog::Bootstrap`),
        // so one that does not read is a var-heap root the mark never saw.
        auto row = catalog.GetSysTableRow(entry.oid);
        if (!row.ok()) {
            marker.Refuse(root + " has no sys.tables row: " + row.status().message());
            continue;
        }
        if (row.value().varheap_page_id != kInvalidPageId) {
            marker.Root("catalog_varheap");
            WalkChain(marker, row.value().varheap_page_id, root + " var-heap", PageType::kVarHeap,
                      [](std::span<std::byte, kPageSize> b) { return varheap::PageNextPageId(b); });
        }
    }
}

bool IsCatalogRelation(catalog::Oid oid) {
    return std::any_of(std::begin(catalog::kCatalogRoots), std::end(catalog::kCatalogRoots),
                       [&](const catalog::CatalogRoot& r) { return r.oid == oid; });
}

void WalkRelation(Marker& marker, PageReclaimer& reclaimer, catalog::Oid oid,
                  catalog::PendingRoots roots, const std::string& root) {
    auto reach = reclaimer.ReachFrom(oid, roots);
    if (!reach.ok()) {
        marker.Refuse(root + " does not walk: " + reach.status().message());
        return;
    }
    if (reach.value().skipped != 0) {
        marker.Refuse(root + " walks with " + std::to_string(reach.value().skipped) +
                      " page(s) refused by the reclaim's checks");
    }
    for (const PageId page : reach.value().pages) {
        if (!marker.Mark(page, root)) return;
    }
}

void WalkWaystoneDirectory(Marker& marker, storage::DevicePageStore& store, PageId dir_root,
                           int depth, const std::string& root) {
    // Level by level: `depth` interior levels of headerless pages, whose
    // last level's children are trail pages (`waystone_dir.cpp`).
    std::vector<PageId> level{dir_root};
    for (int at = 0; at < depth && !level.empty(); ++at) {
        std::vector<PageId> below;
        for (const PageId page : level) {
            if (!store.IsHeaderless(page)) {
                marker.Refuse(root + " names page " + std::to_string(page) +
                              " as a directory page, and it is not headerless");
                return;
            }
            auto ref = store.GetForRead(page);
            if (!ref.ok()) {
                marker.Refuse(root + " directory page " + std::to_string(page) +
                              " does not read: " + ref.status().message());
                return;
            }
            if (!marker.Mark(page, root)) return;
            const std::span<const std::byte, kPageSize> bytes = ref.value().bytes();
            for (std::size_t i = 0; i < stats::kDirFanout; ++i) {
                const PageId child = stats::LoadDirChild(bytes, i);
                if (child != stats::kEmptyDirSlot) below.push_back(child);
            }
        }
        level = std::move(below);
    }
    // The trail pages: headered, or left zeroed until their first record
    // formats them (`LookupOrCreateWaystonePage`).
    for (const PageId page : level) {
        auto ref = marker.Read(page, root, {PageType::kWaystone, PageType::kInvalid});
        if (!ref.ok()) return;
        if (!marker.Mark(page, root)) return;
        if (storage::RawPageType(ref.value().bytes()) != static_cast<std::uint8_t>(PageType::kWaystone)) {
            continue;
        }
        const PageId next = stats::ReadWaystoneHeader(ref.value().bytes()).next_page_id;
        WalkChain(marker, next, root + " trail", PageType::kWaystone,
                  [](std::span<std::byte, kPageSize> b) {
                      return stats::ReadWaystoneHeader(b).next_page_id;
                  });
    }
}

}  // namespace

StatusOr<VolumeCensus> RunVolumeCensus(storage::DevicePageStore& store, catalog::Catalog& catalog,
                                       PageReclaimer& reclaimer, std::uint32_t page_limit,
                                       std::span<const PageId> live_undo_pages) {
    VolumeCensus out;
    Marker marker(store, page_limit, out);

    // ---- The roots ---------------------------------------------------------
    WalkCatalogChains(marker, catalog);

    auto tables = catalog.ListTables();
    if (!tables.ok()) return tables.status();
    for (const catalog::SysObjectRow& object : tables.value()) {
        if (IsCatalogRelation(object.oid)) continue;  // walked as a chain above
        auto row = catalog.GetSysTableRow(object.oid);
        if (!row.ok()) {
            marker.Refuse("relation oid=" + std::to_string(object.oid) +
                          " has no sys.tables row: " + row.status().message());
            continue;
        }
        marker.Root("relation");
        // Every user relation has an anchor (`rows.hpp`'s `anchor_page_id`);
        // one without would walk as nothing, a root the mark never saw.
        if (row.value().anchor_page_id == kInvalidPageId) {
            marker.Refuse("relation oid=" + std::to_string(object.oid) + " has no anchor");
            continue;
        }
        WalkRelation(marker, reclaimer, object.oid,
                     catalog::PendingRoots{row.value().anchor_page_id,
                                           row.value().varheap_page_id},
                     "relation oid=" + std::to_string(object.oid));
    }

    auto pending = catalog.PendingReclaims();
    if (!pending.ok()) return pending.status();
    for (const catalog::Catalog::PendingReclaim& owed : pending.value()) {
        marker.Root("tombstone");
        WalkRelation(marker, reclaimer, owed.oid, owed.roots,
                     "tombstone oid=" + std::to_string(owed.oid));
    }

    auto assertions = exec::ListAssertionTargets(catalog, store);
    if (!assertions.ok()) return assertions.status();
    for (const exec::AssertionDef& def : assertions.value()) {
        if (def.cabin_root == kInvalidPageId) continue;
        marker.Root("assertion");
        const std::string root = "assertion id=" + std::to_string(def.id);
        WalkChain(marker, def.cabin_root, root, PageType::kCabinBound,
                  [&](std::span<std::byte, kPageSize> b) {
                      auto page = storage::cabin::BoundCabinPage::Open(b);
                      if (page.ok()) return page.value().next_page_id();
                      // A page whose link does not read ends the walk short
                      // of the chain's tail: a root not walked whole (DV-R1).
                      marker.Refuse(root + " has a page that does not open: " +
                                    page.status().message());
                      return kInvalidPageId;
                  });
    }

    auto patterns = catalog.ListPatterns();
    if (!patterns.ok()) return patterns.status();
    for (const catalog::SysPatternRow& row : patterns.value()) {
        if (!catalog::HasWaystoneDirectory(row)) continue;
        marker.Root("waystone");
        WalkWaystoneDirectory(marker, store, row.waystone_root, row.dir_depth,
                              "pattern oid=" + std::to_string(row.oid));
    }

    const std::unordered_set<PageId> undo(live_undo_pages.begin(), live_undo_pages.end());

    // ---- The sweep of the id space: allocated and unmarked ----------------
    for (PageId page = 0; page < page_limit; ++page) {
        if (!store.IsAllocated(page)) continue;
        ++out.allocated;
        if (page < kFirstUserPageId || storage::IsMapPageId(page) || marker.Reached(page) ||
            undo.count(page) != 0) {
            ++out.marked;
            continue;
        }
        VolumeCensus::Key key;
        if (store.IsHeaderless(page)) {
            key.page_class = "waystone_dir";
        } else {
            auto ref = store.GetForRead(page);
            if (!ref.ok()) {
                // An allocated page that does not read is listed, never
                // freed: the census only reports.
                key.page_class = "unreadable";
            } else {
                const std::span<const std::byte, kPageSize> bytes = ref.value().bytes();
                key.page_class = PageClassName(storage::RawPageType(bytes));
                key.owner = storage::GetOwnerOid(bytes);
            }
        }
        // A previous run's undo page can still be named by a tuple's
        // `undo_ptr` (`undo_log.hpp`'s `tail_`): no root list proves it
        // unreachable, so it is counted apart, never as unmarked.
        if (key.page_class == "undo") {
            ++out.unprovable;
            continue;
        }
        ++out.unmarked;
        ++out.unmarked_by_class[key];
    }
    return out;
}

}  // namespace kds::server
