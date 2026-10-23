#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/device_page_store.hpp"
#include "kds/storage/free_map.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/storage/page_header.hpp"

// The map write order (`src/storage/device_page_store.cpp`, above
// `FlushMaps`): **a headerless bit is durable before the free bit that makes
// its id exist is written.** A device keeps any subset of the writes since
// its last sync, so issuing the headerless map first orders nothing; a power
// cut that kept the free map and lost the headerless map left a Waystone
// directory page allocated, headered to every reader, and failing a checksum
// it never had (`Corruption`), and the headerless-map page's own free bit
// allocated over bytes that never landed. The cut cells lose the power at
// every page write of a flush, keep every subset a seed picks of what was
// unsynced, and read the directory page back after a remount.

namespace kds::storage {
namespace {

using Page = std::array<std::byte, kPageSize>;

constexpr PageId kFirstNewPage = 128;
// Pre-sized, so a cut never reverts a growth and takes a page with it: the
// cells are about the order of the map writes, not the file's length.
constexpr std::uint32_t kDevicePages = 512;
constexpr std::uint64_t kSeedsPerCut = 32;

std::unique_ptr<MemoryPageDevice> MakeDevice() {
    auto created = MemoryPageDevice::Create(/*extent_pages=*/64, kDevicePages);
    EXPECT_TRUE(created.ok()) << created.status().message();
    return created.ok() ? std::move(created.value()) : nullptr;
}

std::unique_ptr<DevicePageStore> OpenStore(PageDevice& device) {
    auto opened = DevicePageStore::Open(device, FrameCapacity{4096}, kFirstNewPage);
    EXPECT_TRUE(opened.ok()) << opened.status().message();
    return opened.ok() ? std::move(opened.value()) : nullptr;
}

PageId MakeHeadered(DevicePageStore& store, std::uint8_t fill) {
    auto made = store.CreateNew();
    EXPECT_TRUE(made.ok()) << made.status().message();
    if (!made.ok()) return kInvalidPageId;
    auto bytes = made.value().second.bytes();
    FormatPage(bytes, PageType::kHeap);
    std::fill(bytes.begin() + kPageBodyOffset, bytes.end(), std::byte{fill});
    return made.value().first;
}

// A directory page as `stats::CreateDirPage` makes one: every child slot
// 0xFF, so the bytes where a header's checksum would sit are not one.
PageId MakeHeaderless(DevicePageStore& store) {
    auto made = store.CreateNewHeaderless();
    EXPECT_TRUE(made.ok()) << made.status().message();
    if (!made.ok()) return kInvalidPageId;
    auto bytes = made.value().second.bytes();
    std::fill(bytes.begin(), bytes.end(), std::byte{0xFF});
    return made.value().first;
}

Page DeviceImage(PageDevice& device, PageId id) {
    Page page{};
    EXPECT_TRUE(device.ReadPage(id, std::span<std::byte, kPageSize>(page)).ok());
    return page;
}

bool BitSet(const Page& map, PageId id) {
    return FreeMapIsAllocated(std::span<const std::byte, kPageSize>(map), FreeMapBitIndexOf(id));
}

bool AllZero(const Page& page) {
    return std::all_of(page.begin(), page.end(), [](std::byte b) { return b == std::byte{0}; });
}

// What a cut must never leave: the directory page refused `Corruption` or
// read as headered, or an allocated headerless-map page the device holds as
// zeros.
void ExpectReadable(MemoryPageDevice& device, PageId directory, const std::string& where) {
    auto store = OpenStore(device);
    ASSERT_NE(store, nullptr) << where;
    if (store->IsAllocated(directory)) {
        auto got = store->GetForRead(directory);
        if (!got.ok()) {
            EXPECT_NE(got.status().code(), StatusCode::kCorruption)
                << where << ": the directory page " << directory
                << " is allocated and refused: " << got.status().message();
        } else {
            EXPECT_TRUE(store->IsHeaderless(directory))
                << where << ": the directory page " << directory
                << " is allocated and read as headered";
        }
    }
    if (store->IsAllocated(kHeaderlessMapPageId)) {
        EXPECT_FALSE(AllZero(DeviceImage(device, kHeaderlessMapPageId)))
            << where << ": the headerless map's id is allocated and its bytes never landed";
    }
}

// The state a cut cell cuts in: a few headered pages synced, then a
// directory page created - with `existing_map`, after a first one was
// synced, so the flush carries the bit alone and not the bitmap's
// installation - and one more headered page, so the flush writes data too.
struct Scene {
    std::unique_ptr<MemoryPageDevice> device;
    std::unique_ptr<DevicePageStore> store;
    PageId directory = kInvalidPageId;
};

Scene Build(bool existing_map) {
    Scene scene;
    scene.device = MakeDevice();
    scene.store = OpenStore(*scene.device);
    for (std::uint8_t i = 0; i < 3; ++i) MakeHeadered(*scene.store, i);
    if (existing_map) MakeHeaderless(*scene.store);
    EXPECT_TRUE(scene.store->Sync().ok());
    scene.directory = MakeHeaderless(*scene.store);
    MakeHeadered(*scene.store, 9);
    return scene;
}

// Power lost at the k-th page write of `flush`, for every k it reaches, and
// at none; each cut kept as `kSeedsPerCut` different subsets of what was
// unsynced.
void CutEveryWrite(bool existing_map, const std::function<Status(DevicePageStore&)>& flush) {
    for (std::uint64_t k = 0;; ++k) {
        bool lost = false;
        for (std::uint64_t seed = 0; seed < kSeedsPerCut; ++seed) {
            Scene scene = Build(existing_map);
            ASSERT_NE(scene.directory, kInvalidPageId);
            scene.device->LosePowerAtWrite(k, nullptr);
            (void)flush(*scene.store);
            lost = scene.device->power_lost();
            scene.store.reset();
            scene.device->CrashPersisting(seed, /*tear=*/false);
            ExpectReadable(*scene.device, scene.directory,
                           "cut at write " + std::to_string(k) + ", seed " + std::to_string(seed));
            if (::testing::Test::HasFailure()) return;
        }
        if (!lost) return;  // the flush finished before its k-th write
    }
}

TEST(MapWriteOrderTest, ACutAnywhereInASyncLeavesTheDirectoryPageReadable) {
    // The bug entry's path: `SYNC`, the synchronous anchor publish and clean
    // shutdown all reach the maps through `Sync` -> `Flush` -> `FlushMaps`.
    // The bitmap's installation and the page's bit are in one flush.
    CutEveryWrite(/*existing_map=*/false, [](DevicePageStore& s) { return s.Sync(); });
}

TEST(MapWriteOrderTest, ACutAnywhereInASyncOfABitAloneLeavesTheDirectoryPageReadable) {
    CutEveryWrite(/*existing_map=*/true, [](DevicePageStore& s) { return s.Sync(); });
}

TEST(MapWriteOrderTest, ACutAnywhereInPersistMapsLeavesTheDirectoryPageReadable) {
    // A reclaim's map sync, whose flush writes whatever region another
    // core's directory create dirtied.
    CutEveryWrite(/*existing_map=*/false, [](DevicePageStore& s) { return s.PersistMaps(); });
}

TEST(MapWriteOrderTest, ACutAnywhereInTheRunTasksHalvesLeavesTheDirectoryPageReadable) {
    // The checkpoint's form, with the data-sync thread not started: the
    // first half's ticket is synced inline, so the second half follows.
    CutEveryWrite(/*existing_map=*/false, [](DevicePageStore& s) -> Status {
        auto ticket = s.WriteHeaderlessMaps();
        if (!ticket.ok()) return ticket.status();
        return s.WriteFreeMaps().status();
    });
}

// The device's write and sync order, for the cells that assert it, and a
// sync that can be held, for the one that runs the data-sync thread.
class TracingDevice final : public PageDevice {
public:
    explicit TracingDevice(PageDevice& inner) : inner_(inner) {}
    std::uint32_t page_capacity() const noexcept override { return inner_.page_capacity(); }
    Status ReadPage(PageId id, std::span<std::byte, kPageSize> out) override {
        if (on_read) on_read(id);
        return inner_.ReadPage(id, out);
    }
    Status WritePage(PageId id, std::span<const std::byte, kPageSize> in) override {
        Note('w', id);
        return inner_.WritePage(id, in);
    }
    Status WritePageRun(PageId first, std::uint32_t n, std::span<const std::byte> in) override {
        for (std::uint32_t i = 0; i < n; ++i) Note('w', first + i);
        return inner_.WritePageRun(first, n, in);
    }
    Status EnsureCapacity(std::uint32_t n) override { return inner_.EnsureCapacity(n); }
    Status Sync() override {
        {
            std::unique_lock lock(mu_);
            cv_.wait(lock, [this] { return !held_; });
            trace_.push_back({'s', 0});
        }
        return inner_.Sync();
    }

