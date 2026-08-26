#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/catalog/rows.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/sched/clock.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"

// K-M1: what the engine *does* with Keystone ids today
// (`docs/rules/keystoneid-invariant.md` §5).
//
// The milestone's own wording is why this file exists: "failing tests that
// *demonstrate* any reuse that exists today ... reuse behavior of the
// current engine is documented fact, not assumption." So every test below
// is written to be evidence rather than aspiration.
//
// Two of them pin behaviour that is **wrong** by K1 and stay green anyway.
// That is a deliberate choice over leaving them red: a failing test in a
// suite nobody can make green is a test that gets deleted or ignored, and
// this hazard needs to survive until recovery lands. Each carries the
// condition under which it must be inverted, and
// `docs/rules/keystoneid-k0-findings.md` cites both by name.
//
// What is deliberately *not* here: any fix. K0 is an audit.

namespace kds::server {
namespace {

constexpr std::uint64_t kSegmentSize = 1 << 20;

// ---- 1. Issue-once holds within a run -----------------------------------

class KeystoneIdTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    // Every id currently live in `rel`, read back through SELECT.
    //
    // Every digit run in the reply is an id: the text protocol's row
    // separator is the two-character escape `\n` rather than a newline
    // (docs/spec/client-manual.md), and the only header this projection carries
    // is the column name "id", which has no digits in it.
    std::vector<std::uint64_t> Ids(const std::string& rel) {
        std::vector<std::uint64_t> ids;
        const std::string out = Run("SELECT id FROM " + rel);
        for (std::size_t i = 0; i < out.size();) {
            while (i < out.size() && (out[i] < '0' || out[i] > '9')) ++i;
            std::uint64_t v = 0;
            bool any = false;
            while (i < out.size() && out[i] >= '0' && out[i] <= '9') {
                v = v * 10 + static_cast<std::uint64_t>(out[i] - '0');
                ++i;
                any = true;
            }
            if (any) ids.push_back(v);
        }
        return ids;
    }

    catalog::Oid OidOf(const std::string& rel) {
        auto oid = boot_->catalog.FindTableOidByName(rel);
        EXPECT_TRUE(oid.ok()) << oid.status().message();
        return oid.ok() ? oid.value() : 0;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
};

TEST_F(KeystoneIdTest, RetiringATupleDoesNotReturnItsIdToTheAllocator) {
    // The §1.1 hazard stated directly: a pk freed and re-issued while an
    // old snapshot can still see the prior tuple resolves to the wrong
    // incarnation. There is no SQL DELETE yet, so the slot is retired at
    // the page - which is the *strongest* form of the question, because a
    // physically retired slot is the one case an allocator might plausibly
    // treat as reclaimable.
    ASSERT_EQ(Run("CREATE TABLE t (id int64, v int64)").substr(0, 7), "CREATED");
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(Run("INSERT INTO t VALUES (" + std::to_string(i) + ")").substr(0, 8),
                  "INSERTED");
    }
    ASSERT_EQ(Ids("t").size(), 5u);

    auto row = boot_->catalog.GetSysTableRow(OidOf("t"));
    ASSERT_TRUE(row.ok()) << row.status().message();
    ASSERT_EQ(row.value().next_id, 6u);
    {
        auto bytes = store_.Get(row.value().desc_page_id);
        ASSERT_TRUE(bytes.ok());
        heap::PageView page(bytes.value().bytes());
        ASSERT_TRUE(page.RetireSlot(2).ok());  // the tuple holding id 3
    }
    ASSERT_EQ(Ids("t").size(), 4u);

    ASSERT_EQ(Run("INSERT INTO t VALUES (99)").substr(0, 8), "INSERTED");

