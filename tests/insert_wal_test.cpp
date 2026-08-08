#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/clock.hpp"
#include "kds/server/command_dispatcher.hpp"

#include <algorithm>
#include <cstring>

#include "kds/storage/index/index_page.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/varheap.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"

// INSERT as a *logged* statement (command_dispatcher.hpp's WAL section,
// wal.md sections 1, 5.2, 8-1). Three questions, and nothing else here:
//
//   1. Are the right records on the platter, describing the tuple that was
//      actually written? A record set that does not name the same page,
//      slot and bytes the heap holds is worse than no log at all.
//   2. Is the page ordered behind them - page_lsn stamped, recLSN captured,
//      and the store's gate refusing to write ahead of the log?
//   3. Does the durability class decide *when*, as manager.hpp promises,
//      rather than being ignored once a dispatcher is between the caller
//      and the manager?
//
// The log device is a MemoryLogDevice throughout, so "durable" means
// "survives Crash()" and can be asserted rather than argued about.

namespace kds::server {
namespace {

constexpr std::uint64_t kSegmentSize = 1 << 20;  // 1 MiB: no segment rollover here

class InsertWalTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto page_device = storage::MemoryPageDevice::Create(/*extent_pages=*/64,
                                                             /*initial_pages=*/64);
        ASSERT_TRUE(page_device.ok()) << page_device.status().message();
        device_ = std::move(page_device.value());

        auto log_device = wal::MemoryLogDevice::Create(kSegmentSize);
        ASSERT_TRUE(log_device.ok()) << log_device.status().message();
        log_device_ = std::move(log_device.value());

        auto wal = wal::WalManager::Open(log_device_.get(), clock_, /*core_id=*/0);
        ASSERT_TRUE(wal.ok()) << wal.status().message();
        wal_ = std::move(wal.value());

        auto store = storage::DevicePageStore::Open(*device_, kFirstUserPageId);
        ASSERT_TRUE(store.ok()) << store.status().message();
        store_ = std::move(store.value());
        store_->SetWalGate(wal_.get());

        auto boot = bootstrap::BootstrapDatabase(*store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
    }

    // A dispatcher wired to the WAL, at the class under test.
    CommandDispatcher Dispatcher(wal::DurabilityClass durability) {
        return CommandDispatcher(boot_->superblock, boot_->catalog, *store_, /*log=*/nullptr,
                                 /*clock=*/nullptr, wal_.get(), durability);
    }

    // Every record the *device* holds, in stream order. Deliberately read
    // back through the device rather than asked of the manager: what the
    // manager believes it appended is not evidence of what a crash leaves.
    std::vector<wal::DecodedRecord> DeviceRecords(std::vector<std::vector<std::byte>>& storage) {
        std::vector<wal::DecodedRecord> found;
        for (std::uint64_t seg = 0; seg < log_device_->segment_count(); ++seg) {
            storage.emplace_back(kSegmentSize - wal::kSegmentHeaderSize);
            std::vector<std::byte>& body = storage.back();
            EXPECT_TRUE(log_device_->ReadAt(seg, wal::kSegmentHeaderSize, body).ok());
            wal::RecordReader reader(body, seg * kSegmentSize + wal::kSegmentHeaderSize);
            while (std::optional<wal::DecodedRecord> record = reader.Next()) {
                if (record->type() == wal::RecordType::kPad) break;
                found.push_back(*record);
            }
        }
        return found;
    }

    std::vector<wal::RecordType> RecordTypes() {
        std::vector<std::vector<std::byte>> storage;
        std::vector<wal::RecordType> types;
        for (const wal::DecodedRecord& record : DeviceRecords(storage)) {
            types.push_back(record.type());
        }
        return types;
    }

    static std::size_t CountOf(const std::vector<wal::RecordType>& types, wal::RecordType want) {
        std::size_t n = 0;
        for (const wal::RecordType type : types) {
            if (type == want) ++n;
        }
        return n;
    }

