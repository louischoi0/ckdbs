#pragma once

#include <string_view>
#include <vector>

#include "kds/catalog/rows.hpp"

// In-memory registry of well-known sys-objects: namespaces and scalar
// types, registered at bootstrap and never afterwards.
//
// A std::vector with a linear scan, deliberately. The registry holds about
// twenty entries and is written once, so a hash table would be complexity
// without payoff; and a growable container means there is no capacity cap
// to size wrong and no overflow case to handle.

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