    const std::vector<std::uint64_t> ids = Ids("t");
    const std::set<std::uint64_t> unique(ids.begin(), ids.end());
    EXPECT_EQ(ids.size(), unique.size()) << "an id was rebound to a second tuple";
    // The retired 3 is gone for good; the new tuple got 6.
    EXPECT_EQ(unique.count(3), 0u);
    EXPECT_EQ(unique.count(6), 1u);
}

TEST_F(KeystoneIdTest, AnInsertThatFailsAfterAllocationBurnsTheIdRatherThanReusingIt) {
    // `AllocateRowId` bumps and persists *before* the caller encodes, so a
    // failure between the two leaves a gap. K3 makes that legal, and the
    // ordering is the point: the reverse would reissue after a crash.
    ASSERT_EQ(Run("CREATE TABLE t (id int64, s varchar)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO t VALUES ('a')").substr(0, 8), "INSERTED");

    // Larger than one var-heap page, so EncodeRow answers Unsupported -
    // after the id has already been issued and persisted.
    const std::string too_long(9000, 'x');
    EXPECT_EQ(Run("INSERT INTO t VALUES ('" + too_long + "')").substr(0, 4), "ERR ");

    ASSERT_EQ(Run("INSERT INTO t VALUES ('b')").substr(0, 8), "INSERTED");
    const std::vector<std::uint64_t> ids = Ids("t");
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], 1u);
    EXPECT_EQ(ids[1], 3u) << "the failed insert's id must not be handed out again";
}

