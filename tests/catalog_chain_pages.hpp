#pragma once

// **Every page a catalog relation occupies, found by walking its chain.**
// A catalog chain's pages past its fixed root come from the free map's
// general supply (`crosscore.md` CC12/CR3), so no id range names them: a
// cell that has to tell a catalog page from a user page asks the chains,
// not the id. Shared by the cells that classify records or pages that way
// (`insert_wal_test.cpp`, `catalog_page_ceiling_test.cpp`).

#include <set>
#include <string>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/storage/visit.hpp"

namespace kds::testing_catalog {

// The chain from `root`, root first, in link order. Bounded by the walk
// budget every next-page loop owes, so a cyclic link is a `Corruption`
// rather than a hung cell.
inline StatusOr<std::vector<PageId>> CatalogChainPages(storage::PageStore& store, PageId root) {
    std::vector<PageId> pages;
    for (PageId at = root; at != kInvalidPageId;) {
        if (Status s = storage::CheckPageWalkBudget(static_cast<std::uint32_t>(pages.size()), root,
                                                    "catalog chain");
            !s.ok()) {
            return s;
        }
        auto bytes = store.GetForRead(at);
        if (!bytes.ok()) {
            return bytes.status().WithContext("catalog chain from page " + std::to_string(root) +
                                              " at page " + std::to_string(at));
        }
        pages.push_back(at);
        at = heap::PageView(bytes.value().bytes()).next_page_id();
    }
    return pages;
}

// Every page of every catalog chain (`catalog::kCatalogRoots`).
inline StatusOr<std::set<PageId>> EveryCatalogPage(storage::PageStore& store) {
    std::set<PageId> every;
    for (const catalog::CatalogRoot& root : catalog::kCatalogRoots) {
        auto chain = CatalogChainPages(store, root.page);
        if (!chain.ok()) return chain.status();
        every.insert(chain.value().begin(), chain.value().end());
    }
    return every;
}

}  // namespace kds::testing_catalog