    void Hold() {
        std::lock_guard lock(mu_);
        held_ = true;
    }
    void Release() {
        std::lock_guard lock(mu_);
        held_ = false;
        cv_.notify_all();
    }
    void Clear() {
        std::lock_guard lock(mu_);
        trace_.clear();
    }
    std::size_t size() {
        std::lock_guard lock(mu_);
        return trace_.size();
    }
    std::pair<char, PageId> at(std::size_t i) {
        std::lock_guard lock(mu_);
        return trace_.at(i);
    }
    // The first sync, or the first write of `id`, at or after `from`;
    // `size()` when there is none.
    std::size_t Find(char kind, PageId id, std::size_t from = 0) {
        std::lock_guard lock(mu_);
        for (std::size_t i = from; i < trace_.size(); ++i) {
            if (trace_[i].first == kind && (kind == 's' || trace_[i].second == id)) return i;
        }
        return trace_.size();
    }

    // Set and cleared while no other thread reads the device.
    std::function<void(PageId)> on_read;

private:
    void Note(char kind, PageId id) {
        std::lock_guard lock(mu_);
        trace_.push_back({kind, id});
    }
    PageDevice& inner_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool held_ = false;
    std::vector<std::pair<char, PageId>> trace_;
};

TEST(MapWriteOrderTest, AFlushWithNoHeaderlessChangeOwesNoSync) {
    // The common case pays nothing: a volume with no Waystone directory, or
    // one whose directory pages are all placed and flushed.
    auto memory = MakeDevice();
    TracingDevice device(*memory);
    auto store = OpenStore(device);
    ASSERT_NE(store, nullptr);
    MakeHeadered(*store, 1);
    auto ticket = store->WriteHeaderlessMaps();
    ASSERT_TRUE(ticket.ok());
    EXPECT_EQ(ticket.value(), 0u);
    ASSERT_TRUE(store->WriteFreeMaps().ok());

    MakeHeaderless(*store);
    ASSERT_TRUE(store->Sync().ok());
    MakeHeadered(*store, 2);
    device.Clear();
    ASSERT_TRUE(store->PersistMaps().ok());
    // The free map and PersistMaps' own sync, and nothing between them.
    ASSERT_EQ(device.size(), 2u);
    EXPECT_EQ(device.at(0), std::make_pair('w', kFreeMapPageId));
    EXPECT_EQ(device.at(1).first, 's');
}

TEST(MapWriteOrderTest, TheHeaderlessMapIsSyncedBeforeTheFreeMapIsWritten) {
    auto memory = MakeDevice();
    TracingDevice device(*memory);
    auto store = OpenStore(device);
    ASSERT_NE(store, nullptr);
    MakeHeaderless(*store);
    device.Clear();
    ASSERT_TRUE(store->PersistMaps().ok());
    const std::size_t headerless = device.Find('w', kHeaderlessMapPageId);
    const std::size_t free = device.Find('w', kFreeMapPageId);
    ASSERT_LT(headerless, device.size());
    ASSERT_LT(free, device.size());
    EXPECT_LT(device.Find('s', 0, headerless), free)
        << "the free map went out with the headerless map unsynced";
}

TEST(MapWriteOrderTest, TheSecondHalfWaitsForTheFirstHalfsSync) {
    // The run task's halves with the data-sync thread running: the free maps
    // are refused while the ticket is pending, and written once it is done.
    auto memory = MakeDevice();
    TracingDevice device(*memory);
    auto store = OpenStore(device);
    ASSERT_NE(store, nullptr);
    MakeHeaderless(*store);
    store->data_sync().Start();
    device.Hold();
    // Released on every exit, ahead of the store, whose syncer's `Stop`
    // joins the thread after the sync in flight.
    struct ReleaseOnExit {
        TracingDevice& device;
        ~ReleaseOnExit() { device.Release(); }
    } release{device};
    device.Clear();
    auto ticket = store->WriteHeaderlessMaps();
    ASSERT_TRUE(ticket.ok());
    ASSERT_NE(ticket.value(), 0u) << "a headerless map was written and no sync is owed";
    ASSERT_LT(device.Find('w', kHeaderlessMapPageId), device.size());
    auto refused = store->WriteFreeMaps();
    EXPECT_FALSE(refused.ok()) << "the free maps were written ahead of the sync";
    if (!refused.ok()) {
        EXPECT_EQ(refused.status().code(), StatusCode::kOutOfRange);
    }
    EXPECT_EQ(device.Find('w', kFreeMapPageId), device.size())
        << "a refused second half wrote a free map";

    device.Release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!store->data_sync().Done(ticket.value())) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "the ticket never completed";
        std::this_thread::yield();
    }
    auto wrote = store->WriteFreeMaps();
    ASSERT_TRUE(wrote.ok()) << wrote.status().message();
    EXPECT_EQ(wrote.value(), 1u);
    EXPECT_LT(device.Find('s', 0), device.Find('w', kFreeMapPageId));
    // Nothing left: a second call writes nothing.
    EXPECT_EQ(store->WriteFreeMaps().value(), 0u);
    store->data_sync().Stop();
}

