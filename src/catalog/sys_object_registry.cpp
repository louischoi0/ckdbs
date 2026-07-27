#include "kds/catalog/sys_object_registry.hpp"

namespace kds::catalog {

void SysObjectRegistry::Register(const SysObjectRow& obj) { objects_.push_back(obj); }

const SysObjectRow* SysObjectRegistry::GetByOid(Oid oid) const noexcept {
    for (const auto& obj : objects_) {
        if (obj.oid == oid) return &obj;
    }
    return nullptr;
}

const SysObjectRow* SysObjectRegistry::GetByName(std::string_view name) const noexcept {
    for (const auto& obj : objects_) {
        if (NameView(obj.name) == name) return &obj;
    }
    return nullptr;
}

}  // namespace kds::catalog
