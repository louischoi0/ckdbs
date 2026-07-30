#include "kds/server/command_dispatcher.hpp"

#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/log.hpp"
#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/clock.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"

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
    EXPECT_NE(out.response.find("version=" + std::to_string(kSuperBlockVersion)),
              std::string::npos);
    EXPECT_NE(out.response.find("wal_anchor_count=0"), std::string::npos);
    EXPECT_FALSE(out.should_stop);
}

TEST_F(CommandDispatcherTest, ShowTablesIncludesBootstrapTables) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("SHOW TABLES");
    EXPECT_NE(out.response.find("tables"), std::string::npos);
    EXPECT_NE(out.response.find("objects"), std::string::npos);
    EXPECT_NE(out.response.find("columns"), std::string::npos);
}

TEST_F(CommandDispatcherTest, DescribeReportsTheHeaderTheOldFindTableDid) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("DESCRIBE tables");
    EXPECT_NE(out.response.find("oid=" + std::to_string(catalog::kSysTablesTable)),
              std::string::npos);
    EXPECT_NE(out.response.find("root_page_id=" + std::to_string(catalog::kCatalogPageTables)),
              std::string::npos);
    EXPECT_NE(out.response.find("clustered_type=HEAP"), std::string::npos);
}

TEST_F(CommandDispatcherTest, DescribeListsColumnsAndMarksThePrimaryKey) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int32, name varchar)").response.substr(0, 7),
              "CREATED");

    auto out = d.Dispatch("DESCRIBE acct");
    EXPECT_NE(out.response.find("columns=2"), std::string::npos) << out.response;
    // The declared type name round-trips back out of sys.types.
    EXPECT_NE(out.response.find("name=id type=int32"), std::string::npos) << out.response;
    EXPECT_NE(out.response.find("name=name type=varchar"), std::string::npos) << out.response;
    // Column 0 is the Keystone pk; nothing else is.
    EXPECT_NE(out.response.find("name=id type=int32 len=4 notnull=yes pk=yes autoincrement=yes"),
              std::string::npos)
        << out.response;
    EXPECT_NE(out.response.find("pk=no autoincrement=no"), std::string::npos) << out.response;
    // One "\n"-escaped section per column, and never a raw newline byte.
    EXPECT_EQ(out.response.find('\n'), std::string::npos);
}

TEST_F(CommandDispatcherTest, DescAbbreviationIsAccepted) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    EXPECT_EQ(d.Dispatch("DESC tables").response.substr(0, 4), "oid=");
}

TEST_F(CommandDispatcherTest, DescribeMissingNameIsError) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    EXPECT_EQ(d.Dispatch("DESCRIBE").response.substr(0, 4), "ERR ");
}

TEST_F(CommandDispatcherTest, FindTableIsNoLongerACommand) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    EXPECT_EQ(d.Dispatch("FIND TABLE tables").response, "ERR unknown command");
}

TEST_F(CommandDispatcherTest, DescribeUnknownNameIsError) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("DESCRIBE nope");
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
    auto slot0 = view.value().InsertTuple(bytes, /*trx_id=*/1);
    ASSERT_TRUE(slot0.ok());
    auto slot1 = view.value().InsertTuple(bytes, /*trx_id=*/2);
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
    auto slot0 = view.value().InsertTuple(bytes, /*trx_id=*/1);
    ASSERT_TRUE(slot0.ok());
    auto slot1 = view.value().InsertTuple(bytes, /*trx_id=*/2);
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
    ASSERT_TRUE(view.value().InsertTuple(bytes, /*trx_id=*/1).ok());

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
    auto out = d.Dispatch("CREATE TABLE accounts (id int64)");
    EXPECT_EQ(out.response.substr(0, 8), "CREATED ");
    EXPECT_NE(out.response.find("oid="), std::string::npos);

    auto found = d.Dispatch("DESCRIBE accounts");
    EXPECT_EQ(found.response.substr(0, 4), "oid=");
}

// The bare, pre-parser form asks for a zero-column table, and every
// relation's first column is its mandatory Keystone pk - so there is no
// such table to create.
TEST_F(CommandDispatcherTest, BareCreateTableIsRefusedForHavingNoPrimaryKey) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("CREATE TABLE accounts");
    EXPECT_EQ(out.response.substr(0, 4), "ERR ");
    EXPECT_NE(out.response.find("no columns"), std::string::npos) << out.response;
}

