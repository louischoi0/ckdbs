#include "kds/server/command_dispatcher.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>   // std::make_unique, for the scheduler-view test's FunctionTask
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/log.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/parser/parser.hpp"
#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "view_text.hpp"

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

// ---- SHOW PATTERNS: `sys.patterns` (docs/spec/waystone-concpets.md section 4) --

TEST_F(CommandDispatcherTest, ShowPatternsOnAFreshDatabaseReportsNone) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    const auto shown = testing_view::ReadViewText(d.Dispatch("SHOW PATTERNS").response);
    EXPECT_FALSE(shown.columns.empty());
    EXPECT_TRUE(shown.rows.empty());
}

TEST_F(CommandDispatcherTest, ShowPatternsListsARegisteredPattern) {
    ASSERT_TRUE(boot_->catalog.RegisterPattern(0xABCD, catalog::kStmtClassUnclassified).ok());

    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    const std::string reply = d.Dispatch("SHOW PATTERNS").response;
    const auto shown = testing_view::ReadViewText(reply);
    ASSERT_EQ(shown.rows.size(), 1u) << reply;
    const std::string id = std::to_string(0xABCD);
    EXPECT_EQ(shown.Find("fetch_id", id, "use_count"), "0") << reply;
    // No trail has been recorded, so there is no directory - and the view
    // says so with NULL rather than a root of page 0 or the invalid id.
    EXPECT_EQ(shown.Find("fetch_id", id, "waystone_root"), "NULL") << reply;
    EXPECT_EQ(shown.Find("fetch_id", id, "fingerprint_version"),
              std::to_string(parser::kFingerprintVersion))
        << reply;
}

TEST_F(CommandDispatcherTest, ShowPatternsReportsAWaystoneDirectory) {
    ASSERT_TRUE(boot_->catalog.RegisterPattern(7, catalog::kStmtClassUnclassified).ok());
    ASSERT_TRUE(boot_->catalog.ClaimPatternWaystoneRoot(7, 4096, 2).ok());

    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    const std::string reply = d.Dispatch("SHOW PATTERNS").response;
    const auto shown = testing_view::ReadViewText(reply);
    EXPECT_EQ(shown.Find("fetch_id", "7", "waystone_root"), "4096") << reply;
    EXPECT_EQ(shown.Find("fetch_id", "7", "dir_depth"), "2") << reply;
}

TEST_F(CommandDispatcherTest, ShowPatternsMarksRowsFromAnotherFingerprintVersion) {
    // Written straight onto the page, the way a stale row really appears:
    // an older build recorded it and then the version moved. No API can
    // produce one, since RegisterPattern() stamps the current version.
    auto bytes = store_.Get(catalog::kCatalogPagePatterns);
    ASSERT_TRUE(bytes.ok());
    heap::PageView page(bytes.value().bytes());
    catalog::SysPatternRow row{};
    row.oid = 999;
    row.fetch_id = 0x5151;
    row.fingerprint_version = parser::kFingerprintVersion + 1;
    row.waystone_root = kInvalidPageId;
    auto encoded = row.Encode();
    ASSERT_TRUE(page.InsertTuple(encoded, catalog::kBootstrapXid).ok());

    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    const std::string reply = d.Dispatch("SHOW PATTERNS").response;

    // Listed, not hidden: this is an inspection surface, and a row nothing
    // will ever look up again is exactly what an operator needs to see. Its
    // version column is how a reader tells it is stale.
    EXPECT_EQ(testing_view::ReadViewText(reply).Find("fetch_id", std::to_string(0x5151),
                                                     "fingerprint_version"),
              std::to_string(parser::kFingerprintVersion + 1))
        << reply;

    // But it is still invisible to a lookup, which is where the version
    // rule lives.
    EXPECT_FALSE(boot_->catalog.FindPattern(0x5151).ok());
}

TEST_F(CommandDispatcherTest, ShowPatternsResponseIsOneLine) {
    ASSERT_TRUE(boot_->catalog.RegisterPattern(1, catalog::kStmtClassUnclassified).ok());
    ASSERT_TRUE(boot_->catalog.RegisterPattern(2, catalog::kStmtClassUnclassified).ok());

    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("SHOW PATTERNS");

    // The wire contract: one command, one line back. Sections are the
    // two-character "\n" escape, never a raw newline byte.
    EXPECT_EQ(out.response.find('\n'), std::string::npos);
    EXPECT_NE(out.response.find("\\n"), std::string::npos) << out.response;
}

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
    using testing_view::MetaField;
    EXPECT_EQ(MetaField(out.response, "version"), std::to_string(kSuperBlockVersion));
    // The fixture bootstraps a real database, so its log is the instance's
    // one stream (AR0 M0) - and the anchor count then says nothing a reader
    // could act on, always being 1, so the topology is printed instead.
    EXPECT_EQ(MetaField(out.response, "wal_topology"), "single") << out.response;
    EXPECT_EQ(MetaField(out.response, "wal_anchor_count"), std::nullopt)
        << "a field that can only ever say one thing: " << out.response;
    EXPECT_FALSE(out.should_stop);
}

// ---- The cross-core write refusal counters, retired (AT-S9) -------------
//
// §6's per-core counter keyed (home core, target core, relation) counted a
// write whose relation another core owned. No core owns a relation since
// AT-S9, so there is nothing for the key to name and SHOW META prints none
// of it - on a quiet core, and after a write from a core that did not
// create the relation, which is what the counter used to record.

TEST_F(CommandDispatcherTest, ShowMetaPrintsNoCrossCoreWriteCounters) {
    CommandDispatcher owner(boot_->superblock, boot_->catalog, store_);
    EXPECT_EQ(owner.Dispatch("SHOW META").response.find("cross_core_write_refusal"),
              std::string::npos);
    ASSERT_EQ(owner.Dispatch("CREATE TABLE acct (id int64, name varchar)")
                  .response.substr(0, 7),
              "CREATED");

    CommandDispatcher peer(boot_->superblock, boot_->catalog, store_, nullptr, nullptr,
                           nullptr, wal::DurabilityClass::kGroup, exec::Budget(),
                           /*recorder=*/nullptr, /*replay_enabled=*/false,
                           /*access_statistics=*/true, /*cabins=*/nullptr,
                           /*txn=*/nullptr, txn::IsolationLevel::kReadCommitted,
                           /*core_id=*/1);
    auto written = peer.Dispatch("INSERT INTO acct VALUES ('alice')");
    ASSERT_EQ(written.response.rfind("INSERTED", 0), 0u) << written.response;
    const std::string meta = peer.Dispatch("SHOW META").response;
    EXPECT_EQ(meta.find("cross_core_write_refusal"), std::string::npos) << meta;
}

// ---- The group-accounting block (sched.md §4, T4) ----------------------

TEST_F(CommandDispatcherTest, ShowMetaOmitsGroupAccountingWithNoReactorAttached) {
    // The recovery block's rule: absent, not zeroed. A dispatcher with no
    // reactor has not "spent no time in the foreground", it has no answer,
    // and printing zeroes would be one.
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    const std::string meta = d.Dispatch("SHOW META").response;
    EXPECT_EQ(testing_view::MetaField(meta, "sched_wall_us"), std::nullopt) << meta;
    EXPECT_EQ(testing_view::MetaField(meta, "sched_foreground_polls"), std::nullopt) << meta;
}

TEST_F(CommandDispatcherTest, ShowMetaReportsGroupAccountingAgainstWallTime) {
    sched::ManualClock clock;
    sched::NullIoBackend io;
    sched::Scheduler scheduler(clock, io);
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    d.set_scheduler_view(&scheduler);

    clock.SetNow(0);
    scheduler.Submit(std::make_unique<sched::FunctionTask>(
        sched::SchedulingGroup::kSystem, [&] {
            clock.Advance(3'000'000);        // 3 ms inside a system poll
            return sched::PollResult::kDone;
        }));
    scheduler.RunOnce();
    clock.Advance(7'000'000);                // 7 ms outside every poll

    const std::string meta = d.Dispatch("SHOW META").response;
    // Whole values: a counter that grew by a factor of ten is exactly the
    // defect worth catching here.
    const auto field = [&meta](std::string_view key) {
        return testing_view::MetaField(meta, key).value_or("<absent>");
    };
    EXPECT_EQ(field("sched_wall_us"), "10000") << meta;
    EXPECT_EQ(field("sched_system_polled_us"), "3000") << meta;
    EXPECT_EQ(field("sched_system_polls"), "1") << meta;
    EXPECT_EQ(field("sched_foreground_polled_us"), "0") << meta;
    EXPECT_EQ(field("sched_maintenance_polls"), "0") << meta;
    // 10 ms of wall against 3 ms of polls: the 7 ms difference is the time
    // sched.md §4 says is charged to no group, and this is the first build
    // in which a client can compute it.
}

TEST_F(CommandDispatcherTest, ShowTablesIncludesBootstrapTables) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("SHOW TABLES");
    EXPECT_NE(out.response.find("tables"), std::string::npos);
    EXPECT_NE(out.response.find("objects"), std::string::npos);
    EXPECT_NE(out.response.find("columns"), std::string::npos);
}

TEST_F(CommandDispatcherTest, TheTablesViewReportsTheHeaderTheOldFindTableDid) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    const std::string reply =
        d.Dispatch("SELECT name, oid, root_page_id, clustered_type FROM sys.tables").response;
    const auto shown = testing_view::ReadViewText(reply);
    EXPECT_EQ(shown.Find("name", "tables", "oid"), std::to_string(catalog::kSysTablesTable));
    EXPECT_EQ(shown.Find("name", "tables", "root_page_id"),
              std::to_string(catalog::kCatalogPageTables));
    EXPECT_EQ(shown.Find("name", "tables", "clustered_type"), "HEAP");
}

