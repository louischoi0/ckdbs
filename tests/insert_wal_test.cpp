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

    // The same, with a TransactionManager behind it - which UPDATE needs
    // before it logs anything at all: its records are written under
    // `scope.txn`, so a dispatcher with no manager leaves an UPDATE unlogged
    // (the state command_dispatcher.cpp's HEAP_OVERWRITE comment describes).
    // INSERT does not need one, which is why the fixture's default has none.
    CommandDispatcher TransactionalDispatcher(wal::DurabilityClass durability) {
        trx_ids_.emplace(boot_->superblock);
        undo_log_.emplace(*store_, wal_.get());
        txn_manager_.emplace(*trx_ids_, *undo_log_, *store_, wal_.get());
        return CommandDispatcher(boot_->superblock, boot_->catalog, *store_, /*log=*/nullptr,
                                 /*clock=*/nullptr, wal_.get(), durability, exec::Budget(),
                                 /*recorder=*/nullptr, /*replay_enabled=*/false,
                                 /*access_statistics=*/true, /*cabins=*/nullptr,
                                 &*txn_manager_);
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
        return storage::GetPageLsn(page.value().bytes());
    }

    sched::ManualClock clock_;
    std::unique_ptr<storage::MemoryPageDevice> device_;
    std::unique_ptr<wal::MemoryLogDevice> log_device_;
    std::unique_ptr<wal::WalManager> wal_;
    std::unique_ptr<storage::DevicePageStore> store_;
    std::optional<bootstrap::BootstrapResult> boot_;

    // Only TransactionalDispatcher() fills these, and they are declared after
    // everything they reference so teardown is the reverse of construction.
    std::optional<txn::TrxIdSequence> trx_ids_;
    std::optional<txn::UndoLog> undo_log_;
    std::optional<txn::TransactionManager> txn_manager_;
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
    heap::PageView view(page.value().bytes());
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
// is specified but not built (docs/rules/rule-fixed-length-tuple.md section 5).
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

TEST_F(InsertWalTest, ALeafDivisionLogsBothPagesAsImagesAndNoPageInit) {
    // A caller-supplied key that sorts inside a full leaf divides it
    // (docs/spec/heap-and-tuple.md §4.1), which moves versions onto a page no
    // other record describes.
    //
    // `is_new_page` means "a PAGE_INIT is enough, because the HEAP_INSERT
    // after it describes the only tuple on this page" - not "this page is
    // new". A division satisfies neither half: the new leaf receives the
    // moved upper half, and the incoming tuple may land in the *rebuilt old*
    // leaf instead. Logging the new leaf as an init would replay it empty
    // and lose every version the division moved.
    //
    // This test exists because nothing reads the log back yet
    // (docs/inflight/known-gaps.md), so a wrong record set is otherwise invisible
    // until recovery is built and the data is already gone.
    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kStrict);
    std::string sql = "CREATE TABLE t (id int64";
    for (int i = 0; i < kWideColumns; ++i) sql += ", v" + std::to_string(i) + " varchar";
    ASSERT_EQ(d.Dispatch(sql + ") BTREE EXPLICIT").response.substr(0, 7), "CREATED");

    auto insert = [&](int id) {
        std::string s = "INSERT INTO t VALUES (" + std::to_string(id);
        for (int i = 0; i < kWideColumns; ++i) s += ", 'x'";
        return d.Dispatch(s + ")").response;
    };

    // Ascending with gaps until the tree grows, so a leaf is known full.
    int last = 0;
    for (int k = 1; k <= 60; ++k) {
        const std::string reply = insert(k * 10);
        ASSERT_EQ(reply.substr(0, 8), "INSERTED") << reply;
        last = k * 10;
        if (CountOf(RecordTypes(), wal::RecordType::kFullPageImage) > 0) break;
    }
    ASSERT_GT(last, 0);

    // Deltas, not totals: the stream already holds the inits and images of
    // every append-split above, and only this statement's records are the
    // subject.
    std::vector<wal::RecordType> before = RecordTypes();
    const std::size_t inits_before = CountOf(before, wal::RecordType::kPageInit);
    const std::size_t images_before = CountOf(before, wal::RecordType::kFullPageImage);

    // Now land in the first gap, which routes back into a full leaf.
    const std::string divided = insert(15);
    ASSERT_EQ(divided.substr(0, 8), "INSERTED") << divided;

    std::vector<wal::RecordType> after = RecordTypes();
    EXPECT_EQ(CountOf(after, wal::RecordType::kPageInit) - inits_before, 0u)
        << "a divided leaf's contents are not describable by a PAGE_INIT plus one insert";
    EXPECT_GE(CountOf(after, wal::RecordType::kFullPageImage) - images_before, 2u)
        << "both halves of a division have to be logged whole";
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
    EXPECT_EQ(decoded.value().min_key, heap::PageView(page.value().bytes()).min_key());
}