TEST_F(CommandDispatcherTest, CreateTableRejectsANonIntegerPrimaryKey) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("CREATE TABLE bad (name varchar, id int64)");
    EXPECT_EQ(out.response.substr(0, 4), "ERR ");
    EXPECT_NE(out.response.find("must be an integer type"), std::string::npos) << out.response;
}

TEST_F(CommandDispatcherTest, CreateTableIsIdempotentWhenAlreadyExists) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto first = d.Dispatch("CREATE TABLE accounts (id int64)");
    ASSERT_EQ(first.response.substr(0, 8), "CREATED ");
    auto first_oid = first.response.substr(std::string("CREATED oid=").size());

    auto second = d.Dispatch("CREATE TABLE accounts (id int64)");
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

// ---- Keystone primary key: system-generated, unique, autoincrement ------
//
// The pk is not a constraint checked after the fact - it is the id the
// engine issues (heap-and-tuple.md section 4, CLAUDE.md invariant 10), so
// these assert that the caller cannot supply it, cannot collide with it,
// and cannot change it.

TEST_F(CommandDispatcherTest, InsertAssignsAscendingIdsWithoutTheCallerSupplyingThem) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int32, name varchar)").response.substr(0, 7),
              "CREATED");

    auto first = d.Dispatch("INSERT INTO acct VALUES ('alice')");
    auto second = d.Dispatch("INSERT INTO acct VALUES ('bob')");
    EXPECT_NE(first.response.find("id=1"), std::string::npos) << first.response;
    EXPECT_NE(second.response.find("id=2"), std::string::npos) << second.response;

    // And the issued ids are what SELECT reads back for the pk column.
    auto rows = d.Dispatch("SELECT * FROM acct");
    EXPECT_NE(rows.response.find("1,alice"), std::string::npos) << rows.response;
    EXPECT_NE(rows.response.find("2,bob"), std::string::npos) << rows.response;
}

TEST_F(CommandDispatcherTest, SupplyingThePrimaryKeyOnInsertIsRefused) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int32, name varchar)").response.substr(0, 7),
              "CREATED");

    auto out = d.Dispatch("INSERT INTO acct VALUES (1, 'alice')");
    EXPECT_EQ(out.response.substr(0, 4), "ERR ") << out.response;
    EXPECT_NE(out.response.find("primary-key column 'id'"), std::string::npos) << out.response;

    // Nothing was written, and no id was burned.
    EXPECT_NE(d.Dispatch("DESCRIBE acct").response.find("next_id=1"), std::string::npos);
}

TEST_F(CommandDispatcherTest, RepeatedInsertsOfTheSameValuesGetDistinctKeys) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int32, name varchar)").response.substr(0, 7),
              "CREATED");

    // The duplicate the old code allowed: identical rows, same key. Now
    // each gets its own id, so "same id twice" is not expressible.
    ASSERT_EQ(d.Dispatch("INSERT INTO acct VALUES ('alice')").response.substr(0, 8), "INSERTED");
    ASSERT_EQ(d.Dispatch("INSERT INTO acct VALUES ('alice')").response.substr(0, 8), "INSERTED");

    auto out = d.Dispatch("DESCRIBE acct");
    EXPECT_NE(out.response.find("next_id=3"), std::string::npos) << out.response;
}

TEST_F(CommandDispatcherTest, TheIdSequenceDoesNotRestartAfterReopeningTheCatalog) {
    {
        CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
        ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int32, name varchar)").response.substr(0, 7),
                  "CREATED");
        ASSERT_EQ(d.Dispatch("INSERT INTO acct VALUES ('alice')").response.substr(0, 8),
                  "INSERTED");
    }

    // A fresh Catalog over the same store - the sequence lives in the
    // sys.tables row, not in the process.
    catalog::Catalog reopened(store_);
    CommandDispatcher d(boot_->superblock, reopened, store_);
    auto out = d.Dispatch("INSERT INTO acct VALUES ('bob')");
    EXPECT_NE(out.response.find("id=2"), std::string::npos) << out.response;
}