TEST_F(CommandDispatcherTest, DescribeListsColumnsAndMarksTheTupleKey) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int32, name varchar)").response.substr(0, 7),
              "CREATED");

    const std::string reply = d.Dispatch("DESCRIBE acct").response;
    const auto shown = testing_view::ReadViewText(reply);
    EXPECT_EQ(shown.rows.size(), 2u) << reply;
    // The declared type name round-trips back out of sys.types.
    EXPECT_EQ(shown.Find("name", "id", "type"), "int32") << reply;
    EXPECT_EQ(shown.Find("name", "name", "type"), "varchar") << reply;
    // Column 0 is the Keystone tuple key; nothing else is.
    EXPECT_EQ(shown.Find("name", "id", "notnull"), "1") << reply;
    EXPECT_EQ(shown.Find("name", "id", "tuple_key"), "1") << reply;
    EXPECT_EQ(shown.Find("name", "name", "tuple_key"), "0") << reply;
    // One "\n"-escaped line per column, and never a raw newline byte.
    EXPECT_EQ(reply.find('\n'), std::string::npos);
}

// ---- The tuple key in DESCRIBE (heap-and-tuple.md §4.1) --------------------------
//
// `key_mode=` was a declaration and is gone with the mode, and `key_order=`
// with `kUnordered` (BB-Q11 (a)); neither is a column of `sys.columns` or
// `sys.tables`.

TEST_F(CommandDispatcherTest, DescribeCarriesNoKeyOrderField) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int64, qty int64)").response.substr(0, 7),
              "CREATED");

    const std::string reply = d.Dispatch("DESCRIBE acct").response;
    const auto shown = testing_view::ReadViewText(reply);
    EXPECT_EQ(reply.find("key_order"), std::string::npos) << reply;
    EXPECT_EQ(shown.Find("name", "id", "tuple_key"), "1") << reply;
    // The storage, the DDL-only fact, is the relation's row.
    const std::string tables =
        d.Dispatch("SELECT name, clustered_type FROM sys.tables WHERE name = 'acct'").response;
    EXPECT_EQ(testing_view::ReadViewText(tables).Find("name", "acct", "clustered_type"), "BTREE");
    EXPECT_EQ(tables.find("key_order"), std::string::npos) << tables;
}

TEST_F(CommandDispatcherTest, DescAbbreviationIsAccepted) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int64)").response.substr(0, 7), "CREATED");
    EXPECT_EQ(d.Dispatch("DESC acct").response.substr(0, 7), "rel_id,");
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
    auto out = d.Dispatch("FLIBBERTIGIBBET EVERYTHING");
    EXPECT_EQ(out.response, "ERR unknown command");
    EXPECT_FALSE(out.should_stop);
}

TEST_F(CommandDispatcherTest, DropIsAKnownVerbWithANamedTargetList) {
    // `DROP` became a statement head with CREATE PATTERN, so it no longer
    // falls into "unknown command" - and it should not. Naming what DROP
    // does take beats a generic refusal that leaves a client unsure whether
    // the word was recognized. The list in the message is the whole of what
    // exists, so it moves with the targets: CABIN joined it with the Cabin
    // feature (docs/spec/cabin.md), INDEX with the index grammar
    // (docs/spec/index.md §10), ASSERTION with the assertion catalog
    // (docs/spec/assertion.md §8.3, AST03). This test is meant to be edited
    // when one moves - that is what pins the list to reality rather than to
    // whatever it happened to say.
    //
    // `DROP TABLE` joined the list with docs/spec/drop-table.md (DT01) -
    // the edit this comment scheduled - and the RESTRICT hook AST03 could
    // not wire finally has its DDL. An unknown table now answers NotFound
    // from resolution, not a target-list refusal. `DROP PATTERN` **left**
    // the list on 2026-08-31, when the operator withdrew declared patterns:
    // the first edit this comment scheduled in the shrinking direction.
    //
    // The dispatcher and the parser each carry this sentence, and they must
    // agree - the dispatcher answers `DROP EVERYTHING` first, the parser
    // answers it for a caller that reaches `parser::Parse` directly.
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    // `DROP NAMESPACE` joined the list at AF-T3 - the first edit in the
    // *growing* direction, and the one the comment above did not schedule.
    // `DROP TRANSITION` joined it at BV-S2 (`docs/spec/transition.md`).
    EXPECT_EQ(d.Dispatch("DROP EVERYTHING").response,
              "ERR only DROP TABLE, DROP NAMESPACE, DROP CABIN, DROP INDEX, DROP ASSERTION and "
              "DROP TRANSITION are supported");
    EXPECT_EQ(d.Dispatch("DROP TABLE t").response, "ERR no relation named 't' (byte 11)");

    const auto parsed = parser::Parse("DROP EVERYTHING");
    ASSERT_FALSE(parsed.ok());
    EXPECT_NE(parsed.status().message().find("only DROP TABLE, DROP NAMESPACE, DROP CABIN, "
                                             "DROP INDEX, DROP ASSERTION and DROP TRANSITION are "
                                             "supported"),
              std::string::npos)
        << parsed.status().message();
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
    auto view = heap::PageView::CreateEmpty(page.value().bytes(), 42);
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
    const auto shown = testing_view::ReadViewText(out.response);

    // The header row (`slot` NULL), then a row per slot.
    ASSERT_EQ(shown.rows.size(), 3u) << out.response;
    EXPECT_EQ(shown.Find("slot", "NULL", "page_id"), "500");
    EXPECT_EQ(shown.Find("slot", "NULL", "min_key"), "42");
    EXPECT_EQ(shown.Find("slot", "NULL", "nr_slots"), "2");
    EXPECT_EQ(shown.Find("slot", "0", "dead"), "0");
    EXPECT_EQ(shown.Find("slot", "1", "dead"), "1");
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
    auto view = heap::PageView::CreateEmpty(page.value().bytes(), 0);
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

    const auto shown = testing_view::ReadViewText(out.response);
    EXPECT_EQ(shown.Find("slot", "0", "value"), "68656c6c6f") << out.response;
    // Dead slot's value must not be read/shown.
    EXPECT_EQ(shown.Find("slot", "1", "value"), "NULL") << out.response;
}

TEST_F(CommandDispatcherTest, ShowPageWithoutValuesOmitsPayload) {
    constexpr PageId kPageId = 502;
    auto page = store_.CreateAt(kPageId);
    ASSERT_TRUE(page.ok());
    auto view = heap::PageView::CreateEmpty(page.value().bytes(), 0);
    ASSERT_TRUE(view.ok());

    std::string payload = "hello";
    std::vector<std::byte> bytes(payload.size());
    std::memcpy(bytes.data(), payload.data(), payload.size());
    ASSERT_TRUE(view.value().InsertTuple(bytes, /*trx_id=*/1).ok());

    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("SHOW PAGE " + std::to_string(kPageId));
    ASSERT_EQ(out.response.rfind("ERR", 0), std::string::npos) << out.response;
    EXPECT_EQ(testing_view::ReadViewText(out.response).columns.back(), "child") << out.response;
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

    const auto found = testing_view::ReadViewText(d.Dispatch("DESCRIBE accounts").response);
    EXPECT_EQ(found.Find("name", "id", "tuple_key"), "1");
}

// The bare, pre-parser form asks for a zero-column table, and every
// relation's first column is its mandatory Keystone tuple key - so there is no
// such table to create.
TEST_F(CommandDispatcherTest, BareCreateTableIsRefusedForHavingNoTupleKey) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("CREATE TABLE accounts");
    EXPECT_EQ(out.response.substr(0, 4), "ERR ");
    EXPECT_NE(out.response.find("no columns"), std::string::npos) << out.response;
}

TEST_F(CommandDispatcherTest, CreateTableRejectsANonIntegerTupleKey) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("CREATE TABLE bad (name varchar, id int64)");
    EXPECT_EQ(out.response.substr(0, 4), "ERR ");
    EXPECT_NE(out.response.find("must be an integer type"), std::string::npos) << out.response;
}

// ---- The key mode against the storage (PK03, heap-and-tuple.md §4.1) ----
//
// The grammar takes the two trailing words in either order and says
// nothing about which pairs a relation can be stored as. That is this
// layer's question, and these pin where it is answered.

TEST_F(CommandDispatcherTest, CreateTableAcceptsAnExplicitBtreeRelation) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    EXPECT_EQ(d.Dispatch("CREATE TABLE t (id int64) BTREE EXPLICIT").response.substr(0, 8),
              "CREATED ");
    EXPECT_EQ(d.Dispatch("CREATE TABLE u (id int64) EXPLICIT BTREE").response.substr(0, 8),
              "CREATED ");
}

TEST_F(CommandDispatcherTest, HeapStorageIsRefusedWhileSuspendedAndSaysWhy) {
    // **SUS-1, and the only cell that sees the refusal**: the test binary
    // runs with the suspension lifted so the heap paths stay exercised, so
    // this one puts it back for its own duration.
    parser::SuspendHeapStorageForTest suspended;
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);

    auto refused = d.Dispatch("CREATE TABLE t (id int64) HEAP");
    // **The token, not just `ERR`** - AS-R1 is a ruling about *which* code
    // this is, and `ERR` alone is satisfied by `NOT_IMPLEMENTED` and by
    // `INVALID_ARGUMENT` too, so the one thing the operator decided would
    // have had nothing under it. `TheTwoRefusalTokensAreDistinctAndRoundTrip`
    // above is the rule this obeys: a client asks the token, not the prose.
    EXPECT_EQ(refused.response.rfind("ERR UNSUPPORTED retryable=0 ", 0), 0u) << refused.response;
    EXPECT_EQ(StatusFromErrorReply(refused.response).code(), StatusCode::kUnsupported)
        << refused.response;
    EXPECT_NE(refused.response.find("SUS-1"), std::string::npos)
        << "a reader meeting this needs the reason, not just the code: " << refused.response;
    // **The byte, like every other refusal in this file.** `HEAP` sits at
    // offset 26 of the statement above.
    EXPECT_NE(refused.response.find("byte 26"), std::string::npos) << refused.response;
    // And it names where the decision and its resume condition live, because
    // `Unsupported` on something that is *built* would otherwise read as
    // "the engine cannot do this".
    EXPECT_NE(refused.response.find("workorder-as-sus1"), std::string::npos)
        << refused.response;

    // **The default moved with it**: an unqualified statement is a btree,
    // and it is created rather than refused.
    auto created = d.Dispatch("CREATE TABLE t (id int64)");
    EXPECT_EQ(created.response.substr(0, 7), "CREATED") << created.response;
    EXPECT_EQ(testing_view::RelationField(d, "tables", "t", "clustered_type"), "BTREE");
}

