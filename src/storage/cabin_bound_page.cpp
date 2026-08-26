#include "kds/storage/cabin_bound_page.hpp"

#include <cstring>
#include <string>

namespace kds::storage::cabin {

namespace {

// Offsets inside the entry, named so the static_asserts below can pin them
// and so nothing reaches into the layout with a literal.
inline constexpr std::size_t kOffKeystone = 0;   // pk:40 | flags:8 | reserved:16
inline constexpr std::size_t kOffHint = 8;       // page_id:32 | epoch:16 | slot:16
inline constexpr std::size_t kOffValue = 16;     // int64
// AS6a: the first 4 bytes of AST04's 8-byte padding word, which was written
// as a literal zero on every page in existence - so `group_id = 0` is what an
// entry written before this field reads back as, and the width did not move.
inline constexpr std::size_t kOffGroupId = 24;
inline constexpr std::size_t kOffPadding = 28;

static_assert(kOffPadding + 4 == kEntryBytes,
              "keystone(8) + hint(8) + value(8) + group_id(4) + padding(4) tiles 32 exactly");

// Offsets inside the page header that follows the common one.
inline constexpr std::size_t kOffEntryCount = kPageBodyOffset + 0;  // u16
inline constexpr std::size_t kOffNextPage = kPageBodyOffset + 4;    // u32

static_assert(kOffNextPage + 4 <= kEntriesOffset, "the page header fits before the entries");

// Explicit little-endian load/store. Not a memcpy of a struct and not a
// bitfield: `docs/rules/rules.md` forbids both for a persisted format, because
// struct padding and bitfield layout are implementation-defined and this
// format has to read the same on every architecture.
void Store64(std::span<std::byte> at, std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < 8; ++i) {
        at[i] = static_cast<std::byte>((value >> (i * 8)) & 0xFF);
    }
}

std::uint64_t Load64(std::span<const std::byte> at) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(at[i])) << (i * 8);
    }
    return value;
}

void Store32(std::span<std::byte> at, std::uint32_t value) noexcept {
    for (std::size_t i = 0; i < 4; ++i) {
        at[i] = static_cast<std::byte>((value >> (i * 8)) & 0xFF);
    }
}

std::uint32_t Load32(std::span<const std::byte> at) noexcept {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(at[i])) << (i * 8);
    }
    return value;
}

void Store16(std::span<std::byte> at, std::uint16_t value) noexcept {
    at[0] = static_cast<std::byte>(value & 0xFF);
    at[1] = static_cast<std::byte>((value >> 8) & 0xFF);
}

std::uint16_t Load16(std::span<const std::byte> at) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(at[0]) |
                                      (std::to_integer<std::uint8_t>(at[1]) << 8));
}

}  // namespace

Status EncodeEntry(const BoundCabinEntry& entry, std::span<std::byte, kEntryBytes> out) {
    // Refused, never truncated. A truncated pk is a name that means a
    // *different* row, which is exactly the mis-attribution K1 exists to make
    // impossible - so a pk that does not fit is a defect to report, not a
    // value to narrow.
    if (entry.pk > kMaxEntryPk) {
        return Status::InvalidArgument("bound cabin entry: pk " + std::to_string(entry.pk) +
                                        " exceeds the 40-bit Keystone range");
    }

    // pk:40 | flags:8 | reserved:16, by shift and mask - the same packing
    // discipline the Keystone word uses (invariant 6).
    const std::uint64_t keystone =
        (entry.pk & kMaxEntryPk) | (static_cast<std::uint64_t>(entry.flags) << 40);
    Store64(out.subspan(kOffKeystone, 8), keystone);

    Store32(out.subspan(kOffHint, 4), entry.page_id);
    Store16(out.subspan(kOffHint + 4, 2), entry.page_epoch);
    Store16(out.subspan(kOffHint + 6, 2), entry.slot);

    Store64(out.subspan(kOffValue, 8), static_cast<std::uint64_t>(entry.value));
    Store32(out.subspan(kOffGroupId, 4), entry.group_id);
    Store32(out.subspan(kOffPadding, 4), 0);
    return Status::OK();
}

BoundCabinEntry DecodeEntry(std::span<const std::byte, kEntryBytes> in) {
    BoundCabinEntry entry;
    const std::uint64_t keystone = Load64(in.subspan(kOffKeystone, 8));
    entry.pk = keystone & kMaxEntryPk;
    entry.flags = static_cast<std::uint8_t>((keystone >> 40) & 0xFF);

    entry.page_id = Load32(in.subspan(kOffHint, 4));
    entry.page_epoch = Load16(in.subspan(kOffHint + 4, 2));
    entry.slot = Load16(in.subspan(kOffHint + 6, 2));

    entry.value = static_cast<std::int64_t>(Load64(in.subspan(kOffValue, 8)));
    entry.group_id = Load32(in.subspan(kOffGroupId, 4));
    return entry;
}

