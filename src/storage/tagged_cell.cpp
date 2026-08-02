#include "kds/storage/tagged_cell.hpp"

#include <algorithm>
#include <string>

namespace kds::storage {

namespace {

void PutLE(std::byte* out, std::uint64_t v, int width) {
    for (int i = 0; i < width; ++i) {
        out[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
}

std::uint64_t GetLE(const std::byte* in, int width) {
    std::uint64_t v = 0;
    for (int i = 0; i < width; ++i) {
        v |= static_cast<std::uint64_t>(in[i]) << (8 * i);
    }
    return v;
}

// Every writer and reader here starts by agreeing the span really is a
// cell of a legal width. A short span is a caller bug (a layout that
// disagrees with the schema constant), not a data problem, so it is
// InvalidArgument rather than Corruption.
Status CheckCellSpan(std::size_t size) {
    if (size < kMinInlineCellWidth || size > kMaxInlineCellWidth) {
        return Status::InvalidArgument("tagged cell of " + std::to_string(size) +
                                        " byte(s) is outside the legal width range " +
                                        std::to_string(kMinInlineCellWidth) + ".." +
                                        std::to_string(kMaxInlineCellWidth));
    }
    return Status::OK();
}

}  // namespace

Status CheckInlineCellWidth(std::uint32_t width) {
    if (width < kMinInlineCellWidth || width > kMaxInlineCellWidth) {
        return Status::InvalidArgument("inline_cell_width " + std::to_string(width) +
                                        " is outside " + std::to_string(kMinInlineCellWidth) +
                                        ".." + std::to_string(kMaxInlineCellWidth));
    }
    return Status::OK();
}

Status EncodeInlineCell(std::span<std::byte> cell, std::string_view value) {
    if (Status s = CheckCellSpan(cell.size()); !s.ok()) return s;

    const std::uint32_t capacity = InlineCapacity(static_cast<std::uint32_t>(cell.size()));
    if (value.size() > capacity) {
        // Not this function's value to store. The caller decides whether it
        // can spill (it needs a var-heap chain to do so) and calls
        // EncodeSpilledCell() instead; refusing here rather than silently
        // truncating is the whole contract.
        return Status::OutOfSpace("value of " + std::to_string(value.size()) +
                                   " byte(s) exceeds the " + std::to_string(capacity) +
                                   " that fit inline; it belongs in the var-heap");
    }

    // Zero first, then write. An UPDATE overwrites a cell in place, so the
    // padding is not decoration - it is what stops the tail of a longer
    // previous value from surviving underneath a shorter new one.
    std::fill(cell.begin(), cell.end(), std::byte{0});

    cell[kCellTagOffset] = static_cast<std::byte>(CellTag::kInline);
    PutLE(cell.data() + kCellInlineLenOffset, value.size(), 2);
    for (std::size_t i = 0; i < value.size(); ++i) {
        cell[kCellInlineBytesOffset + i] =
            static_cast<std::byte>(static_cast<unsigned char>(value[i]));
    }
    return Status::OK();
}

Status EncodeNullCell(std::span<std::byte> cell) {
    if (Status s = CheckCellSpan(cell.size()); !s.ok()) return s;
    std::fill(cell.begin(), cell.end(), std::byte{0});
    cell[kCellTagOffset] = static_cast<std::byte>(CellTag::kNull);
    return Status::OK();
}

Status EncodeSpilledCell(std::span<std::byte> cell, std::uint32_t len,
                         std::uint64_t varheap_ptr) {
    if (Status s = CheckCellSpan(cell.size()); !s.ok()) return s;

    // Zero first, then write - same reason as EncodeInlineCell(): a cell is
    // overwritten in place, and a spill landing on top of a longer inline
    // value must not leave that value's tail underneath it.
    std::fill(cell.begin(), cell.end(), std::byte{0});

    cell[kCellTagOffset] = static_cast<std::byte>(CellTag::kSpilled);
    PutLE(cell.data() + kCellSpilledLenOffset, len, 4);
    PutLE(cell.data() + kCellSpilledPtrOffset, varheap_ptr, 8);
    return Status::OK();
}

StatusOr<CellValue> DecodeCell(std::span<const std::byte> cell) {
    if (Status s = CheckCellSpan(cell.size()); !s.ok()) return s;

    const auto raw_tag = static_cast<std::uint8_t>(cell[kCellTagOffset]);
    if (raw_tag > kMaxAssignedCellTag) {
        return Status::Corruption("tagged cell carries tag " + std::to_string(raw_tag) +
                                   ", beyond the highest assigned tag " +
                                   std::to_string(kMaxAssignedCellTag));
    }

    switch (static_cast<CellTag>(raw_tag)) {
        case CellTag::kNull:
            return CellValue{};
        case CellTag::kInline: {
            const std::uint64_t len = GetLE(cell.data() + kCellInlineLenOffset, 2);
            const std::uint32_t capacity = InlineCapacity(static_cast<std::uint32_t>(cell.size()));
            if (len > capacity) {
                // Never size a read from a length the page disagrees with.
                return Status::Corruption("inline cell claims " + std::to_string(len) +
                                           " byte(s) but only " + std::to_string(capacity) +
                                           " fit in a " + std::to_string(cell.size()) +
                                           "-byte cell");
            }
            CellValue value;
            value.tag = CellTag::kInline;
            value.bytes = cell.subspan(kCellInlineBytesOffset, static_cast<std::size_t>(len));
            value.len = static_cast<std::uint32_t>(len);
            return value;
        }
        case CellTag::kSpilled: {
            CellValue value;
            value.tag = CellTag::kSpilled;
            value.len = static_cast<std::uint32_t>(GetLE(cell.data() + kCellSpilledLenOffset, 4));
            value.varheap_ptr = GetLE(cell.data() + kCellSpilledPtrOffset, 8);
            return value;
        }
    }
    return Status::Corruption("unreachable: tagged cell tag " + std::to_string(raw_tag));
}

}  // namespace kds::storage