TEST_F(CommandDispatcherTest, DISABLED_HeapSuspensionIsLifted) {
    // **SUS-1's resume condition, as a test rather than a paragraph**
    // (AS-R6). Drop the `DISABLED_` prefix when the suspension lifts; until
    // then this cell is the condition written down somewhere executable, and
    // it fails by design.
    //
    // This is **half** of it. The other half - `docs/spec/parser-v2.md` §9
    // item 8's three-way execution-equivalence corpus (heap x heap,
    // heap x btree, btree x btree) green at `cores = 4` under tsan - is not
    // expressible in one cell and is named in the work order. Note §9, not
    // §8: the order's wording points at §8, which has been "Open decisions"
    // with an empty body since the 2026-09-02 compaction, so a condition
    // written against it would never have been runnable.
    //
    // `SuspendHeapStorageForTest` here is not a test contrivance - it is the
    // *production* state, which the test binary otherwise lifts. Asserting
    // CREATED under it is exactly "a client can make a heap relation again".
    parser::SuspendHeapStorageForTest production_state;
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);

    auto created = d.Dispatch("CREATE TABLE h (id int64, v int64) HEAP");
    EXPECT_EQ(created.response.substr(0, 7), "CREATED") << created.response;
    EXPECT_EQ(testing_view::RelationField(d, "tables", "h", "clustered_type"), "HEAP");
}

TEST_F(CommandDispatcherTest, ASuspendedHeapRelationStillMountsAndServes) {
    // **The claim the whole suspension rests on**: SUS-1 refuses *creation*
    // and nothing else, so a relation that already exists still mounts,
    // reads and writes. That is why no heap code is deleted and why
    // `heap-and-tuple.md` §3.1b keeps its text under a banner.
    //
    // **It has to re-open, or it does not test "mounts".** An earlier
    // version created and served in one process against one live store and
    // wrapped it in a suspension guard - but `INSERT`, `SELECT` and the rest
    // consult nothing about storage suspension, so that guard was decorative
    // and the cell asserted only "the dispatcher serves a heap relation",
    // which forty other cells already pin. The re-open is the half that was
    // missing, and it is the half a mount path growing a storage-form
    // assumption would break.
    {
        // The volume as it was written before the suspension.
        CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
        ASSERT_EQ(d.Dispatch("CREATE TABLE h (id int64, v int64) HEAP").response.substr(0, 7),
                  "CREATED");
        ASSERT_EQ(d.Dispatch("INSERT INTO h VALUES (1, 10)").response.substr(0, 8), "INSERTED");
        ASSERT_EQ(d.Dispatch("SYNC").response.substr(0, 2), "OK");
    }

    // Now open it again with the suspension **in force**, which is the state
    // every pre-SUS-1 volume comes up in: a fresh catalog over the same
    // store, nothing carried over in the process.
    parser::SuspendHeapStorageForTest suspended;
    catalog::Catalog reopened(store_);
    CommandDispatcher d(boot_->superblock, reopened, store_);

    EXPECT_EQ(testing_view::RelationField(d, "tables", "h", "clustered_type"), "HEAP");
    EXPECT_NE(d.Dispatch("SELECT id, v FROM h").response.find("1,10"), std::string::npos);
    ASSERT_EQ(d.Dispatch("INSERT INTO h VALUES (2, 20)").response.substr(0, 8), "INSERTED");
    EXPECT_EQ(d.Dispatch("UPDATE h SET v = 99 WHERE id = 1").response.substr(0, 7), "UPDATED");
    EXPECT_NE(d.Dispatch("SELECT id, v FROM h").response.find("1,99"), std::string::npos);
    EXPECT_EQ(d.Dispatch("DELETE FROM h WHERE id = 2").response.substr(0, 7), "DELETED");

    // And creating one is still refused on that same reopened volume - the
    // suspension survives the mount rather than being a property of the
    // process that set it.
    EXPECT_EQ(d.Dispatch("CREATE TABLE h2 (id int64) HEAP").response.substr(0, 3), "ERR");
}

TEST_F(CommandDispatcherTest, CreateTableNoLongerRefusesAnExplicitHeapRelation) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);

    // The contradiction this test was written for is gone (§4.1): a heap
    // relation takes a caller-named key, provided the key ascends, so there
    // is no pairing of the two words left to refuse.
    auto spelled = d.Dispatch("CREATE TABLE t (id int64) HEAP EXPLICIT");
    EXPECT_EQ(spelled.response.substr(0, 7), "CREATED") << spelled.response;
    EXPECT_EQ(testing_view::RelationField(d, "tables", "t", "clustered_type"), "HEAP");

    // And a bare EXPLICIT still moves storage nowhere - that resolution
    // existed only to keep the refusal above reachable from a written word
    // alone. **What it lands on is BTREE since SUS-1**, which is the default
    // rather than anything this word did.
    auto implied = d.Dispatch("CREATE TABLE t2 (id int64) EXPLICIT");
    EXPECT_EQ(implied.response.substr(0, 7), "CREATED") << implied.response;
    EXPECT_EQ(testing_view::RelationField(d, "tables", "t2", "clustered_type"), "BTREE");
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

// ---- Keystone tuple key: system-generated, unique, autoincrement ------
//
// The tuple key is not a constraint checked after the fact - it is the id the
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

    // And the issued ids are what SELECT reads back for the tuple key column.
    auto rows = d.Dispatch("SELECT * FROM acct");
    EXPECT_NE(rows.response.find("1,alice"), std::string::npos) << rows.response;
    EXPECT_NE(rows.response.find("2,bob"), std::string::npos) << rows.response;
}

TEST_F(CommandDispatcherTest, SupplyingTheTupleKeyOnInsertIsAdmittedAndMovesTheMark) {
    // The inverse of what this test asserted until 2026-08-25 (§4.1). The
    // mark is the part worth keeping: a named key moves it, so the next
    // issued id clears the named one instead of colliding with it.
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int32, name varchar) HEAP").response.substr(0, 7),
              "CREATED");

    auto out = d.Dispatch("INSERT INTO acct VALUES (7, 'alice')");
    EXPECT_NE(out.response.find("id=7"), std::string::npos) << out.response;
    EXPECT_EQ(testing_view::RelationField(d, "tables", "acct", "next_id"), "8");

    // A refused key still burns nothing: below the mark on a heap relation.
    auto refused = d.Dispatch("INSERT INTO acct VALUES (3, 'bob')");
    EXPECT_EQ(refused.response.substr(0, 4), "ERR ") << refused.response;
    EXPECT_EQ(testing_view::RelationField(d, "tables", "acct", "next_id"), "8");
}

TEST_F(CommandDispatcherTest, RepeatedInsertsOfTheSameValuesGetDistinctKeys) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int32, name varchar)").response.substr(0, 7),
              "CREATED");

    // The duplicate the old code allowed: identical rows, same key. Now
    // each gets its own id, so "same id twice" is not expressible.
    ASSERT_EQ(d.Dispatch("INSERT INTO acct VALUES ('alice')").response.substr(0, 8), "INSERTED");
    ASSERT_EQ(d.Dispatch("INSERT INTO acct VALUES ('alice')").response.substr(0, 8), "INSERTED");

    EXPECT_EQ(testing_view::RelationField(d, "tables", "acct", "next_id"), "3");
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

TEST_F(CommandDispatcherTest, UpdatingTheTupleKeyIsRefused) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int32, name varchar)").response.substr(0, 7),
              "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO acct VALUES ('alice')").response.substr(0, 8), "INSERTED");

    auto out = d.Dispatch("UPDATE acct SET id = 99");
    EXPECT_EQ(out.response.substr(0, 4), "ERR ") << out.response;
    EXPECT_NE(out.response.find("cannot be updated"), std::string::npos) << out.response;
    // K-M3: the refusal is `Unsupported`, so it carries a byte position -
    // and since 2026-08-31 it reaches the wire under its own token rather
    // than bare. `UNSUPPORTED`, not `NOT_IMPLEMENTED`, is the assertion
    // that matters here: a tuple key UPDATE is refused by invariant 11, so no
    // later release lifts it and a client must be told that rather than
    // left to feature-detect forever. The bit stays one wide - retryable=0,
    // spelled rather than implied by omission.
    EXPECT_NE(out.response.find("at byte "), std::string::npos) << out.response;
    EXPECT_EQ(out.response.rfind("ERR UNSUPPORTED retryable=0 ", 0), 0u) << out.response;
    EXPECT_EQ(out.response.find("NOT_IMPLEMENTED"), std::string::npos) << out.response;

    // The row is untouched, key included.
    EXPECT_NE(d.Dispatch("SELECT * FROM acct").response.find("1,alice"), std::string::npos);
}

// The pair at the wire (operator rule, 2026-08-31). Two refusals of the
// same statement class - one the architecture forbids, one nobody built -
// must reach a client as two different tokens, or the client cannot tell
// "rewrite this" from "try a newer server".
TEST_F(CommandDispatcherTest, TheTwoRefusalTokensAreDistinctAndRoundTrip) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int32, a int32)").response.substr(0, 7), "CREATED");
    ASSERT_EQ(d.Dispatch("CREATE TABLE u (id int32, b int32)").response.substr(0, 7), "CREATED");

    // A CTE is a form the design admits and the engine has not built (an
    // outer join was this example until BU-S6 built it), so it is the
    // "not yet" half.
    const auto not_built = d.Dispatch("WITH w AS (SELECT id FROM t) SELECT id FROM w");
    EXPECT_EQ(not_built.response.rfind("ERR NOT_IMPLEMENTED retryable=0 ", 0), 0u)
        << not_built.response;
    EXPECT_NE(not_built.response.find("(byte 0)"), std::string::npos) << not_built.response;

    // A tuple key UPDATE is invariant 11, so it is the "never" half. Same
    // statement class, same `ERR ` prefix, different answer to the only
    // question a client library asks about a refused form.
    const auto forbidden = d.Dispatch("UPDATE t SET id = 99");
    EXPECT_EQ(forbidden.response.rfind("ERR UNSUPPORTED retryable=0 ", 0), 0u)
        << forbidden.response;

    // Both recover as themselves. That was what a peer's refusal needed to
    // survive the ring while statements shipped (until AT-S6), and
    // `StatusFromErrorReply` is still the only reader of the token for the
    // KWP paths that hold a rendered line: a code that renders but does not
    // recover would come back as kInvalidArgument.
    EXPECT_EQ(StatusFromErrorReply(not_built.response).code(), StatusCode::kNotImplemented);
    EXPECT_EQ(StatusFromErrorReply(forbidden.response).code(), StatusCode::kUnsupported);
    EXPECT_FALSE(StatusFromErrorReply(not_built.response).retryable());
    EXPECT_FALSE(StatusFromErrorReply(forbidden.response).retryable());
    EXPECT_EQ(ErrorReply(StatusFromErrorReply(not_built.response)), not_built.response);
    EXPECT_EQ(ErrorReply(StatusFromErrorReply(forbidden.response)), forbidden.response);
}

