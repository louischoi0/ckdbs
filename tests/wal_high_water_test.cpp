#include "kds/wal/high_water.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kds/server/superblock.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/extent_lease.hpp"
#include "kds/storage/free_map.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/wal/analysis.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/redo.hpp"
#include "kds/wal/stream.hpp"

// RC04 - RV4's high-water repair (docs/workplan-wal-recovery.md).
//
// The headline test is the first one and it is the whole task: a run
// allocates pages and logs them, a crash reverts the free map below what
// the log names, recovery replays - and the *next allocation* must not be
// handed one of those pages. Without RaiseHighWater it is, and the page's
// bytes are overwritten with zeroes, which the test reads back and fails
// on. That is the "fails without the repair" the workplan asks for, and it
// is why the assertion is on page bytes and not only on an id.
//
// The rest is the contract around it: the floor only rises, a log that
// names no page raises nothing, and a store that *cannot* raise its floor
// refuses the mount instead of reporting a repair that did not happen.

namespace kds::wal {
namespace {

constexpr std::uint64_t kSegmentSize = 16 * 1024;
constexpr PageId kFirstUser = server::kFirstUserPageId;  // 128

using Page = std::array<std::byte, kPageSize>;

std::unique_ptr<storage::MemoryPageDevice> MakeDevice() {
    auto created = storage::MemoryPageDevice::Create(/*extent_pages=*/8, /*initial_pages=*/0);
    EXPECT_TRUE(created.ok()) << created.status().message();
    return created.ok() ? std::move(created.value()) : nullptr;
}

std::unique_ptr<storage::DevicePageStore> OpenStore(storage::PageDevice& device) {
    auto opened = storage::DevicePageStore::Open(device, kFirstUser);
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    return opened.ok() ? std::move(opened.value()) : nullptr;
}

// A recognizable body, so a later read proves the bytes are the ones the
// pre-crash run wrote and not a fresh allocation's zeroes.
void Fill(std::span<std::byte, kPageSize> page, std::uint8_t seed) {
    storage::FormatPage(page, PageType::kHeap);
    for (std::size_t i = storage::kPageBodyOffset; i < kPageSize; ++i) {
        page[i] = static_cast<std::byte>((i * 3u + seed) & 0xFF);
    }
}

bool Matches(std::span<const std::byte, kPageSize> page, std::uint8_t seed) {
    for (std::size_t i = storage::kPageBodyOffset; i < kPageSize; ++i) {
        if (page[i] != static_cast<std::byte>((i * 3u + seed) & 0xFF)) return false;
    }
    return true;
}

Page ReadDevicePage(storage::PageDevice& device, PageId page_id) {
    Page out{};
    Status s = device.ReadPage(page_id, std::span<std::byte, kPageSize>(out));
    EXPECT_TRUE(s.ok()) << s.message();
    return out;
}

// An AnalysisResult built by hand, for the cases whose input is a number
// rather than a stream - the range and sentinel refusals below. The
// function's contract is over the struct, so scripting a log to reach a
// page id of 70,000 would test the log writer, not the repair.
AnalysisResult Named(PageId max_page_id, std::uint64_t max_txn_id = 0) {
    AnalysisResult out;
    out.max_page_id = max_page_id;
    out.max_txn_id = max_txn_id;
    return out;
}

// ---- The task ------------------------------------------------------------

TEST(HighWaterTest, ARecoveredStoreDoesNotHandOutAPageTheLogNamed) {
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);
    auto log_owned = MemoryLogDevice::Create(kSegmentSize);
    ASSERT_TRUE(log_owned.ok()) << log_owned.status().message();
    MemoryLogDevice& log = *log_owned.value();

