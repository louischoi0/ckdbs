#include "kds/storage/in_memory_page_store.hpp"

#include <cstring>

#include <gtest/gtest.h>

namespace kds::storage {
namespace {

TEST(InMemoryPageStoreTest, CreateAtThenGetSeesWrittenBytes) {
    InMemoryPageStore store;

    auto created = store.CreateAt(42);
    ASSERT_TRUE(created.ok());
    std::memset(created.value().bytes().data(), 0xAB, 4);

    auto fetched = store.Get(42);
    ASSERT_TRUE(fetched.ok());
    EXPECT_EQ(fetched.value().bytes()[0], std::byte{0xAB});
    EXPECT_EQ(fetched.value().bytes()[3], std::byte{0xAB});
    EXPECT_EQ(fetched.value().bytes()[4], std::byte{0});  // rest stays zero-initialized
}

TEST(InMemoryPageStoreTest, CreateAtRejectsDuplicateId) {
    InMemoryPageStore store;
    ASSERT_TRUE(store.CreateAt(1).ok());

    auto second = store.CreateAt(1);
    EXPECT_FALSE(second.ok());
    EXPECT_EQ(second.status().code(), StatusCode::kAlreadyExists);
}

TEST(InMemoryPageStoreTest, GetFailsForUnknownId) {
    InMemoryPageStore store;
    auto fetched = store.Get(999);
    EXPECT_FALSE(fetched.ok());
    EXPECT_EQ(fetched.status().code(), StatusCode::kNotFound);
}

TEST(InMemoryPageStoreTest, CreateNewAssignsIncreasingIds) {
    InMemoryPageStore store(100);

    auto first = store.CreateNew();
    auto second = store.CreateNew();
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(first.value().first, 100u);
    EXPECT_EQ(second.value().first, 101u);
}

TEST(InMemoryPageStoreTest, CreateNewDoesNotCollideWithPriorCreateAt) {
    InMemoryPageStore store(10);
    ASSERT_TRUE(store.CreateAt(5).ok());

    auto created = store.CreateNew();
    ASSERT_TRUE(created.ok());
    EXPECT_EQ(created.value().first, 10u);
}

}  // namespace
}  // namespace kds::storage
