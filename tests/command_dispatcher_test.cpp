#include "kds/server/command_dispatcher.hpp"

#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"

namespace kds::server {
namespace {

class CommandDispatcherTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
};

TEST_F(CommandDispatcherTest, Ping) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("PING");
    EXPECT_EQ(out.response, "PONG");
    EXPECT_FALSE(out.should_stop);
}

TEST_F(CommandDispatcherTest, PingIsCaseInsensitive) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    EXPECT_EQ(d.Dispatch("ping").response, "PONG");
    EXPECT_EQ(d.Dispatch("PiNg").response, "PONG");
}

TEST_F(CommandDispatcherTest, Stop) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("STOP");
    EXPECT_EQ(out.response, "OK bye");
    EXPECT_TRUE(out.should_stop);
}

TEST_F(CommandDispatcherTest, ShowMetaReportsSuperblockFields) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("SHOW META");
    EXPECT_NE(out.response.find("version=1"), std::string::npos);
    EXPECT_NE(out.response.find("max_page_id=128"), std::string::npos);
    EXPECT_FALSE(out.should_stop);
}

TEST_F(CommandDispatcherTest, ShowTablesIncludesBootstrapTables) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("SHOW TABLES");
    EXPECT_NE(out.response.find("tables"), std::string::npos);
    EXPECT_NE(out.response.find("objects"), std::string::npos);
    EXPECT_NE(out.response.find("columns"), std::string::npos);
}

TEST_F(CommandDispatcherTest, FindTableReturnsOid) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("FIND TABLE tables");
    EXPECT_EQ(out.response, "oid=" + std::to_string(catalog::kSysTablesTable));
}

TEST_F(CommandDispatcherTest, FindTableUnknownNameIsError) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("FIND TABLE nope");
    EXPECT_EQ(out.response.substr(0, 4), "ERR ");
    EXPECT_FALSE(out.should_stop);
}

TEST_F(CommandDispatcherTest, UnknownCommandIsErrorNotCrash) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("DROP EVERYTHING");
    EXPECT_EQ(out.response, "ERR unknown command");
    EXPECT_FALSE(out.should_stop);
}

TEST_F(CommandDispatcherTest, EmptyLineIsError) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("   ");
    EXPECT_EQ(out.response, "ERR empty command");
}

TEST_F(CommandDispatcherTest, ShowPageReportsHeaderAndSlots) {
    constexpr PageId kPageId = 500;
    auto page = store_.CreateAt(kPageId);
    ASSERT_TRUE(page.ok());
    auto view = heap::PageView::CreateEmpty(page.value(), 42);
    ASSERT_TRUE(view.ok());

    std::string payload = "hello";
    std::vector<std::byte> bytes(payload.size());
    std::memcpy(bytes.data(), payload.data(), payload.size());
    auto slot0 = view.value().InsertTuple(bytes, /*xmin=*/1);
    ASSERT_TRUE(slot0.ok());
    auto slot1 = view.value().InsertTuple(bytes, /*xmin=*/2);
    ASSERT_TRUE(slot1.ok());
    ASSERT_TRUE(view.value().RetireSlot(slot1.value()).ok());

    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("SHOW PAGE " + std::to_string(kPageId));

    EXPECT_NE(out.response.find("page_id=500\\n"), std::string::npos);
    EXPECT_NE(out.response.find("min_key=42\\n"), std::string::npos);
    EXPECT_NE(out.response.find("nr_slots=2\\n"), std::string::npos);
    EXPECT_NE(out.response.find("slot[0]"), std::string::npos);
    EXPECT_NE(out.response.find("slot[1]"), std::string::npos);
    EXPECT_NE(out.response.find("dead=1"), std::string::npos);
    EXPECT_EQ(out.response.find('\n'), std::string::npos);  // one wire line, only escaped "\n"
}

TEST_F(CommandDispatcherTest, ShowPageMissingIdIsError) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("SHOW PAGE");
    EXPECT_EQ(out.response.substr(0, 4), "ERR ");
}

TEST_F(CommandDispatcherTest, ShowPageInvalidIdIsError) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("SHOW PAGE notanumber");
    EXPECT_EQ(out.response.substr(0, 4), "ERR ");
}