    std::uint64_t PageLsnOf(PageId page_id) {
        auto page = store_->Get(page_id);
        EXPECT_TRUE(page.ok()) << page.status().message();
        return storage::GetPageLsn(page.value());
    }

    sched::ManualClock clock_;
    std::unique_ptr<storage::MemoryPageDevice> device_;
    std::unique_ptr<wal::MemoryLogDevice> log_device_;
    std::unique_ptr<wal::WalManager> wal_;
    std::unique_ptr<storage::DevicePageStore> store_;
    std::optional<bootstrap::BootstrapResult> boot_;
};

// ---- 1. The records describe the tuple that was written -----------------

TEST_F(InsertWalTest, InsertEmitsBeginHeapInsertAndCommit) {
    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kStrict);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, v int32)").response.substr(0, 7), "CREATED");

    const std::size_t before = RecordTypes().size();
    EXPECT_EQ(d.Dispatch("INSERT INTO t VALUES (7)").response.substr(0, 8), "INSERTED");

    std::vector<wal::RecordType> types = RecordTypes();
    ASSERT_EQ(types.size(), before + 3) << "one insert is BEGIN + HEAP_INSERT + COMMIT";
    EXPECT_EQ(types[before + 0], wal::RecordType::kTxnBegin);
    EXPECT_EQ(types[before + 1], wal::RecordType::kHeapInsert);
    EXPECT_EQ(types[before + 2], wal::RecordType::kTxnCommit);
}

TEST_F(InsertWalTest, TheLoggedTupleIsByteIdenticalToTheOneInThePage) {
    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kStrict);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, v int32)").response.substr(0, 7), "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (4242)").response.substr(0, 8), "INSERTED");

    std::vector<std::vector<std::byte>> storage;
    std::vector<wal::DecodedRecord> records = DeviceRecords(storage);

    const wal::DecodedRecord* insert = nullptr;
    for (const wal::DecodedRecord& record : records) {
        if (record.type() == wal::RecordType::kHeapInsert) insert = &record;
    }
    ASSERT_NE(insert, nullptr);

    auto decoded = wal::DecodeHeapWrite(insert->payload);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();

    // The record's page/slot must resolve to the same bytes in the heap.
    auto page = store_->Get(insert->header.page_id);
    ASSERT_TRUE(page.ok()) << page.status().message();
    heap::PageView view(page.value());
    auto tuple = view.ReadTuple(decoded.value().fields.slot);
    ASSERT_TRUE(tuple.ok()) << tuple.status().message();

    ASSERT_EQ(decoded.value().tuple.size(), tuple.value().payload.size());
    EXPECT_EQ(std::memcmp(decoded.value().tuple.data(), tuple.value().payload.data(),
                          tuple.value().payload.size()),
              0);
    // An insert supersedes no version, so its undo chain ends at itself.
    EXPECT_EQ(decoded.value().fields.undo_ptr, 0u);
}

TEST_F(InsertWalTest, NoWalManagerMeansNoRecordsAndTheInsertStillWorks) {
    // The unlogged shape, which the socket-free tests still use.
    CommandDispatcher d(boot_->superblock, boot_->catalog, *store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, v int32)").response.substr(0, 7), "CREATED");

    const std::size_t before = RecordTypes().size();
    EXPECT_EQ(d.Dispatch("INSERT INTO t VALUES (1)").response.substr(0, 8), "INSERTED");
    EXPECT_EQ(RecordTypes().size(), before);
}

// ---- Chain growth logs both pages it touched ----------------------------

// A row wide enough that a page fills in a countable number of inserts.
//
// Under the fixed-length rule (invariant 13) a row's width comes from its
// *schema*, never from the values in it: every varchar occupies one tagged
// cell of kds.inline_cell_width bytes whatever it holds. So a wide row is a
// row with many columns. These tests used to insert one 500-byte varchar,
// which is now refused - a value that long belongs in the var-heap, which
// is specified but not built (docs/rule-fixed-length-tuple.md section 5).
//
// 20 cells of the default 64 bytes plus the Keystone word is a 1288-byte
// row, so roughly six fit a page and growth happens well inside the loops
// below.
constexpr int kWideColumns = 20;

