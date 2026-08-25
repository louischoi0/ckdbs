#include "kds/catalog/catalog_cache.hpp"

#include <utility>

// Concurrency: core-local, no internal synchronization (rules.md #3). See
// catalog_cache.hpp for what may be cached and why, and for the reason the
// id sequence is not among it.

namespace kds::catalog {

const Oid* CatalogCache::FindOidByName(std::string_view name) noexcept {
    auto it = name_to_oid_.find(name);
    if (it == name_to_oid_.end()) {
        ++stats_.misses;
        return nullptr;
    }
    ++stats_.hits;
    return &it->second;
}

void CatalogCache::PutOidByName(std::string_view name, Oid oid) {
    // try_emplace, not insert_or_assign: an entry already present must keep
    // its address and value, because a caller may be holding the pointer
    // FindOidByName() handed out.
    auto [it, inserted] = name_to_oid_.try_emplace(std::string(name), oid);
    (void)it;
    if (inserted) ++stats_.fills;
}

const TableAccess* CatalogCache::FindTableAccess(Oid oid) noexcept {
    auto it = table_access_.find(oid);
    if (it == table_access_.end()) {
        ++stats_.misses;
        return nullptr;
    }
    ++stats_.hits;
    return &it->second;
}

const TableAccess* CatalogCache::PutTableAccess(TableAccess access) {
    const Oid oid = access.oid;
    auto [it, inserted] = table_access_.try_emplace(oid, std::move(access));
    if (inserted) ++stats_.fills;
    return &it->second;
}

const std::vector<SysTypeRow>* CatalogCache::FindTypes() noexcept {
    if (!types_.has_value()) {
        ++stats_.misses;
        return nullptr;
    }
    ++stats_.hits;
    return &*types_;
}

const std::vector<SysTypeRow>* CatalogCache::PutTypes(std::vector<SysTypeRow> types) {
    if (!types_.has_value()) {
        types_ = std::move(types);
        ++stats_.fills;
    }
    return &*types_;
}

void CatalogCache::InvalidateTypes() noexcept {
    if (!types_.has_value()) return;
    types_.reset();
    ++stats_.invalidations;
}

const std::vector<SysObjectRow>* CatalogCache::FindTableList() noexcept {
    if (!table_list_.has_value()) {
        ++stats_.misses;
        return nullptr;
    }
    ++stats_.hits;
    return &*table_list_;
}

const std::vector<SysObjectRow>* CatalogCache::PutTableList(std::vector<SysObjectRow> tables) {
    if (!table_list_.has_value()) {
        table_list_ = std::move(tables);
        ++stats_.fills;
    }
    return &*table_list_;
}

const PatternAccess* CatalogCache::FindPattern(std::uint64_t pattern_id) noexcept {
    auto it = patterns_.find(pattern_id);
    if (it == patterns_.end()) {
        ++stats_.misses;
        return nullptr;
    }
    ++stats_.hits;
    return &it->second;
}

const PatternAccess* CatalogCache::PutPattern(PatternAccess access) {
    const std::uint64_t key = access.pattern_id;
    auto [it, inserted] = patterns_.try_emplace(key, std::move(access));
    if (inserted) ++stats_.fills;
    return &it->second;
}

void CatalogCache::UpdatePatternWaystone(std::uint64_t pattern_id, PageId root,
                                         std::uint8_t depth) noexcept {
    auto it = patterns_.find(pattern_id);
    if (it == patterns_.end()) return;
    it->second.waystone_root = root;
    it->second.dir_depth = depth;
}

void CatalogCache::UpdatePatternOrigin(std::uint64_t pattern_id, std::uint8_t origin,
                                       std::uint16_t flags) noexcept {
    auto it = patterns_.find(pattern_id);
    if (it == patterns_.end()) return;
    it->second.origin = origin;
    it->second.flags = flags;
}

void CatalogCache::UpdateIndexRoot(Oid rel_oid, Oid index_oid, PageId root) noexcept {
    auto it = table_access_.find(rel_oid);
    if (it == table_access_.end()) return;
    for (TableAccess::IndexRef& ix : it->second.indexes) {
        if (ix.index_oid != index_oid) continue;
        ix.root_page_id = root;
        return;
    }
}

void CatalogCache::UpdateDescPage(Oid rel_oid, PageId root) noexcept {
    auto it = table_access_.find(rel_oid);
    if (it == table_access_.end()) return;
    it->second.desc_page_id = root;
}

void CatalogCache::MarkKeysUnordered(Oid rel_oid) noexcept {
    auto it = table_access_.find(rel_oid);
    if (it == table_access_.end()) return;
    it->second.key_order = KeyOrder::kUnordered;
}

void CatalogCache::Invalidate() noexcept {
    // types_ is deliberately kept: sys.types is written only by Bootstrap()
    // (see catalog_cache.hpp's table of what is cacheable).
    table_access_.clear();
    name_to_oid_.clear();
    table_list_.reset();
    patterns_.clear();
    ++stats_.invalidations;
}

}  // namespace kds::catalog
