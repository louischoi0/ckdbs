#include "kds/storage/heap/heap_page.hpp"

#include <array>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace kds::heap {
namespace {

using PageBuf = std::array<std::byte, kPageSize>;

std::span<std::byte, kPageSize> AsSpan(PageBuf& buf) {
    return std::span<std::byte, kPageSize>(buf);
}

std::vector<std::byte> BytesOf(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

std::string StringOf(std::span<const std::byte> b) {
    std::string out(b.size(), '\0');
    std::memcpy(out.data(), b.data(), b.size());
    return out;
}

TEST(HeapPageTest, CreateEmptyInitializesHeader) {
    PageBuf buf{};
    auto view = PageView::CreateEmpty(AsSpan(buf), 100);
    ASSERT_TRUE(view.ok());

    EXPECT_EQ(view.value().min_key(), 100u);
    EXPECT_EQ(view.value().slot_count(), 0u);
    EXPECT_EQ(view.value().next_page_id(), kInvalidPageId);

    std::size_t expected_free = kNextPageIdOffset - kHeaderSize;
    EXPECT_EQ(view.value().free_space(), expected_free);
}

TEST(HeapPageTest, CreateEmptyRejectsMinKeyBeyond40Bits) {
    PageBuf buf{};
    auto view = PageView::CreateEmpty(AsSpan(buf), (std::uint64_t{1} << 40));
    EXPECT_FALSE(view.ok());
    EXPECT_EQ(view.status().code(), StatusCode::kInvalidArgument);
}

TEST(HeapPageTest, InsertThenReadRoundTrip) {
    PageBuf buf{};
    auto created = PageView::CreateEmpty(AsSpan(buf), 0);
    ASSERT_TRUE(created.ok());
    PageView page = created.value();

    auto payload = BytesOf("hello heap page");
    auto slot = page.InsertTuple(payload, /*xmin=*/7, /*xmax=*/0, /*undo_ptr=*/0);
    ASSERT_TRUE(slot.ok());
    EXPECT_EQ(slot.value(), 0u);

    auto tuple = page.ReadTuple(slot.value());
    ASSERT_TRUE(tuple.ok());
    EXPECT_EQ(tuple.value().xmin, 7u);
    EXPECT_EQ(tuple.value().xmax, 0u);
    EXPECT_EQ(tuple.value().undo_ptr, 0u);
    EXPECT_EQ(StringOf(tuple.value().payload), "hello heap page");
}

TEST(HeapPageTest, InsertAssignsIncreasingSlotIndices) {
    PageBuf buf{};
    auto created = PageView::CreateEmpty(AsSpan(buf), 0);
    ASSERT_TRUE(created.ok());
    PageView page = created.value();

    auto s0 = page.InsertTuple(BytesOf("a"), 1);
    auto s1 = page.InsertTuple(BytesOf("bb"), 2);
    auto s2 = page.InsertTuple(BytesOf("ccc"), 3);
    ASSERT_TRUE(s0.ok());
    ASSERT_TRUE(s1.ok());
    ASSERT_TRUE(s2.ok());
    EXPECT_EQ(s0.value(), 0u);
    EXPECT_EQ(s1.value(), 1u);
    EXPECT_EQ(s2.value(), 2u);
    EXPECT_EQ(page.slot_count(), 3u);

    EXPECT_EQ(StringOf(page.ReadTuple(0).value().payload), "a");
    EXPECT_EQ(StringOf(page.ReadTuple(1).value().payload), "bb");
    EXPECT_EQ(StringOf(page.ReadTuple(2).value().payload), "ccc");
}

TEST(HeapPageTest, OverwriteTupleInPlaceKeepsSlotIndex) {
    PageBuf buf{};
    auto created = PageView::CreateEmpty(AsSpan(buf), 0);
    ASSERT_TRUE(created.ok());
    PageView page = created.value();

    auto slot = page.InsertTuple(BytesOf("original"), 1, 0, 0);
    ASSERT_TRUE(slot.ok());

    auto cap = page.SlotCapacity(slot.value());
    ASSERT_TRUE(cap.ok());
    EXPECT_EQ(cap.value(), 8u);  // strlen("original")

    Status overwritten = page.OverwriteTuple(slot.value(), BytesOf("replaced"), 2, 3, 4);
    EXPECT_TRUE(overwritten.ok());

    auto tuple = page.ReadTuple(slot.value());
    ASSERT_TRUE(tuple.ok());
    EXPECT_EQ(tuple.value().xmin, 2u);
    EXPECT_EQ(tuple.value().xmax, 3u);
    EXPECT_EQ(tuple.value().undo_ptr, 4u);
    EXPECT_EQ(StringOf(tuple.value().payload), "replaced");
    EXPECT_EQ(page.slot_count(), 1u);  // no new slot was created
}

TEST(HeapPageTest, OverwriteTupleFailsWhenPayloadExceedsReservation) {
    PageBuf buf{};
    auto created = PageView::CreateEmpty(AsSpan(buf), 0);
    ASSERT_TRUE(created.ok());
    PageView page = created.value();

    auto slot = page.InsertTuple(BytesOf("short"), 1);
    ASSERT_TRUE(slot.ok());

    Status overwritten = page.OverwriteTuple(slot.value(), BytesOf("this is much too long"), 1, 0, 0);
    EXPECT_FALSE(overwritten.ok());
    EXPECT_EQ(overwritten.code(), StatusCode::kOutOfSpace);

    // A failed overwrite must not have touched the existing tuple.
    EXPECT_EQ(StringOf(page.ReadTuple(slot.value()).value().payload), "short");
}

TEST(HeapPageTest, OverwriteTupleOutOfRangeIsNotFound) {
    PageBuf buf{};
    auto created = PageView::CreateEmpty(AsSpan(buf), 0);
    ASSERT_TRUE(created.ok());
    PageView page = created.value();

    Status overwritten = page.OverwriteTuple(0, BytesOf("x"), 1, 0, 0);
    EXPECT_FALSE(overwritten.ok());
    EXPECT_EQ(overwritten.code(), StatusCode::kNotFound);
}

TEST(HeapPageTest, ZeroLengthPayloadRoundTrips) {
    PageBuf buf{};
    auto created = PageView::CreateEmpty(AsSpan(buf), 0);
    ASSERT_TRUE(created.ok());
    PageView page = created.value();

    auto slot = page.InsertTuple(std::span<const std::byte>{}, 5);
    ASSERT_TRUE(slot.ok());

    auto tuple = page.ReadTuple(slot.value());
    ASSERT_TRUE(tuple.ok());
    EXPECT_EQ(tuple.value().payload.size(), 0u);
}

TEST(HeapPageTest, ReadTupleOutOfRangeIsNotFound) {
    PageBuf buf{};
    auto created = PageView::CreateEmpty(AsSpan(buf), 0);
    ASSERT_TRUE(created.ok());
    PageView page = created.value();

    auto tuple = page.ReadTuple(0);
    EXPECT_FALSE(tuple.ok());
    EXPECT_EQ(tuple.status().code(), StatusCode::kNotFound);
}

TEST(HeapPageTest, RetireSlotHidesTupleFromRead) {
    PageBuf buf{};
    auto created = PageView::CreateEmpty(AsSpan(buf), 0);
    ASSERT_TRUE(created.ok());
    PageView page = created.value();

    auto slot = page.InsertTuple(BytesOf("x"), 1);
    ASSERT_TRUE(slot.ok());

    Status retire = page.RetireSlot(slot.value());
    EXPECT_TRUE(retire.ok());

    auto tuple = page.ReadTuple(slot.value());
    EXPECT_FALSE(tuple.ok());
    EXPECT_EQ(tuple.status().code(), StatusCode::kNotFound);

    // Retiring an already-dead slot is reported, not silently accepted.
    Status retire_again = page.RetireSlot(slot.value());
    EXPECT_FALSE(retire_again.ok());
}

TEST(HeapPageTest, RetireSlotOutOfRangeIsNotFound) {
    PageBuf buf{};
    auto created = PageView::CreateEmpty(AsSpan(buf), 0);
    ASSERT_TRUE(created.ok());
    PageView page = created.value();

    Status retire = page.RetireSlot(0);
    EXPECT_FALSE(retire.ok());
    EXPECT_EQ(retire.code(), StatusCode::kNotFound);
}

TEST(HeapPageTest, InsertFailsWithOutOfSpaceWhenPageIsFull) {
    PageBuf buf{};
    auto created = PageView::CreateEmpty(AsSpan(buf), 0);
    ASSERT_TRUE(created.ok());
    PageView page = created.value();

    // A payload sized to exactly the page's initial free space can never
    // fit alongside its own slot + tuple header overhead.
    std::vector<std::byte> huge(page.free_space(), std::byte{0});
    auto slot = page.InsertTuple(huge, 1);
    EXPECT_FALSE(slot.ok());
    EXPECT_EQ(slot.status().code(), StatusCode::kOutOfSpace);

    // A failed insert must not have mutated page state.
    EXPECT_EQ(page.slot_count(), 0u);
}

TEST(HeapPageTest, HasSpaceForMatchesInsertOutcome) {
    PageBuf buf{};
    auto created = PageView::CreateEmpty(AsSpan(buf), 0);
    ASSERT_TRUE(created.ok());
    PageView page = created.value();

    // Fill the page with small fixed-size tuples until it reports full,
    // and check HasSpaceFor's prediction matches InsertTuple's outcome at
    // every step.
    const std::string payload(16, 'z');
    int inserted = 0;
    while (true) {
        bool predicted = page.HasSpaceFor(static_cast<std::uint16_t>(payload.size()));
        auto slot = page.InsertTuple(BytesOf(payload), 1);
        EXPECT_EQ(predicted, slot.ok());
        if (!slot.ok()) {
            EXPECT_EQ(slot.status().code(), StatusCode::kOutOfSpace);
            break;
        }
        ++inserted;
        ASSERT_LT(inserted, 10000);  // guard against an infinite loop on a bug
    }
    EXPECT_GT(inserted, 0);
}

TEST(HeapPageTest, NextPageIdDefaultsToInvalidAndRoundTrips) {
    PageBuf buf{};
    auto created = PageView::CreateEmpty(AsSpan(buf), 0);
    ASSERT_TRUE(created.ok());
    PageView page = created.value();

    EXPECT_EQ(page.next_page_id(), kInvalidPageId);

    page.set_next_page_id(42);
    EXPECT_EQ(page.next_page_id(), 42u);
}

TEST(HeapPageTest, MinKeyUnaffectedByInserts) {
    PageBuf buf{};
    auto created = PageView::CreateEmpty(AsSpan(buf), 555);
    ASSERT_TRUE(created.ok());
    PageView page = created.value();

    ASSERT_TRUE(page.InsertTuple(BytesOf("a"), 1).ok());
    ASSERT_TRUE(page.InsertTuple(BytesOf("b"), 2).ok());

    EXPECT_EQ(page.min_key(), 555u);
}

}  // namespace
}  // namespace kds::heap