TEST_F(CommandDispatcherTest, UpdatingThePrimaryKeyIsRefused) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int32, name varchar)").response.substr(0, 7),
              "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO acct VALUES ('alice')").response.substr(0, 8), "INSERTED");

    auto out = d.Dispatch("UPDATE acct SET id = 99");
    EXPECT_EQ(out.response.substr(0, 4), "ERR ") << out.response;
    EXPECT_NE(out.response.find("cannot be updated"), std::string::npos) << out.response;

    // The row is untouched, key included.
    EXPECT_NE(d.Dispatch("SELECT * FROM acct").response.find("1,alice"), std::string::npos);
}

TEST_F(CommandDispatcherTest, UpdatingANonKeyColumnPreservesTheKey) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int32, name varchar)").response.substr(0, 7),
              "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO acct VALUES ('alice')").response.substr(0, 8), "INSERTED");

    // Same-length replacement: PageView::OverwriteTuple is an in-place
    // HOT-style write and a growing tuple needs the not-yet-built
    // relocation path, which is unrelated to what this test is about.
    ASSERT_EQ(d.Dispatch("UPDATE acct SET name = 'wendy'").response.substr(0, 7), "UPDATED");
    EXPECT_NE(d.Dispatch("SELECT * FROM acct").response.find("1,wendy"), std::string::npos);
}

TEST_F(CommandDispatcherTest, CreateTableMissingNameIsError) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("CREATE TABLE");
    EXPECT_EQ(out.response.substr(0, 4), "ERR ");
}

// SYNC is what makes a mutation outlive the process: without it, a kill
// (here: MemoryPageDevice::Crash) drops everything written since startup.
TEST(CommandDispatcherSyncTest, SyncPersistsThroughAnUncleanShutdown) {
    auto device = storage::MemoryPageDevice::Create(8);
    ASSERT_TRUE(device.ok());

    {
        auto store = storage::DevicePageStore::Open(*device.value(), kFirstUserPageId);
        ASSERT_TRUE(store.ok());
        auto boot = bootstrap::BootstrapDatabase(*store.value(), 1000);
        ASSERT_TRUE(boot.ok());

        CommandDispatcher d(boot.value().superblock, boot.value().catalog, *store.value());
        ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, note varchar)").response.substr(0, 7),
                  "CREATED");
        ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES ('seven')").response.substr(0, 8), "INSERTED");
        EXPECT_EQ(d.Dispatch("SYNC").response, "OK synced");
    }
    device.value()->Crash();

    auto store = storage::DevicePageStore::Open(*device.value(), kFirstUserPageId);
    ASSERT_TRUE(store.ok());
    auto boot = bootstrap::BootstrapDatabase(*store.value(), 2000);
    ASSERT_TRUE(boot.ok());

    CommandDispatcher d(boot.value().superblock, boot.value().catalog, *store.value());
    EXPECT_NE(d.Dispatch("SELECT * FROM t").response.find("seven"), std::string::npos);
}

// ---- Diagnostics ---------------------------------------------------------
//
// Every critical operation reports, and the level it reports at is the
// contract: `info` must stay quiet under ordinary load, so a running server
// is not paying a write() per tuple to say nothing.

class DispatcherLogTest : public CommandDispatcherTest {
protected:
    // Builds a dispatcher logging at `level` into `sink_`.
    CommandDispatcher Make(LogLevel level) {
        logger_.emplace(&sink_, wall_clock_, level);
        return CommandDispatcher(boot_->superblock, boot_->catalog, store_, &*logger_, &clock_);
    }

    bool Logged(std::string_view needle) const {
        for (const std::string& line : sink_.lines) {
            if (line.find(needle) != std::string::npos) return true;
        }
        return false;
    }

    MemoryLogSink sink_;
    ManualWallClock wall_clock_{1000};
    sched::ManualClock clock_;
    std::optional<Logger> logger_;
};

TEST_F(DispatcherLogTest, CreateTableIsLoggedAtInfo) {
    CommandDispatcher d = Make(LogLevel::kInfo);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int64, name varchar)").response.substr(0, 7),
              "CREATED");

    EXPECT_TRUE(Logged("[ddl] created table 'acct'")) << sink_.lines.size();
    EXPECT_TRUE(Logged("columns=2"));
}