TEST_F(KeystoneIdTest, EachRelationHasItsOwnSequence) {
    // The id space is per-relation (§6, "the id remains per-relation"), so
    // two relations both starting at 1 is correct, not a collision.
    ASSERT_EQ(Run("CREATE TABLE a (id int64, v int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("CREATE TABLE b (id int64, v int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO a VALUES (1)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO b VALUES (1)").substr(0, 8), "INSERTED");
    EXPECT_EQ(Ids("a"), std::vector<std::uint64_t>{1});
    EXPECT_EQ(Ids("b"), std::vector<std::uint64_t>{1});
}

// ---- 2. The same allocator serves three different meanings ---------------

TEST_F(KeystoneIdTest, TheAllocatorAlsoIssuesCatalogOidsAndCatalogKeystones) {
    // `Catalog::AllocateRowId` has three callers and only one of them
    // issues what §1 describes. This pins the other two so the findings
    // note's claim is checkable rather than asserted:
    //
    //   - kSysPatternsTable    -> an **oid** for a sys.patterns row
    //                             (`RegisterPattern`): a body field, not a
    //                             Keystone word.
    //   - kSysPatternDefsTable -> a real Keystone id, for a row-codec row.
    //
    // K1 is therefore already a claim about two id spaces with one
    // implementation, which is what makes "issue-once" ambiguous until the
    // findings note says which one it binds.
    catalog::Catalog& cat = boot_->catalog;

    auto oid_a = cat.AllocateRowId(catalog::kSysPatternsTable);
    ASSERT_TRUE(oid_a.ok()) << oid_a.status().message();
    auto oid_b = cat.AllocateRowId(catalog::kSysPatternsTable);
    ASSERT_TRUE(oid_b.ok()) << oid_b.status().message();
    EXPECT_EQ(oid_b.value(), oid_a.value() + 1);

    // Separate sequences: issuing a sys.pattern_defs Keystone does not
    // disturb the sys.patterns oid stream.
    auto def_id = cat.AllocateRowId(catalog::kSysPatternDefsTable);
    ASSERT_TRUE(def_id.ok()) << def_id.status().message();
    auto oid_c = cat.AllocateRowId(catalog::kSysPatternsTable);
    ASSERT_TRUE(oid_c.ok());
    EXPECT_EQ(oid_c.value(), oid_b.value() + 1);
}

TEST_F(KeystoneIdTest, AnExhaustedSequenceRefusesRatherThanWrapping) {
    // K4's lifetime budget is a hard stop, and this is its one enforcement
    // point. Reached by driving `next_id` to the ceiling in the catalog row
    // directly - 2^40 inserts is not a test - which is also the only way to
    // touch it, since nothing else may write that field.
    ASSERT_EQ(Run("CREATE TABLE t (id int64, v int64)").substr(0, 7), "CREATED");
    const catalog::Oid oid = OidOf("t");

    {
        auto bytes = store_.Get(catalog::kCatalogPageTables);
        ASSERT_TRUE(bytes.ok());
        heap::PageView page(bytes.value().bytes());
        bool patched = false;
        for (std::uint16_t i = 0; i < page.slot_count(); ++i) {
            auto tuple = page.ReadTuple(i);
            if (!tuple.ok()) continue;
            auto row = catalog::SysTableRow::Decode(tuple.value().payload);
            ASSERT_TRUE(row.ok());
            if (row.value().oid != oid) continue;
            row.value().next_id = kMaxKeystoneId;
            const auto encoded = row.value().Encode();
            ASSERT_TRUE(page.OverwriteTuple(i, encoded, tuple.value().trx_id,
                                            tuple.value().undo_ptr)
                            .ok());
            patched = true;
            break;
        }
        ASSERT_TRUE(patched);
    }

    auto last = boot_->catalog.AllocateRowId(oid);
    ASSERT_TRUE(last.ok()) << last.status().message();
    EXPECT_EQ(last.value(), kMaxKeystoneId);

    auto past = boot_->catalog.AllocateRowId(oid);
    EXPECT_FALSE(past.ok());
    EXPECT_EQ(past.status().code(), StatusCode::kOutOfRange);
}

// ---- 3. What a crash does ------------------------------------------------

// Fixture over a crashable page device, so "durable" is a fact rather than
// an argument.
class KeystoneIdCrashTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto device = storage::MemoryPageDevice::Create(/*extent_pages=*/64,
                                                        /*initial_pages=*/64);
        ASSERT_TRUE(device.ok()) << device.status().message();
        device_ = std::move(device.value());
    }

    // Opens a store + catalog + dispatcher over the device, runs `body`,
    // and tears them all down - one process lifetime.
    template <typename Body>
    void Boot(Body&& body) {
        auto store = storage::DevicePageStore::Open(*device_, kFirstUserPageId);
        ASSERT_TRUE(store.ok()) << store.status().message();
        auto boot = bootstrap::BootstrapDatabase(*store.value(), 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        CommandDispatcher d(boot.value().superblock, boot.value().catalog, *store.value());
        body(d, boot.value().catalog, *store.value());
    }

    std::unique_ptr<storage::MemoryPageDevice> device_;
};

TEST_F(KeystoneIdCrashTest, AnUnsyncedCrashLosesTheSequenceAndTheTuplesTogether) {
    // The plain crash, and the reason it is survivable *today*: the
    // sys.tables page (id 4) and the heap pages (id >= 128) are lost as one
    // unit, so the reverted sequence re-issues ids to a relation that has
    // also forgotten the tuples holding them. No reuse is observable.
    //
    // That is a consequence of "nothing reads the log back", not a designed
    // guarantee - the next test is the same setup with the log kept, and it
    // does not hold there.
    Boot([](CommandDispatcher& d, catalog::Catalog&, storage::PageStore&) {
        ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, v int64)").response.substr(0, 7),
                  "CREATED");
        ASSERT_EQ(d.Dispatch("SYNC").response, "OK synced");
        for (int i = 0; i < 3; ++i) {
            ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (" + std::to_string(i) + ")")
                          .response.substr(0, 8),
                      "INSERTED");
        }
    });
    device_->Crash();

    Boot([](CommandDispatcher& d, catalog::Catalog&, storage::PageStore&) {
        // The sequence is back at 1...
        EXPECT_NE(d.Dispatch("DESCRIBE t").response.find("next_id=1"), std::string::npos);
        // ...and so is the relation, so re-issuing 1 collides with nothing.
        // The reply is the "id" header and no rows, so it carries no digit.
        const std::string rows = d.Dispatch("SELECT id FROM t").response;
        EXPECT_EQ(rows.find_first_of("0123456789"), std::string::npos) << rows;
    });
}

