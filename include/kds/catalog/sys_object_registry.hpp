#pragma once

#include <string_view>
#include <vector>

#include "kds/catalog/rows.hpp"

// In-memory registry of well-known sys-objects (namespaces, scalar
// types) - ported from the legacy kernel engine's fixed-capacity-array-
// plus-linear-search registry (kds_catalog_register_sys_object() /
// kds_catalog_get_sys_object[_by_name]()). Legacy used a fixed array with
// a panic on overflow because the kernel avoided dynamic allocation for
// this; here a std::vector removes the need for that cap entirely (RAII
// container, no manual capacity to get wrong), so there is no overflow
// case to handle. The registry is bootstrap-time and small (~20 entries)
// - a hash table would be complexity without payoff, same rationale the
// legacy file-level comment gives.

namespace kds::catalog {

class SysObjectRegistry {
public:
    void Register(const SysObjectRow& obj);

    const SysObjectRow* GetByOid(Oid oid) const noexcept;
    const SysObjectRow* GetByName(std::string_view name) const noexcept;

private:
    std::vector<SysObjectRow> objects_;
};

}  // namespace kds::catalog
