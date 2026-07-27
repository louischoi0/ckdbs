#include "kds/catalog/oid.hpp"

#include <algorithm>
#include <cstring>

namespace kds::catalog {

void SetName(Name& dst, std::string_view s) noexcept {
    std::size_t n = std::min(s.size(), kCatalogNameMax - 1);
    std::memcpy(dst.data(), s.data(), n);
    std::memset(dst.data() + n, 0, kCatalogNameMax - n);
}

std::string_view NameView(const Name& src) noexcept {
    const char* end = static_cast<const char*>(std::memchr(src.data(), '\0', kCatalogNameMax));
    std::size_t len = end != nullptr ? static_cast<std::size_t>(end - src.data()) : kCatalogNameMax;
    return std::string_view(src.data(), len);
}

}  // namespace kds::catalog
