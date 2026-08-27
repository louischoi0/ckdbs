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
#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/scheduler.hpp"
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

// ---- SHOW PATTERNS (docs/spec/waystone-concpets.md section 4) ------------------

TEST_F(CommandDispatcherTest, ShowPatternsOnAFreshDatabaseReportsNone) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("SHOW PATTERNS");
    EXPECT_EQ(out.response, "patterns=0");
}

TEST_F(CommandDispatcherTest, ShowPatternsListsARegisteredPattern) {
    ASSERT_TRUE(boot_->catalog.RegisterPattern(0xABCD, catalog::kStmtClassUnclassified).ok());

    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("SHOW PATTERNS");

    EXPECT_NE(out.response.find("patterns=1"), std::string::npos) << out.response;
    // Hex, because comparing two 64-bit fingerprints in decimal is not a
    // thing anyone can do by eye.
    EXPECT_NE(out.response.find("pattern_id=0xabcd"), std::string::npos) << out.response;
    EXPECT_NE(out.response.find("uses=0"), std::string::npos) << out.response;
    // No trail has been recorded, so there is no directory - and the
    // report must say so rather than printing a root of page 0.
    EXPECT_NE(out.response.find("waystone=none"), std::string::npos) << out.response;
    EXPECT_EQ(out.response.find("stale="), std::string::npos) << out.response;
}

TEST_F(CommandDispatcherTest, ShowPatternsReportsAWaystoneDirectory) {
    ASSERT_TRUE(boot_->catalog.RegisterPattern(7, catalog::kStmtClassUnclassified).ok());
    ASSERT_TRUE(boot_->catalog.SetPatternWaystoneRoot(7, 4096, 2).ok());

    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("SHOW PATTERNS");
    EXPECT_NE(out.response.find("waystone=root=4096,depth=2"), std::string::npos) << out.response;
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
    row.pattern_id = 0x5151;
    row.fingerprint_version = parser::kFingerprintVersion + 1;
    row.waystone_root = kInvalidPageId;
    auto encoded = row.Encode();
    ASSERT_TRUE(page.InsertTuple(encoded, catalog::kBootstrapXid).ok());

    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("SHOW PATTERNS");

    // Listed, not hidden: this is an inspection surface, and a row nothing
    // will ever look up again is exactly what an operator needs to see.
    EXPECT_NE(out.response.find("pattern_id=0x5151"), std::string::npos) << out.response;
    EXPECT_NE(out.response.find("stale=v"), std::string::npos) << out.response;

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
    EXPECT_NE(out.response.find("version=" + std::to_string(kSuperBlockVersion)),
              std::string::npos);
    EXPECT_NE(out.response.find("wal_anchor_count=0"), std::string::npos);
    EXPECT_FALSE(out.should_stop);
}

// ---- The cross-core write refusal counters (crosscore.md §6, T5) --------
//
// §6 specifies a per-core counter keyed (home core, target core, relation)
// and calls it "the input the future placement/2PC decision will be made
// from". The class and both recording sites existed; nothing printed them,
// so the number could not be read from outside the process - which is the
// whole of what a metric is for.

TEST_F(CommandDispatcherTest, ShowMetaReportsNoCrossCoreWriteRefusalsOnAQuietCore) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto out = d.Dispatch("SHOW META");
    // Zero prints. It is an answer - *this workload asked for no cross-core
    // write* - and the before-shipping era is recorded for exactly that
    // reading, so it must not be omitted the way an absent subsystem is.
    EXPECT_NE(out.response.find("cross_core_write_refusals=0"), std::string::npos)
        << out.response;
    EXPECT_NE(out.response.find("cross_core_write_refusal_keys=0"), std::string::npos)
        << out.response;
    EXPECT_EQ(out.response.find("cross_core_write_refusal_detail="), std::string::npos)
        << "no keys, no detail token: " << out.response;
}