TEST_F(CommandDispatcherTest, ShowPageUnknownIdIsError) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("SHOW PAGE 999999");
    EXPECT_EQ(out.response.substr(0, 4), "ERR ");
}

TEST_F(CommandDispatcherTest, ShowPageValuesIncludesLiveTuplePayloadHexEncoded) {
    constexpr PageId kPageId = 501;
    auto page = store_.CreateAt(kPageId);
    ASSERT_TRUE(page.ok());
    auto view = heap::PageView::CreateEmpty(page.value(), 0);
    ASSERT_TRUE(view.ok());

    std::string payload = "hello";  // hex: 68656c6c6f
    std::vector<std::byte> bytes(payload.size());
    std::memcpy(bytes.data(), payload.data(), payload.size());
    auto slot0 = view.value().InsertTuple(bytes, /*xmin=*/1);
    ASSERT_TRUE(slot0.ok());
    auto slot1 = view.value().InsertTuple(bytes, /*xmin=*/2);
    ASSERT_TRUE(slot1.ok());
    ASSERT_TRUE(view.value().RetireSlot(slot1.value()).ok());

    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("SHOW PAGE " + std::to_string(kPageId) + " VALUES");

    EXPECT_NE(out.response.find("value=68656c6c6f"), std::string::npos);
    // Dead slot's value must not be read/shown.
    auto slot1_pos = out.response.find("slot[1]");
    ASSERT_NE(slot1_pos, std::string::npos);
    auto next_slot_marker = out.response.find("\\n", slot1_pos);
    std::string slot1_section = out.response.substr(
        slot1_pos, next_slot_marker == std::string::npos ? std::string::npos
                                                          : next_slot_marker - slot1_pos);
    EXPECT_EQ(slot1_section.find("value="), std::string::npos);
}

TEST_F(CommandDispatcherTest, ShowPageWithoutValuesOmitsPayload) {
    constexpr PageId kPageId = 502;
    auto page = store_.CreateAt(kPageId);
    ASSERT_TRUE(page.ok());
    auto view = heap::PageView::CreateEmpty(page.value(), 0);
    ASSERT_TRUE(view.ok());

    std::string payload = "hello";
    std::vector<std::byte> bytes(payload.size());
    std::memcpy(bytes.data(), payload.data(), payload.size());
    ASSERT_TRUE(view.value().InsertTuple(bytes, /*xmin=*/1).ok());

    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("SHOW PAGE " + std::to_string(kPageId));
    EXPECT_EQ(out.response.find("value="), std::string::npos);
}

TEST_F(CommandDispatcherTest, ShowPageUnknownOptionIsError) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("SHOW PAGE 500 BOGUS");
    EXPECT_EQ(out.response.substr(0, 4), "ERR ");
}

TEST_F(CommandDispatcherTest, CreateTableCreatesNewTable) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("CREATE TABLE accounts");
    EXPECT_EQ(out.response.substr(0, 8), "CREATED ");
    EXPECT_NE(out.response.find("oid="), std::string::npos);

    auto found = d.Dispatch("FIND TABLE accounts");
    EXPECT_EQ(found.response.substr(0, 4), "oid=");
}

TEST_F(CommandDispatcherTest, CreateTableIsIdempotentWhenAlreadyExists) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto first = d.Dispatch("CREATE TABLE accounts");
    ASSERT_EQ(first.response.substr(0, 8), "CREATED ");
    auto first_oid = first.response.substr(std::string("CREATED oid=").size());

    auto second = d.Dispatch("CREATE TABLE accounts");
    EXPECT_EQ(second.response.substr(0, 7), "EXISTS ");
    EXPECT_EQ(second.response.substr(std::string("EXISTS oid=").size()), first_oid);

    // Only one row should exist for this name.
    auto tables = d.Dispatch("SHOW TABLES");
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = tables.response.find("accounts", pos)) != std::string::npos) {
        ++count;
        pos += std::string("accounts").size();
    }
    EXPECT_EQ(count, 1u);
}

TEST_F(CommandDispatcherTest, CreateTableMissingNameIsError) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("CREATE TABLE");
    EXPECT_EQ(out.response.substr(0, 4), "ERR ");
}

}  // namespace
}  // namespace kds::server