std::string WideCreateTable() {
    std::string sql = "CREATE TABLE t (id int64";
    for (int i = 0; i < kWideColumns; ++i) sql += ", v" + std::to_string(i) + " varchar";
    return sql + ")";
}

std::string WideInsert() {
    std::string sql = "INSERT INTO t VALUES (";
    for (int i = 0; i < kWideColumns; ++i) sql += (i == 0 ? "'x'" : ", 'x'");
    return sql + ")";
}

TEST_F(InsertWalTest, ChainGrowthLogsTheNewPageAndTheLinkThatReachesIt) {
    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kStrict);
    // A wide row so the page fills in a countable number of inserts rather
    // than thousands - see WideCreateTable().
    ASSERT_EQ(d.Dispatch(WideCreateTable()).response.substr(0, 7), "CREATED");

    PageId grew_into = kInvalidPageId;
    for (int i = 0; i < 40 && grew_into == kInvalidPageId; ++i) {
        const std::string reply = d.Dispatch(WideInsert()).response;
        ASSERT_EQ(reply.substr(0, 8), "INSERTED") << reply;

        std::vector<wal::RecordType> types = RecordTypes();
        if (CountOf(types, wal::RecordType::kPageInit) > 0) {
            // The growth insert logged four records, not two: the old
            // tail's image (carrying the link) and the new page's init,
            // between BEGIN and the tuple itself.
            EXPECT_EQ(CountOf(types, wal::RecordType::kFullPageImage), 1u);
            EXPECT_EQ(CountOf(types, wal::RecordType::kPageInit), 1u);
            grew_into = 1;  // marker: growth observed
        }
    }
    ASSERT_NE(grew_into, kInvalidPageId) << "the chain never grew; the page never filled";
}

TEST_F(InsertWalTest, ThePageInitRecordCarriesTheNewPagesMinKey) {
    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kStrict);
    ASSERT_EQ(d.Dispatch(WideCreateTable()).response.substr(0, 7), "CREATED");

    for (int i = 0; i < 40; ++i) {
        ASSERT_EQ(d.Dispatch(WideInsert()).response.substr(0, 8), "INSERTED");
    }

    std::vector<std::vector<std::byte>> storage;
    const wal::DecodedRecord* init = nullptr;
    std::vector<wal::DecodedRecord> records = DeviceRecords(storage);
    for (const wal::DecodedRecord& record : records) {
        if (record.type() == wal::RecordType::kPageInit) init = &record;
    }
    ASSERT_NE(init, nullptr) << "the chain never grew";

    auto decoded = wal::DecodePageInit(init->payload);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().page_type, static_cast<std::uint8_t>(PageType::kHeap));

    // min_key is the id of the tuple that caused the growth, and it must
    // match what the page itself ended up holding (invariant 2).
    auto page = store_->Get(init->header.page_id);
    ASSERT_TRUE(page.ok()) << page.status().message();
    EXPECT_EQ(decoded.value().min_key, heap::PageView(page.value()).min_key());
}

// ---- A spilled value is logged, and logged first -------------------------

TEST_F(InsertWalTest, ASpilledValueIsLoggedBeforeTheTupleThatPointsAtIt) {
    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kStrict);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, s varchar)").response.substr(0, 7), "CREATED");

    // Long enough to spill: the cell holds a pointer, the bytes go to the
    // var-heap (docs/rule-fixed-length-tuple.md section 5).
    const std::string spilled(500, 's');
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES ('" + spilled + "')").response.substr(0, 8),
              "INSERTED");

    std::vector<wal::RecordType> types = RecordTypes();
    auto index_of = [&](wal::RecordType type) -> std::size_t {
        for (std::size_t i = 0; i < types.size(); ++i) {
            if (types[i] == type) return i;
        }
        return types.size();
    };

    const std::size_t vh = index_of(wal::RecordType::kVarHeapAppend);
    const std::size_t insert = index_of(wal::RecordType::kHeapInsert);
    ASSERT_LT(vh, types.size()) << "the value spilled but no VARHEAP_APPEND was logged";
    ASSERT_LT(insert, types.size());

    // The ordering *is* the recovery story: a replay must never reach a
    // cell whose pointer resolves to nothing. The reverse - a value with no
    // tuple - is an unreferenced value purge collects, which is why this
    // direction is the one that is asserted.
    EXPECT_LT(vh, insert) << "VARHEAP_APPEND must precede the HEAP_INSERT pointing at it";
}