// The other half of the same compile step, at the wire: an unknown SET
// target is `InvalidArgument` - simply wrong - and must stay
// distinguishable from the refusal above rather than collapsing into one
// "bad UPDATE" reply.
TEST_F(CommandDispatcherTest, UpdatingAnUnknownColumnIsRefusedWithItsByte) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int32, name varchar)").response.substr(0, 7),
              "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO acct VALUES ('alice')").response.substr(0, 8), "INSERTED");

    auto out = d.Dispatch("UPDATE acct SET nope = 1");
    EXPECT_EQ(out.response.substr(0, 4), "ERR ") << out.response;
    EXPECT_NE(out.response.find("unknown column 'nope'"), std::string::npos) << out.response;
    EXPECT_NE(out.response.find("at byte "), std::string::npos) << out.response;

    EXPECT_NE(d.Dispatch("SELECT * FROM acct").response.find("1,alice"), std::string::npos);
}

TEST_F(CommandDispatcherTest, AJoinExecutesAndReturnsOnlyMatchingPairs) {
    // V05 taught the grammar joins and refused to run them; V17 runs them.
    // The property that survives the whole way through is the one the
    // refusal existed to protect: a join must never answer with one
    // relation's rows. Now that it executes, that shows up as the
    // non-matching row being absent rather than as an error.
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int32, name varchar)").response.substr(0, 7),
              "CREATED");
    ASSERT_EQ(d.Dispatch("CREATE TABLE trade (id int32, acct_id int32, sym varchar)")
                  .response.substr(0, 7),
              "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO acct VALUES ('alice')").response.substr(0, 8), "INSERTED");
    ASSERT_EQ(d.Dispatch("INSERT INTO acct VALUES ('bob')").response.substr(0, 8), "INSERTED");
    // Two trades, both belonging to alice (id 1). bob has none.
    ASSERT_EQ(d.Dispatch("INSERT INTO trade VALUES (1, 'AAPL')").response.substr(0, 8),
              "INSERTED");
    ASSERT_EQ(d.Dispatch("INSERT INTO trade VALUES (1, 'MSFT')").response.substr(0, 8),
              "INSERTED");

    auto out = d.Dispatch(
        "SELECT acct.name, trade.sym FROM trade JOIN acct ON trade.acct_id = acct.id");
    ASSERT_NE(out.response.substr(0, 4), "ERR ") << out.response;
    EXPECT_NE(out.response.find("alice,AAPL"), std::string::npos) << out.response;
    EXPECT_NE(out.response.find("alice,MSFT"), std::string::npos) << out.response;
    EXPECT_EQ(out.response.find("bob"), std::string::npos)
        << "bob has no trade, so no row may carry him: " << out.response;
    EXPECT_EQ(out.response.find("acct.name,trade.sym"), 0u) << "the header names the projection";
}

TEST_F(CommandDispatcherTest, AnExplicitProjectionEmitsOnlyTheNamedColumns) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int32, name varchar, tier varchar)")
                  .response.substr(0, 7),
              "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO acct VALUES ('alice', 'gold')").response.substr(0, 8),
              "INSERTED");

    auto out = d.Dispatch("SELECT name FROM acct");
    ASSERT_NE(out.response.substr(0, 4), "ERR ") << out.response;
    EXPECT_NE(out.response.find("alice"), std::string::npos) << out.response;
    EXPECT_EQ(out.response.find("gold"), std::string::npos)
        << "a column the client did not name must not appear: " << out.response;
}

TEST_F(CommandDispatcherTest, SubqueryPredicatesExecuteAndActuallyFilter) {
    // This test began life as a refusal, added because the old
    // name-matching evaluator answered a subquery predicate *wrongly*:
    // EXISTS has no column, the lookup found nothing, the row was
    // reported "no match", and every row was dropped - an empty result
    // set that reads as "nothing matched" rather than "never evaluated".
    //
    // V18 executes them, so the same statements are checked for the right
    // answer instead of the right error. `trade` is empty, which makes
    // the two negations true and the two positives false - and gets both
    // directions from one fixture.
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int32, name varchar)").response.substr(0, 7),
              "CREATED");
    ASSERT_EQ(d.Dispatch("CREATE TABLE trade (id int32, acct_id int32)").response.substr(0, 7),
              "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO acct VALUES ('alice')").response.substr(0, 8), "INSERTED");

    for (const char* sql : {"SELECT * FROM acct WHERE EXISTS (SELECT trade.id FROM trade)",
                            "SELECT * FROM acct WHERE id IN (SELECT acct_id FROM trade)"}) {
        auto out = d.Dispatch(sql);
        ASSERT_NE(out.response.substr(0, 4), "ERR ") << sql << " -> " << out.response;
        EXPECT_EQ(out.response.find("alice"), std::string::npos)
            << sql << " must exclude every row over an empty relation: " << out.response;
    }
    for (const char* sql : {"SELECT * FROM acct WHERE NOT EXISTS (SELECT trade.id FROM trade)",
                            "SELECT * FROM acct WHERE id NOT IN (SELECT acct_id FROM trade)"}) {
        auto out = d.Dispatch(sql);
        ASSERT_NE(out.response.substr(0, 4), "ERR ") << sql << " -> " << out.response;
        EXPECT_NE(out.response.find("alice"), std::string::npos)
            << sql << " must admit every row over an empty relation: " << out.response;
    }

    // UPDATE had the same hole and reported "UPDATED 0". Now the
    // predicate is real: no trade points at alice, so still zero - but
    // for the right reason, and the row is untouched.
    auto upd = d.Dispatch("UPDATE acct SET name = 'x' WHERE id IN (SELECT acct_id FROM trade)");
    EXPECT_EQ(upd.response, "UPDATED 0") << upd.response;
    EXPECT_NE(d.Dispatch("SELECT * FROM acct").response.find("alice"), std::string::npos);
}

// A NULL tested against a non-empty subquery result is UNKNOWN for IN and
// NOT IN alike, so neither admits the row - in a SELECT and in a write's
// WHERE. NOT IN used to answer TRUE and admit it. Against an empty result
// NOT IN is TRUE whatever the probe.
TEST_F(CommandDispatcherTest, ANullProbeIsUnknownAgainstANonEmptySubquery) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE n (id int64, v int64 NULL)").response.substr(0, 7),
              "CREATED");
    ASSERT_EQ(d.Dispatch("CREATE TABLE s (id int64, x int64)").response.substr(0, 7), "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO n VALUES (NULL)").response.substr(0, 8), "INSERTED");
    ASSERT_EQ(d.Dispatch("INSERT INTO n VALUES (1)").response.substr(0, 8), "INSERTED");
    ASSERT_EQ(d.Dispatch("INSERT INTO s VALUES (2)").response.substr(0, 8), "INSERTED");

    // Row 2 (v = 1) is not in {2}; row 1 (v = NULL) is unknown.
    EXPECT_EQ(d.Dispatch("SELECT id FROM n WHERE v NOT IN (SELECT x FROM s)").response,
              "id\\n2");
    EXPECT_EQ(d.Dispatch("SELECT id FROM n WHERE v IN (SELECT x FROM s)").response, "id");

    // A write's WHERE reaches the same verdict: only row 2 is touched.
    EXPECT_EQ(d.Dispatch("UPDATE n SET v = 3 WHERE v NOT IN (SELECT x FROM s)").response,
              "UPDATED 1");
    EXPECT_EQ(d.Dispatch("SELECT id FROM n WHERE v = 3").response, "id\\n2");
    EXPECT_EQ(d.Dispatch("DELETE FROM n WHERE v NOT IN (SELECT x FROM s)").response,
              "DELETED 1");
    EXPECT_EQ(d.Dispatch("SELECT id FROM n").response, "id\\n1");

    // An empty result admits the NULL row: NULL NOT IN (empty) is TRUE.
    EXPECT_EQ(d.Dispatch("SELECT id FROM n WHERE v NOT IN (SELECT x FROM s WHERE x = 9)")
                  .response,
              "id\\n1");
}