TEST_F(CommandDispatcherTest, ARefusedCrossCoreWriteIsCountedAndPrintedByKey) {
    // Core 0 creates the relation, so it is owned by core 0.
    CommandDispatcher owner(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(owner.Dispatch("CREATE TABLE acct (id int64, name varchar)")
                  .response.substr(0, 7),
              "CREATED");

    // A dispatcher running as core 1 may not write it (CC3).
    CommandDispatcher peer(boot_->superblock, boot_->catalog, store_, nullptr, nullptr,
                           nullptr, wal::DurabilityClass::kGroup, exec::Budget(),
                           /*recorder=*/nullptr, /*replay_enabled=*/false,
                           /*access_statistics=*/true, /*cabins=*/nullptr,
                           /*txn=*/nullptr, txn::IsolationLevel::kReadCommitted,
                           /*core_id=*/1);
    auto refused = peer.Dispatch("INSERT INTO acct VALUES ('alice')");
    ASSERT_NE(refused.response.find("bound to core"), std::string::npos)
        << refused.response;

    auto meta = peer.Dispatch("SHOW META");
    EXPECT_NE(meta.response.find("cross_core_write_refusals=1"), std::string::npos)
        << meta.response;
    EXPECT_NE(meta.response.find("cross_core_write_refusal_keys=1"), std::string::npos)
        << meta.response;
    // home>target:oid=count - the key §6 names, in the order the ordered map
    // gives, which is what keeps two runs' reports comparable. Asserted as
    // the **whole token**: a bare find("=1") over the reply passes on a
    // dozen unrelated fields (`ddl_durable=1`, `catalog_recovered=1`) and
    // would therefore pass with no count printed at all.
    const std::size_t at = meta.response.find("cross_core_write_refusal_detail=");
    ASSERT_NE(at, std::string::npos) << meta.response;
    const std::string detail =
        meta.response.substr(at, meta.response.find(' ', at) - at);
    EXPECT_EQ(detail.rfind("cross_core_write_refusal_detail=1>0:", 0), 0u) << detail;
    EXPECT_EQ(detail.substr(detail.size() - 2), "=1") << detail;

    // A second refusal of the same shape is the same key, counted twice -
    // not a second key. The distinction is the whole point of a keyed
    // counter: "one relation refused twice" and "two relations refused
    // once" are different evidence for 2PC.
    ASSERT_NE(peer.Dispatch("INSERT INTO acct VALUES ('bob')").response.find("bound to core"),
              std::string::npos);
    auto meta2 = peer.Dispatch("SHOW META");
    EXPECT_NE(meta2.response.find("cross_core_write_refusals=2"), std::string::npos)
        << meta2.response;
    EXPECT_NE(meta2.response.find("cross_core_write_refusal_keys=1"), std::string::npos)
        << meta2.response;
}

TEST_F(CommandDispatcherTest, TheOwningCoresOwnWritesAreNotCountedAsCrossCore) {
    // The counter answers "did this workload want a cross-core write". A
    // write that succeeds locally is not evidence for 2PC and must not
    // appear - the sibling half of the undercount stated at the print site.
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int64, name varchar)").response.substr(0, 7),
              "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO acct VALUES ('alice')").response.substr(0, 8),
              "INSERTED");
    EXPECT_NE(d.Dispatch("SHOW META").response.find("cross_core_write_refusals=0"),
              std::string::npos);
}

// ---- The group-accounting block (sched.md §4, T4) ----------------------

TEST_F(CommandDispatcherTest, ShowMetaOmitsGroupAccountingWithNoReactorAttached) {
    // The recovery block's rule: absent, not zeroed. A dispatcher with no
    // reactor has not "spent no time in the foreground", it has no answer,
    // and printing zeroes would be one.
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    const std::string meta = d.Dispatch("SHOW META").response;
    EXPECT_EQ(meta.find("sched_wall_us="), std::string::npos) << meta;
    EXPECT_EQ(meta.find("sched_foreground_polls="), std::string::npos) << meta;
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
    // Whole tokens: `find("...=10000")` also matches `=100000`, and a
    // counter that grew by a factor of ten is exactly the defect worth
    // catching here.
    const auto token = [&meta](std::string_view key) {
        const std::size_t at = meta.find(key);
        if (at == std::string::npos) return std::string("<absent>");
        const std::size_t end = meta.find(' ', at);
        return meta.substr(at, end == std::string::npos ? end : end - at);
    };
    EXPECT_EQ(token("sched_wall_us="), "sched_wall_us=10000") << meta;
    EXPECT_EQ(token("sched_system_polled_us="), "sched_system_polled_us=3000") << meta;
    EXPECT_EQ(token("sched_system_polls="), "sched_system_polls=1") << meta;
    EXPECT_EQ(token("sched_foreground_polled_us="), "sched_foreground_polled_us=0") << meta;
    EXPECT_EQ(token("sched_maintenance_polls="), "sched_maintenance_polls=0") << meta;
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
    EXPECT_NE(out.response.find("name=id type=int32 notnull=yes pk=yes autoincrement=if-omitted"),
              std::string::npos)
        << out.response;
    EXPECT_NE(out.response.find("pk=no autoincrement=no"), std::string::npos) << out.response;
    // One "\n"-escaped section per column, and never a raw newline byte.
    EXPECT_EQ(out.response.find('\n'), std::string::npos);
}