TEST(MapWriteOrderTest, ABlockingFlushAdoptsTheRunTasksStage) {
    // A `Sync` or a reclaim's `PersistMaps` between the run task's halves
    // finds the regions clean - the first half cleared them - and must still
    // cover what the first half copied (BF-R5): it writes the staged free
    // maps itself, and the second half then finds nothing.
    auto memory = MakeDevice();
    TracingDevice device(*memory);
    auto store = OpenStore(device);
    ASSERT_NE(store, nullptr);
    const PageId directory = MakeHeaderless(*store);
    ASSERT_TRUE(store->WriteHeaderlessMaps().ok());
    device.Clear();
    ASSERT_TRUE(store->PersistMaps().ok());
    ASSERT_LT(device.Find('w', kFreeMapPageId), device.size())
        << "a returning PersistMaps left a copied free map unwritten";
    EXPECT_EQ(store->WriteFreeMaps().value(), 0u);
    EXPECT_TRUE(BitSet(DeviceImage(*memory, kFreeMapPageId), directory));
}

TEST(MapWriteOrderTest, ABlockingFlushSyncsAPendingStageBeforeItWritesItsFreeMaps) {
    // A `SYNC` or a reclaim's `PersistMaps` that lands inside the run task's
    // park, while the first half's sync is still in flight: it adopts the
    // stage and must not write its free maps until a sync that began after
    // the headerless map was written has ended.
    auto memory = MakeDevice();
    TracingDevice device(*memory);
    auto store = OpenStore(device);
    ASSERT_NE(store, nullptr);
    store->SetLatchArmed(true);
    MakeHeaderless(*store);
    store->data_sync().Start();
    device.Hold();
    struct ReleaseOnExit {
        TracingDevice& device;
        ~ReleaseOnExit() { device.Release(); }
    } release{device};
    device.Clear();
    auto ticket = store->WriteHeaderlessMaps();
    ASSERT_TRUE(ticket.ok());
    ASSERT_NE(ticket.value(), 0u);
    ASSERT_FALSE(store->data_sync().Done(ticket.value()));

    Status persisted = Status::OK();
    std::thread flusher([&] { persisted = store->PersistMaps(); });
    // Long enough for a flusher that did not wait to have written its free
    // map ahead of the held sync.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    device.Release();
    flusher.join();
    ASSERT_TRUE(persisted.ok()) << persisted.message();
    const std::size_t headerless = device.Find('w', kHeaderlessMapPageId);
    const std::size_t free = device.Find('w', kFreeMapPageId);
    ASSERT_LT(headerless, device.size());
    ASSERT_LT(free, device.size()) << "the adopting flush left the staged free map unwritten";
    EXPECT_LT(device.Find('s', 0, headerless), free)
        << "the adopting flush wrote the free map while the first half's sync was pending";
    EXPECT_EQ(store->WriteFreeMaps().value(), 0u);
    store->data_sync().Stop();
}