    // ---- The run before the crash ----------------------------------------
    //
    // Four pages allocated and written, and the free map durable *before*
    // them - which is the state the crash reverts to.
    Page clean_map{};
    {
        auto store = OpenStore(*device);
        ASSERT_NE(store, nullptr);
        ASSERT_TRUE(store->Sync().ok());
        clean_map = ReadDevicePage(*device, storage::kFreeMapPageId);

        for (std::uint8_t i = 0; i < 4; ++i) {
            auto created = store->CreateNew();
            ASSERT_TRUE(created.ok()) << created.status().message();
            ASSERT_EQ(created.value().first, kFirstUser + i);
            Fill(created.value().second.bytes(), static_cast<std::uint8_t>(0x40 + i));
        }
        ASSERT_TRUE(store->Sync().ok());
    }
    const PageId kReplayed = kFirstUser;      // 128, named by a PAGE_INIT
    const PageId kCheckpointed = kFirstUser + 3;  // 131, named only by the dirty table
    ASSERT_TRUE(Matches(ReadDevicePage(*device, kCheckpointed), 0x43));

    // The stream those four pages were written under. Two shapes on
    // purpose: 128 has a PAGE_INIT, so redo re-creates it and sets its map
    // bit unaided; 131 appears only in a CHECKPOINT_BEGIN dirty entry with
    // a recLSN of 0 - dirty but described by no record (wal.md §11-3) - so
    // no applier ever runs for it and nothing but this repair protects it.
    {
        auto s = WalStream::Open(&log, /*core_id=*/0);
        ASSERT_TRUE(s.ok()) << s.status().message();

        const CheckpointDirtyPage dirty[] = {{kCheckpointed, /*rec_lsn=*/0}};
        std::vector<std::byte> cp(CheckpointBeginSize(0, 1), std::byte{0});
        auto encoded = EncodeCheckpointBegin(cp, {}, dirty);
        ASSERT_TRUE(encoded.ok()) << encoded.status().message();
        ASSERT_TRUE(s.value()
                        ->Append({RecordType::kCheckpointBegin, kNoTxnId, kInvalidPageId},
                                 std::span(cp).first(encoded.value()))
                        .ok());

        std::vector<std::byte> init(kPageInitPayloadSize, std::byte{0});
        const PageInitPayload fields{/*min_key=*/1, static_cast<std::uint8_t>(PageType::kHeap),
                                     {0, 0, 0}, /*reserved2=*/0, /*owner_oid=*/0};
        ASSERT_TRUE(EncodePageInit(init, fields).ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kPageInit, 9, kReplayed}, init).ok());
        ASSERT_TRUE(s.value()->Sync().ok());
    }

    // ---- The crash that reverts the mark ---------------------------------
    //
    // The free map goes back to the image that was durable before the four
    // allocations - exactly what a crash between a data write-back and the
    // map write-back leaves behind (device_page_store.cpp's FlushMaps
    // ordering note calls the result an orphan; the log is what makes it
    // more than one).
    ASSERT_TRUE(device->WritePage(storage::kFreeMapPageId,
                                  std::span<const std::byte, kPageSize>(clean_map))
                    .ok());
    ASSERT_TRUE(device->Sync().ok());

    // ---- The mount -------------------------------------------------------
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);
    EXPECT_FALSE(store->IsAllocated(kReplayed)) << "the crash did not revert the mark";
    EXPECT_FALSE(store->IsAllocated(kCheckpointed));

    auto analysis = Analyze(log, /*core_id=*/0, AnalysisStart{});
    ASSERT_TRUE(analysis.ok()) << analysis.status().message();
    EXPECT_EQ(analysis.value().max_page_id, kCheckpointed);

    auto redone = Redo(log, /*core_id=*/0, *store, analysis.value());
    ASSERT_TRUE(redone.ok()) << redone.status().message();
    EXPECT_EQ(redone.value().pages_created, 1u) << "redo should have re-created page 128 alone";
    EXPECT_TRUE(store->IsAllocated(kReplayed)) << "redo's CreateAt closes the PAGE_INIT case";
    EXPECT_FALSE(store->IsAllocated(kCheckpointed)) << "and leaves the other one open";

    auto repair = RaiseHighWater(*store, analysis.value());
    ASSERT_TRUE(repair.ok()) << repair.status().message();
    EXPECT_TRUE(repair.value().page_floor_raised);
    EXPECT_EQ(repair.value().page_floor, kCheckpointed + 1);
    // The repair raises a floor; it does not invent a page. Marking 131
    // allocated would turn Get()'s honest NotFound into a read of whatever
    // the device holds there.
    EXPECT_FALSE(store->IsAllocated(kCheckpointed));

    // ---- What the repair buys --------------------------------------------
    //
    // Four allocations, none of which may be a page the log named. Without
    // the raise the first is 129 and the third is 131, and the Sync below
    // writes zeroes over bytes the pre-crash run wrote.
    for (int i = 0; i < 4; ++i) {
        auto created = store->CreateNew();
        ASSERT_TRUE(created.ok()) << created.status().message();
        EXPECT_GT(created.value().first, analysis.value().max_page_id)
            << "allocation " << i << " handed out a page the log names";
        storage::FormatPage(created.value().second.bytes(), PageType::kHeap);
    }
    ASSERT_TRUE(store->Sync().ok());

    EXPECT_TRUE(Matches(ReadDevicePage(*device, kCheckpointed), 0x43))
        << "an allocation overwrote a page the log still names";
}