// ---- The key order in DESCRIBE (heap-and-tuple.md §4.1) -----------------
//
// `key_mode=` was a declaration and is gone with the mode. `key_order=` is
// an observation in the same position, and the pk column's `autoincrement=`
// is now `if-omitted` on every relation - the sequence runs when the INSERT
// omits the key and does not when the INSERT names one, and both are legal
// everywhere, so neither `yes` nor `no` would be true.

TEST_F(CommandDispatcherTest, DescribeReportsANewRelationAsAscending) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int64, qty int64)").response.substr(0, 7),
              "CREATED");

    auto out = d.Dispatch("DESCRIBE acct");
    // Beside the storage, which is the DDL-only fact on the line.
    EXPECT_NE(out.response.find("clustered_type=HEAP key_order=ascending"), std::string::npos)
        << out.response;
    EXPECT_NE(out.response.find("name=id type=int64 notnull=yes pk=yes autoincrement=if-omitted"),
              std::string::npos)
        << out.response;
}

TEST_F(CommandDispatcherTest, DescribeReportsUnorderedAfterABelowMarkKey) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    auto created = d.Dispatch("CREATE TABLE t (id int64, qty int64) BTREE");
    ASSERT_EQ(created.response.substr(0, 7), "CREATED") << created.response;

    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (100, 1)").response.substr(0, 8), "INSERTED");
    EXPECT_NE(d.Dispatch("DESCRIBE t").response.find("key_order=ascending"), std::string::npos);

    // Below the mark 100 left behind: admitted, and the line changes.
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (50, 2)").response.substr(0, 8), "INSERTED");
    auto out = d.Dispatch("DESCRIBE t");
    EXPECT_NE(out.response.find("clustered_type=BTREE key_order=unordered"), std::string::npos)
        << out.response;
    EXPECT_NE(out.response.find("name=id type=int64 notnull=yes pk=yes autoincrement=if-omitted"),
              std::string::npos)
        << out.response;
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
    auto out = d.Dispatch("FLIBBERTIGIBBET EVERYTHING");
    EXPECT_EQ(out.response, "ERR unknown command");
    EXPECT_FALSE(out.should_stop);
}

