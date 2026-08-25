#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/log.hpp"
#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/page_mgr/page_mgr.hpp"
#include "kds/wal/durability.hpp"

// What every subsystem owes the log, in one place: the events that change
// what is on disk are reported, and the failures that have no caller to
// return a Status to are reported at a level nobody filters out.
//
// These tests assert the *event*, not the wording - they match on the
// component tag and a couple of stable substrings, so rephrasing a message
// does not fail a test while deleting the call site does. What they pin
// down is the property log.hpp cares about: the level check happens before
// the message is built, so a subsystem below its level writes nothing at
// all.

namespace kds {
namespace {

// A logger over memory, with the level a given test needs.
class CapturedLog {
public:
    explicit CapturedLog(LogLevel level = LogLevel::kTrace)
        : logger_(&sink_, clock_, level) {}

    Logger* get() noexcept { return &logger_; }
    const std::vector<std::string>& lines() const noexcept { return sink_.lines; }

    // Lines carrying "[<component>] " and every one of `needles`.
    std::vector<std::string> Matching(const std::string& component,
                                      const std::vector<std::string>& needles) const {
        std::vector<std::string> hits;
        for (const std::string& line : sink_.lines) {
            if (line.find("[" + component + "]") == std::string::npos) continue;
            bool all = true;
            for (const std::string& needle : needles) {
                if (line.find(needle) == std::string::npos) {
                    all = false;
                    break;
                }
            }
            if (all) hits.push_back(line);
        }
        return hits;
    }