TEST(MapWriteOrderTest, AFailedSyncBetweenTheHalvesWritesNoFreeMap) {
    // `Done` is true on a failure as well, so only `Result` keeps a free map
    // from following a headerless map whose sync failed.
    auto memory = MakeDevice();
    TracingDevice device(*memory);
    auto store = OpenStore(device);
    ASSERT_NE(store, nullptr);
    MakeHeaderless(*store);
    device.Clear();
    memory->FailNextSync(Status::IoError("injected sync failure"));
    auto ticket = store->WriteHeaderlessMaps();  // no thread: synced inline
    ASSERT_TRUE(ticket.ok());
    ASSERT_NE(ticket.value(), 0u);
    ASSERT_TRUE(store->data_sync().Done(ticket.value()));

    auto second = store->WriteFreeMaps();
    EXPECT_FALSE(second.ok()) << "the second half wrote over a failed sync";
    if (!second.ok()) {
        EXPECT_EQ(second.status().code(), StatusCode::kIoError);
    }
    EXPECT_FALSE(store->PersistMaps().ok()) << "an adopting flush wrote over a failed sync";
    EXPECT_EQ(device.Find('w', kFreeMapPageId), device.size())
        << "a free map was written after its headerless map's sync failed";

    // The failure gave the stage back: the next flush copies the headerless
    // map again (and is refused at its sync, the failure being sticky).
    const std::size_t from = device.size();
    EXPECT_FALSE(store->PersistMaps().ok());
    EXPECT_LT(device.Find('w', kHeaderlessMapPageId, from), device.size())
        << "the failed copy's headerless map is no longer owed";
    EXPECT_EQ(device.Find('w', kFreeMapPageId), device.size());
}