// The same crash with a WAL attached, which is where K1 actually breaks.
class KeystoneIdWalCrashTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto device = storage::MemoryPageDevice::Create(64, 64);
        ASSERT_TRUE(device.ok());
        device_ = std::move(device.value());
        auto log_device = wal::MemoryLogDevice::Create(kSegmentSize);
        ASSERT_TRUE(log_device.ok());
        log_device_ = std::move(log_device.value());
    }

    // Keystone ids named by durable HEAP_INSERT records, in stream order.
    std::vector<std::uint64_t> LoggedIds() {
        std::vector<std::uint64_t> ids;
        for (std::uint64_t seg = 0; seg < log_device_->segment_count(); ++seg) {
            std::vector<std::byte> body(kSegmentSize - wal::kSegmentHeaderSize);
            EXPECT_TRUE(log_device_->ReadAt(seg, wal::kSegmentHeaderSize, body).ok());
            wal::RecordReader reader(body, seg * kSegmentSize + wal::kSegmentHeaderSize);
            while (std::optional<wal::DecodedRecord> record = reader.Next()) {
                if (record->type() == wal::RecordType::kPad) break;
                if (record->type() != wal::RecordType::kHeapInsert) continue;
                auto decoded = wal::DecodeHeapWrite(record->payload);
                if (!decoded.ok()) continue;
                auto id = exec::RowKeystoneId(decoded.value().tuple);
                if (id.ok()) ids.push_back(id.value());
            }
        }
        return ids;
    }

    sched::ManualClock clock_;
    std::unique_ptr<storage::MemoryPageDevice> device_;
    std::unique_ptr<wal::MemoryLogDevice> log_device_;
};

// **This is the K1 violation the audit was looking for.** It passes, and
// that is the bad news: it pins the hazard, it does not endorse it.
//
// Under `strict` durability the HEAP_INSERT records are on the platter
// before the client is answered, while the sys.tables row carrying
// `next_id` is **unlogged** and only reaches disk at a checkpoint. Crash
// between the two and the durable log names tuples with ids 1..3 that the
// reverted allocator is about to hand out again. Nothing collides *today*
// only because nothing reads the log back; the moment recovery redoes those
// HEAP_INSERTs, one id belongs to two tuples.
//
// **Invert this test when recovery lands** (`docs/spec/wal.md`): the assertion
// must become "the allocator resumes above every logged id".
TEST_F(KeystoneIdWalCrashTest, ACrashReissuesIdsThatTheDurableLogStillClaims) {
    {
        auto wal_mgr = wal::WalManager::Open(log_device_.get(), clock_, /*core_id=*/0);
        ASSERT_TRUE(wal_mgr.ok()) << wal_mgr.status().message();
        auto store = storage::DevicePageStore::Open(*device_, kFirstUserPageId);
        ASSERT_TRUE(store.ok());
        store.value()->SetWalGate(wal_mgr.value().get());
        auto boot = bootstrap::BootstrapDatabase(*store.value(), 1000);
        ASSERT_TRUE(boot.ok());

        CommandDispatcher d(boot.value().superblock, boot.value().catalog, *store.value(),
                            /*log=*/nullptr, /*clock=*/nullptr, wal_mgr.value().get(),
                            wal::DurabilityClass::kStrict);
        ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, v int64)").response.substr(0, 7),
                  "CREATED");
        ASSERT_EQ(d.Dispatch("SYNC").response, "OK synced");
        for (int i = 0; i < 3; ++i) {
            ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (" + std::to_string(i) + ")")
                          .response.substr(0, 8),
                      "INSERTED");
        }
    }

    // The log survives; the un-checkpointed pages do not. That asymmetry is
    // what a WAL is *for*, and here it is what breaks issue-once.
    const std::vector<std::uint64_t> logged = LoggedIds();
    ASSERT_EQ(logged, (std::vector<std::uint64_t>{1, 2, 3}));
    device_->Crash();

    auto store = storage::DevicePageStore::Open(*device_, kFirstUserPageId);
    ASSERT_TRUE(store.ok());
    auto boot = bootstrap::BootstrapDatabase(*store.value(), 2000);
    ASSERT_TRUE(boot.ok());
    CommandDispatcher d(boot.value().superblock, boot.value().catalog, *store.value());
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (77)").response.substr(0, 8), "INSERTED");

    auto oid = boot.value().catalog.FindTableOidByName("t");
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    auto reissued = boot.value().catalog.AllocateRowId(oid.value());
    ASSERT_TRUE(reissued.ok());

    // 1 went to the row just inserted, 2 to the call above - both of them
    // ids the durable log has already given to other tuples.
    EXPECT_EQ(reissued.value(), 2u);
    EXPECT_NE(std::find(logged.begin(), logged.end(), reissued.value()), logged.end())
        << "K1 holds after a crash - invert this test, it is pinning a bug";
}

