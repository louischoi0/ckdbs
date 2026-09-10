#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/exec/assertion_check.hpp"
#include "kds/parser/ast.hpp"
#include "kds/storage/cabin_bound_page.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"

// **One assertion registry, reserved into from more than one core** (AT-S5d,
// AT-R15, D1).
//
// `assertion_enforce_test.cpp` asks every question one dispatcher can ask.
// This file asks the ones AT-S5 created and AT-S5d answers: a write runs
// where its session is, so the registry is the instance's and two threads
// check and reserve into one directory. Driven on the registry directly, over
// an armed store, because the dispatcher around it is one thread's.
//
//   - **Two cores admitting into one group admit exactly one**, and the
//     loser is refused at its *admission*, where a refused row burns
//     nothing. A pure admission let both through at count 0 once a second
//     thread existed - the defect the registry's hold closes.
//   - **An assertion adopted between an admission and its reservation is
//     checked at the reservation**, the arm that keeps a hold that does not
//     cover it from reserving unchecked.
//   - **A hold counts in every admission and in no snapshot**, and a hold
//     given back is room again.
//   - **A snapshot holds the directory against a reservation on another
//     core**, which is what keeps a checkpoint's base equal to the fold of
//     the records before it.
//   - **Eight cores reserving into one cabin lose no entry**, which pins
//     the directory latch and - measured, and said where - not the chain
//     latch.
//
// Mutations are recorded against each cell.

namespace kds::exec {
namespace {

inline constexpr catalog::Oid kOid = 4000;
inline constexpr int kThreads = 2;

// `btree_race_test.cpp`'s rendezvous, for its reason: two threads started
// together drift apart after a few calls, and the window each cell is about
// opens only when both are inside the same step at once.
class Rendezvous {
  public:
    explicit Rendezvous(int parties) noexcept : parties_(parties) {}

    void Wait() noexcept {
        const int generation = generation_.load(std::memory_order_acquire);
        if (waiting_.fetch_add(1, std::memory_order_acq_rel) + 1 == parties_) {
            waiting_.store(0, std::memory_order_release);
            generation_.fetch_add(1, std::memory_order_acq_rel);
            return;
        }
        while (generation_.load(std::memory_order_acquire) == generation) {
            std::this_thread::yield();
        }
    }