TEST(MapWriteOrderTest, AFailedMapWriteOwesTheHeaderlessMapAndItsSyncAgain) {
    // A failed write gives the copy back. The retry must write the
    // headerless map and sync before the free map: owing only the free map
    // would put it out with nothing ahead of it - the bug, again.
    for (std::uint64_t skip = 0; skip < 2; ++skip) {  // the headerless write, then the free
        SCOPED_TRACE("failing write " + std::to_string(skip));
        auto memory = MakeDevice();
        TracingDevice device(*memory);
        auto store = OpenStore(device);
        ASSERT_NE(store, nullptr);
        MakeHeaderless(*store);
        memory->FailNextWrite(Status::IoError("injected write failure"), skip);
        ASSERT_FALSE(store->PersistMaps().ok());
        device.Clear();
        ASSERT_TRUE(store->PersistMaps().ok());
        const std::size_t headerless = device.Find('w', kHeaderlessMapPageId);
        const std::size_t free = device.Find('w', kFreeMapPageId);
        ASSERT_LT(headerless, device.size()) << "the retry did not write the headerless map";
        ASSERT_LT(free, device.size());
        EXPECT_LT(device.Find('s', 0, headerless), free)
            << "the retry wrote the free map with the headerless map unsynced";
    }
}

TEST(MapWriteOrderTest, TheSecondHalfAnswersAFailedAdoptersLoss) {
    // A blocking flush adopts the run task's stage and fails writing it: the
    // stage goes back to the regions, and the run's second half, finding it
    // empty, must not report the maps written - its anchor would stand for
    // free bits no device holds.
    auto memory = MakeDevice();
    auto store = OpenStore(*memory);
    ASSERT_NE(store, nullptr);
    const PageId directory = MakeHeaderless(*store);
    ASSERT_TRUE(store->WriteHeaderlessMaps().ok());
    memory->FailNextWrite(Status::IoError("injected write failure"));
    ASSERT_FALSE(store->PersistMaps().ok());
    auto second = store->WriteFreeMaps();
    EXPECT_FALSE(second.ok()) << "the run's second half reported maps a failed flush gave back";

    ASSERT_TRUE(store->PersistMaps().ok());
    EXPECT_TRUE(BitSet(DeviceImage(*memory, kFreeMapPageId), directory));
}

TEST(MapWriteOrderTest, AMapCopyBetweenAHeaderlessClaimAndItsMarkLeavesTheIdOut) {
    // Above one core a flush on another core can copy the region after a
    // directory create has claimed its id and before it has set the
    // headerless bit. That copy must not put the free bit on the device:
    // nothing would have written the headerless bit ahead of it. The
    // create's own device read of the id is in the window; the flush runs
    // inside that read.
    auto memory = MakeDevice();
    TracingDevice device(*memory);
    auto store = OpenStore(device);
    ASSERT_NE(store, nullptr);
    MakeHeaderless(*store);  // the bitmap exists, so the window carries a bit alone
    ASSERT_TRUE(store->Sync().ok());

    PageId claimed = kInvalidPageId;
    Page free_in_window{};
    device.on_read = [&](PageId id) {
        if (claimed != kInvalidPageId || id < kFirstNewPage) return;
        claimed = id;
        EXPECT_TRUE(store->PersistMaps().ok());
        free_in_window = DeviceImage(*memory, kFreeMapPageId);
    };
    const PageId directory = MakeHeaderless(*store);
    device.on_read = nullptr;
    ASSERT_EQ(claimed, directory) << "the create read no other page first";
    EXPECT_FALSE(BitSet(free_in_window, directory))
        << "a copy in the claim-to-mark window put the free bit on the device alone";

    ASSERT_TRUE(store->PersistMaps().ok());
    EXPECT_TRUE(BitSet(DeviceImage(*memory, kFreeMapPageId), directory));
    EXPECT_TRUE(BitSet(DeviceImage(*memory, kHeaderlessMapPageId), directory));
}

