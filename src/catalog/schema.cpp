#include "kds/catalog/schema.hpp"

namespace kds::catalog {

const SysColumnRow* Schema::FindColumn(std::string_view name) const noexcept {
    for (const auto& col : columns) {
        if (NameView(col.name) == name) {
            return &col;
        }
    }
    return nullptr;
}

}  // namespace kds::catalog