TEST_F(CommandDispatcherTest, ANestedUncorrelatedExistsFiltersAWriteToo) {
    // An uncorrelated EXISTS inside a sub-chain was dropped when the block
    // was lowered, for a write's WHERE exactly as for a SELECT's: the
    // UPDATE and the DELETE answered as if the inner predicate were absent.
    // The inner set is empty (no 'NONE' trade), so no row may be touched;
    // with 'AAPL' it is non-empty and the correlation decides.
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int32, name varchar)").response.substr(0, 7),
              "CREATED");
    ASSERT_EQ(d.Dispatch("CREATE TABLE trade (id int32, acct_id int32, sym varchar)")
                  .response.substr(0, 7),
              "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO acct VALUES ('alice')").response.substr(0, 8), "INSERTED");
    ASSERT_EQ(d.Dispatch("INSERT INTO acct VALUES ('bob')").response.substr(0, 8), "INSERTED");
    ASSERT_EQ(d.Dispatch("INSERT INTO trade VALUES (1, 'AAPL')").response.substr(0, 8),
              "INSERTED");

    const std::string none =
        "EXISTS (SELECT trade.id FROM trade WHERE trade.acct_id = acct.id "
        "AND EXISTS (SELECT trade.id FROM trade WHERE trade.sym = 'NONE'))";
    const std::string aapl =
        "EXISTS (SELECT trade.id FROM trade WHERE trade.acct_id = acct.id "
        "AND EXISTS (SELECT trade.id FROM trade WHERE trade.sym = 'AAPL'))";

    EXPECT_EQ(d.Dispatch("UPDATE acct SET name = 'x' WHERE " + none).response, "UPDATED 0");
    EXPECT_EQ(d.Dispatch("DELETE FROM acct WHERE " + none).response, "DELETED 0");
    EXPECT_NE(d.Dispatch("SELECT * FROM acct").response.find("alice"), std::string::npos);

    // Only alice has a trade, so only she is renamed - and only she is deleted.
    EXPECT_EQ(d.Dispatch("UPDATE acct SET name = 'x' WHERE " + aapl).response, "UPDATED 1");
    EXPECT_EQ(d.Dispatch("SELECT * FROM acct").response.find("alice"), std::string::npos);
    EXPECT_EQ(d.Dispatch("DELETE FROM acct WHERE " + aapl).response, "DELETED 1");
    const std::string left = d.Dispatch("SELECT * FROM acct").response;
    EXPECT_NE(left.find("bob"), std::string::npos) << left;
}

TEST_F(CommandDispatcherTest, WithIsDeclinedByNameRatherThanAsAnUnknownCommand) {
    // Also found by the live-server script. Dispatch() routes on the
    // first word, so WITH fell through to "unknown command" and the
    // parser's truthful "CTEs are not supported, subqueries are allowed
    // in predicate position only" was unreachable from a client - even
    // though the parser test for it passed, because that test calls
    // parser::Parse() directly.
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("WITH x AS (SELECT * FROM t) SELECT * FROM x");
    EXPECT_EQ(out.response.substr(0, 4), "ERR ") << out.response;
    EXPECT_NE(out.response.find("common table expressions"), std::string::npos) << out.response;
    EXPECT_EQ(out.response.find("unknown command"), std::string::npos) << out.response;
}

// ---- Catalog views (sys.*) ------------------------------------------------

TEST_F(CommandDispatcherTest, CatalogViewsAreSelectable) {
    // The catalog's own rows are not row-codec tuples - fixed offsets, no
    // Keystone word - so they are read through the typed readers and
    // handed out as a materialized view (exec/catalog_view.hpp). What that
    // buys is checked here: real column names, real rows, and a user table
    // showing up in them.
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int64, name varchar)").response.substr(0, 7),
              "CREATED");

    auto tables = d.Dispatch("SELECT * FROM sys.tables");
    ASSERT_NE(tables.response.substr(0, 4), "ERR ") << tables.response;
    EXPECT_NE(tables.response.find("oid,namespace_oid,name"), std::string::npos)
        << tables.response;
    EXPECT_NE(tables.response.find("acct"), std::string::npos) << tables.response;
    // The catalog's own relations are in there too - they are tables.
    EXPECT_NE(tables.response.find("patterns"), std::string::npos) << tables.response;

    // sys.columns joins the relation's name in itself, because a column
    // list keyed only by oid is unreadable and joining is what a view
    // cannot do.
    auto columns = d.Dispatch("SELECT * FROM sys.columns");
    ASSERT_NE(columns.response.substr(0, 4), "ERR ") << columns.response;
    EXPECT_NE(columns.response.find("acct"), std::string::npos) << columns.response;

    for (const char* sql : {"SELECT * FROM sys.objects", "SELECT * FROM sys.types",
                            "SELECT * FROM sys.patterns"}) {
        EXPECT_NE(d.Dispatch(sql).response.substr(0, 4), "ERR ") << sql;
    }
}

TEST_F(CommandDispatcherTest, ACatalogViewTakesAProjectionAndAWhereClause) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int64, name varchar)").response.substr(0, 7),
              "CREATED");

    auto filtered = d.Dispatch("SELECT name, desc_page_id FROM sys.tables WHERE name = 'acct'");
    ASSERT_NE(filtered.response.substr(0, 4), "ERR ") << filtered.response;
    EXPECT_EQ(filtered.response.find("name,desc_page_id"), 0u) << filtered.response;
    EXPECT_NE(filtered.response.find("acct"), std::string::npos) << filtered.response;
    // The WHERE really filtered: no other table's name came through.
    EXPECT_EQ(filtered.response.find("patterns"), std::string::npos) << filtered.response;
}

TEST_F(CommandDispatcherTest, ACatalogViewsBetweenTakesBothBounds) {
    // `BETWEEN` reaches this path as a `kBetween` *kind* with its bounds in
    // `val`/`val_high` and `op` left at its `kEq` default, because the step
    // compiler lowers the kind into two conjuncts and no evaluator
    // downstream ever reads it. The view path is the one consumer that
    // never got that lowering: it used to read `op`, answer `oid = <low>`,
    // and return one row where a range qualifies - a silently truncated
    // catalog answer, which is the one kind of wrong this file exists to
    // catch.
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto oid_of = [&d](const char* sql) {
        const std::string reply = d.Dispatch(sql).response;
        EXPECT_EQ(reply.substr(0, 7), "CREATED") << reply;
        const auto at = reply.find("oid=");
        return at == std::string::npos ? 0ull : std::strtoull(reply.c_str() + at + 4, nullptr, 10);
    };
    const std::uint64_t lo = oid_of("CREATE TABLE a (id int64, v int64)");
    oid_of("CREATE TABLE b (id int64, v int64)");
    const std::uint64_t hi = oid_of("CREATE TABLE c (id int64, v int64)");
    ASSERT_GT(hi, lo);

    // All three and *nothing else*, both ends inclusive. Asserted as the
    // whole reply rather than as three substring searches: `find("a")`
    // matches the header `name`, and `b`/`c` are carried by catalog
    // relation names outside the range - so a regression that dropped the
    // WHERE entirely would have satisfied all three.
    const std::string ranged =
        d.Dispatch("SELECT name FROM sys.tables WHERE oid BETWEEN " + std::to_string(lo) +
                   " AND " + std::to_string(hi)).response;
    EXPECT_EQ(ranged, "name\\na\\nb\\nc") << ranged;

    // The control that makes the assertion above mean something: an
    // equality really does answer one row, so a passing BETWEEN is not
    // just a view that ignored the predicate.
    const std::string exact =
        d.Dispatch("SELECT name FROM sys.tables WHERE oid = " + std::to_string(lo)).response;
    EXPECT_EQ(exact, "name\\na") << exact;

    // A range holding nothing answers the header, not everything.
    const std::string empty =
        d.Dispatch("SELECT name FROM sys.tables WHERE oid BETWEEN " + std::to_string(hi + 1000) +
                   " AND " + std::to_string(hi + 2000)).response;
    EXPECT_EQ(empty, "name") << empty;
}

TEST_F(CommandDispatcherTest, AWriteWhoseTupleKeyPredicateIsBetweenTouchesEveryRowInTheRange) {
    // **The same `op`-default hazard as the test above, in a second
    // consumer.** `TupleKeyEqualityTarget` answers "is this a bare tuple key point
    // lookup" from the raw `Condition`, and it read `op` alone - which a
    // `kBetween` leaves at `kEq` while `val` holds the *low bound*. So
    // `WHERE id BETWEEN 2 AND 5` reported itself as `WHERE id = 2`, and
    // the point-lookup fast path then wrote that one row and answered
    // `UPDATED 1`. On a **btree** relation the descent makes that fast
    // path fire, which is why the relation here is btree and not heap:
    // on a heap one the locator declines and the scan hides the defect.
    //
    // Read-side `BETWEEN` was always right (`exec::CompileWhere` lowers
    // the kind into two conjuncts), so the SELECT below is the control
    // that says the predicate itself parses and matches four rows - and
    // therefore that `UPDATED 1` was a write disagreeing with a read of
    // its own WHERE clause, not a predicate nobody supported.
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE b (id int64, v int64) BTREE").response.substr(0, 7),
              "CREATED");
    for (int i = 1; i <= 6; ++i) {
        const std::string n = std::to_string(i);
        ASSERT_EQ(d.Dispatch("INSERT INTO b VALUES (" + n + ", " + n + ")").response.substr(0, 8),
                  "INSERTED");
    }

    EXPECT_EQ(d.Dispatch("SELECT id FROM b WHERE id BETWEEN 2 AND 5").response,
              "id\\n2\\n3\\n4\\n5");
    EXPECT_EQ(d.Dispatch("UPDATE b SET v = 99 WHERE id BETWEEN 2 AND 5").response, "UPDATED 4");
    EXPECT_EQ(d.Dispatch("SELECT id FROM b WHERE v = 99").response, "id\\n2\\n3\\n4\\n5");
    EXPECT_EQ(d.Dispatch("DELETE FROM b WHERE id BETWEEN 2 AND 5").response, "DELETED 4");
    EXPECT_EQ(d.Dispatch("SELECT id FROM b").response, "id\\n1\\n6");

    // The control the fix must not cost: a real tuple key equality still takes
    // the point path and still answers one row.
    EXPECT_EQ(d.Dispatch("UPDATE b SET v = 7 WHERE id = 1").response, "UPDATED 1");
    EXPECT_EQ(d.Dispatch("SELECT v FROM b WHERE id = 1").response, "v\\n7");
}