TEST_F(DispatcherLogTest, SyncIsLoggedAtInfo) {
    CommandDispatcher d = Make(LogLevel::kInfo);
    ASSERT_EQ(d.Dispatch("SYNC").response, "OK synced");
    EXPECT_TRUE(Logged("[storage] client SYNC"));
}

TEST_F(DispatcherLogTest, OrdinaryReadsAndWritesAreSilentAtInfo) {
    CommandDispatcher d = Make(LogLevel::kInfo);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int64, name varchar)").response.substr(0, 7),
              "CREATED");
    const std::size_t after_ddl = sink_.lines.size();

    d.Dispatch("INSERT INTO acct VALUES ('alice')");
    d.Dispatch("SELECT * FROM acct");
    d.Dispatch("PING");

    // The whole point of the level choices: a busy server at the default
    // level writes nothing per query.
    EXPECT_EQ(sink_.lines.size(), after_ddl);
}

TEST_F(DispatcherLogTest, EachQueryIsLoggedAtDebugWithADuration) {
    CommandDispatcher d = Make(LogLevel::kDebug);
    d.Dispatch("PING");

    EXPECT_TRUE(Logged("[query] \"PING\"")) << "the command itself must appear";
    EXPECT_TRUE(Logged("us")) << "a duration must appear when a clock is injected";
}

TEST_F(DispatcherLogTest, AFailedQueryIsLoggedAtWarnWithItsReason) {
    CommandDispatcher d = Make(LogLevel::kWarn);
    d.Dispatch("SELECT * FROM nosuchtable");

    // Warn, not Debug: an error is the one case where the whole reply is
    // worth keeping, and it must survive a threshold above debug.
    EXPECT_TRUE(Logged("[query]"));
    EXPECT_TRUE(Logged("ERR "));
}

TEST_F(DispatcherLogTest, ASuccessfulReplyIsSummarizedNotEchoed) {
    CommandDispatcher d = Make(LogLevel::kDebug);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int64, name varchar)").response.substr(0, 7),
              "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO acct VALUES ('supersecretvalue')").response.substr(0, 8),
              "INSERTED");
    sink_.lines.clear();

    d.Dispatch("SELECT * FROM acct");

    // A log that reproduces result sets is a log that cannot be kept.
    EXPECT_TRUE(Logged("B reply"));
    EXPECT_FALSE(Logged("supersecretvalue"));
}

TEST_F(DispatcherLogTest, HeapInsertIsLoggedAtTraceWithPageSlotAndKey) {
    CommandDispatcher d = Make(LogLevel::kTrace);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int64, name varchar)").response.substr(0, 7),
              "CREATED");
    sink_.lines.clear();

    ASSERT_EQ(d.Dispatch("INSERT INTO acct VALUES ('alice')").response.substr(0, 8), "INSERTED");

    EXPECT_TRUE(Logged("[heap] insert page="));
    EXPECT_TRUE(Logged("slot=0"));
    EXPECT_TRUE(Logged("id=1"));
}

TEST_F(DispatcherLogTest, HeapOverwriteIsLoggedAtTrace) {
    CommandDispatcher d = Make(LogLevel::kTrace);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int64, name varchar)").response.substr(0, 7),
              "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO acct VALUES ('alice')").response.substr(0, 8), "INSERTED");
    sink_.lines.clear();

    ASSERT_EQ(d.Dispatch("UPDATE acct SET name = 'wendy'").response.substr(0, 7), "UPDATED");
    // An UPDATE spans the whole page chain now, so the line counts pages
    // touched rather than naming the one page a table used to be.
    EXPECT_TRUE(Logged("[heap] overwrite rows=1"));
    EXPECT_TRUE(Logged("1 page(s)"));
}

TEST_F(DispatcherLogTest, ANullLoggerLeavesEveryCommandWorking) {
    // The default construction path the socket-free tests use.
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    EXPECT_EQ(d.Dispatch("PING").response, "PONG");
    EXPECT_EQ(d.Dispatch("CREATE TABLE acct (id int64)").response.substr(0, 7), "CREATED");
    EXPECT_TRUE(sink_.lines.empty());
}

}  // namespace
}  // namespace kds::server