TEST_F(InsertWalTest, AnInlineValueLogsNoVarHeapRecord) {
    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kStrict);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, s varchar)").response.substr(0, 7), "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES ('short')").response.substr(0, 8), "INSERTED");

    EXPECT_EQ(CountOf(RecordTypes(), wal::RecordType::kVarHeapAppend), 0u);
}

TEST_F(InsertWalTest, TheLoggedVarHeapValueIsByteIdenticalToTheStoredOne) {
    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kStrict);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, s varchar)").response.substr(0, 7), "CREATED");

    const std::string spilled(700, 'w');
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES ('" + spilled + "')").response.substr(0, 8),
              "INSERTED");

    std::vector<std::vector<std::byte>> storage;
    const wal::DecodedRecord* found = nullptr;
    std::vector<wal::DecodedRecord> records = DeviceRecords(storage);
    for (const wal::DecodedRecord& record : records) {
        if (record.type() == wal::RecordType::kVarHeapAppend) found = &record;
    }
    ASSERT_NE(found, nullptr);

    auto decoded = wal::DecodeVarHeapAppend(found->payload);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    ASSERT_EQ(decoded.value().value.size(), spilled.size());

    // A record set that does not carry the same bytes the page holds is
    // worse than no log at all.
    auto page = store_->Get(found->header.page_id);
    ASSERT_TRUE(page.ok()) << page.status().message();
    auto stored = varheap::PageRead(page.value(), decoded.value().fields.slot);
    ASSERT_TRUE(stored.ok()) << stored.status().message();
    EXPECT_TRUE(std::equal(stored.value().begin(), stored.value().end(),
                           decoded.value().value.begin()));
}

// ---- 2. The page is ordered behind the records --------------------------

TEST_F(InsertWalTest, TheInsertedPageCarriesTheHeapInsertsLsn) {
    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kStrict);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, v int32)").response.substr(0, 7), "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (9)").response.substr(0, 8), "INSERTED");

    std::vector<std::vector<std::byte>> storage;
    std::vector<wal::DecodedRecord> records = DeviceRecords(storage);
    const wal::DecodedRecord* insert = nullptr;
    for (const wal::DecodedRecord& record : records) {
        if (record.type() == wal::RecordType::kHeapInsert) insert = &record;
    }
    ASSERT_NE(insert, nullptr);

    EXPECT_EQ(PageLsnOf(insert->header.page_id), insert->header.lsn)
        << "the page must name the record that last described it";
}

TEST_F(InsertWalTest, TheDirtyTableReportsTheFirstRecordToDirtyAPageNotTheLast) {
    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kStrict);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, v int32)").response.substr(0, 7), "CREATED");
    ASSERT_TRUE(store_->Sync().ok());  // everything clean, so recLSNs start empty

    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (1)").response.substr(0, 8), "INSERTED");
    const std::uint64_t after_first = wal_->appended_lsn();
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (2)").response.substr(0, 8), "INSERTED");

    // The heap page took two inserts. recLSN is the *oldest* record redo
    // must replay to make it whole, so it must sit before the second one.
    bool checked = false;
    for (const auto& [page_id, rec_lsn] : store_->DirtyPagesWithRecLsn()) {
        if (rec_lsn == 0) continue;  // catalog pages: dirtied outside the log
        EXPECT_LT(rec_lsn, after_first)
            << "page " << page_id << " adopted a later record as its recLSN";
        EXPECT_LT(rec_lsn, PageLsnOf(page_id)) << "recLSN must trail page_lsn after two writes";
        checked = true;
    }
    EXPECT_TRUE(checked) << "no logged page was dirty";
}