TEST_F(CommandDispatcherTest, ACatalogViewComparesItsIntegersUnsigned) {
    // Every integer a view emits is built from a `uint64_t`
    // (catalog_view.cpp's `Int()`), and `sys.patterns.fetch_id` is a
    // full-range 64-bit fingerprint - so comparing `int_val` signed put
    // every id above INT64_MAX below every id under it. `fetch_id < 100`
    // answered the eight largest ids in the catalog, each printed in full
    // by the same statement that claimed it was small.
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE r (id int64, a int64, b int64)").response.substr(0, 7),
              "CREATED");

    // Registered from **real fingerprints**, taken of real statements: what
    // this test needs is ids that use the full 64-bit range, and inventing
    // numbers would prove nothing about the ones the engine actually
    // stores. The declarations that used to mint these rows went with
    // `CREATE PATTERN` on 2026-08-31, and the auto route cannot stand in -
    // a FilterScan collects no trail, so traffic on these shapes registers
    // nothing (stats/trail_recorder.hpp).
    //
    // Distinct bodies, because a fetch_id is the fingerprint of the shape
    // and identical shapes would collide into one row.
    const char* wheres[] = {"id = 1", "a = 1", "b = 1", "a > 1", "b > 1", "a < 1",
                            "b < 1", "a >= 1", "b >= 1", "a != 1", "b != 1",
                            "id > 1", "id < 1", "id >= 1"};
    int made = 0;
    for (const char* where : wheres) {
        const auto fp = parser::FingerprintOf(std::string("SELECT a FROM r WHERE ") + where);
        ASSERT_TRUE(fp.has_value()) << where;
        if (boot_->catalog.RegisterPattern(fp->fetch_id, catalog::kStmtClassUnclassified)
                .ok()) {
            ++made;
        }
    }
    ASSERT_GT(made, 0) << "the fixture must register patterns to have ids to compare";

    const std::string all = d.Dispatch("SELECT oid FROM sys.patterns").response;
    const std::string ids = d.Dispatch("SELECT fetch_id FROM sys.patterns").response;
    // Non-vacuous only if some id really does use the top bit. A
    // fingerprint is deterministic, so this either holds or the corpus
    // moved and the body list above wants another entry - which is what
    // the message says rather than the test quietly proving nothing.
    bool any_high = false;
    for (std::size_t at = 0; (at = ids.find("\\n", at)) != std::string::npos;) {
        at += 2;
        if (std::strtoull(ids.c_str() + at, nullptr, 10) > (1ull << 63)) any_high = true;
    }
    ASSERT_TRUE(any_high) << "no fetch_id above INT64_MAX, so this proves nothing: " << ids;

    // Every uint64 is >= 0 and none is < 0. Signed, the high-bit ids
    // answered the second and were missing from the first.
    EXPECT_EQ(d.Dispatch("SELECT oid FROM sys.patterns WHERE fetch_id >= 0").response, all);
    EXPECT_EQ(d.Dispatch("SELECT oid FROM sys.patterns WHERE fetch_id < 0").response, "oid");
    EXPECT_EQ(d.Dispatch("SELECT oid FROM sys.patterns WHERE fetch_id < 100").response, "oid");
}

TEST_F(CommandDispatcherTest, ACatalogViewRefusesABogusQualifierInAWhereToo) {
    // The projection has always refused a qualifier naming no relation in
    // the statement; the WHERE discarded it and answered. One resolver now,
    // so the same spelling gets the same answer in both halves.
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);

    const std::string where_bad =
        d.Dispatch("SELECT name FROM sys.tables WHERE zzz.oid = 100").response;
    EXPECT_NE(where_bad.find("names no relation in this statement"), std::string::npos)
        << where_bad;
    const std::string proj_bad = d.Dispatch("SELECT zzz.oid FROM sys.tables").response;
    EXPECT_NE(proj_bad.find("names no relation in this statement"), std::string::npos)
        << proj_bad;

    // A qualifier that *does* name the statement's binding still works, in
    // both halves - the refusal is of a wrong name, not of qualification.
    // The header carries the view's own column name rather than the
    // spelling the client wrote, which is what a view has: names printed
    // from `column_names`, not a projection list echoed back. A chain
    // echoes the qualified form; a view does not, and that difference is
    // older than this test.
    const std::string aliased =
        d.Dispatch("SELECT t.name FROM sys.tables AS t WHERE t.oid = 100").response;
    EXPECT_EQ(aliased, "name\\ntypes") << aliased;
    const std::string unaliased =
        d.Dispatch("SELECT name FROM sys.tables WHERE tables.oid = 100").response;
    EXPECT_EQ(unaliased, "name\\ntypes") << unaliased;
}

TEST_F(CommandDispatcherTest, ACatalogViewRefusesAMalformedPredicateWithNoRowsToApplyItTo) {
    // The refusals below are properties of the *statement*, so they are
    // decided before a row is read. Asked per row they were silenced by an
    // empty view: `sys.patterns` holds nothing on a fresh instance, so the
    // loop never ran and a predicate no view can answer came back as a
    // successful header. A refusal that data can silence is not a refusal.
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);

    const std::string rows = d.Dispatch("SELECT * FROM sys.patterns").response;
    ASSERT_EQ(rows.find("\\n"), std::string::npos) << "sys.patterns must be empty here: " << rows;

    const std::string col_to_col =
        d.Dispatch("SELECT * FROM sys.patterns WHERE oid = fetch_id").response;
    EXPECT_NE(col_to_col.find("column-to-column comparison"), std::string::npos) << col_to_col;

    const std::string no_column =
        d.Dispatch("SELECT * FROM sys.patterns WHERE nosuchcol = 1").response;
    EXPECT_NE(no_column.find("has no column"), std::string::npos) << no_column;
}

TEST_F(CommandDispatcherTest, ACatalogViewRefusesWhatItCannotDoAndSaysWhich) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int64, name varchar)").response.substr(0, 7),
              "CREATED");

    // A view is materialized, so there is no relation for a join step to
    // walk. Refused by name rather than producing nothing.
    auto joined = d.Dispatch("SELECT t.name FROM sys.tables AS t JOIN acct ON t.oid = acct.id");
    EXPECT_EQ(joined.response.substr(0, 4), "ERR ") << joined.response;
    EXPECT_NE(joined.response.find("cannot be joined"), std::string::npos) << joined.response;

    // A known schema with an unknown view is a different mistake from an
    // unknown schema, and gets a different message - reporting the second
    // for the first sends the reader looking in the wrong place.
    auto bad_view = d.Dispatch("SELECT * FROM sys.nosuch");
    EXPECT_NE(bad_view.response.find("no catalog view named"), std::string::npos)
        << bad_view.response;
    // **AF-T3 changed this one, and the change is the feature.** `public`
    // is a namespace now, so `public.acct` names the relation an
    // unqualified `acct` names and is answered rather than refused. What
    // still refuses is a namespace that does not exist - and it refuses by
    // name, with the byte.
    auto qualified = d.Dispatch("SELECT * FROM public.acct");
    EXPECT_NE(qualified.response.substr(0, 4), "ERR ") << qualified.response;
    auto bad_schema = d.Dispatch("SELECT * FROM nosuchns.acct");
    EXPECT_NE(bad_schema.response.find("no namespace named 'nosuchns'"), std::string::npos)
        << bad_schema.response;

    auto bad_column = d.Dispatch("SELECT * FROM sys.tables WHERE nosuchcol = 1");
    EXPECT_NE(bad_column.response.find("has no column"), std::string::npos) << bad_column.response;
}

// types.md §3.1a over a catalog view: a literal is judged against the type
// the view declares for its column, before any row is read, so a mismatch
// is refused with its byte rather than answered with no rows - on a view
// with rows and on one without.
TEST_F(CommandDispatcherTest, ACatalogViewRefusesALiteralOfTheWrongKind) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int64, owner int64)").response.substr(0, 7),
              "CREATED");

    const std::pair<const char*, StatusCode> cases[] = {
        {"SELECT oid FROM sys.tables WHERE name = 5", StatusCode::kInvalidArgument},
        {"SELECT oid FROM sys.tables WHERE oid = 'acct'", StatusCode::kInvalidArgument},
        {"SELECT oid FROM sys.tables WHERE oid = -1", StatusCode::kOutOfRange},
        {"SELECT oid FROM sys.tables WHERE oid = 18446744073709551617", StatusCode::kOutOfRange},
        {"SELECT oid FROM sys.tables WHERE oid BETWEEN 1 AND 'z'", StatusCode::kInvalidArgument},
        // sys.patterns holds no row on a fresh instance; it refuses all the same.
        {"SELECT oid FROM sys.patterns WHERE fetch_id = 'x'", StatusCode::kInvalidArgument}};
    for (const auto& [sql, code] : cases) {
        const DispatchOutcome out = d.Dispatch(sql);
        EXPECT_EQ(out.response.rfind("ERR ", 0), 0u) << sql << ": " << out.response;
        EXPECT_NE(out.response.find("literal at byte "), std::string::npos)
            << sql << ": " << out.response;
        EXPECT_EQ(out.status.code(), code) << sql << ": " << out.response;
    }
    EXPECT_EQ(d.Dispatch("SELECT name FROM sys.tables WHERE name = 'acct'").response,
              "name\\nacct");
    EXPECT_EQ(d.Dispatch("SELECT name FROM sys.tables WHERE name = 'acct' AND oid > 0").response,
              "name\\nacct");
    EXPECT_EQ(d.Dispatch("SELECT name FROM sys.tables WHERE name IS NULL").response, "name");
}

// ---- AF-T3: the syntax ---------------------------------------------------
//
// `instructions/v2.8.0/ratification-af-namespace.md` AF-6, the operator's
// shape (a): `CREATE NAMESPACE <name>` plus qualified names `ns.table`.
// The decision AF-8 left to this task and these cells pin: **a relation's
// name stays instance-global**, so a qualifier declares placement and never
// identity (`parser/ast.hpp`'s namespace-qualifier rule).

TEST_F(CommandDispatcherTest, ANamespaceIsCreatedListedAndDropped) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);

    // The two that exist before any DDL are listed by their SQL spellings,
    // not by the registry names their rows carry.
    EXPECT_EQ(testing_view::ReadViewText(d.Dispatch("SHOW NAMESPACES").response).Column("name"), (std::vector<std::string>{"sys", "public"}));

    auto created = d.Dispatch("CREATE NAMESPACE orders");
    EXPECT_EQ(created.response.substr(0, 18), "CREATED NAMESPACE ") << created.response;
    EXPECT_EQ(testing_view::ReadViewText(d.Dispatch("SHOW NAMESPACES").response).Column("name"), (std::vector<std::string>{"sys", "public", "orders"}));

    // Name-unique, and the second attempt says so rather than making a
    // second namespace of one name.
    EXPECT_NE(d.Dispatch("CREATE NAMESPACE orders").response.find("already exists"),
              std::string::npos);

    auto dropped = d.Dispatch("DROP NAMESPACE orders");
    EXPECT_EQ(dropped.response.substr(0, 18), "DROPPED NAMESPACE ") << dropped.response;
    EXPECT_EQ(testing_view::ReadViewText(d.Dispatch("SHOW NAMESPACES").response).Column("name"), (std::vector<std::string>{"sys", "public"}));
}