// ---- The contract around it ---------------------------------------------

TEST(HighWaterTest, ALogThatNamesNoPageRaisesNothing) {
    // A clean shutdown's stream: transaction boundaries and nothing else.
    // "Nothing to repair" must be distinguishable from "the repair did not
    // run", so it is reported rather than inferred from a floor of zero.
    storage::InMemoryPageStore store(kFirstUser);
    auto repair = RaiseHighWater(store, Named(kInvalidPageId));
    ASSERT_TRUE(repair.ok()) << repair.status().message();
    EXPECT_FALSE(repair.value().page_floor_raised);
    EXPECT_EQ(repair.value().page_floor, kInvalidPageId);

    auto created = store.CreateNew();
    ASSERT_TRUE(created.ok());
    EXPECT_EQ(created.value().first, kFirstUser) << "an empty repair moved the floor";
}

TEST(HighWaterTest, TheFloorOnlyEverRises) {
    storage::InMemoryPageStore store(kFirstUser);
    ASSERT_TRUE(RaiseHighWater(store, Named(500)).ok());
    // A second, lower recovery result - a re-run over a shorter range, or a
    // second core's stream. It must not walk the floor back down.
    ASSERT_TRUE(RaiseHighWater(store, Named(200)).ok());
    ASSERT_TRUE(RaiseHighWater(store, Named(500)).ok());

    auto created = store.CreateNew();
    ASSERT_TRUE(created.ok());
    EXPECT_EQ(created.value().first, 501u);
}

TEST(HighWaterTest, ThePlacedPageIsSkippedAndTheRaiseIsStillWhatCoversTheLog) {
    // **Two different hazards, and only one of them is the raise's.**
    //
    // A replay CreateAt's the pages the log describes. Handing one of those
    // back from CreateNew() is a bug the store answers itself: the id is
    // occupied, so the cursor steps over it. This test used to assert the
    // opposite - that CreateNew() wedged at AlreadyExists until a raise
    // moved the counter - which was a deficiency of InMemoryPageStore
    // written down rather than a contract. DevicePageStore never had it,
    // because its CreateAt marks the free map its CreateNew searches.
    storage::InMemoryPageStore store(kFirstUser);
    ASSERT_TRUE(store.CreateAt(kFirstUser).ok());

    auto after_place = store.CreateNew();
    ASSERT_TRUE(after_place.ok()) << after_place.status().message();
    EXPECT_EQ(after_place.value().first, kFirstUser + 1)
        << "an occupied id must be stepped over, not reported as a collision";

    // The raise covers what skipping cannot: a page the log *names* but that
    // the crash left unallocated here. Nothing about it is occupied, so no
    // amount of stepping over existing ids protects it - which is why RV4
    // exists and why this half of the test outlives the other.
    ASSERT_TRUE(RaiseHighWater(store, Named(kFirstUser + 40)).ok());
    auto after_raise = store.CreateNew();
    ASSERT_TRUE(after_raise.ok()) << after_raise.status().message();
    EXPECT_EQ(after_raise.value().first, kFirstUser + 41);
}

// ---- Refusals ------------------------------------------------------------