// ---- A spilled value is logged, and logged first -------------------------

TEST_F(InsertWalTest, ASpilledValueIsLoggedBeforeTheTupleThatPointsAtIt) {
    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kStrict);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, s varchar)").response.substr(0, 7), "CREATED");

    // Long enough to spill: the cell holds a pointer, the bytes go to the
    // var-heap (docs/rules/rule-fixed-length-tuple.md section 5).
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

TEST_F(InsertWalTest, GrowingTheVarHeapChainLogsTheNewPageAndTheLinkThatReachesIt) {
    // The hole recovery walked into. `varheap::ChainAppend` grows a chain
    // through the store's plain allocation path, and a VARHEAP_APPEND says
    // nothing about a page being new - so a crash that lost the new page's
    // write-back left a durable append naming a page no record creates, and
    // redo refused the mount:
    //
    //   VARHEAP_APPEND at lsn 700128 names page 172, which the store does not
    //   hold and no PAGE_INIT or full page image in the replay range creates
    //
    // Reproduced before the fix at `ckdbs-sim --seed 7 --ops 3000 --mode
    // crash --iterations 3`; asserted here as records rather than as a seed.
    CommandDispatcher d = Dispatcher(wal::DurabilityClass::kStrict);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, s varchar)").response.substr(0, 7), "CREATED");

    // Fill the root page and then some: each value is a fifth of a page, so
    // the chain must grow inside this loop.
    const std::string spilled(1600, 'g');
    for (int i = 0; i < 12; ++i) {
        ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES ('" + spilled + "')").response.substr(0, 8),
                  "INSERTED");
    }

    std::vector<std::vector<std::byte>> storage;
    std::vector<wal::DecodedRecord> records = DeviceRecords(storage);

    // A PAGE_INIT whose payload says kVarHeap - the record that was missing.
    // `wal::ApplyPageInit` formats that class already, so what was absent was
    // the record and never the applier.
    std::size_t varheap_inits = 0;
    for (const wal::DecodedRecord& record : records) {
        if (record.type() != wal::RecordType::kPageInit) continue;
        auto fields = wal::DecodePageInit(record.payload);
        ASSERT_TRUE(fields.ok()) << fields.status().message();
        if (static_cast<PageType>(fields.value().page_type) == PageType::kVarHeap) {
            ++varheap_inits;
        }
    }
    EXPECT_GT(varheap_inits, 0u) << "the chain grew and no var-heap PAGE_INIT was logged";

    // And the link edit that makes the new page reachable, as a full page
    // image of the *old* tail - no record type describes a next-page link, so
    // this is the same answer the heap path gives. Without it a replay leaves
    // a value page that exists and no chain walk reaches.
    //
    // Found by walking the chain: every page but the last is a tail whose link
    // was edited, so each must carry an image.
    auto oid = boot_->catalog.FindTableOidByName("t");
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    auto access = boot_->catalog.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok()) << access.status().message();
    PageId page_id = access.value()->varheap_page_id;
    ASSERT_NE(page_id, kInvalidPageId);

    std::vector<PageId> imaged;
    for (const wal::DecodedRecord& record : records) {
        if (record.type() == wal::RecordType::kFullPageImage) imaged.push_back(record.header.page_id);
    }

    std::size_t links = 0;
    while (true) {
        auto page = store_->GetForRead(page_id);
        ASSERT_TRUE(page.ok()) << page.status().message();
        const PageId next = varheap::PageNextPageId(page.value().bytes());
        if (next == kInvalidPageId) break;
        ++links;
        EXPECT_NE(std::find(imaged.begin(), imaged.end(), page_id), imaged.end())
            << "page " << page_id << " links to " << next << " and no image describes that link";
        page_id = next;
    }
    EXPECT_GT(links, 0u) << "the chain never grew; the test proves nothing";
}