TEST_F(CommandDispatcherTest, TheTwoReservedSpellingsAreRefusedAndSysStillMeansTheViews) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);

    for (const char* reserved : {"sys", "public"}) {
        auto refused = d.Dispatch(std::string("CREATE NAMESPACE ") + reserved);
        EXPECT_EQ(refused.response.substr(0, 4), "ERR ") << reserved << ": " << refused.response;
        EXPECT_NE(refused.response.find("reserved"), std::string::npos) << refused.response;
    }

    // The older meaning of a qualifier is untouched: `sys.` still names the
    // catalog views and nothing AF added shadows them.
    auto view = d.Dispatch("SELECT * FROM sys.tables");
    EXPECT_NE(view.response.substr(0, 4), "ERR ") << view.response;

    // And a namespace does not nest - its own row carries its own oid.
    auto nested = d.Dispatch("CREATE NAMESPACE public.orders");
    EXPECT_EQ(nested.response.substr(0, 4), "ERR ") << nested.response;
    EXPECT_NE(nested.response.find("do not nest"), std::string::npos) << nested.response;
}

TEST_F(CommandDispatcherTest, ACreatesQualifierSelectsTheNamespaceAndIsNeverImplicit) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE NAMESPACE orders").response.substr(0, 7), "CREATED");

    ASSERT_EQ(d.Dispatch("CREATE TABLE orders.line (id int64, note varchar)").response.substr(0, 7),
              "CREATED");
    // An unqualified CREATE still lands in `public`, so every statement
    // written before AF means what it meant.
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int64, note varchar)").response.substr(0, 7),
              "CREATED");

    auto placed = d.Dispatch("SELECT namespace_oid FROM sys.tables WHERE name = 'line'");
    EXPECT_NE(placed.response.find(std::to_string(catalog::kUserOidStart)), std::string::npos)
        << "a relation created in `orders` did not land in it: " << placed.response;

    // **Implicit creation is refused**, which is the whole reason shape (a)
    // beat shape (b): a typo must not be indistinguishable from an intent.
    auto typo = d.Dispatch("CREATE TABLE ordrs.line2 (id int64)");
    EXPECT_EQ(typo.response.substr(0, 4), "ERR ") << typo.response;
    EXPECT_NE(typo.response.find("no namespace named 'ordrs'"), std::string::npos)
        << typo.response;
    EXPECT_NE(typo.response.find("byte "), std::string::npos)
        << "the refusal carried no byte: " << typo.response;
    EXPECT_EQ(testing_view::ReadViewText(d.Dispatch("SHOW NAMESPACES").response).Column("name"), (std::vector<std::string>{"sys", "public", "orders"}))
        << "a typo created a namespace";

    // **And the typo is refused when the *name* already exists**, which is
    // the arm the cell above cannot reach: `line2` did not exist, so the
    // refusal above never had to beat the name-collision check. With a name
    // that does exist, resolving the namespace after the collision made
    // `EXISTS oid=n` the answer to a statement naming no namespace at all -
    // the qualifier absorbed rather than verified, and AF-6's rule broken
    // by the one arm that never reached the resolver.
    for (const char* absorbed : {"CREATE TABLE ordrs.acct (id int64)",
                                 "CREATE TABLE ordrs.acct",  // the bare debug form too
                                 "CREATE TABLE sys.acct (id int64)"}) {
        auto out = d.Dispatch(absorbed);
        EXPECT_EQ(out.response.substr(0, 4), "ERR ") << absorbed << " -> " << out.response;
        EXPECT_EQ(out.response.find("EXISTS"), std::string::npos)
            << absorbed << " was answered about a relation in another namespace: "
            << out.response;
    }

    // **A name is taken within its namespace** (NS6): `public` holding
    // `acct` leaves `orders.acct` free, and each name answers `EXISTS` only
    // in the namespace that holds it.
    EXPECT_EQ(d.Dispatch("CREATE TABLE orders.acct (id int64)").response.substr(0, 7), "CREATED");
    EXPECT_EQ(d.Dispatch("CREATE TABLE orders.acct (id int64)").response.substr(0, 6), "EXISTS");
    EXPECT_EQ(d.Dispatch("CREATE TABLE acct (id int64)").response.substr(0, 6), "EXISTS");
}

TEST_F(CommandDispatcherTest, AQualifierIsCheckedWhereverARelationIsNamed) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE NAMESPACE orders").response.substr(0, 7), "CREATED");
    ASSERT_EQ(d.Dispatch("CREATE NAMESPACE ledger").response.substr(0, 7), "CREATED");
    ASSERT_EQ(d.Dispatch("CREATE TABLE orders.line (id int64, note varchar)").response.substr(0, 7),
              "CREATED");

    // The true qualifier is accepted everywhere the false one is refused,
    // and the pairs below are the whole of AF-6's list that this surface
    // reaches.
    EXPECT_EQ(d.Dispatch("INSERT INTO orders.line VALUES ('a')").response.substr(0, 8),
              "INSERTED");
    EXPECT_NE(d.Dispatch("SELECT * FROM orders.line").response.find("a"), std::string::npos);
    EXPECT_EQ(d.Dispatch("UPDATE orders.line SET note = 'b' WHERE id = 1").response.substr(0, 7),
              "UPDATED");
    EXPECT_NE(d.Dispatch("DESCRIBE orders.line").response.substr(0, 4), "ERR ");

    const char* wrong[] = {
        "INSERT INTO ledger.line VALUES ('c')",
        "SELECT * FROM ledger.line",
        "UPDATE ledger.line SET note = 'd' WHERE id = 1",
        "DELETE FROM ledger.line WHERE id = 1",
        "DESCRIBE ledger.line",
        "ALTER TABLE ledger.line RENAME TO moved",
        "CREATE INDEX ix ON ledger.line(note)",
        "CREATE CABIN ON ledger.line(note)",
        "DROP TABLE ledger.line",
    };
    for (const char* sql : wrong) {
        auto out = d.Dispatch(sql);
        EXPECT_EQ(out.response.substr(0, 4), "ERR ") << sql << " -> " << out.response;
        // Names where the relation *is*, because the useful answer to a
        // wrong placement assertion is the placement.
        EXPECT_NE(out.response.find("is in namespace 'orders'"), std::string::npos)
            << sql << " -> " << out.response;
    }

    // The relation is untouched by every one of those.
    EXPECT_NE(d.Dispatch("SELECT * FROM orders.line").response.find("b"), std::string::npos);
}

// ---- USE and namespace-scoped names (namespace.md NS5, NS6, NS8) ----------

TEST_F(CommandDispatcherTest, AnUnqualifiedNameMeansTheSessionsNamespaceOnly) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE NAMESPACE orders").response.substr(0, 7), "CREATED");
    ASSERT_EQ(d.Dispatch("CREATE TABLE orders.line (id int64, note varchar)").response.substr(0, 7),
              "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO orders.line VALUES ('o')").response.substr(0, 8),
              "INSERTED");

    // The session starts in `public`: a relation elsewhere is reached only
    // by its qualifier, and the refusal says which one to write.
    for (const char* sql : {"SELECT * FROM line", "INSERT INTO line VALUES ('x')",
                            "UPDATE line SET note = 'x' WHERE id = 1",
                            "DELETE FROM line WHERE id = 1", "DESCRIBE line",
                            "DROP TABLE line", "CREATE INDEX ix ON line(note)"}) {
        auto out = d.Dispatch(sql);
        EXPECT_EQ(out.response.substr(0, 4), "ERR ") << sql << " -> " << out.response;
        EXPECT_NE(out.response.find("write orders.line"), std::string::npos)
            << sql << " -> " << out.response;
    }

    // `USE orders` makes the bare name mean `orders.line`.
    EXPECT_EQ(d.Dispatch("USE orders").response, "USE namespace=orders");
    EXPECT_NE(d.Dispatch("SELECT * FROM line").response.find("1,o"), std::string::npos);
    EXPECT_EQ(d.Dispatch("UPDATE line SET note = 'p' WHERE id = 1").response.substr(0, 7),
              "UPDATED");
    // A subquery and a join bind in the same namespace.
    EXPECT_NE(d.Dispatch("SELECT * FROM line WHERE id IN (SELECT id FROM line)")
                  .response.find("1,p"),
              std::string::npos);

    // And `public` is now the qualified one.
    ASSERT_EQ(d.Dispatch("CREATE TABLE public.acct (id int64)").response.substr(0, 7), "CREATED");
    auto bare = d.Dispatch("SELECT * FROM acct");
    EXPECT_NE(bare.response.find("write public.acct"), std::string::npos) << bare.response;
    EXPECT_EQ(d.Dispatch("SELECT * FROM public.acct").response, "id");
}

