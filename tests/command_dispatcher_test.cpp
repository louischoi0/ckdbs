#include "kds/server/command_dispatcher.hpp"

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
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
    CommandDispatcher d(boot_->superblock, boot_->catalog);
    auto out = d.Dispatch("PING");
    EXPECT_EQ(out.response, "PONG");
    EXPECT_FALSE(out.should_stop);
}

TEST_F(CommandDispatcherTest, PingIsCaseInsensitive) {
    CommandDispatcher d(boot_->superblock, boot_->catalog);
    EXPECT_EQ(d.Dispatch("ping").response, "PONG");
    EXPECT_EQ(d.Dispatch("PiNg").response, "PONG");
}

TEST_F(CommandDispatcherTest, Stop) {
    CommandDispatcher d(boot_->superblock, boot_->catalog);
    auto out = d.Dispatch("STOP");
    EXPECT_EQ(out.response, "OK bye");
    EXPECT_TRUE(out.should_stop);
}

TEST_F(CommandDispatcherTest, ShowMetaReportsSuperblockFields) {
    CommandDispatcher d(boot_->superblock, boot_->catalog);
    auto out = d.Dispatch("SHOW META");
    EXPECT_NE(out.response.find("version=1"), std::string::npos);
    EXPECT_NE(out.response.find("max_page_id=128"), std::string::npos);
    EXPECT_FALSE(out.should_stop);
}

TEST_F(CommandDispatcherTest, ListTablesIncludesBootstrapTables) {
    CommandDispatcher d(boot_->superblock, boot_->catalog);
    auto out = d.Dispatch("LIST TABLES");
    EXPECT_NE(out.response.find("tables"), std::string::npos);
    EXPECT_NE(out.response.find("objects"), std::string::npos);
    EXPECT_NE(out.response.find("columns"), std::string::npos);
}

TEST_F(CommandDispatcherTest, FindTableReturnsOid) {
    CommandDispatcher d(boot_->superblock, boot_->catalog);
    auto out = d.Dispatch("FIND TABLE tables");
    EXPECT_EQ(out.response, "oid=" + std::to_string(catalog::kSysTablesTable));
}

TEST_F(CommandDispatcherTest, FindTableUnknownNameIsError) {
    CommandDispatcher d(boot_->superblock, boot_->catalog);
    auto out = d.Dispatch("FIND TABLE nope");
    EXPECT_EQ(out.response.substr(0, 4), "ERR ");
    EXPECT_FALSE(out.should_stop);
}

TEST_F(CommandDispatcherTest, UnknownCommandIsErrorNotCrash) {
    CommandDispatcher d(boot_->superblock, boot_->catalog);
    auto out = d.Dispatch("DROP EVERYTHING");
    EXPECT_EQ(out.response, "ERR unknown command");
    EXPECT_FALSE(out.should_stop);
}

TEST_F(CommandDispatcherTest, EmptyLineIsError) {
    CommandDispatcher d(boot_->superblock, boot_->catalog);
    auto out = d.Dispatch("   ");
    EXPECT_EQ(out.response, "ERR empty command");
}

}  // namespace
}  // namespace kds::server