TEST_F(InsertWalTest, FlushingAPageSyncsTheLogThatDescribesItFirst) {
    // Relaxed: nothing syncs at commit, so the log is behind on purpose
    // and the flush is the only thing that can catch it up.
    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kRelaxed);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, v int32)").response.substr(0, 7), "CREATED");
    ASSERT_TRUE(wal_->SyncAll().ok());

    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (5)").response.substr(0, 8), "INSERTED");
    const std::uint64_t syncs_before = wal_->stats().syncs;
    ASSERT_LT(wal_->durable_lsn(), wal_->appended_lsn()) << "relaxed left the log behind";

    ASSERT_TRUE(store_->Flush().ok());

    EXPECT_GT(wal_->stats().syncs, syncs_before)
        << "the gate must sync the log before the page goes out";
    EXPECT_GE(wal_->durable_lsn(), wal_->appended_lsn());
}

TEST_F(InsertWalTest, AFlushIsRefusedWhenTheLogCannotBeMadeDurable) {
    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kRelaxed);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, v int32)").response.substr(0, 7), "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (5)").response.substr(0, 8), "INSERTED");

    log_device_->FailNextSync(Status::IoError("device is on fire"));

    Status flushed = store_->Flush();
    EXPECT_FALSE(flushed.ok()) << "a page must not be written when its log cannot be";
    EXPECT_EQ(flushed.code(), StatusCode::kIoError);
}

TEST_F(InsertWalTest, AStoreWithNoGateFlushesExactlyAsItAlwaysDid) {
    store_->SetWalGate(nullptr);
    CommandDispatcher d(boot_->superblock, boot_->catalog, *store_);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, v int32)").response.substr(0, 7), "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (5)").response.substr(0, 8), "INSERTED");

    const std::uint64_t syncs_before = wal_->stats().syncs;
    EXPECT_TRUE(store_->Flush().ok());
    EXPECT_EQ(wal_->stats().syncs, syncs_before);
}

// ---- 3. The durability class decides when -------------------------------

TEST_F(InsertWalTest, StrictInsertIsDurableBeforeTheReplyIsProduced) {
    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kStrict);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, v int32)").response.substr(0, 7), "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (11)").response.substr(0, 8), "INSERTED");

    log_device_->Crash();  // revert to the last Sync()
    std::vector<wal::RecordType> types = RecordTypes();
    EXPECT_EQ(CountOf(types, wal::RecordType::kHeapInsert), 1u)
        << "a strict insert that survived the reply must survive the crash";
    EXPECT_EQ(CountOf(types, wal::RecordType::kTxnCommit), 1u);
}

TEST_F(InsertWalTest, GroupInsertIsAlsoDurableOnReturnBecauseTheDispatcherWaits) {
    // Same durability point as strict (manager.hpp); only the batching
    // differs, and with one caller there is no batch to form.
    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kGroup);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, v int32)").response.substr(0, 7), "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (12)").response.substr(0, 8), "INSERTED");

    log_device_->Crash();
    EXPECT_EQ(CountOf(RecordTypes(), wal::RecordType::kTxnCommit), 1u);
    EXPECT_FALSE(wal_->HasPendingGroupCommits());
}

TEST_F(InsertWalTest, RelaxedInsertReturnsWithoutSyncingAndIsLostToACrash) {
    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kRelaxed);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, v int32)").response.substr(0, 7), "CREATED");
    const std::uint64_t syncs_before = wal_->stats().syncs;

    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (13)").response.substr(0, 8), "INSERTED");
    EXPECT_EQ(wal_->stats().syncs, syncs_before) << "relaxed must not wait on the device";

    log_device_->Crash();
    EXPECT_EQ(CountOf(RecordTypes(), wal::RecordType::kHeapInsert), 0u)
        << "that is the loss window relaxed buys its latency with";
}