TEST_F(InsertWalTest, AnUpdateThatSpillsLogsTheValueItSpilled) {
    // The third hole, and the quietest: the UPDATE path built its VarHeapSink
    // with no collector, so a value an UPDATE spilled was written into a
    // var-heap page and **never logged at all**. Recovery then restored a
    // tuple whose cell pointed at bytes no record described.
    CommandDispatcher d = TransactionalDispatcher(wal::DurabilityClass::kStrict);
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, s varchar)").response.substr(0, 7), "CREATED");
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES ('short')").response.substr(0, 8), "INSERTED");
    // The insert stayed inline, so every var-heap record after this point
    // belongs to the UPDATE - which is what makes the count below an
    // assertion about the UPDATE path rather than about both.
    ASSERT_EQ(CountOf(RecordTypes(), wal::RecordType::kVarHeapAppend), 0u);

    const std::string spilled(900, 'u');
    ASSERT_EQ(d.Dispatch("UPDATE t SET s = '" + spilled + "' WHERE id = 1").response.substr(0, 7),
              "UPDATED");

    std::vector<wal::RecordType> types = RecordTypes();
    const std::size_t vh = CountOf(types, wal::RecordType::kVarHeapAppend);
    EXPECT_EQ(vh, 1u) << "the UPDATE spilled and logged no VARHEAP_APPEND";

    // Same ordering rule the INSERT path obeys: the value precedes the tuple
    // record whose cell points at it.
    auto index_of = [&](wal::RecordType type) -> std::size_t {
        for (std::size_t i = 0; i < types.size(); ++i) {
            if (types[i] == type) return i;
        }
        return types.size();
    };
    const std::size_t append = index_of(wal::RecordType::kVarHeapAppend);
    const std::size_t overwrite = index_of(wal::RecordType::kHeapOverwrite);
    ASSERT_LT(append, types.size());
    ASSERT_LT(overwrite, types.size());
    EXPECT_LT(append, overwrite) << "VARHEAP_APPEND must precede the HEAP_OVERWRITE";
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
    auto stored = varheap::PageRead(page.value().bytes(), decoded.value().fields.slot);
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


// ---- INDEX_INSERT (docs/spec/index.md §12.1, workplan IX08) --------------

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
        index::IndexLeafView leaf(page.value().bytes());
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

// ---- 4. Nothing logs a catalog page, not even a rollback ----------------

// A transactional DDL registers its catalog rows on the trail so `ROLLBACK`
// can undo them (workplan-ddl-transactional.md DT3a/DT5). The forward
// writes are unlogged - catalog writes have no records and the catalog is
// not recovered (known-gaps.md RV3) - so a *compensation* record naming a
// catalog page would be the only record in the stream that does, and
// recovery would apply it to a page image that never saw the write it
// undoes. A SLOT_RETIRE for a slot the on-disk page does not have yet is
// `NotFound` from the applier, and redo reports rather than skips: the
// consequence is a failed mount, not a lost row.
TEST_F(InsertWalTest, ARolledBackDdlWritesNoRecordAgainstACatalogPage) {
    CommandDispatcher d = TransactionalDispatcher(wal::DurabilityClass::kStrict);
    ASSERT_EQ(d.Dispatch("BEGIN").response.substr(0, 5), "BEGIN");
    ASSERT_EQ(d.Dispatch("CREATE TABLE gone (id int64, v int32)").response.substr(0, 7),
              "CREATED");
    ASSERT_EQ(d.Dispatch("ROLLBACK").response.substr(0, 8), "ROLLBACK");
    ASSERT_TRUE(wal_->Flush().ok());

    std::vector<std::vector<std::byte>> storage;
    for (const wal::DecodedRecord& record : DeviceRecords(storage)) {
        const PageId page_id = record.header.page_id;
        if (page_id == kInvalidPageId) continue;
        EXPECT_GE(page_id, catalog::kCatalogOverflowLimit)
            << wal::RecordTypeName(record.type()) << " names catalog page " << page_id
            << ", whose forward writes are unlogged";
    }
}

// The same for a drop, whose compensation is a delete-unmark plus the
// tombstone's overwrite rather than a retire (DT5).
TEST_F(InsertWalTest, ARolledBackDropWritesNoRecordAgainstACatalogPage) {
    CommandDispatcher d = TransactionalDispatcher(wal::DurabilityClass::kStrict);
    ASSERT_EQ(d.Dispatch("CREATE TABLE doomed (id int64, v int32)").response.substr(0, 7),
              "CREATED");
    ASSERT_EQ(d.Dispatch("BEGIN").response.substr(0, 5), "BEGIN");
    ASSERT_EQ(d.Dispatch("DROP TABLE doomed").response.rfind("ERR", 0), std::string::npos);
    ASSERT_EQ(d.Dispatch("ROLLBACK").response.substr(0, 8), "ROLLBACK");
    ASSERT_TRUE(wal_->Flush().ok());

    std::vector<std::vector<std::byte>> storage;
    for (const wal::DecodedRecord& record : DeviceRecords(storage)) {
        const PageId page_id = record.header.page_id;
        if (page_id == kInvalidPageId) continue;
        EXPECT_GE(page_id, catalog::kCatalogOverflowLimit)
            << wal::RecordTypeName(record.type()) << " names catalog page " << page_id
            << ", whose forward writes are unlogged";
    }
}

}  // namespace
}  // namespace kds::server