TEST_F(CommandDispatcherTest, DropIsAKnownVerbWithANamedTargetList) {
    // `DROP` became a statement head with CREATE PATTERN, so it no longer
    // falls into "unknown command" - and it should not. There is still no
    // DROP TABLE, and naming what DROP does take beats a generic refusal
    // that leaves a client unsure whether the word was recognized. The list
    // in the message is the whole of what exists, so it grows with the
    // targets: CABIN joined it with the Cabin feature (docs/spec/cabin.md),
    // INDEX with the index grammar (docs/spec/index.md §10), ASSERTION with
    // the assertion catalog (docs/spec/assertion.md §8.3, AST03). This test
    // is meant to be edited when one is added - that is what pins the list to
    // reality rather than to whatever it happened to say.
    //
    // `DROP TABLE` joined the list with docs/spec/drop-table.md (DT01) -
    // the edit this comment scheduled - and the RESTRICT hook AST03 could
    // not wire finally has its DDL. An unknown table now answers NotFound
    // from resolution, not a target-list refusal.
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    EXPECT_EQ(d.Dispatch("DROP EVERYTHING").response,
              "ERR only DROP TABLE, DROP PATTERN, DROP CABIN, DROP INDEX and DROP ASSERTION "
              "are supported");
    EXPECT_EQ(d.Dispatch("DROP TABLE t").response, "ERR no table with this name");
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
    auto view = heap::PageView::CreateEmpty(page.value().bytes(), 0);
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

TEST_F(CommandDispatcherTest, CreateTableNoLongerRefusesAnExplicitHeapRelation) {
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);

    // The contradiction this test was written for is gone (§4.1): a heap
    // relation takes a caller-named key, provided the key ascends, so there
    // is no pairing of the two words left to refuse.
    auto spelled = d.Dispatch("CREATE TABLE t (id int64) HEAP EXPLICIT");
    EXPECT_EQ(spelled.response.substr(0, 7), "CREATED") << spelled.response;
    EXPECT_NE(d.Dispatch("DESCRIBE t").response.find("clustered_type=HEAP"), std::string::npos);

    // And a bare EXPLICIT no longer pulls storage to btree with it - that
    // resolution existed only to keep the refusal above reachable from a
    // written word alone.
    auto implied = d.Dispatch("CREATE TABLE t2 (id int64) EXPLICIT");
    EXPECT_EQ(implied.response.substr(0, 7), "CREATED") << implied.response;
    EXPECT_NE(d.Dispatch("DESCRIBE t2").response.find("clustered_type=HEAP key_order=ascending"),
              std::string::npos);
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

TEST_F(CommandDispatcherTest, SupplyingThePrimaryKeyOnInsertIsAdmittedAndMovesTheMark) {
    // The inverse of what this test asserted until 2026-08-25 (§4.1). The
    // mark is the part worth keeping: a named key moves it, so the next
    // issued id clears the named one instead of colliding with it.
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE acct (id int32, name varchar)").response.substr(0, 7),
              "CREATED");

    auto out = d.Dispatch("INSERT INTO acct VALUES (7, 'alice')");
    EXPECT_NE(out.response.find("id=7"), std::string::npos) << out.response;
    EXPECT_NE(d.Dispatch("DESCRIBE acct").response.find("next_id=8"), std::string::npos);

    // A refused key still burns nothing: below the mark on a heap relation.
    auto refused = d.Dispatch("INSERT INTO acct VALUES (3, 'bob')");
    EXPECT_EQ(refused.response.substr(0, 4), "ERR ") << refused.response;
    EXPECT_NE(d.Dispatch("DESCRIBE acct").response.find("next_id=8"), std::string::npos);
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
    // K-M3: the refusal is `Unsupported`, so it carries a byte position
    // and reaches the wire as a plain `ERR` - none of the three coded
    // tokens, because a client cannot fix this by retrying or by changing
    // an argument. Asserting the absence is what keeps the compatibility
    // surface one bit wide.
    EXPECT_NE(out.response.find("at byte "), std::string::npos) << out.response;
    EXPECT_EQ(out.response.find("retryable="), std::string::npos) << out.response;

    // The row is untouched, key included.
    EXPECT_NE(d.Dispatch("SELECT * FROM acct").response.find("1,alice"), std::string::npos);
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

TEST_F(CommandDispatcherTest, ACatalogViewComparesItsIntegersUnsigned) {
    // Every integer a view emits is built from a `uint64_t`
    // (catalog_view.cpp's `Int()`), and `sys.patterns.pattern_id` is a
    // full-range 64-bit fingerprint - so comparing `int_val` signed put
    // every id above INT64_MAX below every id under it. `pattern_id < 100`
    // answered the eight largest ids in the catalog, each printed in full
    // by the same statement that claimed it was small.
    CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE r (id int64, a int64, b int64)").response.substr(0, 7),
              "CREATED");
    // Distinct bodies, because a pattern_id is the fingerprint of the shape
    // and identical shapes would collide into one row.
    const char* wheres[] = {"id = $x", "a = $x", "b = $x", "a > $x", "b > $x", "a < $x",
                            "b < $x", "a >= $x", "b >= $x", "a != $x", "b != $x",
                            "id > $x", "id < $x", "id >= $x"};
    int made = 0;
    for (const char* where : wheres) {
        const std::string reply =
            d.Dispatch("CREATE PATTERN p" + std::to_string(made) + " ($x int64) OF SELECT a "
                       "FROM r WHERE " + where).response;
        if (reply.substr(0, 4) != "ERR ") ++made;
    }
    ASSERT_GT(made, 0) << "the fixture must register patterns to have ids to compare";

    const std::string all = d.Dispatch("SELECT oid FROM sys.patterns").response;
    const std::string ids = d.Dispatch("SELECT pattern_id FROM sys.patterns").response;
    // Non-vacuous only if some id really does use the top bit. A
    // fingerprint is deterministic, so this either holds or the corpus
    // moved and the body list above wants another entry - which is what
    // the message says rather than the test quietly proving nothing.
    bool any_high = false;
    for (std::size_t at = 0; (at = ids.find("\\n", at)) != std::string::npos;) {
        at += 2;
        if (std::strtoull(ids.c_str() + at, nullptr, 10) > (1ull << 63)) any_high = true;
    }
    ASSERT_TRUE(any_high) << "no pattern_id above INT64_MAX, so this proves nothing: " << ids;

    // Every uint64 is >= 0 and none is < 0. Signed, the high-bit ids
    // answered the second and were missing from the first.
    EXPECT_EQ(d.Dispatch("SELECT oid FROM sys.patterns WHERE pattern_id >= 0").response, all);
    EXPECT_EQ(d.Dispatch("SELECT oid FROM sys.patterns WHERE pattern_id < 0").response, "oid");
    EXPECT_EQ(d.Dispatch("SELECT oid FROM sys.patterns WHERE pattern_id < 100").response, "oid");
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
        d.Dispatch("SELECT * FROM sys.patterns WHERE oid = pattern_id").response;
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
    auto bad_schema = d.Dispatch("SELECT * FROM public.acct");
    EXPECT_NE(bad_schema.response.find("unknown schema"), std::string::npos)
        << bad_schema.response;

    auto bad_column = d.Dispatch("SELECT * FROM sys.tables WHERE nosuchcol = 1");
    EXPECT_NE(bad_column.response.find("has no column"), std::string::npos) << bad_column.response;
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