  private:
    const int parties_;
    std::atomic<int> waiting_{0};
    std::atomic<int> generation_{0};
};

std::unique_ptr<storage::DevicePageStore> ArmedStore(
    std::unique_ptr<storage::MemoryPageDevice>& device) {
    auto made = storage::MemoryPageDevice::Create(/*extent_pages=*/512, /*initial_pages=*/0);
    EXPECT_TRUE(made.ok()) << made.status().message();
    device = std::move(made.value());
    auto store = storage::DevicePageStore::Open(*device, /*first_new_page_id=*/16);
    EXPECT_TRUE(store.ok()) << store.status().message();
    // Armed: the page latch queues a second core only where the store is
    // shared, and the chain cell below needs it to.
    store.value()->SetLatchArmed(true, /*concurrent_pinners=*/16);
    return std::move(store.value());
}

// `GROUP BY (v) CHECK COUNT(*) <= bound` on `(id, v)`, built by hand with a
// chain rooted in `store` - what `CreateAssertion`'s build hands `Adopt`,
// minus the scan.
LiveAssertion CountCap(storage::PageStore& store, std::uint64_t id, std::int64_t bound) {
    LiveAssertion a;
    a.assertion_id = id;
    a.target_oid = kOid;
    a.name = "cap";
    a.aggregate = BoundAggregate::kCount;
    a.group_cols = {1};
    a.group_col_names = {"v"};
    a.group_type_vals = {catalog::kTypeValInt64};
    a.chain = BoundCabinChainWriter(id);
    EXPECT_TRUE(a.chain.EnsureRoot(store, /*wal=*/nullptr).ok());
    a.cabin = BoundCabin(BoundAggregate::kCount, bound);
    return a;
}

// An `INSERT`'s VALUES list after the pk: the one column, `v`.
std::vector<parser::AstValue> Row(std::int64_t v) {
    std::vector<parser::AstValue> row(1);
    row[0].type = parser::ValueType::kInt;
    row[0].int_val = v;
    return row;
}

// The group's count in a snapshot, 0 where the snapshot has no such group.
std::int64_t SnapshotCount(const AssertionEnforcer& enforcer, std::int64_t v) {
    const std::string key = EncodeGroupKey(Row(v));
    for (const wal::AssertionCabinSnapshot& cabin : enforcer.SnapshotAssertions()) {
        for (const wal::AssertionSnapshotGroup& group : cabin.groups) {
            if (group.key == key) return group.count;
        }
    }
    return 0;
}

TEST(AssertionRaceTest, TwoCoresAdmittingIntoOneGroupAdmitExactlyOneAndRefuseTheOtherAtAdmission) {
    // **D1's second half.** Both threads admit a row into the same group of
    // `COUNT(*) <= 1`, and the rendezvous between the two calls holds both
    // on the far side of their admission before either reserves - which is
    // the interleaving the page work between the two calls opens on a real
    // write path, and the one a pure admission cannot survive: both read
    // count 0.
    //
    // **What is asserted is where the loser is refused**, not only that it
    // is: at `AdmitInsert`, before an id or a placement, so a refused row
    // still burns nothing (BI9). `ReserveInsert` re-checks an assertion its
    // hold does not cover, so without the hold one row would still land -
    // but only after the second had been placed and refused at its
    // reservation.
    //
    // **Mutation**, measured: `AdmitInsert` takes no hold (`if (hold ==
    // nullptr) continue;` made unconditional) - both admissions answer OK,
    // killed 5 runs in 5.
    std::unique_ptr<storage::MemoryPageDevice> device;
    auto store = ArmedStore(device);
    ASSERT_NE(store, nullptr);
    AssertionEnforcer enforcer(/*shared=*/true);
    enforcer.Adopt(CountCap(*store, /*id=*/1, /*bound=*/1));

    constexpr int kRounds = 200;
    Rendezvous gate(kThreads);
    std::vector<std::atomic<int>> admitted(kRounds);
    std::vector<std::atomic<int>> refused_at_admission(kRounds);
    std::atomic<int> reservation_failures{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            SetCurrentCore(static_cast<std::uint32_t>(t));
            const std::uint64_t txn = 100 + static_cast<std::uint64_t>(t);
            for (int round = 0; round < kRounds; ++round) {
                const auto values = Row(round);
                AssertionEnforcer::Hold hold;
                gate.Wait();
                const Status admit = enforcer.AdmitInsert(kOid, values, txn, nullptr, &hold);
                gate.Wait();  // both have asked; neither has reserved
                if (!admit.ok()) {
                    if (admit.code() == StatusCode::kAssertionViolation) {
                        refused_at_admission[round].fetch_add(1);
                    }
                    continue;
                }
                const std::uint64_t pk = static_cast<std::uint64_t>(round) * 2 + t + 1;
                if (enforcer.ReserveInsert(*store, /*wal=*/nullptr, txn, hold, kOid, values, pk,
                                           kInvalidPageId, 0)
                        .ok()) {
                    admitted[round].fetch_add(1);
                } else {
                    reservation_failures.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& thread : threads) thread.join();

    EXPECT_EQ(reservation_failures.load(), 0);
    for (int round = 0; round < kRounds; ++round) {
        EXPECT_EQ(admitted[round].load(), 1) << "round " << round;
        EXPECT_EQ(refused_at_admission[round].load(), 1)
            << "round " << round << ": the loser was not refused at its admission";
        EXPECT_EQ(SnapshotCount(enforcer, round), 1) << "round " << round;
    }
}

TEST(AssertionRaceTest, AnAssertionAdoptedBetweenAnAdmissionAndItsReservationIsCheckedThere) {
    // The arm that keeps a hold honest about what it covers. The row is
    // admitted while the relation carries no assertion - its hold is empty -
    // and `COUNT(*) <= 0` is adopted before the reservation, which is the
    // shape a build finishing on another core between the two calls takes.
    // Reserving it unchecked would put a row in a group the constraint says
    // must stay empty; the reservation checks it instead, and refuses.
    //
    // **Mutation**, measured: `ReserveInsert` reserves an uncovered
    // assertion without refusing on `FitsLocked` - the reservation answers
    // OK and the group reads 1, killed 1 in 1 (the cell is single-threaded).
    std::unique_ptr<storage::MemoryPageDevice> device;
    auto store = ArmedStore(device);
    ASSERT_NE(store, nullptr);
    AssertionEnforcer enforcer(/*shared=*/true);

    const auto values = Row(7);
    AssertionEnforcer::Hold hold;
    ASSERT_TRUE(enforcer.AdmitInsert(kOid, values, /*writer_txn=*/1, nullptr, &hold).ok());
    enforcer.Adopt(CountCap(*store, /*id=*/1, /*bound=*/0));

    const Status reserved =
        enforcer.ReserveInsert(*store, /*wal=*/nullptr, 1, hold, kOid, values, /*pk=*/1,
                               kInvalidPageId, 0);
    EXPECT_EQ(reserved.code(), StatusCode::kAssertionViolation) << reserved.message();
    EXPECT_EQ(SnapshotCount(enforcer, 7), 0);
}

TEST(AssertionRaceTest, AHoldCountsInEveryAdmissionAndInNoSnapshotAndIsRoomAgainOnceGivenBack) {
    // What a hold is, stated on one thread. Held, it refuses the next
    // admission to its group and names its transaction as the one to wait
    // for; it is in no snapshot, because no record describes it yet; given
    // back unreserved, the group has its room again; reserved, it is header.
    //
    // **Mutation**, measured: a `Hold` whose release gives nothing back
    // fails the third admission - killed 1 in 1 (single-threaded).
    std::unique_ptr<storage::MemoryPageDevice> device;
    auto store = ArmedStore(device);
    ASSERT_NE(store, nullptr);
    AssertionEnforcer enforcer(/*shared=*/true);
    enforcer.Adopt(CountCap(*store, /*id=*/1, /*bound=*/1));
    const auto values = Row(7);

    {
        AssertionEnforcer::Hold first;
        ASSERT_TRUE(enforcer.AdmitInsert(kOid, values, /*writer_txn=*/1, nullptr, &first).ok());
        EXPECT_EQ(SnapshotCount(enforcer, 7), 0) << "a hold reached the header before its record";

        std::uint64_t reserver = 0;
        AssertionEnforcer::Hold second;
        const Status refused = enforcer.AdmitInsert(kOid, values, /*writer_txn=*/2, &reserver,
                                                    &second);
        EXPECT_EQ(refused.code(), StatusCode::kAssertionViolation) << refused.message();
        EXPECT_EQ(reserver, 1u) << "a refusal caused by a hold names nobody to wait for";
    }  // `first` gives its hold back here, unreserved

    AssertionEnforcer::Hold third;
    ASSERT_TRUE(enforcer.AdmitInsert(kOid, values, /*writer_txn=*/3, nullptr, &third).ok())
        << "a hold given back left the group full";
    ASSERT_TRUE(enforcer
                    .ReserveInsert(*store, /*wal=*/nullptr, 3, third, kOid, values, /*pk=*/3,
                                   kInvalidPageId, 0)
                    .ok());
    EXPECT_EQ(SnapshotCount(enforcer, 7), 1);
}

TEST(AssertionRaceTest, ASnapshotHoldsTheDirectoryAgainstAReservationOnAnotherCore) {
    // **The checkpoint's half.** A checkpoint's `ASSERT_SNAPSHOT` is a base
    // only if it equals the fold of every `ASSERT_*` record before it, and a
    // reservation on another core appends its record and moves the header
    // in one step. So `VisitSnapshots` holds the directory latch until the
    // snapshot's own records are appended, and a reservation that arrives
    // meanwhile waits for it. Here the visitor stands for the checkpointer:
    // it holds the call open while the other thread reserves, and reads
    // whether the reservation got in.
    //
    // **Mutation**, measured: `VisitSnapshots` calls `visit(SnapshotLocked())`
    // without the guard - the reservation lands inside the visit, killed 3
    // runs in 3.
    std::unique_ptr<storage::MemoryPageDevice> device;
    auto store = ArmedStore(device);
    ASSERT_NE(store, nullptr);
    AssertionEnforcer enforcer(/*shared=*/true);
    enforcer.Adopt(CountCap(*store, /*id=*/1, /*bound=*/10));
    const auto values = Row(7);

    std::atomic<bool> admitted{false};
    std::atomic<bool> visiting{false};
    std::atomic<bool> reserving{false};
    std::atomic<bool> reserved{false};
    std::thread other([&] {
        SetCurrentCore(1);
        AssertionEnforcer::Hold hold;
        EXPECT_TRUE(enforcer.AdmitInsert(kOid, values, /*writer_txn=*/1, nullptr, &hold).ok());
        admitted.store(true, std::memory_order_release);
        while (!visiting.load(std::memory_order_acquire)) std::this_thread::yield();
        reserving.store(true, std::memory_order_release);
        EXPECT_TRUE(enforcer
                        .ReserveInsert(*store, /*wal=*/nullptr, 1, hold, kOid, values, /*pk=*/1,
                                       kInvalidPageId, 0)
                        .ok());
        reserved.store(true, std::memory_order_release);
    });

    SetCurrentCore(0);
    // The admission takes the same latch, so it goes first: a visit begun
    // before it would hold the thread this visit is waiting on.
    while (!admitted.load(std::memory_order_acquire)) std::this_thread::yield();
    bool landed_inside = false;
    std::int64_t seen = -1;
    const Status visited = enforcer.VisitSnapshots(
        [&](const std::vector<wal::AssertionCabinSnapshot>& cabins) -> Status {
            seen = cabins.empty() || cabins.front().groups.empty() ? 0
                                                                   : cabins.front().groups[0].count;
            visiting.store(true, std::memory_order_release);
            while (!reserving.load(std::memory_order_acquire)) std::this_thread::yield();
            // An in-memory reservation takes microseconds; a tenth of a
            // second is the margin that makes "it did not land" a reading
            // rather than a race.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            landed_inside = reserved.load(std::memory_order_acquire);
            return Status::OK();
        });
    other.join();

    ASSERT_TRUE(visited.ok()) << visited.message();
    EXPECT_EQ(seen, 0);
    EXPECT_FALSE(landed_inside) << "a reservation moved the header while a snapshot was held";
    EXPECT_TRUE(reserved.load());
    EXPECT_EQ(SnapshotCount(enforcer, 7), 1);
}

TEST(AssertionRaceTest, EightCoresReservingIntoOneCabinLoseNoEntryAndCountEveryOne) {
    // Every entry any core appended is on the chain, and the header counts
    // exactly them, with eight writers reserving into one assertion and
    // crossing the chain's page boundary some thirty times.
    //
    // **What this pins is the directory latch**: unarmed, the eight corrupt
    // the maps and the directory between them. **Mutation**, measured: the
    // registry's latch never armed - killed 5 in 5.
    //
    // **What it does not pin is the chain latch**, and the measurement says
    // why rather than leaving it to be assumed. Without it two writers can
    // both find the tail full and both grow, but a growth links from the
    // *live* tail under that page's latch rather than from the page the
    // grower found full, so the second growth chains onto the first's new
    // page instead of relinking past it. Losing a link takes a third writer
    // reading the tail inside another's growth, a window this shape does not
    // open: the chain latch removed survived 10 runs in 10, at two writers
    // and again at eight. The latch stays because the writer's tail and page
    // count are plain fields two threads would otherwise write.
    std::unique_ptr<storage::MemoryPageDevice> device;
    auto store = ArmedStore(device);
    ASSERT_NE(store, nullptr);
    AssertionEnforcer enforcer(/*shared=*/true);
    LiveAssertion cap = CountCap(*store, /*id=*/1, /*bound=*/1'000'000);
    const PageId root = cap.chain.root();
    enforcer.Adopt(std::move(cap));

    constexpr int kWriters = 8;
    constexpr int kPerThread = 1000;
    Rendezvous gate(kWriters);
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kWriters; ++t) {
        threads.emplace_back([&, t] {
            SetCurrentCore(static_cast<std::uint32_t>(t));
            const std::uint64_t txn = 100 + static_cast<std::uint64_t>(t);
            const auto values = Row(t);
            for (int n = 0; n < kPerThread; ++n) {
                AssertionEnforcer::Hold hold;
                gate.Wait();
                const std::uint64_t pk = static_cast<std::uint64_t>(n) * kWriters + t + 1;
                if (!enforcer.AdmitInsert(kOid, values, txn, nullptr, &hold).ok() ||
                    !enforcer
                         .ReserveInsert(*store, /*wal=*/nullptr, txn, hold, kOid, values, pk,
                                        kInvalidPageId, 0)
                         .ok()) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    ASSERT_EQ(failures.load(), 0);

    std::uint64_t on_chain = 0;
    std::uint32_t pages = 0;
    for (PageId id = root; id != kInvalidPageId; ++pages) {
        ASSERT_LT(pages, 1000u) << "the chain's links form a cycle";
        auto page = store->GetForRead(id);
        ASSERT_TRUE(page.ok()) << page.status().message();
        auto view = storage::cabin::BoundCabinPage::Open(page.value().bytes());
        ASSERT_TRUE(view.ok()) << view.status().message();
        on_chain += view.value().entry_count();
        id = view.value().next_page_id();
    }
    EXPECT_GT(pages, 2u) << "the run never grew the chain, so it tested nothing";
    EXPECT_EQ(on_chain, static_cast<std::uint64_t>(kWriters) * kPerThread)
        << "an entry was appended to a page the chain no longer reaches";
    for (int t = 0; t < kWriters; ++t) EXPECT_EQ(SnapshotCount(enforcer, t), kPerThread);
}

}  // namespace
}  // namespace kds::exec