TEST(MapWriteOrderTest, AHeaderlessBitWhoseFreeBitWasLostIsClearedAtMount) {
    // The state a cut between the sync and the free-map write leaves: the
    // headerless bit durable, the id free. Kept, it would make the id's
    // next, headered, owner headerless - written unstamped, read unverified,
    // and refused by a later `FreePage`.
    auto device = MakeDevice();
    PageId directory = kInvalidPageId;
    {
        auto store = OpenStore(*device);
        ASSERT_NE(store, nullptr);
        directory = MakeHeaderless(*store);
        ASSERT_TRUE(store->Sync().ok());
    }
    {
        Page free_map = DeviceImage(*device, kFreeMapPageId);
        FreeMapRelease(std::span<std::byte, kPageSize>(free_map), FreeMapBitIndexOf(directory));
        StampPageChecksum(std::span<std::byte, kPageSize>(free_map));
        ASSERT_TRUE(
            device->WritePage(kFreeMapPageId, std::span<const std::byte, kPageSize>(free_map)).ok());
        ASSERT_TRUE(device->Sync().ok());
    }
    {
        auto store = OpenStore(*device);
        ASSERT_NE(store, nullptr);
        EXPECT_FALSE(store->IsHeaderless(directory));
        const PageId reissued = MakeHeadered(*store, 7);
        ASSERT_EQ(reissued, directory) << "the cursor did not reissue the id";
        EXPECT_FALSE(store->IsHeaderless(reissued));
        ASSERT_TRUE(store->Sync().ok());
    }
    auto store = OpenStore(*device);
    ASSERT_NE(store, nullptr);
    EXPECT_FALSE(store->IsHeaderless(directory))
        << "the stale headerless bit outlived the id's reissue as a headered page";
    EXPECT_FALSE(BitSet(DeviceImage(*device, kHeaderlessMapPageId), directory));
    auto got = store->GetForRead(directory);
    ASSERT_TRUE(got.ok()) << got.status().message();
    EXPECT_EQ(got.value().bytes()[kPageBodyOffset], std::byte{7});
}

TEST(MapWriteOrderTest, AClaimGivenBackAfterItsFreeBitLandedIsNeverAHeaderlessId) {
    // A split claims ids ahead (`PageClaims`) and gives back what it did not
    // use. Above one core another core's flush can copy the region while
    // the claims stand, so the device holds their free bits. Had the cursor
    // stepped back over a given-back id, a directory create could take it:
    // its free bit durable, its headerless bit not, and its unstamped bytes
    // written by any writeback before the next map flush - the bug entry's
    // `Corruption`, with no flush in between to order anything.
    auto device = MakeDevice();
    PageId directory = kInvalidPageId;
    {
        auto store = OpenStore(*device);
        ASSERT_NE(store, nullptr);
        MakeHeadered(*store, 1);
        ASSERT_TRUE(store->Sync().ok());
        {
            auto claims = store->ClaimNew(2);
            ASSERT_TRUE(claims.ok()) << claims.status().message();
            ASSERT_TRUE(store->PersistMaps().ok());  // the other core's flush
        }  // both given back, highest first
        directory = MakeHeaderless(*store);
        ASSERT_NE(directory, kInvalidPageId);
        const PageId ids[] = {directory};
        ASSERT_TRUE(store->WriteBackPages(ids, nullptr).ok());
        ASSERT_TRUE(device->Sync().ok());  // a later data-file sync covers it
        store.reset();
        device->Crash();  // before the next map flush
    }
    ExpectReadable(*device, directory, "a directory page on a given-back claim");
}

}  // namespace
}  // namespace kds::storage