    bool Has(const std::string& component, const std::vector<std::string>& needles) const {
        return !Matching(component, needles).empty();
    }

private:
    MemoryLogSink sink_;
    ManualWallClock clock_{1'700'000'000};
    Logger logger_;
};

// A minimal valid relation: a uint64 Keystone column (invariant 10 -
// catalog::CheckKeystoneColumn rejects anything else in first position)
// plus one body column, so "columns=2" in the CREATE TABLE line has
// something to count.
catalog::Schema TwoColumnSchema() {
    catalog::Schema schema;
    catalog::SysColumnRow id_col{};
    id_col.pos = 0;
    catalog::SetName(id_col.name, "id");
    id_col.type_val = catalog::kTypeValUint64;
    id_col.len = 8;
    id_col.notnull = true;
    schema.columns.push_back(id_col);

    catalog::SysColumnRow amount_col{};
    amount_col.pos = 1;
    catalog::SetName(amount_col.name, "amount");
    amount_col.type_val = catalog::kTypeValInt64;
    amount_col.len = 8;
    amount_col.notnull = false;
    schema.columns.push_back(amount_col);
    return schema;
}

// A log whose durability point only moves when a test says so.
class ScriptedWal final : public wal::WalDurability {
public:
    wal::Lsn durable_lsn() const noexcept override { return durable_; }
    Status EnsureDurable(wal::Lsn lsn) override {
        durable_ = lsn + 1;
        return Status::OK();
    }
    void SetDurable(wal::Lsn lsn) noexcept { durable_ = lsn; }

private:
    wal::Lsn durable_ = 0;
};

// ---- Buffer pool: the page-modification journal -------------------------

TEST(SubsystemLoggingTest, DirtyingAPageIsLogged) {
    storage::InMemoryPageStore backing;
    storage::BufferPool pool(backing, /*nr_frames=*/4);
    CapturedLog log;
    pool.SetLogger(log.get());

    auto frame = pool.AllocNew(7);
    ASSERT_TRUE(frame.ok()) << frame.status().message();
    storage::FormatPage(frame.value()->bytes(), PageType::kHeap);
    frame.value()->MarkDirty(/*record_lsn=*/42);
    pool.Unpin(*frame.value());

    EXPECT_TRUE(log.Has("buffer", {"alloc", "page=7"}));
    // The page-modification line carries both LSNs: page_lsn is what the
    // flush gate waits on, rec_lsn is what a checkpoint's redo start is
    // computed from, and reading one without the other explains nothing.
    EXPECT_TRUE(log.Has("page", {"dirty", "page=7", "lsn=42", "rec_lsn=42"}));
}

TEST(SubsystemLoggingTest, PageEventsAreSilentBelowTraceLevel) {
    storage::InMemoryPageStore backing;
    storage::BufferPool pool(backing, /*nr_frames=*/4);
    CapturedLog log(LogLevel::kInfo);
    pool.SetLogger(log.get());

    auto frame = pool.AllocNew(7);
    ASSERT_TRUE(frame.ok()) << frame.status().message();
    frame.value()->MarkDirty(/*record_lsn=*/42);
    pool.Unpin(*frame.value());

    // One line per page touch is a journal at Trace and noise at Info.
    EXPECT_TRUE(log.lines().empty());
}

TEST(SubsystemLoggingTest, LoggerReachesFramesRegisteredBeforeItWasSet) {
    storage::InMemoryPageStore backing;
    storage::BufferPool pool(backing, /*nr_frames=*/4);

    auto frame = pool.AllocNew(3);
    ASSERT_TRUE(frame.ok()) << frame.status().message();

    CapturedLog log;
    pool.SetLogger(log.get());  // after the frame is already resident
    frame.value()->MarkDirty(/*record_lsn=*/9);
    pool.Unpin(*frame.value());

    EXPECT_TRUE(log.Has("page", {"dirty", "page=3"}));
}

TEST(SubsystemLoggingTest, GateRefusalIsLoggedAtError) {
    storage::InMemoryPageStore backing;
    storage::BufferPool pool(backing, /*nr_frames=*/4);
    CapturedLog log(LogLevel::kError);
    pool.SetLogger(log.get());  // no WalDurability injected

    auto frame = pool.AllocNew(5);
    ASSERT_TRUE(frame.ok()) << frame.status().message();
    storage::FormatPage(frame.value()->bytes(), PageType::kHeap);
    frame.value()->MarkDirty(/*record_lsn=*/11);
    pool.Unpin(*frame.value());

    EXPECT_FALSE(pool.Flush(*frame.value()).ok());
    // A missing seam is a wiring fault whose Status surfaces far from the
    // SetWalDurability() call that was never made, so it is an Error even
    // though nothing was lost.
    EXPECT_TRUE(log.Has("buffer", {"flush refused", "page 5"}));
}

TEST(SubsystemLoggingTest, WalWaitAndFlushBatchAreLoggedAtDebug) {
    storage::InMemoryPageStore backing;
    storage::BufferPool pool(backing, /*nr_frames=*/4);
    ScriptedWal wal;
    pool.SetWalDurability(&wal);
    CapturedLog log(LogLevel::kDebug);
    pool.SetLogger(log.get());

    auto frame = pool.AllocNew(6);
    ASSERT_TRUE(frame.ok()) << frame.status().message();
    storage::FormatPage(frame.value()->bytes(), PageType::kHeap);
    frame.value()->MarkDirty(/*record_lsn=*/20);
    pool.Unpin(*frame.value());

    ASSERT_TRUE(pool.Flush(*frame.value()).ok());
    EXPECT_TRUE(log.Has("buffer", {"wal wait", "page 6", "20"}));
    EXPECT_TRUE(log.Has("buffer", {"flushed", "1 page"}));
    // Debug is a per-batch level: no per-page Trace lines leak into it.
    EXPECT_TRUE(log.Matching("page", {"dirty"}).empty());
}

TEST(SubsystemLoggingTest, FrameExhaustionIsLoggedAtWarn) {
    storage::InMemoryPageStore backing;
    storage::BufferPool pool(backing, /*nr_frames=*/1);
    CapturedLog log(LogLevel::kWarn);
    pool.SetLogger(log.get());

    ASSERT_TRUE(pool.AllocNew(1).ok());
    EXPECT_FALSE(pool.AllocNew(2).ok());
    EXPECT_TRUE(log.Has("buffer", {"frame table full"}));
}

// ---- Device page store: what actually reaches the device ----------------

TEST(SubsystemLoggingTest, PageStoreLogsAllocationAndWriteBack) {
    auto device = storage::MemoryPageDevice::Create(/*extent_pages=*/8);
    ASSERT_TRUE(device.ok()) << device.status().message();
    auto store = storage::DevicePageStore::Open(*device.value(), /*first_new_page_id=*/128);
    ASSERT_TRUE(store.ok()) << store.status().message();
    CapturedLog log;
    store.value()->SetLogger(log.get());

    auto page = store.value()->CreateAt(4);
    ASSERT_TRUE(page.ok()) << page.status().message();
    storage::FormatPage(page.value().bytes(), PageType::kHeap);
    ASSERT_TRUE(store.value()->Sync().ok());

    EXPECT_TRUE(log.Has("pagestore", {"alloc", "page=4"}));
    EXPECT_TRUE(log.Has("pagestore", {"wrote", "page=4"}));
    EXPECT_TRUE(log.Has("pagestore", {"maps written"}));
    EXPECT_TRUE(log.Has("pagestore", {"device synced"}));
}

TEST(SubsystemLoggingTest, ChecksumFailureIsLoggedAsCorruption) {
    auto device = storage::MemoryPageDevice::Create(/*extent_pages=*/8);
    ASSERT_TRUE(device.ok()) << device.status().message();

    {
        auto store = storage::DevicePageStore::Open(*device.value(), /*first_new_page_id=*/128);
        ASSERT_TRUE(store.ok()) << store.status().message();
        auto page = store.value()->CreateAt(4);
        ASSERT_TRUE(page.ok()) << page.status().message();
        storage::FormatPage(page.value().bytes(), PageType::kHeap);
        ASSERT_TRUE(store.value()->Sync().ok());
    }

    // Corrupt the body behind the store's back, then reopen so the page is
    // read from the device rather than served from a resident frame.
    std::array<std::byte, kPageSize> raw{};
    ASSERT_TRUE(device.value()->ReadPage(4, std::span<std::byte, kPageSize>(raw)).ok());
    raw[storage::kPageBodyOffset] = ~raw[storage::kPageBodyOffset];
    ASSERT_TRUE(
        device.value()
            ->WritePage(4, std::span<const std::byte, kPageSize>(raw))
            .ok());

    auto store = storage::DevicePageStore::Open(*device.value(), /*first_new_page_id=*/128);
    ASSERT_TRUE(store.ok()) << store.status().message();
    CapturedLog log(LogLevel::kError);
    store.value()->SetLogger(log.get());

    EXPECT_FALSE(store.value()->Get(4).ok());
    // The one condition this layer can detect that no caller can diagnose
    // from a Status alone - so it says "corruption" in as many words.
    EXPECT_TRUE(log.Has("pagestore", {"corruption", "page 4"}));
}

// ---- Catalog and bootstrap: the structural writes -----------------------

TEST(SubsystemLoggingTest, BootstrapDistinguishesFreshFromExisting) {
    storage::InMemoryPageStore store;

    CapturedLog fresh_log(LogLevel::kInfo);
    auto fresh = bootstrap::BootstrapDatabase(store, /*now_unix_seconds=*/1'700'000'000,
                                              storage::kDefaultInlineCellWidth, /*cores=*/1,
                                              fresh_log.get());
    ASSERT_TRUE(fresh.ok()) << fresh.status().message();
    EXPECT_TRUE(fresh_log.Has("bootstrap", {"fresh database"}));
    EXPECT_TRUE(fresh_log.Has("catalog", {"bootstrapped"}));

    CapturedLog reopen_log(LogLevel::kInfo);
    auto reopened = bootstrap::BootstrapDatabase(store, /*now_unix_seconds=*/1'700'000'100,
                                                 storage::kDefaultInlineCellWidth, /*cores=*/1,
                                                 reopen_log.get());
    ASSERT_TRUE(reopened.ok()) << reopened.status().message();
    // The distinction that matters: an existing database must not be
    // re-bootstrapped, and the log has to make it obvious which path ran.
    EXPECT_TRUE(reopen_log.Has("bootstrap", {"existing database"}));
    EXPECT_TRUE(reopen_log.Matching("catalog", {"bootstrapped"}).empty());
}

TEST(SubsystemLoggingTest, CreateTableIsLoggedWithItsRootPage) {
    storage::InMemoryPageStore store;
    auto database = bootstrap::BootstrapDatabase(store, /*now_unix_seconds=*/1'700'000'000);
    ASSERT_TRUE(database.ok()) << database.status().message();

    CapturedLog log(LogLevel::kDebug);
    database.value().catalog.SetLogger(log.get());

    catalog::Schema schema = TwoColumnSchema();
    auto oid = database.value().catalog.CreateTable(catalog::kNamespacePublic, "ledger", schema,
                                                    catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    // Debug, so the Info log keeps exactly one line per DDL statement (the
    // dispatcher's). The catalog's line adds the root page id.
    EXPECT_TRUE(log.Has("catalog", {"created table", "'ledger'", "root_page=", "columns=2"}));
}

TEST(SubsystemLoggingTest, RowIdIssueIsATraceEvent) {
    storage::InMemoryPageStore store;
    auto database = bootstrap::BootstrapDatabase(store, /*now_unix_seconds=*/1'700'000'000);
    ASSERT_TRUE(database.ok()) << database.status().message();

    catalog::Schema schema = TwoColumnSchema();
    auto oid = database.value().catalog.CreateTable(catalog::kNamespacePublic, "seq", schema,
                                                    catalog::ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    CapturedLog log;
    database.value().catalog.SetLogger(log.get());
    auto id = database.value().catalog.AllocateRowId(oid.value());
    ASSERT_TRUE(id.ok()) << id.status().message();

    // Invariant 10: the id *is* the tuple's identity, so which id a row
    // got is a question asked about every insert that later looks wrong.
    EXPECT_TRUE(log.Has("catalog", {"issued row id", std::to_string(id.value())}));
}

// ---- Scheduler: the top of the stack, with no caller to report to -------

TEST(SubsystemLoggingTest, IoBackendFailureIsLoggedOnceUntilItRecovers) {
    // A backend that fails on demand. The reactor cannot return a Status
    // from Run(), so this is the only place such a failure can surface.
    class FlakyBackend final : public sched::IoBackend {
    public:
        Status PollReady(int, std::vector<sched::IoEvent>& out) override {
            out.clear();
            ++polls;
            if (fail) return Status::IoError("scripted poll failure");
            return Status::OK();
        }
        Status Register(sched::IoHandle, sched::IoInterest) override { return Status::OK(); }
        Status Modify(sched::IoHandle, sched::IoInterest) override { return Status::OK(); }
        Status Unregister(sched::IoHandle) override { return Status::OK(); }

        bool fail = false;
        int polls = 0;
    };

    sched::ManualClock clock;
    FlakyBackend backend;
    sched::Scheduler scheduler(clock, backend);
    CapturedLog log(LogLevel::kDebug);
    scheduler.SetLogger(log.get());

    backend.fail = true;
    scheduler.RunOnce();
    scheduler.RunOnce();
    scheduler.RunOnce();
    // A permanently broken backend fails every iteration; one line per
    // spin would bury everything else, so only the transition is reported.
    EXPECT_EQ(log.Matching("sched", {"poll failed"}).size(), 1u);

    backend.fail = false;
    scheduler.RunOnce();
    EXPECT_TRUE(log.Has("sched", {"recovered", "3 failed poll"}));
}

}  // namespace
}  // namespace kds
