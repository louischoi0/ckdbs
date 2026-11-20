// DB-S1: the image a write fetch owes (`device_page_store.hpp`, "The write
// fetch's image"; `instructions/v3.2.0/workorder-db-torn-page-healing.md`).
//
// The store's half, against a gate that records what it is asked to log: a
// frame owes an image from its fault-in and from every writeback copy, the
// first write fetch after that logs the page as it stands - before its
// holder changes it - and stamps the page with the image's LSN, and a read
// fetch, a created frame, a class no record describes and a store whose
// images are off log nothing. The mount's half (the heal) is
// `wal_redo_test.cpp`'s RedoHealTest; the end-to-end tear is
// `sim_loop_test.cpp`'s.

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/wal/durability.hpp"

namespace kds::storage {
namespace {

// Durable at once, and every image recorded with the bytes it was given.
class ImageGate final : public wal::WalDurability {
public:
    wal::Lsn durable_lsn() const noexcept override { return next_; }
    Status EnsureDurable(wal::Lsn) override { return Status::OK(); }
    StatusOr<LoggedPageImage> LogPageImage(PageId page_id,
                                           std::span<const std::byte, kPageSize> page) override {
        if (refuse) return Status::IoError("refused for the test");
        images.push_back({page_id, std::vector<std::byte>(page.begin(), page.end())});
        next_ += kRecordBytes;
        return LoggedPageImage{next_ - kRecordBytes, kRecordBytes};
    }
    static constexpr std::size_t kRecordBytes = 8224;
    struct Image {
        PageId page_id;
        std::vector<std::byte> bytes;
    };
    std::vector<Image> images;
    bool refuse = false;

private:
    wal::Lsn next_ = 4096;  // a real record LSN is never 0
};

class FetchImageTest : public ::testing::Test {
protected:
    void SetUp() override {
        device_ = std::move(MemoryPageDevice::Create(/*extent_pages=*/8, /*initial_pages=*/0).value());
        Reopen();
    }

    // A fresh store over the device, the gate installed and images on, so
    // every page the test touches is faulted from the device.
    void Reopen() {
        if (store_ != nullptr) ASSERT_TRUE(store_->Sync().ok());
        store_.reset();
        auto store = DevicePageStore::Open(*device_, FrameCapacity{4096}, /*first_new_page_id=*/16);
        ASSERT_TRUE(store.ok()) << store.status().message();
        store_ = std::move(store.value());
        store_->SetWalGate(&gate_);
        store_->EnableFetchImages(true);
    }

    // A formatted page of `type`, owned by `owner`, written to the device.
    PageId MakePage(PageType type, std::uint64_t owner = 99, std::byte fill = std::byte{0x5A}) {
        auto created = store_->CreateNew();
        EXPECT_TRUE(created.ok()) << created.status().message();
        const PageId id = created.value().first;
        auto bytes = created.value().second.bytes();
        FormatPage(bytes, type, /*flags=*/0, owner);
        bytes[kPageBodyOffset] = fill;
        return id;
    }

    std::uint64_t PageLsnOf(PageId id) {
        auto page = store_->GetForRead(id);
        EXPECT_TRUE(page.ok());
        return GetPageLsn(page.value().bytes());
    }