TEST(HighWaterTest, AStoreThatCannotRaiseItsFloorRefusesTheMount) {
    // The default PageStore answers Unsupported, and the repair passes it
    // up rather than treating "no floor to raise" as success. A recovery
    // that could not close the hazard must not serve the database.
    class FloorlessStore final : public storage::PageStore {
    public:
        StatusOr<std::span<std::byte, kPageSize>> CreateAtUnpinned(PageId) override {
            return Status::Unsupported("test");
        }
        StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewUnpinned() override {
            return Status::Unsupported("test");
        }
        StatusOr<std::span<std::byte, kPageSize>> GetUnpinned(PageId) override {
            return Status::NotFound("test");
        }
    };

    FloorlessStore store;
    auto repair = RaiseHighWater(store, Named(300));
    ASSERT_FALSE(repair.ok());
    EXPECT_EQ(repair.status().code(), StatusCode::kUnsupported) << repair.status().message();
}

TEST(HighWaterTest, ALeasedStoreRefusesRatherThanSilentlyDoingNothing) {
    // A leased core allocates from its extent and never consults the floor,
    // so raising it would change nothing. The multicore equivalent - core
    // 0's ExtentAllocator starting above the recovered high-water - belongs
    // to the grant path, and this refusal is what keeps the gap named
    // instead of hidden behind a repair that reported success.
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    storage::LeasedIdSource lease(storage::Extent{/*first=*/1024, /*count=*/64});
    store->SetCoreOwnership(/*core_id=*/1, &lease, /*system_page_limit=*/kFirstUser);

    auto repair = RaiseHighWater(*store, Named(1100));
    ASSERT_FALSE(repair.ok());
    EXPECT_EQ(repair.status().code(), StatusCode::kUnsupported) << repair.status().message();
}

TEST(HighWaterTest, APageIdBeyondTheDesignCeilingRefusesTheMount) {
    // The log naming a page this build could not have written is a refusal,
    // not a repair. What "could not have written" means moved when the free
    // map became multi-page: it was one bitmap page's 65,280 ids, and it is
    // now the 2^31-page design ceiling (FM3).
    auto device = MakeDevice();
    ASSERT_NE(device, nullptr);
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);

    auto repair = RaiseHighWater(*store, Named(kMaxPageCount + 10));
    ASSERT_FALSE(repair.ok());
    EXPECT_EQ(repair.status().code(), StatusCode::kOutOfRange) << repair.status().message();
}

TEST(HighWaterTest, APageIdWhoseSuccessorIsTheInvalidSentinelIsCorruption) {
    // One below the reserved id (invariant 1): the floor would be
    // kInvalidPageId, which every range check downstream reads as "unset".
    storage::InMemoryPageStore store(kFirstUser);
    auto repair = RaiseHighWater(store, Named(kInvalidPageId - 1));
    ASSERT_FALSE(repair.ok());
    EXPECT_EQ(repair.status().code(), StatusCode::kCorruption) << repair.status().message();
}

// ---- The transaction-id half --------------------------------------------

TEST(HighWaterTest, TheTransactionCeilingIsOnePastTheHighestIdTheLogNames) {
    storage::InMemoryPageStore store(kFirstUser);
    auto repair = RaiseHighWater(store, Named(300, /*max_txn_id=*/9000));
    ASSERT_TRUE(repair.ok()) << repair.status().message();
    EXPECT_EQ(repair.value().next_trx_id, 9001u);

    // Applied by the caller, because the superblock sits above wal/. The
    // point of computing it here is that the number has one derivation.
    server::SuperBlock sb = server::SuperBlock::CreateFresh(/*now_unix_seconds=*/1);
    ASSERT_TRUE(sb.SetNextTrxId(repair.value().next_trx_id).ok());
    EXPECT_EQ(sb.next_trx_id(), 9001u);
    // And it never falls: the sequence's own rule, relied on here so the
    // repair can be re-run without reissuing ids.
    EXPECT_FALSE(sb.SetNextTrxId(5).ok());
}

TEST(HighWaterTest, ALogThatNamesNoTransactionOwesNoCeiling) {
    storage::InMemoryPageStore store(kFirstUser);
    auto repair = RaiseHighWater(store, Named(300));
    ASSERT_TRUE(repair.ok()) << repair.status().message();
    EXPECT_EQ(repair.value().next_trx_id, 0u) << "0 means the caller has nothing to apply";
}

}  // namespace
}  // namespace kds::wal