TEST_F(InsertWalTest, RelaxedBecomesDurableOnTheDrainInterval) {
    wal::WalManagerConfig config;
    config.relaxed_flush_interval_ns = 1'000'000;  // 1 ms
    auto wal = wal::WalManager::Open(log_device_.get(), clock_, /*core_id=*/0, config);
    ASSERT_TRUE(wal.ok()) << wal.status().message();
    wal_ = std::move(wal.value());
    store_->SetWalGate(wal_.get());

    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kRelaxed);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, v int32)").response.substr(0, 7), "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (14)").response.substr(0, 8), "INSERTED");

    ASSERT_TRUE(wal_->DrainOnce().ok());  // interval not elapsed: still nothing
    EXPECT_EQ(CountOf(RecordTypes(), wal::RecordType::kHeapInsert), 0u);

    clock_.Advance(2'000'000);
    ASSERT_TRUE(wal_->DrainOnce().ok());
    EXPECT_EQ(CountOf(RecordTypes(), wal::RecordType::kHeapInsert), 1u);
}

TEST_F(InsertWalTest, ClientSyncMakesARelaxedInsertDurableWithoutWaitingForTheInterval) {
    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kRelaxed);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, v int32)").response.substr(0, 7), "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (15)").response.substr(0, 8), "INSERTED");

    EXPECT_EQ(d.Dispatch("SYNC").response, "OK synced");

    log_device_->Crash();
    EXPECT_EQ(CountOf(RecordTypes(), wal::RecordType::kHeapInsert), 1u);
}

// ---- The config spelling ------------------------------------------------

TEST(DurabilityClassNames, ParseAcceptsBothSpellingsAndRejectsTheRest) {
    EXPECT_EQ(wal::ParseDurabilityClass("strict").value(), wal::DurabilityClass::kStrict);
    EXPECT_EQ(wal::ParseDurabilityClass("D1").value(), wal::DurabilityClass::kStrict);
    EXPECT_EQ(wal::ParseDurabilityClass("Group").value(), wal::DurabilityClass::kGroup);
    EXPECT_EQ(wal::ParseDurabilityClass("d2").value(), wal::DurabilityClass::kGroup);
    EXPECT_EQ(wal::ParseDurabilityClass("RELAXED").value(), wal::DurabilityClass::kRelaxed);
    EXPECT_EQ(wal::ParseDurabilityClass("d3").value(), wal::DurabilityClass::kRelaxed);

    auto bad = wal::ParseDurabilityClass("eventually");
    EXPECT_FALSE(bad.ok());
    EXPECT_EQ(bad.status().code(), StatusCode::kInvalidArgument);
}

TEST(DurabilityClassNames, EveryClassRoundTripsThroughItsName) {
    for (const wal::DurabilityClass c : {wal::DurabilityClass::kStrict,
                                         wal::DurabilityClass::kGroup,
                                         wal::DurabilityClass::kRelaxed}) {
        EXPECT_EQ(wal::ParseDurabilityClass(wal::DurabilityClassName(c)).value(), c);
    }
}


// ---- INDEX_INSERT (docs/feat-index.md §12.1, workplan IX08) --------------

TEST_F(InsertWalTest, AnIndexEntryIsLoggedBeforeTheRowItPointsAt) {
    // The direction is forced, not stylistic: if the index record is durable
    // and the row's is not, redo produces a dangling entry that verification
    // drops on sight. The reverse produces a row no probe can find.
    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kStrict);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, owner int64) BTREE").response.substr(0, 3),
              "CRE");
    ASSERT_NE(d.Dispatch("CREATE INDEX ix ON t (owner)").response.find("CREATED"),
              std::string::npos);
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (42)").response.substr(0, 3), "INS");

    const std::vector<wal::RecordType> types = RecordTypes();
    ASSERT_EQ(CountOf(types, wal::RecordType::kIndexInsert), 1u);
    ASSERT_EQ(CountOf(types, wal::RecordType::kHeapInsert), 1u);

    const auto index_at = std::find(types.begin(), types.end(), wal::RecordType::kIndexInsert);
    const auto heap_at = std::find(types.begin(), types.end(), wal::RecordType::kHeapInsert);
    EXPECT_LT(index_at - types.begin(), heap_at - types.begin())
        << "INDEX_INSERT must precede the HEAP_INSERT it points at";
}