// ---- The page ------------------------------------------------------------

Status BoundCabinPage::Format(std::span<std::byte, kPageSize> page) {
    FormatPage(page, PageType::kCabinBound);
    Store16(page.subspan(kOffEntryCount, 2), 0);
    Store32(page.subspan(kOffNextPage, 4), kInvalidPageId);
    return Status::OK();
}

StatusOr<BoundCabinPage> BoundCabinPage::Open(std::span<std::byte, kPageSize> page) {
    const PageHeaderFields header = ReadPageHeader(std::span<const std::byte, kPageSize>(page));
    if (header.page_type != static_cast<std::uint8_t>(PageType::kCabinBound)) {
        return Status::Corruption("not a bound cabin page: page_type=" +
                                  std::to_string(header.page_type));
    }
    BoundCabinPage view(page);
    if (view.entry_count() > kMaxEntriesPerPage) {
        return Status::Corruption("bound cabin page claims " +
                                  std::to_string(view.entry_count()) + " entries, capacity is " +
                                  std::to_string(kMaxEntriesPerPage));
    }
    return view;
}

std::uint16_t BoundCabinPage::entry_count() const noexcept {
    return Load16(page_.subspan(kOffEntryCount, 2));
}

void BoundCabinPage::SetEntryCount(std::uint16_t count) noexcept {
    Store16(page_.subspan(kOffEntryCount, 2), count);
}

PageId BoundCabinPage::next_page_id() const noexcept {
    return Load32(page_.subspan(kOffNextPage, 4));
}

void BoundCabinPage::SetNextPageId(PageId next) noexcept {
    Store32(page_.subspan(kOffNextPage, 4), next);
}

StatusOr<std::uint16_t> BoundCabinPage::Append(const BoundCabinEntry& entry) {
    const std::uint16_t at = entry_count();
    if (at >= kMaxEntriesPerPage) {
        return Status::OutOfSpace("bound cabin page is full (" +
                                  std::to_string(kMaxEntriesPerPage) + " entries)");
    }
    if (Status s = Write(at, entry); !s.ok()) return s;
    SetEntryCount(static_cast<std::uint16_t>(at + 1));
    return at;
}

StatusOr<BoundCabinEntry> BoundCabinPage::Read(std::uint16_t index) const {
    if (index >= entry_count()) {
        return Status::OutOfRange("bound cabin entry " + std::to_string(index) +
                                  " is past the page's " + std::to_string(entry_count()));
    }
    const std::size_t at = kEntriesOffset + static_cast<std::size_t>(index) * kEntryBytes;
    return DecodeEntry(page_.subspan(at).first<kEntryBytes>());
}

Status BoundCabinPage::Write(std::uint16_t index, const BoundCabinEntry& entry) {
    // `index == entry_count()` is legal, and is how Append writes the slot it
    // is about to publish. Anything past that is out of range.
    if (index > entry_count() || index >= kMaxEntriesPerPage) {
        return Status::OutOfRange("bound cabin entry " + std::to_string(index) +
                                  " is not writable on a page holding " +
                                  std::to_string(entry_count()));
    }
    const std::size_t at = kEntriesOffset + static_cast<std::size_t>(index) * kEntryBytes;
    return EncodeEntry(entry, page_.subspan(at).first<kEntryBytes>());
}

// Both flag moves share this: read the entry, move one bit, write it back.
// `Read` is what bounds-checks, so neither needs its own check.
Status BoundCabinPage::SetFlags(std::uint16_t index, std::uint8_t set, std::uint8_t clear) {
    auto entry = Read(index);
    if (!entry.ok()) return entry.status();
    BoundCabinEntry moved = entry.value();
    moved.flags = static_cast<std::uint8_t>((moved.flags | set) & ~clear);
    return Write(index, moved);
}

Status BoundCabinPage::ClearReserved(std::uint16_t index) {
    return SetFlags(index, /*set=*/0, /*clear=*/kEntryReserved);
}

Status BoundCabinPage::MarkOrphaned(std::uint16_t index) {
    return SetFlags(index, /*set=*/kEntryOrphaned, /*clear=*/0);
}

}  // namespace kds::storage::cabin