TEST_F(CommandDispatcherTest, AnUnqualifiedCreateLandsInTheSessionsNamespace) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE NAMESPACE ledger").response.substr(0, 7), "CREATED");
    ASSERT_EQ(d.Dispatch("USE ledger").response, "USE namespace=ledger");
    ASSERT_EQ(d.Dispatch("CREATE TABLE entry (id int64, amt int64)").response.substr(0, 7),
              "CREATED");
    auto placed = d.Dispatch("SELECT namespace_oid FROM sys.tables WHERE name = 'entry'");
    EXPECT_NE(placed.response.find(std::to_string(catalog::kUserOidStart)), std::string::npos)
        << "an unqualified create under USE ledger did not land in it: " << placed.response;

    // Two namespaces may each hold a relation of one name; each name means
    // the one in the namespace it is resolved in.
    ASSERT_EQ(d.Dispatch("CREATE TABLE public.entry (id int64, amt int64)").response.substr(0, 7),
              "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO entry VALUES (10)").response.substr(0, 8), "INSERTED");
    ASSERT_EQ(d.Dispatch("INSERT INTO public.entry VALUES (20)").response.substr(0, 8),
              "INSERTED");
    EXPECT_NE(d.Dispatch("SELECT amt FROM entry").response.find("10"), std::string::npos);
    EXPECT_EQ(d.Dispatch("SELECT amt FROM entry").response.find("20"), std::string::npos);
    EXPECT_NE(d.Dispatch("SELECT amt FROM public.entry").response.find("20"), std::string::npos);
    EXPECT_NE(d.Dispatch("SELECT amt FROM ledger.entry").response.find("10"), std::string::npos);

    // A rename is asked in the relation's own namespace.
    EXPECT_EQ(d.Dispatch("ALTER TABLE entry RENAME TO entry").response.substr(0, 4), "ERR ");
    ASSERT_EQ(d.Dispatch("CREATE TABLE other (id int64)").response.substr(0, 7), "CREATED");
    EXPECT_EQ(d.Dispatch("ALTER TABLE public.entry RENAME TO other").response.substr(0, 4),
              "RENA")
        << "public holds no `other`, so the rename is free there";

    // Dropping one leaves the other.
    ASSERT_EQ(d.Dispatch("DROP TABLE entry").response.substr(0, 7), "DROPPED");
    EXPECT_NE(d.Dispatch("SELECT amt FROM public.other").response.find("20"), std::string::npos);
}

TEST_F(CommandDispatcherTest, UseRefusesWhatCannotBeASessionsNamespace) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto missing = d.Dispatch("USE ordrs");
    EXPECT_EQ(missing.response.substr(0, 4), "ERR ") << missing.response;
    EXPECT_NE(missing.response.find("no namespace named 'ordrs' (byte 4)"), std::string::npos)
        << missing.response;

    auto sys = d.Dispatch("USE sys");
    EXPECT_EQ(sys.response.substr(0, 4), "ERR ") << sys.response;
    EXPECT_NE(sys.response.find("holds the catalog"), std::string::npos) << sys.response;

    for (const char* bad : {"USE", "USE a b", "USE a.b"}) {
        auto out = d.Dispatch(bad);
        EXPECT_NE(out.response.find("USE takes one namespace name"), std::string::npos)
            << bad << " -> " << out.response;
    }
    // A refused USE leaves the session where it was.
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64)").response.substr(0, 7), "CREATED");
    EXPECT_EQ(d.Dispatch("SELECT * FROM t").response, "id");
    EXPECT_EQ(d.Dispatch("USE public").response, "USE namespace=public");
}

TEST_F(CommandDispatcherTest, ACatalogRelationIsNamedOnlyAsAQualifiedView) {
    // The defect this closes: `SELECT * FROM tables` bound the bootstrap
    // relation, which has no column rows, and answered "no columns for this
    // rel_id".
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    for (const char* sql : {"SELECT * FROM tables", "SELECT name FROM columns",
                            "SELECT * FROM public.tables"}) {
        auto out = d.Dispatch(sql);
        EXPECT_EQ(out.response.find("no columns"), std::string::npos) << sql << " -> " << out.response;
        EXPECT_NE(out.response.find("read it as sys."), std::string::npos)
            << sql << " -> " << out.response;
    }
    for (const char* sql : {"DELETE FROM sys.tables WHERE oid = 1", "DESCRIBE sys.tables",
                            "SELECT oid FROM sys.tables WHERE oid IN (SELECT oid FROM sys.objects)"}) {
        auto out = d.Dispatch(sql);
        EXPECT_EQ(out.response.substr(0, 4), "ERR ") << sql << " -> " << out.response;
        EXPECT_EQ(out.response.find("no columns"), std::string::npos) << sql << " -> " << out.response;
    }
    // The view itself still reads.
    EXPECT_NE(d.Dispatch("SELECT name FROM sys.tables").response.find("objects"),
              std::string::npos);
}

TEST_F(CommandDispatcherTest, ANamespaceHoldingARelationIsNotDropped) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE NAMESPACE orders").response.substr(0, 7), "CREATED");
    ASSERT_EQ(d.Dispatch("CREATE TABLE orders.line (id int64)").response.substr(0, 7), "CREATED");

    // RESTRICT, and it names the relation that blocked it rather than a
    // count - the user's next act is to drop that relation.
    auto refused = d.Dispatch("DROP NAMESPACE orders");
    EXPECT_EQ(refused.response.substr(0, 4), "ERR ") << refused.response;
    EXPECT_NE(refused.response.find("line"), std::string::npos) << refused.response;

    ASSERT_EQ(d.Dispatch("DROP TABLE orders.line").response.substr(0, 7), "DROPPED");
    EXPECT_EQ(d.Dispatch("DROP NAMESPACE orders").response.substr(0, 18), "DROPPED NAMESPACE ");
    EXPECT_NE(d.Dispatch("DROP NAMESPACE orders").response.find("no namespace named 'orders'"),
              std::string::npos);
}

TEST_F(CommandDispatcherTest, AUserTableNamedLikeAViewIsUnaffected) {
    // The views live under `sys` and nowhere else: an unqualified name
    // always means a user relation, so a table called `tables` is still
    // reachable and is not shadowed by sys.tables.
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto created = d.Dispatch("CREATE TABLE t (id int64, name varchar)");
    ASSERT_EQ(created.response.substr(0, 7), "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES ('mine')").response.substr(0, 8), "INSERTED");

    auto user = d.Dispatch("SELECT * FROM t");
    EXPECT_NE(user.response.find("mine"), std::string::npos) << user.response;
}

TEST_F(CommandDispatcherTest, AnAliasedSingleRelationSelectStillResolvesItsTable) {
    // The catalog is looked up by table_name, never by the binding - an
    // alias renames the relation for predicates, not for the catalog.
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int32, name varchar)").response.substr(0, 7),
              "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO acct VALUES ('alice')").response.substr(0, 8), "INSERTED");

    EXPECT_NE(d.Dispatch("SELECT * FROM acct AS a").response.find("1,alice"), std::string::npos);
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
        auto store = storage::DevicePageStore::Open(*device.value(), ::kds::storage::FrameCapacity{4096}, kFirstUserPageId);
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

    auto store = storage::DevicePageStore::Open(*device.value(), ::kds::storage::FrameCapacity{4096}, kFirstUserPageId);
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
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int64, name varchar) HEAP").response.substr(0, 7),
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

// ---- Authorization (role.hpp; docs/spec/protocol.md §14) -----------------------
//
// The statement-class matrix, exercised at the dispatcher - the one
// choke point - with sessions holding each rank. The default-constructed
// Session is the admin row, which is also the auth-off contract every
// other test in this file has silently relied on since roles landed.

TEST_F(CommandDispatcherTest, RolesGateStatementClasses) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);

    Session admin;  // default kAdmin
    Session writer;
    writer.set_role(Role::kReadWrite);
    Session reader;
    reader.set_role(Role::kReadOnly);

    const auto refused = [](const DispatchOutcome& out) {
        return out.response.rfind("ERR permission: ", 0) == 0;
    };

    ASSERT_FALSE(refused(d.Dispatch("CREATE TABLE t (id INT64, v INT64)", &admin)));

    // The readonly floor: reads, liveness, transaction control, own SET.
    EXPECT_EQ(d.Dispatch("PING", &reader).response, "PONG");
    EXPECT_FALSE(refused(d.Dispatch("SELECT * FROM t", &reader)));
    EXPECT_FALSE(refused(d.Dispatch("ANALYZE SELECT * FROM t", &reader)));
    EXPECT_FALSE(refused(d.Dispatch("SHOW META", &reader)));
    EXPECT_FALSE(refused(d.Dispatch("DESCRIBE tables", &reader)));
    EXPECT_FALSE(refused(d.Dispatch("SET ISOLATION LEVEL READ COMMITTED", &reader)));
    EXPECT_FALSE(refused(d.Dispatch("BEGIN", &reader)));
    EXPECT_FALSE(refused(d.Dispatch("ROLLBACK", &reader)));

    // Writes need readwrite.
    EXPECT_TRUE(refused(d.Dispatch("INSERT INTO t VALUES (7)", &reader)));
    EXPECT_TRUE(refused(d.Dispatch("UPDATE t SET v = 1 WHERE id = 1", &reader)));
    EXPECT_TRUE(refused(d.Dispatch("DELETE FROM t WHERE id = 1", &reader)));
    EXPECT_FALSE(refused(d.Dispatch("INSERT INTO t VALUES (7)", &writer)));

    // DDL, the server's own switches, durability and shutdown need admin.
    for (Session* s : {&reader, &writer}) {
        EXPECT_TRUE(refused(d.Dispatch("CREATE TABLE u (id INT64)", s)));
        EXPECT_TRUE(refused(d.Dispatch("DROP TABLE t", s)));
        EXPECT_TRUE(refused(d.Dispatch("ALTER TABLE t RENAME TO u", s)));
        EXPECT_TRUE(refused(d.Dispatch("SYNC", s)));
        EXPECT_TRUE(refused(d.Dispatch("STOP", s)));
        EXPECT_TRUE(refused(d.Dispatch("SET CABIN_OPTIMIZER ON", s)));
    }

    // The refusal names both roles - actionable without a manual.
    auto out = d.Dispatch("DROP TABLE t", &reader);
    EXPECT_NE(out.response.find("needs admin"), std::string::npos) << out.response;
    EXPECT_NE(out.response.find("this connection is readonly"), std::string::npos)
        << out.response;

    // Unclassified commands are admin's (refused by default, never
    // admitted by omission) - and an admin's typo still reads as a typo,
    // not as a permission problem.
    EXPECT_TRUE(refused(d.Dispatch("FROBNICATE", &writer)));
    EXPECT_EQ(d.Dispatch("FROBNICATE", &admin).response, "ERR unknown command");

    // Admin covers everything below it.
    EXPECT_FALSE(refused(d.Dispatch("SELECT * FROM t", &admin)));
    EXPECT_FALSE(refused(d.Dispatch("SYNC", &admin)));
}

}  // namespace
}  // namespace kds::server