// The control for the test above, and the reason it can be believed: the
// same fixture, the same log, one extra SYNC. With the catalog page durable
// the allocator resumes above every logged id, so the failure it reports is
// reuse rather than a broken harness.
//
// It also names the fix precisely. Nothing about the allocator changes
// here; what changes is that the sequence reached the platter. Closing K1
// across a crash is a *durability* problem - logged catalog writes, then
// recovery - not an allocator one, and K-M2 cannot close it alone.
TEST_F(KeystoneIdWalCrashTest, ASyncedShutdownLeavesTheSequenceAboveEveryLoggedId) {
    {
        auto wal_mgr = wal::WalManager::Open(log_device_.get(), clock_, /*core_id=*/0);
        ASSERT_TRUE(wal_mgr.ok());
        auto store = storage::DevicePageStore::Open(*device_, kFirstUserPageId);
        ASSERT_TRUE(store.ok());
        store.value()->SetWalGate(wal_mgr.value().get());
        auto boot = bootstrap::BootstrapDatabase(*store.value(), 1000);
        ASSERT_TRUE(boot.ok());

        CommandDispatcher d(boot.value().superblock, boot.value().catalog, *store.value(),
                            /*log=*/nullptr, /*clock=*/nullptr, wal_mgr.value().get(),
                            wal::DurabilityClass::kStrict);
        ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, v int64)").response.substr(0, 7),
                  "CREATED");
        for (int i = 0; i < 3; ++i) {
            ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (" + std::to_string(i) + ")")
                          .response.substr(0, 8),
                      "INSERTED");
        }
        ASSERT_EQ(d.Dispatch("SYNC").response, "OK synced");
    }

    const std::vector<std::uint64_t> logged = LoggedIds();
    ASSERT_EQ(logged, (std::vector<std::uint64_t>{1, 2, 3}));
    device_->Crash();

    auto store = storage::DevicePageStore::Open(*device_, kFirstUserPageId);
    ASSERT_TRUE(store.ok());
    auto boot = bootstrap::BootstrapDatabase(*store.value(), 2000);
    ASSERT_TRUE(boot.ok());
    auto oid = boot.value().catalog.FindTableOidByName("t");
    ASSERT_TRUE(oid.ok());
    auto next = boot.value().catalog.AllocateRowId(oid.value());
    ASSERT_TRUE(next.ok());

    EXPECT_EQ(next.value(), 4u);
    EXPECT_EQ(std::find(logged.begin(), logged.end(), next.value()), logged.end());
}

// ---- 4. The oid space, which K1 also depends on --------------------------

