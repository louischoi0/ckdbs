#include "kds/storage/keystone.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

#include "kds/storage/btree/btree_page.hpp"
#include "kds/storage/cabin_bound_page.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/wal/payload.hpp"

// BK-S1's red cells (`instructions/v3.0.0/workorder-bk-keystone-48.md` §3):
// the Keystone id is 48 bits, `id:48 | flags:8 | reserved:8` (BK-R1), and
// every store of an id that spelt 40 on its own takes the upper eight bits
// (BK-R2). Each cell fails at 5979b5e7 by the old answer - a refusal where
// the tree refuses the value, the old layout where it computes one - and
// BK-S2 turns them green. The trail cells are in waystone_replay_test.cpp
// and the named-key cell in sorted_leaf_named_keys_test.cpp, beside the
// harnesses they need.

namespace kds {
namespace {

constexpr std::uint64_t kBit40 = std::uint64_t{1} << 40;
constexpr std::uint64_t kFortyEightBitMax = (std::uint64_t{1} << 48) - 1;

std::array<std::byte, kKeystoneWordSize> LittleEndian(std::uint64_t word) {
    std::array<std::byte, kKeystoneWordSize> out{};
    for (std::size_t i = 0; i < kKeystoneWordSize; ++i) {
        out[i] = static_cast<std::byte>((word >> (8 * i)) & 0xFF);
    }
    return out;
}

TEST(KeystoneWidthTest, TheIdIsFortyEightBitsAndTheMaxRoundTrips) {
    EXPECT_EQ(kKeystoneIdBits, 48);
    EXPECT_EQ(kKeystoneReservedBits, 8);
    EXPECT_EQ(kMaxKeystoneId, kFortyEightBitMax);

    auto word = Keystone::Encode(kFortyEightBitMax, 0xFF, 0xFF);
    ASSERT_TRUE(word.ok()) << word.status().message();
    const Keystone k = Keystone::Decode(word.value());
    EXPECT_EQ(k.id, kFortyEightBitMax);
    EXPECT_EQ(k.flags, 0xFFu);
    EXPECT_EQ(k.reserved, 0xFFu);
}

TEST(KeystoneWidthTest, FlagsIsByteSixAndReservedByteSeven) {
    // The order's §1.1 claimed by grep that no site reads the word's fields
    // by byte offset. This pins, as bytes rather than as shifts, the layout
    // such a site would have to agree with - flags byte 6, reserved byte 7,
    // the payload's id read stopping below both. It runs no such site; the
    // grep is what found none.
    auto word = Keystone::Encode(7, 0xA5, 0x5A);
    ASSERT_TRUE(word.ok()) << word.status().message();
    const auto bytes = LittleEndian(word.value());
    EXPECT_EQ(std::to_integer<unsigned>(bytes[0]), 7u);
    for (std::size_t i = 1; i < 6; ++i) {
        EXPECT_EQ(std::to_integer<unsigned>(bytes[i]), 0u) << "id byte " << i;
    }
    EXPECT_EQ(std::to_integer<unsigned>(bytes[6]), 0xA5u) << "flags is byte 6";
    EXPECT_EQ(std::to_integer<unsigned>(bytes[7]), 0x5Au) << "reserved is byte 7";

    const Keystone k = Keystone::Decode(word.value());
    EXPECT_EQ(k.id, 7u);
    EXPECT_EQ(k.flags, 0xA5u);
    EXPECT_EQ(k.reserved, 0x5Au);
    auto id = KeystoneIdOfPayload(bytes);
    ASSERT_TRUE(id.ok()) << id.status().message();
    EXPECT_EQ(id.value(), 7u);
}

TEST(KeystoneWidthTest, ACabinBoundEntryKeepsTheUpperEightBitsOfItsPk) {
    // Flags 0x3 beside a pk whose bits 40-47 are 0x01: the half move that
    // widens `kMaxEntryPk` and leaves the flags shift at 40 ORs the flags
    // into the pk's upper byte, so pk and flags are both compared. The other
    // fields are `bound_cabin_test.cpp`'s round trip's.
    storage::cabin::BoundCabinEntry in;
    in.pk = kBit40 + 1;
    in.flags = storage::cabin::kEntryReserved | storage::cabin::kEntryHintValid;

    std::array<std::byte, storage::cabin::kEntryBytes> buf{};
    const Status s = storage::cabin::EncodeEntry(in, buf);
    ASSERT_TRUE(s.ok()) << s.message();
    const storage::cabin::BoundCabinEntry out = storage::cabin::DecodeEntry(buf);
    EXPECT_EQ(out.pk, in.pk);
    EXPECT_EQ(out.flags, in.flags);
}

TEST(KeystoneWidthTest, APageInitInTheUpperEightBitsRoundTripsAndFormatsItsLeaf) {
    // A btree leaf, redo's other branch for a PAGE_INIT with a min_key; the
    // heap page is the next cell's.
    wal::PageInitPayload fields{};
    fields.min_key = kBit40 + 1;
    fields.page_type = static_cast<std::uint8_t>(PageType::kBtreeLeaf);

    std::array<std::byte, wal::kPageInitPayloadSize> buf{};
    auto n = wal::EncodePageInit(buf, fields);
    ASSERT_TRUE(n.ok()) << n.status().message();
    auto decoded = wal::DecodePageInit(std::span<const std::byte>(buf).first(n.value()));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().min_key, kBit40 + 1);

    // What redo does with the decoded record (`wal/redo.cpp`'s
    // ApplyPageInit): format the page with that min_key.
    storage::InMemoryPageStore store(8);
    auto page = store.CreateNew();
    ASSERT_TRUE(page.ok()) << page.status().message();
    auto view = heap::PageView::CreateEmptyAs(page.value().second.bytes(), decoded.value().min_key,
                                              PageType::kBtreeLeaf, /*owner_oid=*/0);
    ASSERT_TRUE(view.ok()) << view.status().message();
    EXPECT_EQ(view.value().min_key(), kBit40 + 1);
}

TEST(KeystoneWidthTest, AHeapPageAndASeparatorTakeAnIdInTheUpperEightBits) {
    storage::InMemoryPageStore store(128);

    auto heap_page = store.CreateNew();
    ASSERT_TRUE(heap_page.ok()) << heap_page.status().message();
    auto heap = heap::PageView::CreateEmpty(heap_page.value().second.bytes(), kBit40 + 1);
    ASSERT_TRUE(heap.ok()) << heap.status().message();
    EXPECT_EQ(heap.value().min_key(), kBit40 + 1);

    auto inner_page = store.CreateNew();
    ASSERT_TRUE(inner_page.ok()) << inner_page.status().message();
    auto node = btree::InternalView::CreateEmpty(inner_page.value().second.bytes(), /*level=*/1,
                                                 /*leftmost=*/10, /*owner_oid=*/0);
    ASSERT_TRUE(node.ok()) << node.status().message();
    const Status sep = node.value().InsertEntry(kBit40 + 1, /*child=*/11);
    ASSERT_TRUE(sep.ok()) << sep.message();
    EXPECT_EQ(node.value().ChildFor(kBit40 + 1), 11u);
    EXPECT_EQ(node.value().ChildFor(kBit40), 10u);
    EXPECT_EQ(node.value().ChildFor(kFortyEightBitMax), 11u);
}

}  // namespace
}  // namespace kds
