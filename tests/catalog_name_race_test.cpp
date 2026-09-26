#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/exec/assertion_catalog.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"

#include "armed_race.hpp"

// **One name, one row** (AT-S17, the AT-close order's Q2: the held page).
//
// Every name-creating catalog write checked that the name was free and then
// wrote it as two latch holds. That was one act while every DDL ran on core
// 0 and the reactor ran it to completion; AT-S5 made a DDL run where its
// session is, so two cores could both find a name free and both write it -
// two rows claiming one name, and resolution answering whichever it met
// first. `docs/inflight/bugs/two-cores-can-create-one-name-twice.md` (at
// `4d1e970`; deleted by this stage) found it by reading; these cells reproduce it and pin the close.
//
// Driven on two catalogs over one armed store, one per thread and each
// thread its own core, because the window is between two latch holds and
// the dispatcher around a catalog is one thread's. The rendezvous is placed
// where each cell says - for `CREATE TABLE` between the dispatcher's
// lookup and the catalog's write, which is the bug entry's own reproduction.
//
// **Every cell ran red on the unfixed tree** (`4d1e970`), and each names the
// mutant it was run against.

namespace kds::catalog {
namespace {

using testing_race::ArmedStore;
using testing_race::Rendezvous;

inline constexpr int kThreads = 2;
inline constexpr int kRounds = 40;

Schema PkAnd(std::initializer_list<const char*> names) {
    Schema schema;
    SysColumnRow pk{};
    pk.pos = 0;
    SetName(pk.name, "id");
    pk.type_val = kTypeValInt64;
    pk.len = 8;
    pk.notnull = true;
    schema.columns.push_back(pk);
    for (const char* name : names) {
        SysColumnRow col{};
        col.pos = static_cast<std::uint32_t>(schema.columns.size());
        SetName(col.name, name);
        col.type_val = kTypeValInt64;
        col.len = 8;
        col.notnull = true;
        schema.columns.push_back(col);
    }
    return schema;
}

// Two catalogs over one store, wired to the instance's words as every
// core's is (`CoreRuntime::Open`): a private oid counter per catalog would
// hand both creates one oid, which is AT-S5b's trap and not this one.
struct TwoCatalogs {
    std::unique_ptr<storage::MemoryPageDevice> device;
    std::unique_ptr<storage::DevicePageStore> store;
    std::atomic<Oid> oid_sequence{0};
    std::atomic<std::uint64_t> schema_word{0};
    std::atomic<std::uint64_t> marks{0};
    std::array<std::unique_ptr<Catalog>, kThreads> cores;

    TwoCatalogs() {
        // Past the fixed catalog pages (4-15), as `kFirstUserPageId` is.
        store = ArmedStore(device, /*first_new_page_id=*/128);
        for (auto& c : cores) {
            c = std::make_unique<Catalog>(*store);
            c->SetOidSequence(&oid_sequence);
            c->SetSchemaWord(&schema_word);
            c->SetMarkCounter(&marks);
        }
        const Status booted = cores[0]->Bootstrap();
        EXPECT_TRUE(booted.ok()) << booted.message();
    }

    // A catalog that has cached nothing, so what it answers is the pages.
    std::unique_ptr<Catalog> Fresh() const { return std::make_unique<Catalog>(*store); }