// §1.2 claims "(oid, pk) becomes a forever-unique key ... a stored (oid, pk)
// can dangle, but it can never mis-attribute". The pk half is sound. The
// oid half is not: `Catalog::GenerateUserOid()` is `next_user_oid_++` over
// an in-memory counter seeded at `kUserOidStart` and never read back from
// the catalog, so every boot re-issues the same oids.
//
// The gap itself is known and documented (`catalog.hpp`'s header,
// `well_known.hpp`'s kUserOidStart), and sys.patterns rows avoid it on
// purpose. What is new is that `docs/rules/keystoneid-invariant.md` builds a
// stated guarantee on top of it without naming it. Same posture as the WAL
// test above: green, and pinning a bug.
TEST_F(KeystoneIdCrashTest, ObjectOidsAreUniqueAcrossABoot) {
    // **Inverted 2026-08-08.** This test used to pin the bug
    // docs/rules/keystoneid-k0-findings.md §6 describes - `GenerateUserOid()` was
    // an in-memory counter seeded at kUserOidStart and never read back, so a
    // clean restart plus one CREATE TABLE produced two relations sharing an
    // oid, and resolving that oid returned the *first* row carrying it. Both
    // assertions below carried an "invert this test" message. This is that
    // inversion.
    //
    // The fix recovers the counter from the catalog's own rows on first use
    // rather than persisting it, so no format changed and an existing data
    // file is fixed by being opened. What makes that sound is that the rows
    // *are* the counter: a crash between issuing an oid and writing its row
    // loses the oid instead of duplicating it, and a lost oid is free (K3 -
    // no density promise).
    catalog::Oid a_oid = 0;
    catalog::Oid a_high_column = 0;
    Boot([&](CommandDispatcher& d, catalog::Catalog& cat, storage::PageStore& store) {
        ASSERT_EQ(d.Dispatch("CREATE TABLE a (id int64, v int64)").response.substr(0, 7),
                  "CREATED");
        ASSERT_EQ(d.Dispatch("INSERT INTO a VALUES (11)").response.substr(0, 8), "INSERTED");
        auto oid = cat.FindTableOidByName("a");
        ASSERT_TRUE(oid.ok());
        a_oid = oid.value();
        EXPECT_EQ(a_oid, catalog::kUserOidStart);

        // Columns come from the same counter as relations, which is why the
        // recovery has to read sys.columns as well as sys.objects: the
        // high-water mark after a CREATE TABLE sits on a *column*, not on
        // the table. Two columns, so 4001 and 4002 are taken.
        auto schema = cat.BuildSchemaFromColumns(a_oid);
        ASSERT_TRUE(schema.ok()) << schema.status().message();
        for (const catalog::SysColumnRow& col : schema.value().columns) {
            if (col.oid > a_high_column) a_high_column = col.oid;
        }
        EXPECT_GT(a_high_column, a_oid) << "a column holds the high-water mark";

        ASSERT_TRUE(store.Sync().ok());
    });

    // A clean restart. Nothing crashed, nothing was lost.
    Boot([&](CommandDispatcher& d, catalog::Catalog& cat, storage::PageStore& store) {
        ASSERT_NE(d.Dispatch("SELECT v FROM a").response.find("11"), std::string::npos)
            << "relation a survived the restart";

        ASSERT_EQ(d.Dispatch("CREATE TABLE c (id int64, v int64)").response.substr(0, 7),
                  "CREATED");
        auto c_oid = cat.FindTableOidByName("c");
        ASSERT_TRUE(c_oid.ok());

        // The property §1.2 of docs/rules/keystoneid-invariant.md claims and could
        // not previously deliver: an oid names one object for the life of the
        // database. Everything keyed on (oid, pk) - the access statistics,
        // trail entries, any future change feed - depends on it.
        EXPECT_NE(c_oid.value(), a_oid);

        // And specifically it clears the *columns* too, not just the table.
        // Resuming at a_oid + 1 would collide with a's first column, which is
        // the failure mode a sys.objects-only recovery would have.
        EXPECT_GT(c_oid.value(), a_high_column);

        // The consequence that used to be worse than a merged statistic:
        // resolving the oid took the first sys.tables row carrying it, so 'c'
        // resolved to 'a'. Now each resolves to itself.
        auto row = cat.GetSysTableRow(c_oid.value());
        ASSERT_TRUE(row.ok()) << row.status().message();
        EXPECT_EQ(std::string(catalog::NameView(row.value().name)), "c");

        auto a_row = cat.GetSysTableRow(a_oid);
        ASSERT_TRUE(a_row.ok()) << a_row.status().message();
        EXPECT_EQ(std::string(catalog::NameView(a_row.value().name)), "a");

        ASSERT_TRUE(store.Sync().ok());
    });

    // A third boot, to pin that the recovery is not a one-shot migration:
    // it reads the catalog every process, so it keeps working as the
    // catalog grows.
    Boot([&](CommandDispatcher& d, catalog::Catalog& cat, storage::PageStore&) {
        ASSERT_EQ(d.Dispatch("CREATE TABLE e (id int64, v int64)").response.substr(0, 7),
                  "CREATED");
        auto e_oid = cat.FindTableOidByName("e");
        ASSERT_TRUE(e_oid.ok());
        auto c_oid = cat.FindTableOidByName("c");
        ASSERT_TRUE(c_oid.ok());
        EXPECT_GT(e_oid.value(), c_oid.value());
        EXPECT_NE(e_oid.value(), a_oid);
    });
}


}  // namespace
}  // namespace kds::server
