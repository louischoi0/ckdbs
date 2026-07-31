#pragma once

#include <array>
#include <cstdint>
#include <string_view>

// Object identifiers for catalog rows (sys.objects/tables/columns/...).
// A separate id space from kds::PageId - oids name catalog rows, PageIds
// name storage pages.

namespace kds::catalog {

using Oid = std::uint64_t;

inline constexpr std::size_t kCatalogNameMax = 64;
using Name = std::array<char, kCatalogNameMax>;

// Copies `s` into `dst`, truncating to kCatalogNameMax - 1 and always
// null-terminating.
void SetName(Name& dst, std::string_view s) noexcept;

// Reads `src` back out as a view, stopping at the first '\0' or at
// kCatalogNameMax if there is none.
std::string_view NameView(const Name& src) noexcept;

}  // namespace kds::catalog