    Catalog& operator[](int core) { return *cores[static_cast<std::size_t>(core)]; }
};

// Runs `step(core, round)` on two threads, each acting as its own core, and
// returns every call's status by `[core][round]`.
template <typename Step>
std::array<std::vector<Status>, kThreads> OnTwoCores(Step step) {
    std::array<std::vector<Status>, kThreads> out;
    for (auto& v : out) v.assign(kRounds, Status::OK());
    std::vector<std::thread> threads;
    for (int core = 0; core < kThreads; ++core) {
        threads.emplace_back([&, core] {
            CurrentCoreGuard as(static_cast<std::uint32_t>(core));
            for (int round = 0; round < kRounds; ++round) {
                out[static_cast<std::size_t>(core)][static_cast<std::size_t>(round)] =
                    step(core, round);
            }
        });
    }
    for (std::thread& t : threads) t.join();
    return out;
}

// Exactly one of the two calls in every round took the name, and the other
// was refused `AlreadyExists` - never a second success, never another code.
void ExpectOneWinnerPerRound(const std::array<std::vector<Status>, kThreads>& out) {
    for (int round = 0; round < kRounds; ++round) {
        const Status& a = out[0][static_cast<std::size_t>(round)];
        const Status& b = out[1][static_cast<std::size_t>(round)];
        EXPECT_NE(a.ok(), b.ok()) << "round " << round << ": core 0 '" << a.message()
                                  << "', core 1 '" << b.message() << "'";
        const Status& loser = a.ok() ? b : a;
        if (!loser.ok()) {
            EXPECT_EQ(loser.code(), StatusCode::kAlreadyExists)
                << "round " << round << ": " << loser.message();
        }
    }
}

std::string Name(const char* stem, int round) { return stem + std::to_string(round); }

TEST(CatalogNameRaceTest, TwoCoresCreatingOneTableNameLeaveOneRow) {
    // The bug entry's reproduction: both sessions look the name up - the
    // dispatcher's duplicate check - and only then do both write. Before
    // AT-S17 both writes landed, every round.
    //
    // **Mutations**: drop the binding check under `CreateTable`'s hold,
    // keeping the unheld early one; keep it and drop the hold. Each killed
    // 20 in 20 (and 10 in 10 against the stage's first, wider hold).
    TwoCatalogs cats;
    Rendezvous gate(kThreads);
    const Schema schema = PkAnd({"v"});
    auto out = OnTwoCores([&](int core, int round) {
        const std::string name = Name("t", round);
        gate.Wait();
        const auto seen = cats[core].FindTableOidByName(name);
        gate.Wait();  // both have looked; neither has written
        if (seen.ok()) return Status::AlreadyExists("seen before the write");
        return cats[core].CreateTable(kNamespacePublic, name, schema, ClusteredType::kBtree)
            .status();
    });
    ExpectOneWinnerPerRound(out);

    auto tables = cats.Fresh()->ListTables();
    ASSERT_TRUE(tables.ok()) << tables.status().message();
    for (int round = 0; round < kRounds; ++round) {
        int rows = 0;
        for (const SysObjectRow& row : tables.value()) {
            rows += NameView(row.name) == Name("t", round) ? 1 : 0;
        }
        EXPECT_EQ(rows, 1) << "sys.objects holds " << rows << " rows named " << Name("t", round);
    }
}

TEST(CatalogNameRaceTest, TwoCoresCreatingOneNamespaceLeaveOneRow) {
    // `CreateNamespace` checks inside the call, so the rendezvous is at its
    // door: both enter together and both scans run before either insert.
    //
    // **Mutation**: drop the hold in `CreateNamespace` - killed 10 in 10.
    TwoCatalogs cats;
    Rendezvous gate(kThreads);
    auto out = OnTwoCores([&](int core, int round) {
        gate.Wait();
        return cats[core].CreateNamespace(Name("ns", round)).status();
    });
    ExpectOneWinnerPerRound(out);

    auto spaces = cats.Fresh()->ListNamespaces();
    ASSERT_TRUE(spaces.ok()) << spaces.status().message();
    for (int round = 0; round < kRounds; ++round) {
        int rows = 0;
        for (const SysObjectRow& row : spaces.value()) {
            rows += NameView(row.name) == Name("ns", round) ? 1 : 0;
        }
        EXPECT_EQ(rows, 1) << rows << " namespace rows named " << Name("ns", round);
    }
}

TEST(CatalogNameRaceTest, TwoCoresRenamingTwoTablesOntoOneNameLeaveOneRow) {
    // `ALTER TABLE ... RENAME TO`, two relations renamed onto one name from
    // two cores.
    //
    // **Mutation**: drop the hold in `RenameTable` - killed 10 in 10.
    TwoCatalogs cats;
    const Schema schema = PkAnd({"v"});
    std::array<std::vector<Oid>, kThreads> rels;
    for (int core = 0; core < kThreads; ++core) {
        for (int round = 0; round < kRounds; ++round) {
            auto oid = cats[0].CreateTable(kNamespacePublic,
                                           Name(core == 0 ? "a" : "b", round), schema,
                                           ClusteredType::kBtree);
            ASSERT_TRUE(oid.ok()) << oid.status().message();
            rels[static_cast<std::size_t>(core)].push_back(oid.value());
        }
    }
    Rendezvous gate(kThreads);
    auto out = OnTwoCores([&](int core, int round) {
        gate.Wait();
        return cats[core].RenameTable(
            rels[static_cast<std::size_t>(core)][static_cast<std::size_t>(round)],
            Name("x", round));
    });
    ExpectOneWinnerPerRound(out);

    auto tables = cats.Fresh()->ListTables();
    ASSERT_TRUE(tables.ok()) << tables.status().message();
    for (int round = 0; round < kRounds; ++round) {
        int rows = 0;
        for (const SysObjectRow& row : tables.value()) {
            rows += NameView(row.name) == Name("x", round) ? 1 : 0;
        }
        EXPECT_EQ(rows, 1) << rows << " relations named " << Name("x", round);
    }
}

TEST(CatalogNameRaceTest, TwoCoresRenamingTwoColumnsOntoOneNameLeaveOneColumn) {
    // `RENAME COLUMN`: a sibling collision within one relation, which
    // `HandleAlter` holds no relation lock over. Two columns of one name
    // are one relation whose second column resolution can never reach.
    //
    // **Mutation**: drop the hold in `RenameColumn` - killed 10 in 10.
    TwoCatalogs cats;
    std::vector<Oid> rels;
    for (int round = 0; round < kRounds; ++round) {
        auto oid = cats[0].CreateTable(kNamespacePublic, Name("c", round), PkAnd({"a", "b"}),
                                       ClusteredType::kBtree);
        ASSERT_TRUE(oid.ok()) << oid.status().message();
        rels.push_back(oid.value());
    }
    Rendezvous gate(kThreads);
    auto out = OnTwoCores([&](int core, int round) {
        gate.Wait();
        return cats[core].RenameColumn(rels[static_cast<std::size_t>(round)],
                                       core == 0 ? "a" : "b", "x");
    });
    ExpectOneWinnerPerRound(out);

    auto fresh = cats.Fresh();
    for (int round = 0; round < kRounds; ++round) {
        auto access = fresh->InitTableAccess(rels[static_cast<std::size_t>(round)]);
        ASSERT_TRUE(access.ok()) << access.status().message();
        int named = 0;
        for (const SysColumnRow& col : access.value()->schema.columns) {
            named += NameView(col.name) == "x" ? 1 : 0;
        }
        EXPECT_EQ(named, 1) << Name("c", round) << " has " << named << " columns named x";
    }
}

TEST(CatalogNameRaceTest, TwoCoresCreatingOneIndexNameOnTwoRelationsLeaveOneRow) {
    // Index names are instance-wide (`FindIndexByName` scans every row) and
    // `CREATE INDEX`'s relation `X` covers only its own relation, so two
    // indexes of one name on two relations were two holds apart.
    //
    // The oid is pre-issued, as `PrepareIndexDef` issues it on the DDL
    // path: an index `CreateIndex` issues itself takes `sys.tables`' latch
    // between the two checks, which staggers the threads and narrows the
    // window this cell is about (measured: the mutant below survived 1 run
    // in 10 that way).
    //
    // **Mutation**: drop the hold in `CreateIndex` - killed 20 in 20.
    TwoCatalogs cats;
    std::array<std::vector<Oid>, kThreads> rels;
    for (int core = 0; core < kThreads; ++core) {
        for (int round = 0; round < kRounds; ++round) {
            auto oid = cats[0].CreateTable(kNamespacePublic,
                                           Name(core == 0 ? "p" : "q", round),
                                           PkAnd({"v"}), ClusteredType::kBtree);
            ASSERT_TRUE(oid.ok()) << oid.status().message();
            rels[static_cast<std::size_t>(core)].push_back(oid.value());
        }
    }
    Rendezvous gate(kThreads);
    auto out = OnTwoCores([&](int core, int round) {
        Catalog::IndexDef def;
        def.table_oid = rels[static_cast<std::size_t>(core)][static_cast<std::size_t>(round)];
        def.name = Name("ix", round);
        def.root_page_id = 1000;  // never read: no entry is written or probed
        def.key_width = 9;
        def.entry_width = 17;
        def.key_cols = {1};
        auto issued = cats[core].AllocateRowId(kSysIndexesTable);
        if (!issued.ok()) return issued.status();
        def.index_oid = issued.value();
        gate.Wait();
        return cats[core].CreateIndex(def).status();
    });
    ExpectOneWinnerPerRound(out);

    auto indexes = cats.Fresh()->ListIndexes();
    ASSERT_TRUE(indexes.ok()) << indexes.status().message();
    for (int round = 0; round < kRounds; ++round) {
        int rows = 0;
        for (const SysIndexRow& row : indexes.value()) {
            rows += NameView(row.name) == Name("ix", round) ? 1 : 0;
        }
        EXPECT_EQ(rows, 1) << rows << " indexes named " << Name("ix", round);
    }
}

TEST(CatalogNameRaceTest, TwoCoresPublishingOneAssertionNameOnTwoRelationsLeaveOneRow) {
    // `CREATE ASSERTION`'s publish, `exec::InsertAssertion`: the same shape
    // one relation over, on `sys.assertions`. The build's relation `X`
    // fences the target's writers, not a second assertion on another
    // relation, and assertion names are instance-wide.
    //
    // **Mutation**: drop the hold in `InsertAssertion` - killed 10 in 10.
    TwoCatalogs cats;
    std::array<std::vector<Oid>, kThreads> rels;
    for (int core = 0; core < kThreads; ++core) {
        for (int round = 0; round < kRounds; ++round) {
            auto oid = cats[0].CreateTable(kNamespacePublic,
                                           Name(core == 0 ? "r" : "s", round),
                                           PkAnd({"v"}), ClusteredType::kBtree);
            ASSERT_TRUE(oid.ok()) << oid.status().message();
            rels[static_cast<std::size_t>(core)].push_back(oid.value());
        }
    }
    Rendezvous gate(kThreads);
    auto out = OnTwoCores([&](int core, int round) {
        auto id = cats[core].AllocateRowId(kSysAssertionsTable);
        if (!id.ok()) return id.status();
        gate.Wait();
        return exec::InsertAssertion(
            cats[core], *cats.store, /*wal=*/nullptr, id.value(),
            rels[static_cast<std::size_t>(core)][static_cast<std::size_t>(round)],
            Name("cap", round), "CREATE ASSERTION ... (never parsed here)", kInvalidPageId);
    });
    ExpectOneWinnerPerRound(out);

    auto fresh = cats.Fresh();
    auto defs = exec::ListAssertions(*fresh, *cats.store);
    ASSERT_TRUE(defs.ok()) << defs.status().message();
    for (int round = 0; round < kRounds; ++round) {
        int rows = 0;
        for (const exec::AssertionDef& def : defs.value()) {
            rows += def.name == Name("cap", round) ? 1 : 0;
        }
        EXPECT_EQ(rows, 1) << rows << " assertions named " << Name("cap", round);
    }
}

TEST(CatalogNameRaceTest, TwoCoresCreatingACabinOnOneColumnLeaveOneRow) {
    // Not a name but the same shape, found by AT-S17's review: one cabin per
    // (relation, column), checked and inserted two holds apart, and reached
    // by an operator's `CREATE CABIN` racing another's or the controller's.
    //
    // **Mutation**: drop the hold in `CreateCabin` - killed 20 in 20.
    TwoCatalogs cats;
    std::vector<Oid> rels;
    for (int round = 0; round < kRounds; ++round) {
        auto oid = cats[0].CreateTable(kNamespacePublic, Name("k", round), PkAnd({"v"}),
                                       ClusteredType::kBtree);
        ASSERT_TRUE(oid.ok()) << oid.status().message();
        rels.push_back(oid.value());
    }
    Rendezvous gate(kThreads);
    auto out = OnTwoCores([&](int core, int round) {
        gate.Wait();
        return cats[core].CreateCabin(rels[static_cast<std::size_t>(round)], 1).status();
    });
    ExpectOneWinnerPerRound(out);

    auto cabins = cats.Fresh()->ListCabins();
    ASSERT_TRUE(cabins.ok()) << cabins.status().message();
    for (int round = 0; round < kRounds; ++round) {
        int rows = 0;
        for (const SysCabinRow& row : cabins.value()) {
            rows += row.rel_oid == rels[static_cast<std::size_t>(round)] ? 1 : 0;
        }
        EXPECT_EQ(rows, 1) << rows << " cabins on " << Name("k", round) << ".v";
    }
}

}  // namespace
}  // namespace kds::catalog