TEST_F(InsertWalTest, AnIndexEntrysRecordCarriesTheBytesThatLandedOnThePage) {
    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kStrict);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, owner int64) BTREE").response.substr(0, 3),
              "CRE");
    ASSERT_NE(d.Dispatch("CREATE INDEX ix ON t (owner)").response.find("CREATED"),
              std::string::npos);
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (42)").response.substr(0, 3), "INS");

    std::vector<std::vector<std::byte>> storage;
    for (const wal::DecodedRecord& record : DeviceRecords(storage)) {
        if (record.type() != wal::RecordType::kIndexInsert) continue;

        auto decoded = wal::DecodeIndexInsert(record.payload);
        ASSERT_TRUE(decoded.ok()) << decoded.status().message();

        // The record names the leaf, and the leaf's own header carries the
        // widths - so what is logged can be checked against what is stored
        // with nothing else in hand.
        auto page = store_->Get(record.header.page_id);
        ASSERT_TRUE(page.ok());
        index::IndexLeafView leaf(page.value());
        auto stored = leaf.Entry(decoded.value().fields.slot);
        ASSERT_TRUE(stored.ok()) << stored.status().message();
        ASSERT_EQ(stored.value().size(), decoded.value().entry.size());
        EXPECT_EQ(std::memcmp(stored.value().data(), decoded.value().entry.data(),
                              stored.value().size()),
                  0);
        return;
    }
    FAIL() << "no INDEX_INSERT record was written";
}

TEST_F(InsertWalTest, ARelationWithNoIndexLogsNoIndexRecords) {
    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kStrict);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, owner int64) BTREE").response.substr(0, 3),
              "CRE");
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (42)").response.substr(0, 3), "INS");
    EXPECT_EQ(CountOf(RecordTypes(), wal::RecordType::kIndexInsert), 0u);
}

TEST_F(InsertWalTest, ASplitTakesFullPageImagesAndNoIndexInsert) {
    // The images are taken after the entry is in, so emitting an
    // INDEX_INSERT as well would apply it twice. One rule, and this is what
    // pins it: the count of INDEX_INSERT records is the count of appends
    // that split nothing.
    //
    // Relaxed durability with one flush at the end rather than kStrict: the
    // records only have to reach the device once, and syncing per row turned
    // this into an 18-second test.
    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kRelaxed);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, owner varchar) BTREE").response.substr(0, 3),
              "CRE");
    ASSERT_NE(d.Dispatch("CREATE INDEX ix ON t (owner)").response.find("CREATED"),
              std::string::npos);

    // A varchar key spends kIndexStringKeyBytes + 1 on the key and 8 on the
    // pk, so a leaf holds 8144 / 41 = 198 entries - enough inserts here to
    // divide one without paying for thousands of rows.
    constexpr int kRows = 400;
    for (int i = 0; i < kRows; ++i) {
        ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES ('v" + std::to_string((i * 7919) % kRows) +
                             "')")
                      .response.substr(0, 3),
                  "INS")
            << i;
    }
    ASSERT_TRUE(wal_->Flush().ok());

    const std::vector<wal::RecordType> types = RecordTypes();
    EXPECT_EQ(CountOf(types, wal::RecordType::kHeapInsert), static_cast<std::size_t>(kRows));
    // Fewer than one per row, because the appends that split took images
    // instead - and at least one image was taken.
    EXPECT_LT(CountOf(types, wal::RecordType::kIndexInsert), static_cast<std::size_t>(kRows));
    EXPECT_GT(CountOf(types, wal::RecordType::kFullPageImage), 0u);
}

}  // namespace
}  // namespace kds::server