    ImageGate gate_;
    std::unique_ptr<MemoryPageDevice> device_;
    std::unique_ptr<DevicePageStore> store_;
};

TEST_F(FetchImageTest, AFaultedPageIsImagedAtItsFirstWriteFetchAndStampedWithIt) {
    const PageId id = MakePage(PageType::kHeap);
    ASSERT_TRUE(gate_.images.empty()) << "a created page owes no image";
    Reopen();
    {
        auto page = store_->Get(id);
        ASSERT_TRUE(page.ok()) << page.status().message();
        ASSERT_EQ(gate_.images.size(), 1u);
        EXPECT_EQ(gate_.images[0].page_id, id);
        // The image is the page before its holder writes: what the device
        // held, its header's page_lsn aside.
        EXPECT_EQ(gate_.images[0].bytes[kPageBodyOffset], std::byte{0x5A});
        page.value().bytes()[kPageBodyOffset] = std::byte{0x77};
    }
    EXPECT_EQ(PageLsnOf(id), 4096u) << "the page is not stamped with its image";
    // The debt is paid: a second write fetch logs nothing.
    { ASSERT_TRUE(store_->Get(id).ok()); }
    EXPECT_EQ(gate_.images.size(), 1u);
    EXPECT_EQ(store_->pool_counters().fetch_images, 1u);
    // DH-R6: the bytes are the record's as the gate appended it, not a
    // count times a size.
    EXPECT_EQ(store_->pool_counters().fetch_image_bytes, ImageGate::kRecordBytes);
}

TEST_F(FetchImageTest, EveryWritebackCopyOwesTheNextImage) {
    const PageId id = MakePage(PageType::kBtreeLeaf);
    ASSERT_TRUE(store_->Sync().ok());  // the create's first copy
    { ASSERT_TRUE(store_->Get(id).ok()); }
    ASSERT_EQ(gate_.images.size(), 1u);
    ASSERT_TRUE(store_->Sync().ok());  // the page was dirtied: copied again
    {
        auto page = store_->Get(id);
        ASSERT_TRUE(page.ok());
    }
    EXPECT_EQ(gate_.images.size(), 2u) << "a copy did not set the debt again";
}

TEST_F(FetchImageTest, AReadFetchAndANestedWriteFetchLogNothing) {
    const PageId id = MakePage(PageType::kHeap);
    Reopen();
    { ASSERT_TRUE(store_->GetForRead(id).ok()); }
    EXPECT_TRUE(gate_.images.empty()) << "a read fetch logged an image";
    {
        auto outer = store_->Get(id);
        ASSERT_TRUE(outer.ok());
        auto inner = store_->Get(id);  // re-entrant on the owning core
        ASSERT_TRUE(inner.ok());
    }
    EXPECT_EQ(gate_.images.size(), 1u) << "the nested fetch paid a debt the outer one paid";
}

TEST_F(FetchImageTest, ClassesNoRecordDescribesAreNeverImaged) {
    const PageId waystone = MakePage(PageType::kWaystone);
    const PageId unformatted = [&] {
        auto created = store_->CreateNew();
        EXPECT_TRUE(created.ok());
        return created.value().first;
    }();
    const PageId owned = MakePage(PageType::kHeap, /*owner=*/130);
    const PageId named = MakePage(PageType::kHeap, /*owner=*/0);
    Reopen();
    ASSERT_TRUE(store_->AddUnloggedOwner(130).ok());
    ASSERT_TRUE(store_->AddUnloggedPage(named).ok());
    // `unformatted` is page type kInvalid: written back with its checksum
    // stamped, so it reads, and is no logged class.
    for (const PageId id : {waystone, unformatted, owned, named}) {
        auto page = store_->Get(id);
        ASSERT_TRUE(page.ok()) << id << ": " << page.status().message();
    }
    EXPECT_TRUE(gate_.images.empty()) << gate_.images.size() << " unlogged page(s) imaged";

    // A registration given back makes the class an imaged one again, at the
    // next copy - the debt it dropped is not owed retroactively.
    store_->RemoveUnloggedOwner(130);
    ASSERT_TRUE(store_->Sync().ok());
    { ASSERT_TRUE(store_->Get(owned).ok()); }
    EXPECT_EQ(gate_.images.size(), 1u);
}

TEST_F(FetchImageTest, ImagesOffLogNothingAndTheDebtWaitsForThemToComeOn) {
    const PageId id = MakePage(PageType::kHeap);
    Reopen();
    store_->EnableFetchImages(false);  // as a mount runs
    { ASSERT_TRUE(store_->Get(id).ok()); }
    EXPECT_TRUE(gate_.images.empty());
    store_->EnableFetchImages(true);
    { ASSERT_TRUE(store_->Get(id).ok()); }
    EXPECT_EQ(gate_.images.size(), 1u) << "the fault-in's debt was lost while images were off";
}

TEST_F(FetchImageTest, ARefusedImageFailsTheFetchAndStaysOwed) {
    const PageId id = MakePage(PageType::kHeap);
    Reopen();
    gate_.refuse = true;
    auto refused = store_->Get(id);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kIoError) << refused.status().message();
    EXPECT_EQ(store_->live_pins(), 0u) << "the refused fetch kept its pin";
    gate_.refuse = false;
    { ASSERT_TRUE(store_->Get(id).ok()); }
    EXPECT_EQ(gate_.images.size(), 1u);
}

TEST_F(FetchImageTest, TheRegistryRefusesWhenFull) {
    for (std::uint64_t oid = 1000; oid < 1000 + 32; ++oid) {
        ASSERT_TRUE(store_->AddUnloggedOwner(oid).ok());
    }
    EXPECT_EQ(store_->AddUnloggedOwner(5000).code(), StatusCode::kResourceExhausted);
    EXPECT_TRUE(store_->AddUnloggedOwner(1000).ok()) << "a registration already held is not new";
    store_->RemoveUnloggedOwner(1000);
    EXPECT_TRUE(store_->AddUnloggedOwner(5000).ok());
}

}  // namespace
}  // namespace kds::storage
