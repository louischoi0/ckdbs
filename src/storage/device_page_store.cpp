#include "kds/storage/device_page_store.hpp"

#include <unordered_set>

#include "kds/base/contention.hpp"
#include "kds/base/current_core.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifndef NDEBUG
#include <execinfo.h>
#endif

#include "kds/storage/free_map.hpp"
#include "kds/storage/page_header.hpp"

// A free slot is poisoned under ASan (BE-R1), so a span kept past its frame's
// reclaim is a hard stop as it was while a reclaim freed the page. No-ops in
// any other build.
#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#define KDS_ASAN_POISON(addr, size) ASAN_POISON_MEMORY_REGION((addr), (size))
#define KDS_ASAN_UNPOISON(addr, size) ASAN_UNPOISON_MEMORY_REGION((addr), (size))
#else
#define KDS_ASAN_POISON(addr, size) ((void)(addr), (void)(size))
#define KDS_ASAN_UNPOISON(addr, size) ((void)(addr), (void)(size))
#endif

namespace kds::storage {

namespace {

// "Never written" as the device shows it: every byte zero. The miss path
// and CreateAt ask the same question and must answer it the same way.
bool PageIsAllZero(std::span<const std::byte, kPageSize> page) noexcept {
    return std::all_of(page.begin(), page.end(), [](std::byte b) { return b == std::byte{0}; });
}

// Erase-and-broadcast on **every** exit from `FetchPinned`'s load window,
// including one an exception takes. The window is the only place the
// structure latch is dropped mid-operation, and an id left behind in
// `loading_` is not a leak that costs memory: every later fetch of that
// page finds it in flight and parks on the condition variable for the life
// of the process, because the thread that would have woken them is gone.
// `std::make_unique<Page>()` inside the device read is enough to make that
// reachable, and the build enables exceptions.
class LoadingGuard {
public:
    LoadingGuard(std::unique_lock<Latch>& hold, std::unordered_set<PageId>& loading,
                 std::condition_variable_any& done, PageId page_id) noexcept
        : hold_(hold), loading_(loading), done_(done), page_id_(page_id) {}
    LoadingGuard(const LoadingGuard&) = delete;
    LoadingGuard& operator=(const LoadingGuard&) = delete;
    ~LoadingGuard() {
        // Re-taken here rather than by the caller, so the erase and the
        // broadcast are reached by the normal path and by unwinding alike.
        if (!hold_.owns_lock()) hold_.lock();
        loading_.erase(page_id_);
        // Both arms: a waiter never woken because this load *failed* would
        // sleep until some unrelated fault happened to wake it.
        done_.notify_all();
    }

private:
    std::unique_lock<Latch>& hold_;
    std::unordered_set<PageId>& loading_;
    std::condition_variable_any& done_;
    PageId page_id_;
};

}  // namespace

DevicePageStore::DevicePageStore(PageDevice& device, PageId first_new_page_id) noexcept
    : device_(device), next_new_page_id_(first_new_page_id), system_floor_(first_new_page_id) {}

const DevicePageStore::Page& DevicePageStore::AbsentRegionPage() noexcept {
    static const Page kZero{};
    return kZero;
}

void DevicePageStore::AssertNotUnderMapHold(const char* reader) const noexcept {
#ifndef NDEBUG
    if (HoldsLatch(map_latch())) {
        std::fprintf(stderr,
                     "DevicePageStore: %s was called from inside the free map's own hold. The "
                     "latch is not recursive (base/latch.hpp), so this would hang; call the "
                     "Locked form instead (AM-S3).\n",
                     reader);
        std::abort();
    }
#else
    (void)reader;
#endif
}

void DevicePageStore::AssertOrderBeforeFrames(const char* site) const noexcept {
#ifndef NDEBUG
    if (HoldsLatch(map_latch())) {
        std::fprintf(stderr,
                     "DevicePageStore: %s took the frame table while holding the free map. The "
                     "declared order is frames then map, never the reverse (AM-S3, "
                     "device_page_store.hpp); this is the inversion that would deadlock against "
                     "an allocator on another core.\n",
                     site);
        std::abort();
    }
#else
    (void)site;
#endif
}

Status DevicePageStore::LoadRegionIfPresent(std::uint32_t region) {
    const PageId free_id = FreeMapPageIdFor(FreeMapRegionBase(region));
    if (device_.page_capacity() <= free_id) return Status::OK();

    MapRegion pages;
    bool saw_headerless = false;
    auto view = std::span<std::byte, kPageSize>(pages.free_map);
    if (Status s = device_.ReadPage(free_id, view); !s.ok()) return s;
    // An all-zero page reads back as page_type kInvalid, which is what a
    // sparse never-written page looks like: this region was never created,
    // and looking must not create it.
    if (RawPageType(view) == static_cast<std::uint8_t>(PageType::kInvalid)) return Status::OK();
    if (Status s = ValidateFreeMapPage(view); !s.ok()) return s;

    // The headerless bitmap is loaded **only if the device holds one**
    // (FM6). Nothing at that id reads as kInvalid, which is exactly what a
    // region with no headerless page looks like - and what every database
    // written before the headerless map existed looks like, since such a
    // database predates the only thing that creates them. Either way the
    // answer is "no headerless pages here", which needs no bitmap to say.
    const PageId headerless_id = HeaderlessMapPageIdFor(FreeMapRegionBase(region));
    if (device_.page_capacity() > headerless_id) {
        auto loaded = std::make_unique<Page>();
        auto hview = std::span<std::byte, kPageSize>(*loaded);
        if (Status s = device_.ReadPage(headerless_id, hview); !s.ok()) return s;
        if (RawPageType(hview) != static_cast<std::uint8_t>(PageType::kInvalid)) {
            if (Status s = ValidateFreeMapPage(hview, PageType::kHeaderlessMap); !s.ok()) {
                return s;
            }
            pages.headerless_map = std::move(loaded);
            // Set below, under the hold that publishes the region - not
            // here, where the bitmap is still this thread's private copy
            // (AM-S3's review). A reader that saw `true` before the region
            // was in the map would take the latched path and find nothing,
            // which is harmless; setting it after publication is what makes
            // the two agree in the direction that matters.
            saw_headerless = true;
        }
    }

    // **Published under the map hold, and the loser keeps the winner's**
    // (AM-S3). Every device read above ran outside it, which is the rule;
    // this is the one line that touches the shared map. `try_emplace` for
    // `EnsureRegionResidentLocked`'s reason: two cores can read the same
    // region off the device at once, and the second must not replace a
    // bitmap the first has already handed out.
    AssertNotUnderMapHold("LoadRegionIfPresent");
    LatchGuard map(map_latch());
    auto [it, inserted] = map_regions_.try_emplace(region, std::move(pages));
    if (inserted) {
        allocated_pages_ +=
            FreeMapCountAllocated(std::span<const std::byte, kPageSize>(it->second.free_map));
        if (saw_headerless) any_headerless_.store(true, std::memory_order_release);
    }
    return Status::OK();
}

StatusOr<std::span<std::byte, kPageSize>> DevicePageStore::EnsureHeaderlessMap(PageId page_id) {
    auto region = EnsureRegionResident(FreeMapRegionOf(page_id));
    if (!region.ok()) return region.status();
    // **The grow runs before the hold, unconditionally** (AM-S3). It is
    // this class's rule that device work never runs under a latch, and
    // `EnsureCapacity` is idempotent and a comparison when the file is
    // already large enough - so paying it on the already-installed path is
    // cheaper than the alternative, which is take the hold, drop it to
    // grow, and re-check.
    const PageId headerless_id = HeaderlessMapPageIdFor(page_id);
    if (Status s = device_.EnsureCapacity(headerless_id + 1); !s.ok()) return s;

    // **`region.value()` stays valid across the drop**, which is what makes
    // the split legal: `map_regions_` is a `std::map`, and `std::map` never
    // invalidates a pointer to an existing element when another is
    // inserted. A hash map here would have made this an
    // insert-then-re-look-up.
    //
    // **Under the map hold** (AM-S3), and this is the sharper half of the
    // free map's race: the headerless bitmap is a `unique_ptr` installed
    // lazily on first use (FM6), so an unlatched install is a pointer store
    // racing every `IsHeaderless` read of it - not bits that might be stale
    // but a pointer that might be torn.
    AssertNotUnderMapHold("EnsureHeaderlessMap");
    LatchGuard map(map_latch());
    if (region.value()->headerless_map == nullptr) {
        // The id is claimed *here*, as the page is placed, so the free map
        // never says a page exists whose bytes have not been written. It
        // cannot have been taken in the meantime: every allocation path
        // skips a bitmap id by arithmetic.
        FreeMapAllocate(std::span<std::byte, kPageSize>(region.value()->free_map),
                        FreeMapBitIndexOf(headerless_id));
        ++allocated_pages_;
        auto made = std::make_unique<Page>();
        FormatFreeMapPage(std::span<std::byte, kPageSize>(*made), PageType::kHeaderlessMap);
        region.value()->headerless_map = std::move(made);
        region.value()->dirty = true;
        any_headerless_.store(true, std::memory_order_release);
    }
    return std::span<std::byte, kPageSize>(*region.value()->headerless_map);
}

StatusOr<DevicePageStore::MapRegion*> DevicePageStore::EnsureRegionResident(
    std::uint32_t region) {
    // **The lookup is under the map hold, the device work is not** (AM-S3),
    // which is `FetchPinned`'s retry idiom applied to the one other
    // structure that grows. This function used to run wholly unlatched, and
    // its `map_regions_.emplace` below was a `std::map` insertion racing
    // every other core's `find` - undefined behaviour rather than a stale
    // read. `tests/free_map_race_test.cpp` measures what that cost: with the
    // publication moved back outside this hold, 4 runs in 8 die or report a
    // corrupted region count, one of them on a SIGSEGV.
    //
    // **The peer's private empty region went with the lease** (AW-S1b). A
    // leased store could not read a map page from the device - core 0 owned
    // it, wrote it and did not latch it - so a peer reaching a region
    // after its mount got a private, never-dirty copy instead. One frame
    // table serves every core now, so the unsynchronised read that arm
    // avoided cannot arise: there is one copy, and this is the discipline
    // over it.
    {
        AssertNotUnderMapHold("EnsureRegionResident");
        LatchGuard map(map_latch());
        if (auto it = map_regions_.find(region); it != map_regions_.end()) return &it->second;
    }

    // Outside the hold: this reads the device, and may grow the file.
    // `LoadRegionIfPresent` takes the hold itself for its own insertion.
    if (Status s = LoadRegionIfPresent(region); !s.ok()) return s;
    {
        LatchGuard map(map_latch());
        if (auto it = map_regions_.find(region); it != map_regions_.end()) return &it->second;
    }

    // FM5: the region does not exist, so this is where the map grows.
    const PageId free_id = FreeMapPageIdFor(FreeMapRegionBase(region));
    const PageId headerless_id = HeaderlessMapPageIdFor(FreeMapRegionBase(region));
    if (headerless_id >= kMaxPageCount) {
        return Status::OutOfSpace("DevicePageStore: free-map region " + std::to_string(region) +
                                  " lies beyond the " + std::to_string(kMaxPageCount) +
                                  "-page design ceiling");
    }
    if (Status s = device_.EnsureCapacity(headerless_id + 1); !s.ok()) return s;

    MapRegion pages;
    auto view = std::span<std::byte, kPageSize>(pages.free_map);
    FormatFreeMapPage(view);
    // The region's own free map, marked in itself - self-referential and
    // terminating, which is the property FM1's placement arithmetic exists
    // to give (free_map.hpp's placement note).
    //
    // **The headerless id is not marked**, because under FM6 no page is
    // there yet. An allocated id whose bytes were never written is the
    // signature of a torn creation, and the simulation harness's integrity
    // sweep reads every allocated page and says so - it found exactly this
    // on seed 4 when an earlier form of FM6 reserved the id up front.
    // Nothing needs the reservation: both allocation paths skip a bitmap id
    // by arithmetic (IsMapPageId), which does not depend on a bit having
    // been set at the right moment.
    FreeMapAllocate(view, FreeMapBitIndexOf(free_id));
    (void)headerless_id;
    pages.dirty = true;

    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("pagestore", "free-map region " + std::to_string(region) +
                                     " created, covering ids " +
                                     std::to_string(FreeMapRegionBase(region)) + ".." +
                                     std::to_string(FreeMapRegionBase(region) + kFreeMapBitsPerPage - 1));
    }
    // **Published under the hold, and the loser of a race keeps the
    // winner's region** (AM-S3). Two cores can reach here for one region -
    // both found it absent, both grew the file, and `EnsureCapacity` is
    // idempotent - so `try_emplace` rather than `emplace`: whoever inserted
    // first has the authoritative bitmap, and the second returns it rather
    // than replacing a map another core may already have set bits in.
    // Same shape as `InsertFrame`'s "lose the race rather than win it".
    //
    // `allocated_pages_` is bumped **here**, not where the bit was set, for
    // the same reason: a loser's page was never published, so counting it
    // would double-count the winner's self-referential map page.
    LatchGuard map(map_latch());
    auto [it, inserted] = map_regions_.try_emplace(region, std::move(pages));
    if (inserted) ++allocated_pages_;
    return &it->second;
}

StatusOr<std::unique_ptr<DevicePageStore>> DevicePageStore::Open(PageDevice& device,
                                                                 FrameCapacity capacity,
                                                                 PageId first_new_page_id) {
    if (capacity.frames == 0) {
        return Status::InvalidArgument(
            "DevicePageStore: a pool of 0 frames; buffer_pool_frames is the pool's maximum and "
            "there is no unbounded pool");
    }
    auto store = std::unique_ptr<DevicePageStore>(new DevicePageStore(device, first_new_page_id));
    store->capacity_ = capacity.frames;
    store->configured_capacity_ = capacity.frames;

    // Region 0 always exists - it holds the superblock, both of its own
    // bitmaps and the whole catalog - so it is loaded, or created, rather
    // than merely looked for. A device too small to hold page 1, or one
    // whose page 1 reads as never-written, is a fresh database: neither
    // carries any allocation.
    if (auto region = store->EnsureRegionResident(0); !region.ok()) return region.status();

    // Every further region the file is large enough to hold. Loading them
    // all is what lets IsAllocated and IsHeaderless stay `const noexcept`
    // over a map that is no longer one page: a region absent from the
    // cache is then a region that does not exist, and reads as empty
    // rather than as unknown. A torn map page refuses the mount here, the
    // way a torn catalog page has since RV3, rather than surfacing
    // mid-statement.
    //
    // For every database that fits in one region - which is every database
    // written before this change - the loop body does not run and the
    // mount reads exactly the two pages it always did.
    for (std::uint32_t region = 1;; ++region) {
        const PageId free_id = FreeMapPageIdFor(FreeMapRegionBase(region));
        if (free_id >= kMaxPageCount || device.page_capacity() <= free_id) break;
        if (Status s = store->LoadRegionIfPresent(region); !s.ok()) return s;
    }

#ifndef NDEBUG
    // MG05: `KDS_TEST_FRAME_BUDGET=<n>` puts every debug-build store under
    // eviction pressure without threading a knob through each fixture. Env
    // rather than config on purpose: it exists to run the *whole* suite
    // against a brutal budget, and a config key would have to be planted in
    // hundreds of tests to reach the stores they construct.
    // It only lowers the capacity, and never below the working minimum
    // (BE-R3): a pool the override made smaller than a statement's working
    // set would refuse correct traffic, which is not the pressure the run
    // exists to apply.
    if (const char* budget = std::getenv("KDS_TEST_FRAME_BUDGET"); budget != nullptr) {
        const long parsed = std::strtol(budget, nullptr, 10);
        if (parsed > 0) {
            const std::size_t lowered =
                std::max<std::size_t>(static_cast<std::size_t>(parsed), kWorkingMinimumFrames);
            store->capacity_ = std::min(store->capacity_, lowered);
        }
    }
    // AM-S1's census: `KDS_TEST_PAGE_LATCH=1` arms the page latch on every
    // debug-build store, so one `ctest` run exercises the armed primitive
    // under the whole suite - single-threaded, which is exactly what proves
    // the nesting rules (re-entrancy, never an upgrade) against every access
    // pattern the tree has, rather than against the ones a fixture thought
    // of. Same reasoning as the budget override above.
    if (const char* armed = std::getenv("KDS_TEST_PAGE_LATCH"); armed != nullptr) {
        if (std::strtol(armed, nullptr, 10) > 0) {
            store->latch_forced_ = true;  // the assembly's SetLatchArmed cannot undo it
            store->SetLatchArmed(true);
        }
    }
#endif
    return store;
}

bool DevicePageStore::IsHeaderless(PageId page_id) const noexcept {
    // FM6 / D2(a). This predicate sits on the fault path, the write-back
    // path and the WAL gate, and for a database with no Waystone directory
    // - which has no headerless page anywhere, `waystone_dir.cpp` being the
    // engine's only creator of them - the answer is no, with no lookup.
    if (!any_headerless_.load(std::memory_order_acquire)) return false;
    // A map page's class is arithmetic, never a lookup. §3 of
    // docs/inflight/in-progress/workplan-multi-free-map.md needs this to be true rather than
    // merely convenient: this predicate sits on the fault path, the
    // write-back path and the WAL gate, so answering it by reading a map
    // would be a recursion if a map page could ever be the question. Both
    // bitmap classes are headered, so the answer is no.
    if (IsMapPageId(page_id)) return false;
    AssertNotUnderMapHold("IsHeaderless");
    LatchGuard map(map_latch());
    return IsHeaderlessLocked(page_id);
}

bool DevicePageStore::IsHeaderlessLocked(PageId page_id) const noexcept {
    if (IsMapPageId(page_id)) return false;
    // `IsAllocatedLocked`, not `IsAllocated`: this already holds the latch
    // that one takes, and the pair used to nest freely because neither took
    // anything (AM-S3).
    return FreeMapIsAllocated(headerless_map_bytes_for(page_id), FreeMapBitIndexOf(page_id)) &&
           IsAllocatedLocked(page_id);
}

void DevicePageStore::StampIfHeadered(PageId page_id,
                                      std::span<std::byte, kPageSize> page) const {
    // The one place the decision is made. A write path that stamped
    // directly would have to remember to ask, and the failure mode of
    // forgetting is silent data corruption in a page nobody checksums.
    if (IsHeaderless(page_id)) return;
    StampPageChecksum(page);
}

StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>>
DevicePageStore::CreateNewHeaderlessUnpinned() {
    // The raw sibling, not the pinned base accessor: this *is* the raw
    // seam, and pinning here would leak a pin no handle ever drops. **From
    // the cursor only** (BF-R6), and zeroed and marked before its frame
    // exists: `CreateNewFrom`'s headerless arm.
    auto created = CreateNewFrom(/*headerless=*/true);
    if (created.ok() && log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
        log_->Trace("pagestore",
                    "alloc headerless page=" + std::to_string(created.value().first));
    }
    return created;
}

Status DevicePageStore::MarkHeaderlessBeforeInsert(PageId headerless_page) {
    // **Never over a dead image** (BF-S1's Census A, R1). The cursor finds
    // the bits a previous run's reclaim cleared - it starts at 128 at every
    // clean mount - and a headerless read skips both the all-zero test and
    // the checksum, so after a crash that left this page unwritten the dead
    // page's bytes would read as directory children. Zeros are written and
    // synced before the headerless bit is set, so the bit never reaches the
    // device ahead of them: BF-R5's zero-write fallback, paid once per
    // directory page created on such an id. A never-written id holds zeros
    // already and costs the one read.
    //
    // **And before the frame exists.** A writeback stamps a checksum into
    // any frame it does not yet see as headerless - in place, at byte 4,
    // which is child 1 - so a frame inserted ahead of the mark could be
    // written out stamped while this read, write and sync run, and read
    // back after a crash as a directory whose child 1 is a checksum.
    if (!DeviceHoldsOnlyZeros(headerless_page)) {
        const Page zeros{};
        if (Status s = device_.WritePage(headerless_page,
                                         std::span<const std::byte, kPageSize>(zeros));
            !s.ok()) {
            return s;
        }
        if (Status s = device_.Sync(); !s.ok()) return s;
    }
    // Both map lookups run **before** the hold, because either may reach the
    // device; the region is resident by construction (the claim had to load
    // or create it), so neither does in practice.
    auto map = EnsureHeaderlessMap(headerless_page);
    if (!map.ok()) return map.status();
    auto region = EnsureRegionResident(FreeMapRegionOf(headerless_page));
    if (!region.ok()) return region.status();
    // **The mark is a read-modify-write of one byte in a page every core
    // shares** (AM-S2), so it takes the latch even though the *id* is this
    // caller's alone: a neighbouring id's bit in the same byte belongs to
    // somebody else.
    LatchGuard alloc(map_latch());
    FreeMapAllocate(map.value(), FreeMapBitIndexOf(headerless_page));
    region.value()->dirty = true;
    return Status::OK();
}

StatusOr<std::size_t> DevicePageStore::FlushMaps() {
    // **Every core's store publishes the maps.** It used to carry a check
    // of its own - a leased store dropped its map writes rather than
    // publishing a copy taken at its own mount - and AW-S1b removed the
    // copy along with the lease.
    //
    // **Copy under the hold, write outside it** (AM-S3). The rule this class
    // has had since AM-S2 2b is that device work does not run under the
    // structure latch, and the walk that stood here obeyed it by taking no
    // latch at all - iterating a `std::map` another core inserts regions
    // into, and checksumming pages another core sets bits in. So the pair
    // is split: the bytes are stamped and copied while the map is held, and
    // the writes run after it is dropped.
    //
    // **The dirty flag is cleared at copy time, not after the write.** A
    // concurrent allocation that dirties the region between the two must
    // re-dirty it rather than have its bit lost, which is what clearing
    // afterwards would do. A failed write re-marks the region below.
    struct Pending {
        std::uint32_t region;
        PageId free_id;
        Page free_map;
        bool has_headerless;
        PageId headerless_id;
        Page headerless_map;
    };
    // **The map write barrier** (BF-R5), held from the copy to the last
    // write: a caller that finds every region clean because another core
    // copied them first waits here until that core's write has landed, so
    // its own sync covers it. Outer to the map latch, and held across device
    // I/O, which is why it is not the map latch.
    AssertNotUnderMapHold("FlushMaps");
    LatchGuard barrier(map_flush_latch());
    std::vector<Pending> pending;
    {
        LatchGuard map(map_latch());
        for (auto& [region, pages] : map_regions_) {
            if (!pages.dirty) continue;
            const PageId base = FreeMapRegionBase(region);
            Pending entry;
            entry.region = region;
            entry.free_id = FreeMapPageIdFor(base);
            entry.free_map = pages.free_map;
            entry.has_headerless = pages.headerless_map != nullptr;
            entry.headerless_id = HeaderlessMapPageIdFor(base);
            if (entry.has_headerless) entry.headerless_map = *pages.headerless_map;
            StampPageChecksum(std::span<std::byte, kPageSize>(entry.free_map));
            if (entry.has_headerless) {
                StampPageChecksum(std::span<std::byte, kPageSize>(entry.headerless_map));
            }
            pages.dirty = false;
            pending.push_back(std::move(entry));
        }
    }
    if (pending.empty()) return std::size_t{0};
    if (after_map_copy_for_test_) after_map_copy_for_test_();

    // Ascending by region, which the copy took from `std::map` for free.
    // Regions are independent of one another - a page's reachability rests
    // on its own region's map and nothing else - so the order across them is
    // a determinism choice, not a correctness one. Within a region it is
    // both, and the rule is the one the single-page map always followed.
    // **A failure re-marks this region and every one after it** - the debt
    // clearing the flag at the copy creates, and AM-S3's review caught it
    // paid for one entry only. Regions {3, 7, 9} dirty, the write of 7
    // fails: 9 was copied, cleared, and never written, so leaving it clean
    // means its allocation bits never reach the platter and a restart reads
    // those pages free and re-issues them. The comment on the free-map write
    // below says exactly that - "losing this write loses pages whose bytes
    // did land" - and it was true of the tail as well as of the failure.
    auto remark_from = [&](std::size_t first) {
        for (std::size_t j = first; j < pending.size(); ++j) RemarkRegionDirty(pending[j].region);
    };
    for (std::size_t i = 0; i < pending.size(); ++i) {
        Pending& entry = pending[i];
        // The headerless map first, the free map second. Both orderings are
        // safe, but this one is safe for a reason worth writing down: the
        // free map is what makes a page id *exist*, so a crash between the
        // two leaves a headerless bit set for an id nothing allocated -
        // harmless, since IsHeaderless() also requires allocation. The
        // reverse order would publish an allocated headerless page whose
        // headerless bit had not landed, and the next read of it would
        // verify a checksum that was never written and call the page
        // corrupt.
        if (entry.has_headerless) {
            auto hbytes = std::span<std::byte, kPageSize>(entry.headerless_map);
            if (Status s = device_.WritePage(entry.headerless_id, hbytes); !s.ok()) {
                if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                    log_->Error("pagestore", "headerless-map write failed for region " +
                                                 std::to_string(entry.region) + ": " + s.message());
                }
                remark_from(i);
                return s;
            }
        }

        auto fbytes = std::span<std::byte, kPageSize>(entry.free_map);
        if (Status s = device_.WritePage(entry.free_id, fbytes); !s.ok()) {
            // The map is what makes a page reachable after a restart, so
            // losing this write loses pages whose bytes did land.
            if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                log_->Error("pagestore", "free-map write failed for region " +
                                             std::to_string(entry.region) + ": " + s.message());
            }
            remark_from(i);
            return s;
        }
    }

    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("pagestore", "maps written for " + std::to_string(pending.size()) +
                                     " dirty region(s); " + std::to_string(allocated_pages()) +
                                     " page(s) allocated across the instance");
    }
    return pending.size();
}

// Puts a region's dirty bit back after a failed write, so the next flush
// retries it (AM-S3). Under the hold, because it touches the map, and a
// plain store rather than a test-then-set: an allocation may have
// re-dirtied it already and both readings are "owes a write".
void DevicePageStore::RemarkRegionDirty(std::uint32_t region) noexcept {
    AssertNotUnderMapHold("RemarkRegionDirty");
    LatchGuard map(map_latch());
    if (MapRegion* pages = MutableRegion(region); pages != nullptr) pages->dirty = true;
}

Status DevicePageStore::PersistMaps() {
    // Syncs whether or not this call was the one that wrote: a caller
    // asking for the maps to be durable is owed durability, and under
    // sharing the core that wrote them may be another one - whose write
    // `FlushMaps`' barrier has waited out by the time this syncs (BF-R5).
    if (auto flushed = FlushMaps(); !flushed.ok()) return flushed.status();
    return device_.Sync();
}

bool DevicePageStore::IsAllocated(PageId page_id) const noexcept {
    // The ceiling test needs no map and therefore no hold.
    if (page_id >= kMaxPageCount) return false;
    AssertNotUnderMapHold("IsAllocated");
    LatchGuard map(map_latch());
    return IsAllocatedLocked(page_id);
}

bool DevicePageStore::IsAllocatedLocked(PageId page_id) const noexcept {
    // FM3: the ceiling is the design ceiling now, not one bitmap page's
    // coverage. A region that does not exist reads as empty below it,
    // which is the same answer by a different route.
    if (page_id >= kMaxPageCount) return false;
    return FreeMapIsAllocated(free_map_bytes_for(page_id), FreeMapBitIndexOf(page_id));
}

std::uint32_t DevicePageStore::allocated_pages() const noexcept {
    // D8(a): maintained, not swept. Seeded at mount from the regions it
    // loads and moved by every site that sets a free-map bit, so the
    // number keeps the meaning it has always had - the instance total, not
    // a resident-only sample - at O(1) rather than O(regions), on three
    // paths that print it (mount, shutdown, SHOW META).
    return allocated_pages_;
}

Status DevicePageStore::EnsureAddressable(PageId page_id) {
    // The device's growth lock is a leaf under the page latch and under
    // nothing of this store's (`file_page_device.hpp`, `rules.md` §3's
    // device row); this is the assert that keeps the map half of that
    // sentence true, the way the map's own readers keep theirs.
    AssertNotUnderMapHold("EnsureAddressable");
    if (page_id < device_.page_capacity()) return Status::OK();
    return device_.EnsureCapacity(page_id + 1);
}

StatusOr<std::span<std::byte, kPageSize>> DevicePageStore::InsertFrame(
    PageId page_id, ReservedFrame&& reserved, bool dirty, Inserting why, bool warm) {
    // **AM-S2 R1: the table's one structural mutation takes the structure
    // latch itself**, so a publish never races another core's `find`.
    // Taken here rather than by the callers, which all reach this with the
    // latch *not* held: `ResidentBytes`' miss path (2b drops the latch
    // before the read) and the two `Create*Unpinned` paths.
    //
    // **The declared order is checked here** (AM-S3): this is the frame
    // table's one structural mutation, so it is where a future caller
    // holding the free map would land, and holding the map here is the
    // inversion that deadlocks against an allocator on another core.
    //
    // **And a resident frame is never replaced** (AM-S2 R2): whoever got
    // here first has the authoritative frame, so the bytes filled second
    // are dropped - their slot goes back to the free list - and the resident
    // view is returned. Correct for the miss path, which wanted *the* page
    // and now has it.
    //
    // **A create may not lose it** (BF-R5). Its id is one no other caller
    // holds - `CreateAt` refuses an id in use, a cursor id was clear, and a
    // free-list id is popped only when neither resident nor in `loading_`
    // (BF-R6) - so a frame here is a fault that began after the claim, on an
    // id a reclaim handed out again: a stale location read through a gate
    // defect, carrying the dead page's image. Serving that as the new page
    // would hand the creator another relation's bytes; the create is refused
    // `Corruption` instead, which is defence and not the authority.
    AssertOrderBeforeFrames("InsertFrame");
    LatchGuard structure(structure_latch());
    if (auto resident = frames_.find(page_id); resident != frames_.end()) {
        if (why == Inserting::kCreate) {
            ReturnSlotLocked(std::exchange(reserved.slot_, kNoSlot));
            return Status::Corruption("DevicePageStore: page " + std::to_string(page_id) +
                                      " was resident when it was created; a fault on a freed id "
                                      "raced its reuse");
        }
        // Lost the race. A dirty flag the loser carried is still owed: a
        // miss path that faulted for a write must not leave the winner clean.
        if (dirty) resident->second.MarkDirty(++dirty_gens_);
        if (warm && resident->second.usage < kClockUsageCap) ++resident->second.usage;
        ReturnSlotLocked(std::exchange(reserved.slot_, kNoSlot));
        return std::span<std::byte, kPageSize>(*resident->second.bytes);
    }
    // **The invariant a lost write rests on** (AM-S2-P F-1), stated where
    // it is kept: a frame inserted **clean** for a page some other call is
    // faulting would swallow that call's dirty mark, since `FetchPinned`'s
    // miss arm pins whatever is resident when its guard re-takes the latch.
    // No call site can do it - the three `Create*` paths insert dirty, and
    // `ResidentBytes`' miss is reached from `FetchAndPin`, which publishes
    // the page id to `loading_` first so a second fault waits rather than
    // inserts. (The lost-race arm above closes the remaining way, by
    // carrying the loser's dirty flag to the winner.)
    // Mapped first: an `emplace` that throws leaves the slot untouched and
    // the guard still holding it, which gives it back.
    const std::uint32_t index = reserved.slot_;
    Frame* const slot = &frames_.try_emplace(page_id).first->second;
    reserved.slot_ = kNoSlot;
    SlotOwner(index) = page_id;
    slot->bytes = &SlotPage(index);
    slot->slot = index;
    slot->page_id = page_id;
    slot->dirty = dirty;
    // A fresh generation, never 0: a frame re-faulted for a page a stale
    // writeback copied must not match the generation that copy recorded.
    slot->dirty_gen = ++dirty_gens_;
    // An ordinary miss starts warm (usage 1), not cold: one usage point is
    // one sweep rotation of grace, the same a hit's bump buys. A ring fetch
    // and a `kScan` fault start cold (`FetchHeat`): a scan's first touch is
    // not heat, and the ring's own slot release depends on usage staying
    // zero.
    slot->usage = warm ? std::uint8_t{1} : std::uint8_t{0};
    return std::span<std::byte, kPageSize>(*slot->bytes);
}

// ---- Slots (BE-R1) ---------------------------------------------------------

StatusOr<std::span<std::byte, kPageSize>> DevicePageStore::PublishFreshPage(PageId page_id,
                                                                  ReservedFrame&& slot) {
    std::fill(slot.bytes().begin(), slot.bytes().end(), std::byte{0});
    if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
        log_->Trace("pagestore", "alloc page=" + std::to_string(page_id) + " (allocated=" +
                                     std::to_string(allocated_pages()) + ")");
    }
    // A brand-new page exists only in this frame until it is written back,
    // so it is dirty by definition.
    return InsertFrame(page_id, std::move(slot), /*dirty=*/true, Inserting::kCreate);
}

namespace {
// **Which mode this thread's fills are in** (BE-Q11), for one store at a
// time. Per thread rather than per task, which is sound because a window
// and a drain-mode pass are each opened and closed inside one synchronous
// call: no other task runs on this thread in between.
struct FillMode {
    const void* store = nullptr;  // the store the window is open on
    int window_depth = 0;
    std::size_t window_left = 0;  // the outermost window's unspent share
    const void* drain_store = nullptr;
    int drain_depth = 0;
};
thread_local FillMode t_fill;
// Pins this thread holds, across every store: drain mode writes back only
// when it is zero (`ReserveFrame`). `CountPin` and `UncountPin` move it, and
// a pin is taken and dropped on the thread that holds the handle.
thread_local std::size_t t_pins_here = 0;
}  // namespace

bool NoRefuseWindowOpenOnThisThread() noexcept { return t_fill.window_depth > 0; }

StatusOr<std::uint32_t> DevicePageStore::ReserveFrame() {
    AssertOrderBeforeFrames("ReserveFrame");
    misses_.Add();
    const bool in_window = t_fill.store == this && t_fill.window_depth > 0;
    // MG06's on-demand trigger (EV5), in BE-R2's bounded form: a fill at the
    // limit reclaims a batch before it takes a slot, so the next
    // `ReclaimBatch() - 1` fills find slots free and pay nothing. No frame of
    // this fill exists yet, so the sweep needs no guard pin on it.
    //
    // **One batch per hold, the latch released between them**, so another
    // core's hit never waits behind more than one batch's walk. A batch that
    // frees nothing is not a full pool: the first fill past a pool whose
    // frames are all warm walks a lap decrementing before any frame reaches
    // zero. So a walk ends only as `ReclaimWalk` says: a lap that freed and
    // lowered nothing, or `kClockUsageCap + 1` laps in all.
    for (std::size_t attempt = 0;;) {
        ReclaimWalk walk;
        for (;;) {
            LatchGuard structure(structure_latch());
            const bool spend_share = in_window && t_fill.window_left > 0;
            const std::size_t used = SlotsInUseLocked();
            const std::size_t limit =
                spend_share ? capacity_
                            : (capacity_ > window_promised_ ? capacity_ - window_promised_ : 0);
            if (used < limit) {
                if (spend_share) {
                    --t_fill.window_left;
                    --window_promised_;
                }
                return TakeSlotLocked();
            }
            const SweepResult batch = InlineBatchLocked();
            if (batch.reclaimed != 0) continue;  // the limit test takes it
            if (walk.Done(batch, batch.progressed, SlotCountLocked())) break;
        }
        // **Drain mode, holding no pin** (BE-Q11): the dirty queue the walk
        // just filled is written back, and the next walk reclaims it. Sound
        // because this thread holds no page latch, so the writeback's shared
        // try cannot re-enter a hold of its own, and a durability wait holds
        // nothing. A drain that cleaned something does not count as a retry.
        if (t_fill.drain_store == this && t_fill.drain_depth > 0 && t_pins_here == 0) {
            auto drained = DrainDirtyEvictionQueue();
            if (drained.ok() && drained.value() > 0) continue;
        }
        // BE-R4: retry the walk - a drain on another core may have cleaned
        // frames meanwhile - and nothing on this path writes back or waits.
        if (++attempt < kRefuseRetries) continue;
        if (in_window) FailStop();
        refused_.Add();
        std::size_t pinned = 0;
        std::size_t dirty = 0;
        std::size_t resident = 0;
        {
            LatchGuard structure(structure_latch());
            resident = frames_.size();
            for (const auto& [id, frame] : frames_) {
                pinned += frame.pins != 0;
                dirty += frame.dirty;
            }
        }
        return Status::ResourceExhausted(
            "buffer pool full: buffer_pool_frames " + std::to_string(capacity_) + ", " +
            std::to_string(resident) + " resident (" + std::to_string(pinned) + " pinned, " +
            std::to_string(dirty) + " dirty), nothing reclaimable; raise buffer_pool_frames or "
            "split the statement");
    }
}

std::uint32_t DevicePageStore::TakeSlotLocked() {
    if (free_slots_.empty()) {
        // The caller's limit test proved a slot is due, so the array is below
        // the capacity: grow it by a chunk (BE-R1). **Cut at the configured
        // capacity, not `capacity_`**: `SlotPage` indexes by `kFrameChunk`, so
        // only the last chunk may be short, and `capacity_` can rise after a
        // chunk was cut at it - `ApplyMountFloor` raises a debug-lowered one
        // to the floor - which would leave a short chunk mid-array. The
        // configured value never moves and bounds `capacity_` from above; the
        // limit test, not the array's size, is what holds the ceiling.
        const std::size_t n = std::min(kFrameChunk, configured_capacity_ - slot_count_);
        // Every allocation first, then the indices: a `bad_alloc` anywhere
        // here leaves no free-list entry naming a chunk that died.
        free_slots_.reserve(free_slots_.size() + n);
        chunks_.push_back(FrameChunk{std::make_unique_for_overwrite<std::byte[]>(n * kSlotStride),
                                     std::make_unique<PageId[]>(n)});
        FrameChunk& chunk = chunks_.back();
        // Pushed high to low, so the chunk fills from its first slot.
        for (std::size_t i = n; i-- > 0;) {
            chunk.owners[i] = kInvalidPageId;
            KDS_ASAN_POISON(chunk.pages.get() + i * kSlotStride, kSlotStride);
            free_slots_.push_back(static_cast<std::uint32_t>(slot_count_ + i));
        }
        slot_count_ += n;
    }
    const std::uint32_t slot = free_slots_.back();
    free_slots_.pop_back();
    KDS_ASAN_UNPOISON(SlotPage(slot).data(), kPageSize);
    return slot;
}

DevicePageStore::SweepResult DevicePageStore::InlineBatchLocked() {
    const std::size_t want = ReclaimBatch();
    const SweepResult batch = SweepLocked(want, kBatchStepsPerFrame * want);
    batches_inline_.Add();
    batch_steps_.Add(batch.steps);
    reclaimed_inline_.Add(batch.reclaimed);
    if (batch.reclaimed < want) batches_partial_.Add();
    return batch;
}

void DevicePageStore::FailStop() const {
    const std::string why =
        "buffer pool: a mutation inside its no-refuse window outgrew its share of "
        "buffer_pool_frames " + std::to_string(capacity_) +
        "; stopping rather than return an error into a half-done write (BE-Q11). The log "
        "holds every committed change; restart to recover";
    if (log_ != nullptr) log_->Error("pagestore", why);
    std::fprintf(stderr, "%s\n", why.c_str());
    std::abort();
}

Status DevicePageStore::BeginNoRefuseWindow(std::size_t frames) {
    if (t_fill.store == this && t_fill.window_depth > 0) {
        ++t_fill.window_depth;  // nested: the outermost holds the share
        return Status::OK();
    }
    // One store's window per thread: a second store's would overwrite this
    // one's state and strand its promise. One store serves an instance, so
    // only a defect reaches it.
    assert(t_fill.window_depth == 0 && "a no-refuse window is open on another store");
    // Promised under the same limit an ordinary fill meets, so the share is
    // capacity no window and no fill already holds - reclaimed for if need
    // be, and refused, with nothing written, if the pool cannot give it.
    for (std::size_t attempt = 0;;) {
        ReclaimWalk walk;
        for (;;) {
            LatchGuard structure(structure_latch());
            const std::size_t used = SlotsInUseLocked();
            if (used + window_promised_ + frames <= capacity_) {
                window_promised_ += frames;
                t_fill = FillMode{this, 1, frames, t_fill.drain_store, t_fill.drain_depth};
                return Status::OK();
            }
            const SweepResult batch = InlineBatchLocked();
            if (batch.reclaimed != 0) continue;
            if (walk.Done(batch, batch.progressed, SlotCountLocked())) break;
        }
        if (++attempt < kRefuseRetries) continue;
        refused_.Add();
        return Status::ResourceExhausted(
            "buffer pool full: buffer_pool_frames " + std::to_string(capacity_) +
            " cannot hold another mutation's " + std::to_string(frames) +
            " frames; raise buffer_pool_frames or split the statement");
    }
}

void DevicePageStore::EndNoRefuseWindow() noexcept {
    if (t_fill.store != this || t_fill.window_depth == 0) return;
    if (--t_fill.window_depth > 0) return;
    {
        LatchGuard structure(structure_latch());
        window_promised_ -= t_fill.window_left;
    }
    t_fill.window_left = 0;
    t_fill.store = nullptr;
}

void DevicePageStore::BeginDrainOnPressure() noexcept {
    if (t_fill.drain_store != nullptr && t_fill.drain_store != this) return;
    t_fill.drain_store = this;
    ++t_fill.drain_depth;
}

void DevicePageStore::EndDrainOnPressure() noexcept {
    if (t_fill.drain_store != this || t_fill.drain_depth == 0) return;
    if (--t_fill.drain_depth == 0) t_fill.drain_store = nullptr;
}

Status DevicePageStore::ApplyMountFloor() {
    const std::size_t resident_class = resident_class_frames();
    const std::size_t floor = resident_class + kWorkingMinimumFrames;
    if (configured_capacity_ < floor) {
        return Status::InvalidArgument(
            "buffer_pool_frames " + std::to_string(configured_capacity_) +
            " is below this volume's floor of " + std::to_string(floor) + ": " +
            std::to_string(resident_class) +
            " resident-class pages, which never leave the pool, plus a working minimum of " +
            std::to_string(kWorkingMinimumFrames) + " frames");
    }
    capacity_ = std::max(capacity_, floor);
    return Status::OK();
}

std::size_t DevicePageStore::resident_class_frames() const {
    LatchGuard structure(structure_latch());
    // Every id below the resident limit, faulted yet or not - the system
    // pages fault lazily, and each stays once it does - and the pinned
    // pages above it that are resident now (Bound Cabin pages).
    std::size_t n = first_evictable_page_id_;
    for (const auto& [id, frame] : frames_) {
        n += id >= first_evictable_page_id_ && IsPinnedClassFrame(frame);
    }
    return n;
}

void DevicePageStore::ReturnSlotLocked(std::uint32_t slot) noexcept {
#ifndef NDEBUG
    // MG05's poisoner: a caller that kept a raw span into this slot reads
    // 0xEF, deterministically, instead of the next page to land here. Under
    // ASan the slot is also poisoned until it is reserved again, which keeps
    // the hard stop a freed `Page` used to give (BE-R1).
    std::memset(SlotPage(slot).data(), 0xEF, kPageSize);
#endif
    KDS_ASAN_POISON(SlotPage(slot).data(), kPageSize);
    SlotOwner(slot) = kInvalidPageId;
    free_slots_.push_back(slot);
}

void DevicePageStore::ReleaseFrameLocked(Frame& frame) noexcept {
    const std::uint32_t slot = frame.slot;
    frames_.erase(frame.page_id);  // `frame` dies here
    ReturnSlotLocked(slot);
}

StatusOr<std::span<std::byte, kPageSize>> DevicePageStore::ResidentBytes(PageId page_id,
                                                                         bool mark_dirty,
                                                                         FetchHeat heat) {
    // **PW1c-7's stamp claim went with the fault grants** (AW-S1b). It
    // restored a core's write rights after a restart by reading the page's
    // own stream stamp, and its *read* trigger was "this core may not fault
    // this page" - a question a shared pool cannot ask, since every core
    // faults every page. Its write trigger is unreachable for the same
    // reason the machinery around it is: there is one frame table and one
    // free map, so no store is one core's. The prefetched page it handed
    // down to the read below went with it, so the miss path always reads
    // and always verifies.
    // **The shared-nothing check went with the fault grants** (AW-S1b,
    // AM-R4a). It refused a fault of a page belonging to another core, in
    // debug builds only, and a shared pool is precisely an arrangement in
    // which every core faults every page - so the predicate it called
    // answered `true` unconditionally from AM-S2 step 3 onwards and the
    // check was dead on every mountable volume before it was deleted.
    // The write gate that stood here - `InvalidArgument` to a peer dirtying
    // a system page - went at AT-S5 with the arm it enforced, and the
    // predicate it asked at AT-S18; what serialises each shared page is
    // `crosscore.md` CC11's.

    if (auto it = frames_.find(page_id); it != frames_.end()) {
        // Never clears the flag: a frame already dirty from an earlier
        // mutation stays dirty however many readers touch it afterwards.
        hits_.Add();
        if (mark_dirty) it->second.MarkDirty(++dirty_gens_);
        // §3.1-2: a saturating bump on every hit, including a read -
        // "recently used" is about access, not about mutation. A *ring*
        // fetch is the one exception (§5): a scan's touch is not heat.
        if (heat != FetchHeat::kRing && it->second.usage < kClockUsageCap) ++it->second.usage;
        return std::span<std::byte, kPageSize>(*it->second.bytes);
    }

    // The free map says this page exists and the device cannot address it:
    // the strongest form of "allocated but never written" (the all-zero
    // case below says when the map runs ahead of the bytes). NotFound for
    // the same reason as that case - a PAGE_INIT in the log re-creates it,
    // and calling it Corruption left redo waiting for a full page image.
    if (page_id >= device_.page_capacity()) {
        return Status::NotFound("DevicePageStore: page " + std::to_string(page_id) +
                                " is allocated but was never written (beyond device capacity " +
                                std::to_string(device_.page_capacity()) + ")");
    }

    // The slot first, the read into it (BE-R1): the fill in flight counts
    // against the budget from here, and a failed read gives it back.
    auto reserved = ReserveFrame();
    if (!reserved.ok()) return reserved.status();
    ReservedFrame slot(*this, reserved.value());
    const std::span<std::byte, kPageSize> bytes = slot.bytes();
    if (Status s = device_.ReadPage(page_id, bytes); !s.ok()) return s;

    // Verified on the miss path only, never on a hit (page.md section 10).
    // Every *headered* page this store writes was stamped in Flush(), so a
    // mismatch here is real damage. A headerless page carries no checksum
    // by construction and is skipped - which is why the headerless map has
    // to be durable: this is the moment an in-memory-only set would have
    // already been lost.
    if (!IsHeaderless(page_id)) {
        // A page the map calls allocated whose bytes were never written:
        // all zero, page_type kInvalid - the shape Open() already reads as
        // "fresh" for the free map. Reached when an allocation outruns its
        // first flush: the map records an id as it is claimed and the
        // page's bytes reach the device only at a write-back, so a crash
        // between a PAGE_INIT and that flush leaves exactly this (found by
        // PW1c-7's restart test, where an extent reserved for a peer's
        // lease was allocated whole in core 0's map while the peer wrote
        // its pages lazily). NotFound, not Corruption: nothing was damaged, and
        // redo's PAGE_INIT arm *creates* a page the store does not hold,
        // where its checksum arm can only poison one it holds wrong and
        // wait for a full page image that never comes (wal/redo.cpp). A
        // torn page with a zero header and a nonzero body is not all zero
        // and still fails the checksum below.
        //
        // Run separately from the checksum below rather than left to it:
        // "an all-zero page cannot pass a checksum" is a fact about
        // CRC32C's value over 8192 zero bytes rather than anything this
        // code says, and the store's answer for a never-written page must
        // not depend on that coincidence.
        if (PageIsAllZero(bytes)) {
            return Status::NotFound("DevicePageStore: page " + std::to_string(page_id) +
                                    " is allocated but was never written (all zero)");
        }
        if (Status s = VerifyPageChecksum(std::span<const std::byte, kPageSize>(bytes));
            !s.ok()) {
            if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                log_->Error("pagestore",
                            "corruption: page " + std::to_string(page_id) +
                                " failed checksum verification on read: " + s.message());
            }
            return s;
        }
    }
    if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
        log_->Trace("pagestore", "read page=" + std::to_string(page_id) + " from device");
    }
    return InsertFrame(page_id, std::move(slot), mark_dirty, Inserting::kFault,
                       /*warm=*/heat == FetchHeat::kWarm);
}

void DevicePageStore::ReleaseScanSlot(PageId page_id) noexcept {
    if (page_id == kInvalidPageId) return;
    // **The first of the three erasers to take the structure latch**, and
    // since step 3 `EvictColdFrames` takes it too (AM-S2), as `FreePage`
    // does (BF-R5).
    // The check and the erase have to be one hold: a pin taken between them
    // would be missed and the frame freed under whoever took it. Both
    // callers - the ring's rotation and its destructor - reach this with the
    // latch not held.
    LatchGuard structure(structure_latch());
    auto it = frames_.find(page_id);
    if (it == frames_.end()) return;  // reclaimed by a sweep meanwhile: fine
    // **This function does not touch the pin** (AM-R8b, AM-S2-P S-P1). It
    // used to drop the ring's own pin here, because every slot was pinned
    // and the refusal below would otherwise always see `pins > 0`. The ring
    // unpins through `UnpinFrame` before it calls this now - which is the
    // only way the page latch comes off with the pin, since a `PageRef` and
    // a ring slot are the same act and this function runs under the
    // structure latch, where a page-latch release does not belong. So the
    // refusal below means what it says again: any pin, anyone's, is
    // absolute.
    Frame& frame = it->second;
    // The foreground got there: a dirty write must reach the device, a pin
    // is absolute, a usage bump means a foreground accessor touched it
    // (ring fetches never bump), and a pinned-class page is never dropped
    // by anyone. Each abandons the frame to ordinary pool life.
    if (frame.dirty || frame.pins > 0 || frame.usage > 0 || IsPinnedClassFrame(frame)) return;
    // A latched frame is a pinned frame through M1; the refusal is the
    // shared pool's shape, as in EvictColdFrames and FreePage.
    if (latch_armed_ && PageLatch::IsHeld(frame.latch)) return;
    ReleaseFrameLocked(frame);
}

// The real ring (§5): fixed slots, cyclic reuse, drop-on-rotation unless
// the foreground claimed the frame. Nested so it can reach the frame
// table; handed out as the base-class ScanFetcher so callers stay
// concrete-store-blind.
class DevicePageStore::ScanRing final : public ScanFetcher {
public:
    ScanRing(DevicePageStore& store, std::size_t frames)
        : store_(store), slots_(frames == 0 ? 1 : frames, kInvalidPageId) {}

    ~ScanRing() override {
        // **Unpin, then release** (AM-R8b), for the reason `Fetch` gives: a
        // frame still carrying this ring's own pin is one `ReleaseScanSlot`
        // refuses.
        DropHeld();
        // The scan is over: every slot the foreground did not claim goes
        // back to the device's keeping.
        for (const PageId id : slots_) store_.ReleaseScanSlot(id);
    }

    // **The ring holds one shared, page-latched pin on the page its last
    // `Fetch` returned; every other slot is an ordinary evictable frame**
    // (AM-R8 final, `instructions/v3.0.0/workorder-am-s2-p-scan-ring-port.md`).
    //
    // The model was *no pin, drop on rotation*: `Fetch` handed back a span
    // into a frame it had not pinned, and `heap_chain.hpp` promises that
    // span lives until the next `Fetch`. That promise was a statement about
    // one thread, and no amount of latching *inside* `Fetch` could make it
    // one about several - the exposure is after the latch would drop, for
    // as long as the caller holds the span. A pin holds the span now, which
    // is what `PlainScanFetcher` has always done with its `PageRef`.
    //
    // **One pin and not one per slot**, which the tree tried first and this
    // order replaced. Pinning every occupied slot answers the same
    // exposure, and its `ReleaseScanSlot` dropped the ring's own pin so
    // drop-on-rotation survived - but it holds up to `kScanRingFrames`
    // frames out of the pool per scanning core against a `kPinCeiling` that
    // scales with cores and not with rings (the order's §1.5). And once
    // the port's pin is *latched*, slot-pinning holds a shared page latch
    // on every slot for the life of a scan, which turns a foreground
    // writer's `X` request into a debug abort - a cost the shipped
    // slot-pin model did not have, because its `PinForScan` took no page
    // latch at all and so read pages a writer was mid-write on. The armed
    // suite found the abort the afternoon the latch landed:
    // `RotationSparesAPinnedPageAndDropsAColdOne` asks for `X` on a page an
    // open ring is still holding `S`, which is the never-upgrade check
    // firing on entirely correct traffic. One pin has neither problem.
    //
    // Every other §5 property is unchanged: no usage bump, drop on
    // rotation, abandon to the pool when the foreground claimed the frame,
    // `kds.scan_ring_frames` untouched.
    StatusOr<std::span<std::byte, kPageSize>> Fetch(PageId page_id) override {
        // **The previous fetch's pin and page latch go first** (AM-R8b).
        // Its span died at this call by contract (`page_store.hpp`), so
        // nothing is lost, and two things are gained: the ring never holds
        // two page latches at once, and a slot the rotation below may
        // release is already unpinned when `ReleaseScanSlot` tests
        // `pins > 0`. The reverse order would keep every slot resident for
        // the life of the process - a ring that "works" while the pool
        // fills.
        DropHeld();

        // **One pin path, armed or not** (AM-R8a): the same `loading_`
        // protocol every other fetch takes, with the usage bump refused.
        // What this replaced - `PinForScan` - faulted outside that set and
        // retried eight times when it lost the race; the pin is taken under
        // the hold that made the frame resident now, so there is nothing
        // left to retry. The shared page latch comes with it, which is what
        // makes a foreground writer holding this page exclusive something
        // the scan waits for rather than reads through (AM-R8c).
        bool faulted = false;
        auto bytes = store_.FetchAndPin(page_id, PinMode::kShared, /*for_read=*/true,
                                        FetchHeat::kRing, &faulted);
        if (!bytes.ok()) return bytes.status();
        held_ = page_id;
        if (faulted) {
            // Rotate, and **only** on a fault: a page found resident - the
            // foreground's frame or one this ring already faulted - is used
            // in place and costs no slot (§5's interaction rule). The answer
            // comes from the fetch rather than from a probe before it,
            // because a probe outside that hold is stale before the fetch
            // acts on it and the ring would then record slots it did not
            // fault.
            //
            // Released *after* the fault rather than before, which is what
            // taking an exact answer costs: residency peaks at `frames + 1`
            // for the length of this call. `ReleaseScanSlot` refuses a
            // pinned frame, so the degenerate case - re-faulting the page
            // this slot already names - keeps the frame and re-records it.
            store_.ReleaseScanSlot(slots_[hand_]);
            slots_[hand_] = page_id;
            hand_ = (hand_ + 1) % slots_.size();
        }
        return bytes.value();
    }

private:
    // Drops the hold the last `Fetch` took - this ring's pin and, with it,
    // its shared page latch. The only place `held_` is cleared, so one
    // fetch can never be unpinned twice.
    void DropHeld() noexcept {
        if (held_ == kInvalidPageId) return;
        store_.UnpinFrame(held_);
        held_ = kInvalidPageId;
    }

    DevicePageStore& store_;
    std::vector<PageId> slots_;
    std::size_t hand_ = 0;
    // The page the last `Fetch` returned, pinned and latched shared until
    // the next one. Not always a slot occupant: a page found resident is
    // held too, and takes no slot.
    PageId held_ = kInvalidPageId;
};

std::unique_ptr<ScanFetcher> DevicePageStore::OpenScanRing(std::size_t frames) {
    return std::make_unique<ScanRing>(*this, frames);
}

bool DevicePageStore::DeviceHoldsOnlyZeros(PageId page_id) const {
    // Not addressable is the strongest form of never written. A failed read
    // answers "in use": refusing a CreateAt is the safe error.
    if (page_id >= device_.page_capacity()) return true;
    auto bytes = std::make_unique<Page>();
    if (!device_.ReadPage(page_id, std::span<std::byte, kPageSize>(*bytes)).ok()) return false;
    return PageIsAllZero(std::span<const std::byte, kPageSize>(*bytes));
}

StatusOr<PageId> DevicePageStore::ClaimNextFreeIdLocked(std::uint32_t* missing_region) {
    // FM3/FM5: the search crosses regions. It does **not** create one - that
    // is a device write, and this runs under the free map's latch; a missing
    // region goes back to the caller to load outside the hold.
    for (PageId candidate = next_new_page_id_; candidate < kMaxPageCount;) {
        const std::uint32_t region = FreeMapRegionOf(candidate);
        MapRegion* pages = MutableRegion(region);
        if (pages == nullptr) {
            *missing_region = region;
            return kInvalidPageId;
        }
        auto map = std::span<std::byte, kPageSize>(pages->free_map);
        auto found = FreeMapFindFirstFree(std::span<const std::byte, kPageSize>(map),
                                          FreeMapBitIndexOf(candidate));
        if (found.has_value()) {
            const PageId id = FreeMapRegionBase(region) + *found;
            // A bitmap id is not a free page even when its bit is clear:
            // under FM6 the headerless map's id carries no bit until the
            // page is placed. Arithmetic rather than a reserved bit.
            if (IsMapPageId(id)) {
                candidate = id + 1;
                continue;
            }
            // **A listed id is the free list's, never the cursor's** (BF-R6,
            // the BF-S2 review's B2): only the pop tests it against a fault
            // in flight, and a headerless create must never take one.
            if (free_list_.count(id) != 0) {
                candidate = id + 1;
                continue;
            }
            // **The mark, in the hold that found it.** Everything below this
            // line is why the function exists.
            FreeMapAllocate(map, FreeMapBitIndexOf(id));
            pages->dirty = true;
            ++allocated_pages_;
            next_new_page_id_ = id + 1;
            return id;
        }
        candidate = FreeMapRegionBase(region + 1);
    }
    return Status::OutOfSpace("DevicePageStore: no free page id at or above " +
                              std::to_string(next_new_page_id_));
}

StatusOr<DevicePageStore::ClaimOutcome> DevicePageStore::ClaimNamedIdLocked(PageId page_id,
                                                                           bool zeros_known,
                                                                           bool device_zeros) {
    // An allocated id is in use unless the device proves it was never
    // written - the page redo re-creates after a PAGE_INIT outran its first
    // flush (`ResidentBytes` answers NotFound for the same page, and says
    // why the map can be ahead of the bytes). Decided by a resident frame or
    // the device's bytes, never by the map alone: the map bit is exactly
    // what is true of both a live page and a never-written one.
    //
    // The two maps live in this object, not in a frame, and may not have
    // reached the device yet - in use by definition. Any region's bitmaps,
    // not just region 0's.
    if (IsMapPageId(page_id)) return Status::AlreadyExists("page id already in use");
    const MapRegion* pages = FindRegion(FreeMapRegionOf(page_id));
    if (pages == nullptr) {
        // The caller loaded it before taking the hold and regions are never
        // removed, so this is unreachable; it is a refusal rather than an
        // assert because the alternative is a null dereference.
        return Status::Corruption("DevicePageStore: free-map region for page " +
                                std::to_string(page_id) + " went missing under the latch");
    }
    const std::uint32_t bit = FreeMapBitIndexOf(page_id);
    const bool allocated =
        FreeMapIsAllocated(std::span<const std::byte, kPageSize>(pages->free_map), bit);
    if (allocated) {
        if (frames_.count(page_id) != 0) return Status::AlreadyExists("page id already in use");
        // **Claimed by somebody, just not resident yet** (AM-S3). Without
        // this the torn-creation reading below applies to a page another
        // core claimed microseconds ago and has not inserted, whose bytes
        // are therefore still zeros - and two callers walk away with it.
        if (claiming_.count(page_id) != 0) {
            return Status::AlreadyExists("page id already in use");
        }
        // The bit is set and the caller did not read the device, because it
        // saw the bit clear before it took the hold. It has to look now.
        if (!zeros_known) return ClaimOutcome::kNeedsDeviceRead;
        if (!device_zeros) return Status::AlreadyExists("page id already in use");
    } else {
        ++allocated_pages_;
        // A freed id named directly - redo re-creating it, or a catalog
        // probe - leaves the free list with this claim, so no pop hands it
        // out a second time (BF-R6).
        if (free_list_.erase(page_id) != 0) NoteFreeListSize();
    }
    MapRegion* mutable_pages = MutableRegion(FreeMapRegionOf(page_id));
    FreeMapAllocate(std::span<std::byte, kPageSize>(mutable_pages->free_map), bit);
    mutable_pages->dirty = true;
    return ClaimOutcome::kClaimed;
}

StatusOr<std::span<std::byte, kPageSize>> DevicePageStore::CreateAtUnpinned(PageId page_id) {
    // **No core-0 predicate stands here since AT-S5b** (AT-R11: the
    // replacement is named where the arm was). What admits exactly one
    // caller per id is the claim itself, `ClaimNamedIdLocked` under the
    // structure latch and the map latch below - a property of the claim
    // and not of the asking core - and every loser gets `AlreadyExists`,
    // which is the code `AllocateCatalogPage`'s probe loop walks on from.
    // `catalog.md` CT5 carries why the old arm was wrong;
    // `free_map_race_test.cpp`'s `OnlyOneCallerEverPlacesAPageAtAChosenId`
    // pins the claim across eight cores in the user range, and this
    // class's own `APeerPlacesAPageAtAChosenIdInTheSystemRange` pins the
    // system half.
    if (page_id >= kMaxPageCount) {
        return Status::OutOfRange("DevicePageStore: page id " + std::to_string(page_id) +
                                  " is beyond the " + std::to_string(kMaxPageCount) +
                                  "-page design ceiling");
    }
    // **The device work first, the claim under the hold** (AM-S2). Growing
    // the file and reading it to prove a page was never written are exactly
    // what the structure latch may not span, so they run here and the
    // decision they feed is re-taken inside `ClaimNamedIdLocked` - the
    // retry idiom `FetchPinned` uses for a page in flight.
    //
    // **The slot before the claim** (BE-R1, BE-R4): a full pool refuses here,
    // with no id claimed, rather than after the map bit is set.
    auto reserved = ReserveFrame();
    if (!reserved.ok()) return reserved.status();
    ReservedFrame slot(*this, reserved.value());
    if (Status s = EnsureAddressable(page_id); !s.ok()) return s;
    if (auto region = EnsureRegionResident(FreeMapRegionOf(page_id)); !region.ok()) {
        return region.status();
    }
    // Read only if the bit looks set, which is the one case the answer is
    // needed for; a clear bit is a free page and the device says nothing
    // about it. Racy on purpose - a bit that changes under us comes back as
    // `kNeedsDeviceRead` and this loop reads it then.
    bool zeros_known = false;
    bool device_zeros = false;
    for (;;) {
        if (IsAllocated(page_id) && !zeros_known) {
            device_zeros = DeviceHoldsOnlyZeros(page_id);
            zeros_known = true;
        }
        // **Both latches, frames outer and map inner** - the declared order
        // (`device_page_store.hpp`), and a regression AM-S3's review caught
        // in AM-S3 itself. `ClaimNamedIdLocked` decides one question out of
        // two structures: the bit is the *map's*, and the in-use test
        // `frames_.count(page_id)` is the *frame table's*. Moving this guard
        // to the map latch alone left that `count` racing `InsertFrame`'s
        // `emplace` on an `unordered_map` - the same undefined behaviour
        // this stage removed from `map_regions_`, relocated into the other
        // structure, and with a worse non-crashing outcome: a missed node
        // reads as "not resident", so a live page passes the in-use test and
        // is handed to a second caller.
        //
        // Neither hold spans device work: the two device reads this loop
        // needs (`EnsureAddressable`, `DeviceHoldsOnlyZeros`) happen above,
        // outside both, and are re-validated under them.
        LatchGuard structure(structure_latch());
        LatchGuard alloc(map_latch());
        auto claimed = ClaimNamedIdLocked(page_id, zeros_known, device_zeros);
        if (!claimed.ok()) return claimed.status();
        if (claimed.value() == ClaimOutcome::kClaimed) {
            // Recorded under the same hold that set the bit, so no second
            // claimer can see the bit without seeing this (AM-S3).
            claiming_.insert(page_id);
            break;
        }
        // The bit was set after all and nobody had asked the device yet.
        // The hold ends here, and the next turn asks.
        zeros_known = false;
    }

    // Drops the claim however this function leaves, so a failed
    // `EnsureAddressable` above or a throwing allocation below cannot
    // strand an id in `claiming_` and refuse it forever (AM-S3).
    class ClaimGuard {
    public:
        ClaimGuard(DevicePageStore& store, PageId page_id) noexcept
            : store_(store), page_id_(page_id) {}
        ~ClaimGuard() {
            LatchGuard alloc(store_.map_latch());
            store_.claiming_.erase(page_id_);
        }
        ClaimGuard(const ClaimGuard&) = delete;
        ClaimGuard& operator=(const ClaimGuard&) = delete;

    private:
        DevicePageStore& store_;
        PageId page_id_;
    };
    ClaimGuard claim(*this, page_id);

    // The claim is dropped after this returns, by which time the frame is in
    // the table and `frames_.count` is what refuses the next caller.
    return PublishFreshPage(page_id, std::move(slot));
}

StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> DevicePageStore::CreateNewUnpinned() {
    return CreateNewFrom(/*headerless=*/false);
}

PageId DevicePageStore::PopFreeListLocked() {
    // The lowest id first, so reuse fills the file from the front. An id
    // that is resident or has a fault in flight stays listed for a later
    // pop: a clear bit has no resident frame (`FreePage` erases it in the
    // hold that clears the bit), so a resident one is defence, but a fault
    // that read the bit while it was still set and is reading the device now
    // is real - handing its id out would race the dead image into the new
    // page's frame (BF-R6).
    for (auto listed = free_list_.begin(); listed != free_list_.end();) {
        const PageId id = *listed;
        if (loading_.count(id) != 0 || frames_.count(id) != 0) {
            ++listed;
            continue;
        }
        listed = free_list_.erase(listed);
        MapRegion* pages = MutableRegion(FreeMapRegionOf(id));
        // Regions are never removed, and `FreePage` found this one; a set
        // bit is an id a `CreateAt` took - it leaves the list then, so this
        // is a backstop rather than a live case.
        if (pages == nullptr) continue;
        const auto map = std::span<std::byte, kPageSize>(pages->free_map);
        if (FreeMapIsAllocated(map, FreeMapBitIndexOf(id))) continue;
        FreeMapAllocate(map, FreeMapBitIndexOf(id));
        pages->dirty = true;
        ++allocated_pages_;
        ++pages_reused_;
        NoteFreeListSize();
        return id;
    }
    NoteFreeListSize();
    return kInvalidPageId;
}

StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> DevicePageStore::CreateNewFrom(
    bool headerless) {
    // The slot before the claim, as `CreateAtUnpinned`'s (BE-R1): a
    // reservation may sweep and write back, which no claim is held across.
    // A claim refused below gives the slot back with the guard.
    auto reserved = ReserveFrame();
    if (!reserved.ok()) return reserved.status();
    ReservedFrame slot(*this, reserved.value());
    PageId page_id = kInvalidPageId;
    // **The free list first** (BF-R6), under the frame table then the map -
    // the declared order - because the pop asks both: the bit is the map's,
    // and "resident or in flight" is the frame table's. The relaxed count
    // keeps the common case, an empty list, to one load. Never for a
    // headerless create.
    if (!headerless && free_listed_.load(std::memory_order_relaxed) != 0) {
        AssertOrderBeforeFrames("CreateNew");
        LatchGuard structure(structure_latch());
        LatchGuard alloc(map_latch());
        page_id = PopFreeListLocked();
    }
    const bool popped = page_id != kInvalidPageId;
    // FM3/FM5: the search crosses regions, and creates the next one when it
    // runs off the end of the last. **Every core's allocation since
    // AW-S1b**: a peer used to take its id from a run core 0 had reserved
    // for it, because a store it did not own could not reach the map. It
    // reaches this one under the structure latch, so the reservation had
    // nothing left to work around, and the hint keeps the search to one
    // region's map in the steady state.
    // **Scan and mark are one hold, and the device work is outside it**
    // (AM-S2). This used to choose an id here and mark it two calls away
    // inside `CreateAtUnpinned`, so any number of threads could scan the
    // same clear bit before one of them set it: four threads measured ~14%
    // duplicate ids (`tests/alloc_race_test.cpp`). The loop turns only when
    // the scan reaches a region this store has not loaded, which is the one
    // thing `ClaimNextFreeIdLocked` refuses to do for itself.
    while (page_id == kInvalidPageId) {
        std::uint32_t missing_region = 0;
        {
            LatchGuard alloc(map_latch());
            auto claimed = ClaimNextFreeIdLocked(&missing_region);
            if (!claimed.ok()) return claimed.status();
            page_id = claimed.value();
        }
        if (page_id != kInvalidPageId) break;
        // Outside the hold: this reads the device, or grows the file.
        if (auto region = EnsureRegionResident(missing_region); !region.ok()) {
            return region.status();
        }
    }

    // The id is this caller's alone now - its bit is set, and it was clear
    // under the hold that set it - so the rest needs no hold. A clear bit
    // has no resident frame (`FreePage` erases it in the hold that clears
    // the bit), so the in-use test `CreateAtUnpinned` makes is not repeated
    // here; a fault that races the insert below on a reused id is what
    // `InsertFrame`'s create refusal answers.
    // **A create that fails after its claim gives the claim back.** Left
    // set, the bit is an allocated id no page will ever be written at - a
    // read answers "allocated but never written" for the life of the
    // volume. Found by BF-S4's sim through an injected growth refusal; the
    // order predates BF.
    if (Status s = EnsureAddressable(page_id); !s.ok()) {
        ReleaseClaim(page_id, popped);
        return s;
    }
    if (headerless) {
        if (Status s = MarkHeaderlessBeforeInsert(page_id); !s.ok()) {
            ReleaseClaim(page_id, popped);
            return s;
        }
    }
    auto inserted = PublishFreshPage(page_id, std::move(slot));
    if (!inserted.ok()) return inserted.status();
    return std::make_pair(page_id, inserted.value());
}

void DevicePageStore::ReleaseClaim(PageId page_id, bool popped) noexcept {
    AssertNotUnderMapHold("ReleaseClaim");
    LatchGuard map(map_latch());
    MapRegion* pages = MutableRegion(FreeMapRegionOf(page_id));
    if (pages == nullptr) return;  // the claim found it; regions are never removed
    FreeMapRelease(std::span<std::byte, kPageSize>(pages->free_map), FreeMapBitIndexOf(page_id));
    pages->dirty = true;
    --allocated_pages_;
    if (popped) {
        // Back on the list it came from; the cursor passes over it there.
        --pages_reused_;
        free_list_.insert(page_id);
        NoteFreeListSize();
    } else if (next_new_page_id_ == page_id + 1) {
        // The cursor's own claim, with nothing claimed after it: the cursor
        // steps back, so the next create finds the id again. It was at or
        // above the floor when claimed, so this never lowers past it.
        next_new_page_id_ = page_id;
    } else {
        // Another claim moved the cursor past it: the list hands it out.
        free_list_.insert(page_id);
        NoteFreeListSize();
    }
}

StatusOr<PageStore::FreeOutcome> DevicePageStore::FreePage(PageId page_id) {
    if (page_id < system_floor_ || page_id >= kMaxPageCount || IsMapPageId(page_id)) {
        return Status::Corruption("DevicePageStore: page " + std::to_string(page_id) +
                                  " is a system or map page and is never freed");
    }
    // **One hold, the frame table then the map** (BF-R5): the frame's erase
    // and the bit's clear are one step, so no accessor - each tests the bit
    // under the frame table's hold before it serves a frame - ever finds a
    // clear bit with a resident frame.
    AssertOrderBeforeFrames("FreePage");
    LatchGuard structure(structure_latch());
    LatchGuard map(map_latch());
    if (!IsAllocatedLocked(page_id)) {
        return Status::NotFound("DevicePageStore: page " + std::to_string(page_id) +
                                " is already free");
    }
    // A headerless page is unlogged and carries no checksum, so a reused id
    // would read its dead image after a crash; no relation owns one.
    if (IsHeaderlessLocked(page_id)) {
        return Status::Corruption("DevicePageStore: page " + std::to_string(page_id) +
                                  " is headerless and is never freed");
    }
    // In flight: a fault reading the device, or a `CreateAt` between its
    // claim and its insert. Either finishes, and the caller asks again.
    if (loading_.count(page_id) != 0 || claiming_.count(page_id) != 0) {
        return FreeOutcome::kDeferred;
    }
    if (auto it = frames_.find(page_id); it != frames_.end()) {
        Frame& frame = it->second;
        // A pin or a latch is a holder; a writeback's claim holds a raw
        // `Frame*` from its copy to its clean. The bytes are dead either
        // way, but the frame is not this call's to take from under them.
        if (frame.pins != 0 || frame.writing ||
            (latch_armed_ && PageLatch::IsHeld(frame.latch))) {
            return FreeOutcome::kDeferred;
        }
        if (RawPageType(std::span<const std::byte, kPageSize>(*frame.bytes)) ==
            static_cast<std::uint8_t>(PageType::kCabinBound)) {
            return Status::Corruption("DevicePageStore: page " + std::to_string(page_id) +
                                      " is a Bound Cabin page and is never freed");
        }
        // Dirty or not (BF-Q7 (a)): after the replay gate no record names the
        // page, so its recLSN guards nothing, and a writeback of the dead
        // bytes could land over a reuse. The slot goes back poisoned
        // (`ReturnSlotLocked`), so a span someone kept reads 0xEF.
        ReleaseFrameLocked(frame);
    }
    MapRegion* pages = MutableRegion(FreeMapRegionOf(page_id));  // allocated, so present
    FreeMapRelease(std::span<std::byte, kPageSize>(pages->free_map), FreeMapBitIndexOf(page_id));
    pages->dirty = true;
    --allocated_pages_;
    ++pages_freed_;
    free_list_.insert(page_id);
    NoteFreeListSize();
    if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
        log_->Trace("pagestore", "freed page=" + std::to_string(page_id));
    }
    return FreeOutcome::kFreed;
}

std::uint64_t DevicePageStore::pages_freed() const noexcept {
    AssertNotUnderMapHold("pages_freed");
    LatchGuard map(map_latch());
    return pages_freed_;
}

std::uint64_t DevicePageStore::pages_reused() const noexcept {
    AssertNotUnderMapHold("pages_reused");
    LatchGuard map(map_latch());
    return pages_reused_;
}

Status DevicePageStore::RaiseAllocationFloor(PageId first_allocatable_page_id) {
    // Equal is the legal terminal case - "no id left" - and CreateNew()
    // already reports that as OutOfSpace. Above it there is no bit to find
    // and no page to address, so the log named an id this build cannot have
    // written.
    if (first_allocatable_page_id > kMaxPageCount) {
        return Status::OutOfRange("DevicePageStore: allocation floor " +
                                  std::to_string(first_allocatable_page_id) +
                                  " is beyond the " + std::to_string(kMaxPageCount) +
                                  "-page design ceiling");
    }
    if (first_allocatable_page_id > next_new_page_id_) {
        next_new_page_id_ = first_allocatable_page_id;
        if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
            log_->Debug("pagestore", "allocation floor raised to " +
                                         std::to_string(next_new_page_id_));
        }
    }
    return Status::OK();
}

StatusOr<std::span<std::byte, kPageSize>> DevicePageStore::GetUnpinned(PageId page_id) {
    return Resolve(page_id, /*mark_dirty=*/true, FetchHeat::kWarm);
}

StatusOr<std::span<std::byte, kPageSize>> DevicePageStore::GetForReadUnpinned(PageId page_id) {
    return Resolve(page_id, /*mark_dirty=*/false, FetchHeat::kWarm);
}

StatusOr<std::span<std::byte, kPageSize>> DevicePageStore::Resolve(PageId page_id,
                                                                  bool mark_dirty,
                                                                  FetchHeat heat) {
    // **The device-map adoption went with the lease** (AW-S1b): it existed
    // because a peer's private copy of the free map was taken at its mount
    // and went stale from that moment, and there is one copy now.
    if (!IsAllocated(page_id)) return NotAllocated(page_id);
    return ResidentBytes(page_id, mark_dirty, heat);
}

Status DevicePageStore::NotAllocated(PageId page_id) const {
    // redo.cpp:322 learned this on its own path: "page id not found" alone
    // says nothing about *which* id.
    return Status::NotFound("page id " + std::to_string(page_id) + " not found");
}

Status DevicePageStore::StampPageLsn(PageId page_id, std::uint64_t lsn) {
    if (lsn == wal::kNoLsn) {
        return Status::InvalidArgument(
            "DevicePageStore: page_lsn 0 means 'never logged' and cannot be stamped");
    }
    // **Under the structure latch** (AM-S2 step 3e). This walks a table
    // another core may be growing, and the failure is not theoretical: with
    // three inserters against three stampers,
    // `tests/frame_table_race_test.cpp` reports "a stamp failed on a
    // resident page" without this hold - the `find` raced a rehash and
    // missed, so a page that was certainly resident answered `NotFound` and
    // its caller lost the stamp. Held across the byte writes too, which
    // costs nothing: they are memory, and the page latch the caller already
    // holds is what makes them safe.
    //
    // No path reaches this with the latch held - every caller is a
    // transaction, catalog, recovery or assertion write that got here
    // through a checked accessor first.
    LatchGuard structure(structure_latch());
    auto it = frames_.find(page_id);
    if (it == frames_.end()) {
        return Status::NotFound("DevicePageStore: page " + std::to_string(page_id) +
                                " is not resident, so its page_lsn cannot be stamped");
    }

    SetPageLsn(std::span<std::byte, kPageSize>(*it->second.bytes), lsn);
    // PW1c-3, PL §9 rule 4: the core that last wrote the page. Rides the
    // LSN stamp because the two answered one question - *whose* offset is
    // page_lsn - while streams were per core. With one stream (AR0 M0) and
    // the ownership reading gone (AW-S1b) it is a diagnostic `SHOW PAGE`
    // prints, and nothing refuses on it (`page_header.hpp`).
    //
    // **Unless this pass is recovering on every core's behalf** (AR0 M0;
    // the header's `SetStampSuppressed` says why). The page_lsn above is
    // still stamped, because idempotence is that field's job; what is
    // withheld is the stream stamp.
    //
    // **`CurrentCore()`, not `core_id_`** (AM-S2 step 3, the same argument
    // the page latch's owner field made). The stamp answers *whose* offset
    // the page_lsn beside it is, which is a fact about the core that wrote
    // the record - not about the store the frame happens to live in. They
    // are the same thing only while each core has a store of its own, and
    // step 3 ends that.
    if (!stamp_suppressed_) {
        SetPageStreamStamp(std::span<std::byte, kPageSize>(*it->second.bytes),
                           StreamStampFor(CurrentCore()));
    }
    it->second.MarkDirty(++dirty_gens_);
    // First record since the frame was last written back wins: recLSN is
    // the *oldest* LSN redo must replay to make the page whole, so a later
    // record must never overwrite it (wal.md section 11-1).
    if (it->second.rec_lsn == wal::kNoLsn) it->second.rec_lsn = lsn;
    return Status::OK();
}

Status DevicePageStore::SetCoreWalGate(std::uint32_t core, wal::WalDurability* gate) {
    if (core > kPageLatchMaxCoreId) {
        return Status::InvalidArgument("DevicePageStore: core " + std::to_string(core) +
                                       " is past the page latch's core-id bound " +
                                       std::to_string(kPageLatchMaxCoreId) +
                                       ", so there is no WAL gate slot for it");
    }
    if (gate == nullptr) {
        return Status::InvalidArgument("DevicePageStore: a core's WAL gate is taken back with "
                                       "ClearCoreWalGate, not set to null");
    }
    wal::WalDurability* expected = nullptr;
    if (!core_wal_gates_[core].compare_exchange_strong(expected, gate, std::memory_order_acq_rel) &&
        expected != gate) {
        return Status::AlreadyExists("DevicePageStore: core " + std::to_string(core) +
                                     " already has a WAL gate of its own; a second runtime for "
                                     "a live core id would take its writebacks");
    }
    return Status::OK();
}

void DevicePageStore::ClearCoreWalGate(std::uint32_t core,
                                       const wal::WalDurability* gate) noexcept {
    if (core > kPageLatchMaxCoreId) return;
    wal::WalDurability* expected = const_cast<wal::WalDurability*>(gate);
    core_wal_gates_[core].compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

wal::WalDurability* DevicePageStore::GateForCaller() const noexcept {
    const std::uint32_t core = CurrentCore();
    if (core <= kPageLatchMaxCoreId) {
        if (wal::WalDurability* own = core_wal_gates_[core].load(std::memory_order_acquire)) {
            return own;
        }
    }
    return wal_gate_;
}

Status DevicePageStore::AwaitWalGate(std::span<const PageId> page_ids, wal::WalDurability* gate) {
    if (gate == nullptr) return Status::OK();

    // **The scan under the hold, the wait outside it** (AM-S2 step 3f), and
    // this one was owed rather than noticed: `device_page_store.hpp`'s
    // acquisition-order block already said "whatever latch that scan comes
    // to need must be dropped before `EnsureDurable`, or the wait acquires
    // exactly the 'latched across a durability wait' shape this bullet
    // rules out". `EnsureDurable` waits on an `fdatasync`: the writer
    // thread's for a peer, core 0's own inline one for core 0.
    //
    // One EnsureDurable for the batch maximum, not one per page: the call
    // is a no-op once the watermark is past, so the highest page_lsn in
    // the batch subsumes every other.
    wal::Lsn highest = wal::kNoLsn;
    {
    LatchGuard structure(structure_latch());
    for (const PageId page_id : page_ids) {
        auto it = frames_.find(page_id);
        if (it == frames_.end() || !it->second.dirty) continue;
        // Skipped for the same reason the stamping loop in Flush() skips it
        // (StampIfHeadered): a headerless page has no page_lsn field, so the
        // bytes at that offset are entry data. Reading them yields a
        // meaningless watermark - a page of 0xFF entry bytes reads as
        // 0xFFFF... - and EnsureDurable can only refuse it, which failed
        // every Flush() on any database with a dirty headerless page and so
        // made SYNC and the checkpointer unable to persist anything at all.
        // Sound only while headerless pages are unlogged. If a headerless
        // class becomes logged, its LSN has to reach the gate out of band
        // rather than through a field the format does not have.
        if (IsHeaderless(page_id)) continue;
        const std::uint64_t page_lsn =
            GetPageLsn(std::span<const std::byte, kPageSize>(*it->second.bytes));
        if (page_lsn > highest) highest = page_lsn;
    }
    }
    if (highest == wal::kNoLsn) return Status::OK();  // nothing logged in this batch

    if (Status s = gate->EnsureDurable(highest); !s.ok()) {
        // Refusing the flush is the whole point: writing the page anyway
        // would put data on disk ahead of the log that describes it.
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("pagestore", "WAL gate refused a flush up to page_lsn " +
                                         std::to_string(highest) + ": " + s.message());
        }
        return s;
    }
    return Status::OK();
}

void DevicePageStore::AwaitWritebackClaim(PageId page_id) {
    // A writeback's claim spans a device write and, at worst, a WAL sync -
    // milliseconds, not the nanoseconds a page latch is held for - so the
    // spin is brief and the rest of the wait sleeps rather than yields
    // (`base/latch.hpp` says what a yield loop costs against an fsync).
    for (std::uint32_t turn = 0;; ++turn) {
        {
            LatchGuard structure(structure_latch());
            auto it = frames_.find(page_id);
            if (it == frames_.end() || !it->second.writing) return;
        }
        if (turn < 64) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
}

StatusOr<std::size_t> DevicePageStore::WriteBack(std::span<const PageId> page_ids,
                                                  HeldFrames held) {
    // Ascending and unique: id order is file order (page.md section 13),
    // and the queue this drains may name a page twice across sweeps.
    std::vector<PageId> ordered(page_ids.begin(), page_ids.end());
    std::sort(ordered.begin(), ordered.end());
    ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());

    // (1) durable: one gate call for the batch maximum, before any byte
    // moves - the whole of flush-before-evict. **One gate for the whole
    // writeback** (BA-S1): the calling core's, chosen once, so the batch's
    // call and every run's below ask the same manager.
    wal::WalDurability* const gate = GateForCaller();
    // **Nothing at all once the log fail-stopped** (`wal/durability.hpp`).
    // The page_lsn gate cannot catch it: a page whose record was refused
    // keeps its old, durable page_lsn - a split's leaf, a new page at
    // kNoLsn, a var-heap body - and writing it would put a change on the
    // device that no record lets recovery redo or undo.
    if (gate != nullptr && gate->stopped()) {
        return Status::IoError("pagestore: no page is written back after the log fail-stopped; "
                               "restart the instance to recover");
    }
    if (Status s = AwaitWalGate(ordered, gate); !s.ok()) return s;

    // **Three phases per run, and the device call is the one between the
    // holds** (AM-S2 step 3f). The run detection, the checksum stamp and the
    // clean all walk `frames_`, which another core may be growing; the write
    // is device I/O, which this latch may never span.
    //
    // **Every run is copied under the first hold**, which is AM-S3's answer
    // to a defect the phase split alone did not close: the bytes and the
    // `page_lsn` have to be frozen together, or the gate is asked about a
    // record other than the one describing what goes out. **The claim is
    // what keeps the *frame* alive across the three phases** (AT-S10e's
    // `Frame::writing`): `FreePage` defers a claimed frame, `ReleaseScanSlot`
    // abandons a dirty one and the sweep only queues it, so the third
    // phase's `find` is certain to hit. A frame `FreePage` discarded before
    // the claim is simply not found here, and is not this writeback's.
    //
    // **And each page is copied under its own page latch, shared** (AT-S8,
    // step 1b). The structure latch orders the table, not the bytes: a
    // writer holds a frame's page latch exclusive while it mutates, and the
    // frame is marked dirty at the *fetch*, before the mutation. A copy taken
    // under the structure latch alone could therefore be half of another
    // core's write - persisted with a valid checksum - and the clean below
    // then dropped the dirty mark and the recLSN of a frame whose last write
    // had not gone out: a committed change the next checkpoint's dirty table
    // no longer names and the redo start may pass, and one a clean eviction
    // discards outright. Found by AT-S8's review, present since AM-S2 step 3
    // made one pool serve every core. The share waits out a foreign
    // exclusive holder, which is running rather than parked - no task parks
    // holding a pin (the header's page-latch section) - and one page latch is
    // held at a time, so writeback adds no page-against-page pair. The clean
    // is then conditional on what the copy saw (`CopiedPage`).
    //
    // **And each frame is claimed from its copy to its clean** (AT-S10e,
    // `Frame::writing`). The generation made the clean honest and left the
    // *order* of two writebacks' device writes to chance: one that copied
    // an older image could land it after a newer one had gone out and
    // cleaned the frame. A claimed frame is another writeback's until its
    // clean; `kWait` waits for that and looks again, `kSkip` leaves it. The
    // wait holds no claim of this call's own - a run is claimed, written
    // and released before the next is looked at - so two writebacks cannot
    // wait on each other.
    struct CopiedPage {
        Frame* frame = nullptr;
        std::uint32_t dirty_gen = 0;  // the frame's generation at the copy
    };
    std::size_t written = 0;
    std::vector<std::byte> scratch;
    std::vector<CopiedPage> copied;
    // Reserved once, so the push under the structure latch - after a claim
    // is set - cannot allocate.
    copied.reserve(kWritebackRunPages);
    // Gives back the claims on `copied` when a run leaves its iteration by
    // any path - an error return, a thrown allocation - that is not the
    // clean, which gives them back itself and empties `copied`. A claimed
    // frame is dirty, so it is still in the table.
    struct ClaimRelease {
        DevicePageStore& store;
        std::vector<CopiedPage>& copied;
        ~ClaimRelease() {
            if (copied.empty()) return;
            LatchGuard structure(store.structure_latch());
            for (CopiedPage& page : copied) page.frame->writing = false;
            copied.clear();
        }
    };
    for (std::size_t i = 0; i < ordered.size();) {
        std::size_t run = 0;
        std::uint64_t run_lsn = wal::kNoLsn;
        copied.clear();
        const ClaimRelease claims{*this, copied};
        bool claimed_elsewhere = false;
        {
            LatchGuard structure(structure_latch());
            auto it = frames_.find(ordered[i]);
            if (it == frames_.end() || !it->second.dirty) {
                ++i;  // evicted, or already written by someone else: not ours
                continue;
            }
            if (it->second.writing) {
                if (held == HeldFrames::kSkip) {
                    ++i;  // its writer carries it, or leaves it dirty
                    continue;
                }
                claimed_elsewhere = true;
            } else {
                // Extend the run while the next ids are consecutive,
                // resident, dirty and unclaimed - the shape one
                // WritePageRun can take.
                run = 1;
                while (run < kWritebackRunPages && i + run < ordered.size() &&
                       ordered[i + run] == ordered[i] + run) {
                    auto next = frames_.find(ordered[i + run]);
                    if (next == frames_.end() || !next->second.dirty || next->second.writing) {
                        break;
                    }
                    ++run;
                }

                // Claimed and pinned here, under the hold that found them.
                // The claim and the dirty mark keep the frame resident - only
                // the claim's holder cleans it - and the pin is the half of
                // the page latch `UnpinFrame` gives back after the copy.
                for (std::size_t k = 0; k < run; ++k) {
                    Frame& frame = frames_.find(ordered[i + k])->second;
                    CountPin(frame);
                    frame.writing = true;
                    copied.push_back(CopiedPage{&frame});
                }
            }
        }
        if (claimed_elsewhere) {
            // Outside the hold. The other writeback's copy may be older than
            // what this caller must see written, so this waits for its clean
            // and then asks again: still dirty means a later write it did not
            // carry, and this writeback takes it.
            AwaitWritebackClaim(ordered[i]);
            continue;
        }

        scratch.resize(run * kPageSize);
        for (std::size_t k = 0; k < run; ++k) {
            const PageId page_id = ordered[i + k];
            CopiedPage& page = copied[k];
            // Outside the structure latch, PinFrame's order. In the background
            // drain a held frame ends the run here: the prefix goes out, the
            // rest keep their dirty marks, and their pins are returned.
            if (held == HeldFrames::kWait) {
                AcquirePageLatch(page_id, *page.frame, PinMode::kShared);
            } else if (!TryAcquirePageLatchShared(page_id, *page.frame)) {
                LatchGuard structure(structure_latch());
                for (std::size_t rest = k; rest < run; ++rest) {
                    UncountPin(*copied[rest].frame);
                    copied[rest].frame->writing = false;
                }
                copied.resize(k);
                run = k;
                break;
            }
            {
                LatchGuard structure(structure_latch());
                // Our share excludes every other core's exclusive hold, so the
                // bytes are whole, and the generation covers every write
                // after it. **A flush by a task holding this page exclusive
                // is excluded** (the header's `kWait` rule): the share would
                // re-enter its own latch, but since AT-S10e another writeback
                // can hold the claim and wait on that latch while this one
                // waits on the claim.
                page.dirty_gen = page.frame->dirty_gen;

                // (2) checksum, the last thing that touches a page before it
                // goes out (page.md section 8) - skipped for a headerless
                // page, which has no field to put one in.
                StampIfHeadered(page_id, std::span<std::byte, kPageSize>(*page.frame->bytes));

                // **Copied under the hold, always, and that is what makes the
                // gate below mean anything** (AM-S3). The batch gate above runs
                // once for the batch maximum, and with one table a stamper on
                // another core can raise a page's `page_lsn` between that call
                // and this write - so the bytes going out would be described by
                // a log record the gate never covered. Measured before this
                // copy: **34 pages** reached the device past the durable point
                // in one run of
                // `EvictionWritebackTest.FlushBeforeEvictHoldsWithTwoCoresDirtyingOnePage`.
                //
                // The copy freezes the bytes *and* the LSN together, so the
                // number gated on below is the number in what is written. The
                // single-page arm used to skip this, safe because the stamper
                // and the writeback were one thread; that is what sharing ends,
                // and the memcpy is the cost AM-S3 exists to price.
                std::memcpy(scratch.data() + k * kPageSize, page.frame->bytes->data(), kPageSize);
            }
            UnpinFrame(page_id);  // the share and the pin, one page at a time
            if (after_writeback_copy_for_test_) after_writeback_copy_for_test_(page_id);

            // **Skipped for a headerless page**, for the reason
            // `AwaitWalGate` and the stamping loop skip it: there is no
            // page_lsn field there, so the bytes at that offset are
            // payload and read back as an arbitrary number. Found by the
            // simulation corpus, which failed on nine seeds with
            // `lsn 18446744073709551615 is at or past the append point`
            // - a var-heap body's bytes, gated on as though they were a
            // record.
            if (IsHeaderless(page_id)) continue;
            const std::uint64_t lsn = GetPageLsn(std::span<const std::byte, kPageSize>(
                scratch.data() + k * kPageSize, kPageSize));
            if (lsn > run_lsn) run_lsn = lsn;
        }

        if (run == 0) {  // the run's first frame was held: nothing to write
            ++i;
            continue;
        }

        // **The gate, on exactly what is about to go out.** `AwaitWalGate`
        // above is the batch's one call and stays, because it is a no-op
        // once the watermark is past and covers the ordinary case in one
        // round trip; this is the arm that catches a page whose record
        // arrived after it.
        if (gate != nullptr && run_lsn != wal::kNoLsn && !gate->IsDurable(run_lsn)) {
            if (Status s = gate->EnsureDurable(run_lsn); !s.ok()) {
                if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                    log_->Error("pagestore", "WAL gate refused a writeback up to page_lsn " +
                                                 std::to_string(run_lsn) + ": " + s.message());
                }
                return s;
            }
        }

        // (3) write: one device call for a run, per page otherwise, and
        // best-effort by spec §4 - a device without a real scatter write
        // still sees the pages land in file order.
        const Status wrote =
            run > 1 ? device_.WritePageRun(ordered[i], static_cast<std::uint32_t>(run),
                                           std::span<const std::byte>(scratch))
                    : device_.WritePage(ordered[i], std::span<const std::byte, kPageSize>(
                                                        scratch.data(), kPageSize));
        if (!wrote.ok()) {
            if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                log_->Error("pagestore", "write failed for page " +
                                             std::to_string(ordered[i]) + " (run of " +
                                             std::to_string(run) + "): " + wrote.message());
            }
            return wrote;
        }

        // (4) clean, only now: a failure above leaves the frame dirty and
        // its recLSN intact, so the next writeback retries it. **And only
        // what the copy covered** (step 1b): a generation that moved since
        // the copy is a write fetched after it, whose bytes did not go out;
        // that frame stays dirty with its recLSN, and a later writeback
        // carries it.
        // The claim goes back here too, clean or not, under the same hold: a
        // waiter that sees it gone sees the clean with it.
        {
            LatchGuard structure(structure_latch());
            for (std::size_t k = 0; k < run; ++k) {
                Frame& frame = *copied[k].frame;  // resident: claimed, so dirty
                frame.writing = false;
                if (frame.dirty_gen != copied[k].dirty_gen) continue;
                frame.dirty = false;
                frame.rec_lsn = wal::kNoLsn;  // nothing to replay into it
                if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
                    log_->Trace("pagestore", "wrote page=" + std::to_string(ordered[i + k]));
                }
            }
            copied.clear();  // given back here, so `claims` has nothing to do
        }
        written += run;
        i += run;
    }
    return written;
}

StatusOr<std::size_t> DevicePageStore::DrainDirtyEvictionQueue() {
    const std::vector<PageId> queued = TakeDirtyEvictionQueue();
    if (queued.empty()) return std::size_t{0};
    auto written = WriteBack(queued, HeldFrames::kSkip);
    if (written.ok()) dirty_drained_.Add(written.value());
    if (written.ok() && written.value() > 0 && log_ != nullptr &&
        log_->enabled(LogLevel::kDebug)) {
        log_->Debug("pagestore", "writeback drained " + std::to_string(written.value()) +
                                     " queued dirty page(s)");
    }
    return written;
}

std::size_t DevicePageStore::MaintainFreeReserve() {
    if (capacity_ == 0) return 0;
    const std::size_t low = std::max<std::size_t>(capacity_ / 16, 1);
    const std::size_t high = std::max<std::size_t>(capacity_ / 8, low);
    // Slots in use include every fill in flight: what the next reservation sees.
    auto free_count = [this] {
        const std::size_t used = SlotsInUseLocked() + window_promised_;
        return used < capacity_ ? capacity_ - used : 0;
    };
    {
        LatchGuard structure(structure_latch());
        if (free_count() >= low) return 0;
    }
    std::size_t reclaimed_total = 0;
    ReclaimWalk walk;
    // One deficit's worth per call at most: peers faulting meanwhile can take
    // freed slots as fast as batches free them, and the next tick goes on.
    while (reclaimed_total < high) {
        SweepResult batch;
        std::size_t slots = 0;
        {
            LatchGuard structure(structure_latch());
            const std::size_t free = free_count();
            if (free >= high) break;
            const std::size_t want = std::min(ReclaimBatch(), high - free);
            batch = SweepLocked(want, kBatchStepsPerFrame * want);
            slots = SlotCountLocked();
        }
        batches_background_.Add();
        batch_steps_background_.Add(batch.steps);
        reclaimed_background_.Add(batch.reclaimed);
        reclaimed_total += batch.reclaimed;
        auto drained = DrainDirtyEvictionQueue();  // a failure stays dirty, queued again later
        const std::size_t cleaned = drained.ok() ? drained.value() : 0;
        if (walk.Done(batch, batch.reclaimed != 0 || cleaned != 0 || batch.progressed, slots)) {
            break;
        }
    }
    return reclaimed_total;
}

Status DevicePageStore::Flush() {
    // **Collect under the hold, write outside it** (AM-S2 step 3f). The walk
    // is a read of a table another core may be growing; the writeback below
    // is device I/O, which this latch may never span. A page that turns
    // dirty after the collection is next flush's, which is what "flush what
    // was dirty when asked" has always meant.
    std::vector<PageId> dirty;
    {
        LatchGuard structure(structure_latch());
        dirty.reserve(frames_.size());
        for (const auto& [page_id, frame] : frames_) {
            if (frame.dirty) dirty.push_back(page_id);
        }
    }

    auto written = WriteBack(dirty);
    if (!written.ok()) return written.status();

    if (auto flushed = FlushMaps(); !flushed.ok()) return flushed.status();
    if (written.value() > 0 && log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("pagestore",
                    "flushed " + std::to_string(written.value()) + " dirty page(s)");
    }
    return Status::OK();
}

Status DevicePageStore::Sync() {
    if (Status s = Flush(); !s.ok()) return s;
    Status s = device_.Sync();
    if (log_ != nullptr) {
        if (!s.ok() && log_->enabled(LogLevel::kError)) {
            log_->Error("pagestore", "device sync failed: " + s.message());
        } else if (s.ok() && log_->enabled(LogLevel::kDebug)) {
            log_->Debug("pagestore", "device synced, " + std::to_string(resident_pages()) +
                                         " page(s) resident");
        }
    }
    return s;
}

std::vector<PageId> DevicePageStore::DirtyPageIds() const {
    // The whole body under the hold: it is a collection and touches no
    // device, so there is nothing to hoist out (AM-S2 step 3f).
    LatchGuard structure(structure_latch());
    std::vector<PageId> dirty;
    dirty.reserve(frames_.size());
    for (const auto& [page_id, frame] : frames_) {
        if (frame.dirty) dirty.push_back(page_id);
    }
    std::sort(dirty.begin(), dirty.end());
    return dirty;
}

std::vector<std::pair<PageId, wal::Lsn>> DevicePageStore::DirtyPagesWithRecLsn() const {
    // As `DirtyPageIds`, and for the same reason.
    LatchGuard structure(structure_latch());
    std::vector<std::pair<PageId, wal::Lsn>> dirty;
    dirty.reserve(frames_.size());
    for (const auto& [page_id, frame] : frames_) {
        if (frame.dirty) dirty.emplace_back(page_id, frame.rec_lsn);
    }
    std::sort(dirty.begin(), dirty.end());
    return dirty;
}

Status DevicePageStore::PersistPage(PageId page_id) {
    const PageId ids[] = {page_id};
    if (auto written = WriteBack(ids); !written.ok()) return written.status();
    return device_.Sync();
}

Status DevicePageStore::FlushPages(std::span<const PageId> page_ids) {
    // The checkpointer's route through the one writeback primitive - §4's
    // "consumer of the machinery, not a parallel implementation".
    auto written = WriteBack(page_ids);
    if (!written.ok()) return written.status();
    bool wrote_any = written.value() > 0;

    // The maps go out with them, and after them: a page is only reachable
    // once the map says its id is allocated, so publishing the map first
    // would let a crash expose a page whose bytes never landed. A bit
    // `FreePage` cleared needs no order: no replay names the id (BF-R4).
    //
    // **One walk, not two** (AM-S3). This asked `maps_dirty()` first and
    // then let `FlushMaps` walk the map again; under sharing the two
    // answers can differ, because another core's flush can take the work
    // in between - and this caller would then sync for a write nobody
    // made. The count `FlushMaps` returns is the same answer taken once,
    // and under the hold.
    auto flushed = FlushMaps();
    if (!flushed.ok()) return flushed.status();
    if (flushed.value() > 0) wrote_any = true;

    // **Why a zero count may skip the sync, and where that stops being
    // enough.** Under sharing this call can write nothing because another
    // core's flush already copied the dirty regions and is between its copy
    // and its own sync. For the checkpointer - this function's reason to
    // exist (`wal/checkpointer.cpp`) - that is covered: a checkpoint's
    // durability point is the anchor publish, and
    // `SuperBlockCheckpointAnchor::Publish` syncs the store itself, after
    // every step. The contract above already tolerates the same shape for
    // *pages*: "something else may have flushed them since the snapshot".
    //
    // The one caller that reaches no anchor publish - `expeditor.cpp`'s
    // catalog flush, since AT-S9 retired the range allocator's handoff
    // flush - does not have that cover, and the skip is unchanged in shape
    // from before AM-S3, where
    // `maps_dirty()` read false in the same window. `PersistMaps` syncs
    // unconditionally for exactly this reason and this does not; the
    // asymmetry is deliberate, because a checkpoint step that synced on
    // every round would pay an `fsync` per step for a map it did not write.
    if (!wrote_any) return Status::OK();  // nothing written, nothing to sync
    Status s = device_.Sync();
    if (log_ != nullptr) {
        if (!s.ok() && log_->enabled(LogLevel::kError)) {
            log_->Error("pagestore", "checkpoint sync failed: " + s.message());
        } else if (s.ok() && log_->enabled(LogLevel::kDebug)) {
            log_->Debug("pagestore", "checkpoint wrote " + std::to_string(written.value()) +
                                         " named page(s) and synced");
        }
    }
    return s;
}

// ---- Frame reclamation (docs/inflight/in-progress/workplan-eviction.md EV01-EV02) -------------

// The virtual seam, which is `FetchAndPin` with the ring's fault answer
// thrown away: nothing reachable through `PageStore` has a use for it, and
// widening the base's signature to carry a parameter one nested class reads
// would put it in every store that never faults anything.
StatusOr<std::span<std::byte, kPageSize>> DevicePageStore::FetchPinned(PageId page_id, PinMode mode,
                                                                      bool for_read,
                                                                      FetchHeat heat) {
    return FetchAndPin(page_id, mode, for_read, heat, /*faulted=*/nullptr);
}

StatusOr<std::span<std::byte, kPageSize>> DevicePageStore::FetchAndPin(PageId page_id, PinMode mode,
                                                                      bool for_read,
                                                                      FetchHeat heat,
                                                                      bool* faulted) {
    if (faulted != nullptr) *faulted = false;
    // **AM-S2: the pair the shared pool must not let anything between.**
    // `page_store.hpp` records the obligation; this discharges it. Every
    // accessor used to be `bytes = *Unpinned(id)` then `PinFrame(id)`, and
    // in that window a frame can be evicted and its slot reused - by
    // another page, so `PageRef` reads that page's bytes - or by the *same*
    // page re-faulted into another slot, so the pin lands on a **different**
    // frame while the bytes point at the old slot, with every pin gauge
    // balancing perfectly. Slot reuse (BE-R1) makes that the ordinary
    // outcome of the window rather than a rare one, and it is why this pair
    // is one operation now rather than two calls the accessor makes.
    Latch* latch = structure_latch();
    if (latch == nullptr) {
        // **Unarmed: today's shape and today's cost.** One thread reaches
        // this store, nothing can evict between the two calls, and the
        // window above does not exist - which is G2 as a property of the
        // code rather than of a flag.
        // `Resolve` is what `GetUnpinned` and `GetForReadUnpinned` are, with
        // the usage bump made the caller's (AM-R8a): the ring is the one
        // caller that passes false. The find is the ring's fault answer and
        // is **exact** here for the same reason the window does not exist:
        // one thread. Computed only when someone asked: it is a second hash
        // and probe on the single-core hot path, and the ring is the only
        // caller that passes a non-null `faulted`.
        const bool was_resident =
            faulted != nullptr && frames_.find(page_id) != frames_.end();
        auto bytes = Resolve(page_id, /*mark_dirty=*/!for_read, heat);
        if (!bytes.ok()) return bytes.status();
        PinFrame(page_id, mode);
        if (faulted != nullptr) *faulted = !was_resident;
        return bytes.value();
    }

    // **Armed: the pair runs under one hold of the structure latch**, which
    // is what closes the window. `PinFrame` takes the latch itself, so the
    // pin is done inline here rather than by calling it - a second
    // acquisition on this thread would hang, `Latch` being a plain
    // `std::mutex`.
    //
    // **Step 2b: the latch is never held across the device read.** A miss
    // records the page id in `loading_`, drops the latch, does the raw fetch
    // outside it, then re-takes it to publish and pin. That is what keeps
    // one core's miss from blocking every other core's *hit* for the length
    // of a disk read. It is **not** what makes latching the erasers
    // possible: the sweep runs at a fill's reservation (BE-S2), under holds
    // of its own, on no path this function takes with the latch held.
    //
    // **What that sentence rests on, since it is not local.** The *hit* path
    // below still calls the whole raw fetch under the latch, and `Resolve`
    // begins with `IsAllocated`, whose false arm used to be a device read of
    // its own (`AdoptDeviceMapOnMiss`, struck at AW-S1b with the private map
    // copy it reconciled). No resident page reaches it, because a page is
    // only ever made resident after its bit is set, and `FreePage` erases a
    // frame in the hold that clears its bit, so no clear bit has a resident
    // frame (BF-R5) - "no I/O on a hit" is a property of *that* invariant,
    // not of this function. `ResidentBytes`' first act
    // carried a `ReadPage` too while the stamp claim lived there, and was
    // excluded the same way rather than by not being there: it read the
    // device only on the branch where the frame is *absent*, and on a hit
    // the frame is present, so the stamp came out of the resident bytes.
    AssertOrderBeforeFrames("FetchPinned");
    std::unique_lock<Latch> hold(*latch);
    for (;;) {
        // **The `loading_` half of the test below outlived the race it was
        // written for** - an inline sweep's unlatched hand-pin on the fresh
        // frame, gone since the sweep took the latch - **and keeps a
        // different job.**
        //
        // What the test still does is keep the two halves of a fault from
        // disagreeing: between `InsertFrame` and the erase below the page is
        // resident *and* in flight, and a hit taken in that window would
        // return bytes whose loader has not yet finished its own accounting.
        // Cheap, narrow, and the honest reason - the old one was measured
        // and did not hold up. A counter on the sweep block put its
        // execution count at **0** in both of
        // `am_s2_pin_protocol_test.cpp`'s cells, so the "five runs out of
        // five with the test deleted" that once stood here said only that
        // the fixture never entered the window at all.
        const bool resident = frames_.find(page_id) != frames_.end();
        if (resident && loading_.find(page_id) == loading_.end()) {
            // **The hit path runs the raw fetch under the latch, and that is
            // not the thing forbidden above.** On a hit the fetch touches no
            // device: it finds the frame, applies the dirty mark and the
            // usage bump, and returns the span. Calling it here rather than
            // reproducing those two side effects is what keeps `Get` and
            // `GetForRead` meaning exactly what they meant.
            auto bytes = Resolve(page_id, /*mark_dirty=*/!for_read, heat);
            if (!bytes.ok()) return bytes.status();
            // The `nullopt` arm is **unreachable as the code stands**: the
            // latch is held continuously from the `resident` test above
            // through the tail's own find, and the only eraser the raw fetch
            // can reach is the reservation's sweep, which runs on its miss
            // branch - the branch a resident page does not take. Rounding the loop is
            // the arm a future hit path that could insert would need, but
            // "evicted under us" is not what happens here today.
            //
            // The span the tail returns is `bytes.value()`: `ResidentBytes`'
            // resident branch built that one from this same frame under this
            // same continuous hold, so there is one span and not two.
            auto pinned = PinResidentAndRelease(page_id, mode, hold);
            if (!pinned.has_value()) continue;
            return *pinned;
        }
        if (loading_.find(page_id) != loading_.end()) {
            // Another core is faulting this page. Wait rather than issue a
            // second read whose `InsertFrame` would race the first, and
            // **re-check from the top** on waking rather than assuming the
            // outcome - which is what lets the failure arm below simply
            // erase and broadcast without telling anyone why.
            loading_done_.wait(hold);
            continue;
        }

        loading_.insert(page_id);
        {
            // Re-takes the latch, erases the id and broadcasts on every way
            // out of this scope - the `return` below, falling off the end,
            // and an exception. Scoped rather than function-lifetime so the
            // pin below happens with the id already published and the latch
            // already back; `LoadingGuard` leaves it held for exactly that.
            LoadingGuard published(hold, loading_, loading_done_, page_id);
            hold.unlock();
            if (after_fault_published_for_test_) after_fault_published_for_test_(page_id);
            // The reservation (and its sweep, under holds of its own), the
            // device read and the checksum verify, all outside this hold.
            auto loaded = Resolve(page_id, /*mark_dirty=*/!for_read, heat);
            if (!loaded.ok()) return loaded.status();
        }

        // **Pinned here rather than by rounding the loop, and that is AM-S2
        // R3.** Rounding sent this fault back through the hit path, whose
        // raw fetch is a second `Resolve` - so one armed fault applied the
        // usage bump **twice**, once from `InsertFrame`'s warm insert and
        // once from the hit, and left a freshly faulted frame at 2 where the
        // unarmed path leaves it at 1. Armed and unarmed then disagreed
        // about how many sweep rotations a page survives its own fault, in
        // a suite that runs both and expects them to agree, and no cell
        // could see it. The second fetch goes with the second bump.
        //
        // The frame can still be gone: it is unpinned for the instant
        // between `InsertFrame` and the guard re-taking the latch, so
        // another core's sweep may have taken it. That is the one case
        // rounding the loop was actually for, and it keeps it.
        // **This branch *is* the fault**, which is the whole of the ring's
        // answer (AM-R8): the hit arm above returns without reaching here,
        // and this arm is the only path in the function that runs
        // `InsertFrame`. One inexactness, in the safe direction: if another
        // core loaded the page while this call was outside the latch, the
        // `Resolve` above found it resident and inserted nothing, and this
        // still says faulted - unless that core's fault was a `kScan` one,
        // whose cold frame the ring then drops at rotation, harmlessly: it
        // is clean, unpinned and cold either way. The ring then takes a slot for a frame that
        // core faulted - which its own `InsertFrame` left warm at usage 1,
        // so `ReleaseScanSlot` abandons rather than drops it.
        if (faulted != nullptr) *faulted = true;
        auto pinned = PinResidentAndRelease(page_id, mode, hold);
        if (!pinned.has_value()) continue;
        return *pinned;
    }
}

std::optional<std::span<std::byte, kPageSize>> DevicePageStore::PinResidentAndRelease(
    PageId page_id, PinMode mode, std::unique_lock<Latch>& hold) noexcept {
    auto found = frames_.find(page_id);
    // Gone under us. `hold` is untouched, so the caller rounds its loop with
    // the latch it already had.
    if (found == frames_.end()) return std::nullopt;
    Frame& frame = found->second;
    // The same accounting `PinFrame` does, **including its two debug
    // checks**: this tail replaced the `PinFrame` call the accessors used to
    // make, and writing the increments out by hand here is what once took
    // the MG04 ceiling and the AM-S1 never-upgrade census off the armed path
    // - the only path either one is about.
    CountPin(frame);
    // Taken before the unlock, from the frame the pin now protects.
    // A table node outlives rehashing and a pinned frame is never erased
    // (EV4), so this reference stays good with the latch released; the span
    // points into a slot, which never moves (BE-R1).
    const std::span<std::byte, kPageSize> view(*frame.bytes);
    hold.unlock();
    // Pin first, then wait for the page latch - `PinFrame`'s order, for
    // `PinFrame`'s reason, and with the structure latch genuinely released
    // rather than parenthetically held.
    AcquirePageLatch(page_id, frame, mode);
    return view;
}

StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> DevicePageStore::CreatePinned(
    CreateKind which, PageId page_id) {
    // **The create half of AM-S2's pair, and its window is narrower than
    // `Get`'s for a reason worth stating.** A freshly created frame is
    // **dirty**, not clean - `InsertFrame` is called with `dirty=true` on
    // both create paths - and a dirty frame is only *queued* by
    // `EvictColdFrames`; `FreePage` reaches only an id a reclaim walk found
    // in a dropped relation, never one a create just claimed. So losing it
    // between the create and the pin takes a concurrent writeback first:
    // narrow, and still real, which is why this closes the window rather
    // than documenting it as improbable.
    Latch* latch = structure_latch();
    if (latch == nullptr) {
        // Unarmed: one thread, nothing can evict between the two calls, and
        // the window does not exist. Today's shape and today's cost.
        return PageStore::CreatePinned(which, page_id);
    }

    // The raw create runs **outside** the latch. It grows the file, reads the
    // device to prove a page was never written, and takes the free map -
    // exactly the work step 2b moved off this latch on the fetch side.
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> made =
        Status::InvalidArgument("unreachable");
    if (which == CreateKind::kAt) {
        auto bytes = CreateAtUnpinned(page_id);
        if (!bytes.ok()) return bytes.status();
        made = std::pair<PageId, std::span<std::byte, kPageSize>>(page_id, bytes.value());
    } else {
        made = which == CreateKind::kNew ? CreateNewUnpinned() : CreateNewHeaderlessUnpinned();
        if (!made.ok()) return made.status();
    }
    const PageId id = made.value().first;

    Frame* frame = nullptr;
    {
        LatchGuard structure(latch);
        auto found = frames_.find(id);
        // **Does the resident frame for this id own the bytes we are about
        // to hand out?** That, and only that, is what the address compare
        // decides: pass it and the span points into the frame pinned on the
        // next line, so the pin and the bytes cannot disagree. It is *not*
        // proof that nobody touched the frame - an evict-and-re-fault of
        // this id into the same slot passes too - and does not need to be:
        // that is this id's frame holding bytes at the address the caller
        // was handed, which is exactly as safe. What it excludes is the quiet
        // failure the pair exists to remove: pinning this id's frame in one
        // slot while the span points into another.
        if (found != frames_.end() &&
            static_cast<const void*>(found->second.bytes->data()) ==
                static_cast<const void*>(made.value().second.data())) {
            frame = &found->second;
            // `CountPin`, not the increments written out - the same reason
            // `FetchPinned`'s hit path calls it: a third copy of the gauge,
            // the high-water mark and MG04's ceiling is a third definition
            // to drift, and writing it by hand here took the ceiling off
            // the create path.
            CountPin(*frame);
        }
    }

    if (frame != nullptr) {
        // Pin first, page latch after, with the structure latch released -
        // `PinFrame`'s order, for `PinFrame`'s reason. Through
        // `AcquirePageLatch` for `FetchPinned`'s reason: acquiring the word
        // directly here would take AM-S1's never-upgrade census off this
        // path, and that census is only ever about the armed path.
        AcquirePageLatch(id, *frame, PinMode::kExclusive);
        return made;
    }

    // Evicted under us - which means it was written back first, a created
    // frame being dirty. The bytes are on the device, so faulting it back is
    // both correct and the only thing left; the id is already allocated, so
    // this cannot re-enter the create path.
    auto refetched = FetchPinned(id, PinMode::kExclusive, /*for_read=*/false,
                                 FetchHeat::kWarm);
    if (!refetched.ok()) return refetched.status();
    return std::pair<PageId, std::span<std::byte, kPageSize>>(id, refetched.value());
}

void DevicePageStore::PinFrame(PageId page_id, PinMode mode) noexcept {
    // Called by the base pinned accessors immediately after the raw fetch
    // made the frame resident, on the same single-threaded core - so the
    // find can only miss if something is deeply wrong, and a miss is left
    // as a no-op pin rather than an abort: the failure it produces (a frame
    // evictable while a handle lives) is the one the poisoner (MG05)
    // detects deterministically.
    // **AM-S2: the pin is taken first, under the structure latch, and the
    // page latch is waited for afterwards.** That order is the whole of why
    // neither latch is held across the other. A frame with `pins > 0` is
    // never an eviction victim (EV4), so taking the pin is what keeps this
    // frame alive while this core waits for the page latch - where holding
    // the structure latch across that wait would put every other core's
    // frame lookup behind one page's contention.
    //
    // **True for the reason it gives, since step 3.** Every eraser
    // (`ReleaseScanSlot`, `FreePage`, `EvictColdFrames`) reads `pins`
    // under this latch now, so a pin taken here really is visible to a
    // concurrent sweep rather than merely invisible to a second thread that
    // could not exist. The ordering was written for that state and no
    // longer rests on single-threadedness.
    Frame* frame = nullptr;
    {
        LatchGuard structure(structure_latch());
        auto found = frames_.find(page_id);
        if (found == frames_.end()) return;
        frame = &found->second;
        // A table node outlives rehashing and the pin keeps it from being
        // erased, so this pointer stays good after the guard drops - the
        // property that lets the page-latch wait happen outside the latch.
        CountPin(*frame);
    }
    AcquirePageLatch(page_id, *frame, mode);
}

// The pin accounting, with the structure latch already held by the caller.
// Shared by `PinFrame` and by `FetchPinned`'s armed hit path, which is the
// same act reached two ways: one call each keeps the gauge, the high-water
// mark and the ceiling from having two definitions that can drift.
void DevicePageStore::CountPin(Frame& frame) noexcept {
    ++frame.pins;
    ++t_pins_here;
    ++live_pins_;
    if (live_pins_ > pin_high_water_) pin_high_water_ = live_pins_;
#ifndef NDEBUG
    // MG04's ceiling, asserted rather than logged: a workload that holds
    // more pins than the audit derived is either a new Shape-B site
    // missing its bound or a leak, and both should fail the test that
    // reaches them.
    if (live_pins_ > pin_ceiling_) {
        std::fprintf(stderr,
                     "DevicePageStore: %zu live pins exceeds the ceiling %zu "
                     "(kPinCeiling %zu x the concurrent pinners SetLatchArmed was told "
                     "about; docs/spec/page.md §3)\n",
                     live_pins_, pin_ceiling_, kPinCeiling);
        std::abort();
    }
#endif
}

#ifndef NDEBUG
namespace {
// **The pages this thread holds shared**, and nothing else reads it: it is
// the never-upgrade detector's whole state (AM-S2 step 3b). A function
// rather than a bare variable so the thread-local is initialised on first
// use in every translation unit that could reach it, and a multiset because
// two handles on one page are two shared holds.
std::unordered_multiset<PageId>& SharedHoldsHere() {
    static thread_local std::unordered_multiset<PageId> held;
    return held;
}
}  // namespace
#endif

// The page-latch half of taking a pin, with **no** latch held: the pin is
// already counted, and a frame with pins > 0 is never an eviction victim
// (EV4), so it is this frame that is waited for however long the wait is.
void DevicePageStore::AcquirePageLatch(PageId page_id, Frame& frame, PinMode mode) noexcept {
#ifdef NDEBUG
    // Read only by the never-upgrade detector below, which is debug-only.
    // Named rather than dropped from the signature: a release-only unused
    // parameter is the shape a reader mistakes for a forgotten argument.
    (void)page_id;
#endif
    if (latch_armed_) {
        // The page latch (AM-S1, the header's "The page latch" section).
        // Taken where the pin is taken, in the accessor's mode, and never
        // upgraded: a task that holds this frame shared and now asks for it
        // exclusive would wait for its own share forever. That is a protocol
        // defect in the caller, aborted in debug naming the page; a release
        // build hangs in the spin below, as a recursive std::mutex
        // acquisition does (base/latch.hpp).
#ifndef NDEBUG
        // **The test is the thread's own record, and it had to become one**
        // (AM-S2 step 3b). It read `frame.pins > 1 &&
        // HasSharedHolders(...)`, on the argument `page_latch.hpp` states
        // outright: the word cannot tell two holders on one core apart, but
        // the store can, "its pins are this core's through M1". Step 3 ends
        // that - one table serves every core, so `pins > 1` becomes "two
        // cores hold one pin each" as readily as "this core holds two", and
        // the detector would fire on correct traffic while missing the
        // defect it exists for. `page_latch.hpp` said AM-S2 must revisit
        // this test once a foreign core can hold a share; this is that.
        //
        // A thread-local multiset of the pages *this thread* holds shared is
        // exact where the proxy was circumstantial, and exact on the same
        // premise `CurrentCore()` rests on: one reactor per thread, no
        // handle crossing threads. A multiset because two handles on one
        // page are two shared holds. Debug only - it is a detector, and the
        // release build's failure is the hang.
        if (mode == PinMode::kExclusive && SharedHoldsHere().count(page_id) != 0) {
            std::fprintf(stderr,
                         "DevicePageStore: page %u is held shared by this thread (%zu hold(s)) "
                         "and was asked for exclusive - a page latch is never upgraded "
                         "(docs/spec/page.md section 6)\n",
                         page_id, SharedHoldsHere().count(page_id));
            // The census's whole value is naming the site: raw frames, for
            // `addr2line -e <binary>` - the executable is not linked
            // -rdynamic, so symbol names are not available here.
            void* frames_out[48];
            const int depth = backtrace(frames_out, 48);
            backtrace_symbols_fd(frames_out, depth, 2);
            std::abort();
        }
#endif
        // The turns it spun are dropped: a contention gauge is AM-S3's, when
        // it has a number to want (the cells read Acquire's return directly).
        //
        // **`CurrentCore()`, not `core_id_`** (AM-S2 step 3,
        // `base/current_core.hpp`). The word records the core that *asked*,
        // and step 3 gives one store to every core - a store stamping its
        // own id would then record core 0 for every holder and make the
        // owner field a lie, taking the re-entrancy rule and the
        // never-upgrade census with it.
        //
        // **The two do not agree everywhere, and a first draft of this
        // comment claimed they did.** Instrumented, the armed suite takes
        // **14,965** acquisitions where `CurrentCore() != core_id_`, all of
        // them in fixtures: `CoreRuntimeTest` (10,792) and
        // `CoreRuntimePerCoreStreamTest` (4,144) build several cores and
        // drive all of their stores from the one test thread, and
        // `ExpeditorTest` (12) dispatches into a peer between `Start()` and
        // `RunUntilStopped()`. Production takes none - every acquisition
        // there is on a reactor thread, which declares itself in
        // `Scheduler::RunOnce`, or in the mount pass, which declares the
        // core it is opening.
        //
        // Sound in both, and for the same reason rather than by luck: the
        // owner field's job is to say who may re-enter and who may release,
        // and in a fixture that one thread is the only holder of any of
        // those words. Acquire and release read the same value, so
        // `PageLatch::Release`'s owner check is what enforces it - a
        // disagreement between the two would fail there rather than pass
        // quietly. **What those fixtures do become at step 4** is a thread
        // holding one shared table's word while claiming to be several
        // cores, which is a shape production cannot reach; they take a
        // `CurrentCoreGuard` then.
        if (const std::uint64_t turns =
                PageLatch::Acquire(frame.latch, LatchModeFor(mode), CurrentCore());
            turns > 0) {
            Contention::Add(Tally::kPageLatchWaits);
            Contention::Add(Tally::kPageLatchSpinTurns, turns);
        }
#ifndef NDEBUG
        // Recorded **after** the acquire, so the set never claims a hold
        // this thread is still waiting for.
        if (mode == PinMode::kShared) SharedHoldsHere().insert(page_id);
#endif
        // **An exclusive hold marks the frame again once it has the latch**
        // (AT-S8 step 1b's review). The fetch marks it before the page latch
        // is taken, so another core's writeback can copy in the gap - the
        // old bytes, under the generation the fetch already moved - and clean
        // the frame this holder is about to write. A logged write re-marks
        // through its stamp; an unlogged one (page 0's anchor and
        // transaction-id ceiling) would leave its bytes in a clean frame no
        // flush writes. After the acquire, no foreign copy can fall between
        // this mark and the write. Every exclusive hold is a write fetch
        // (`Get`, the creates), so this adds no dirtiness of its own; its
        // cost is one structure-latch hold per exclusive fetch, armed only.
        if (mode == PinMode::kExclusive) {
            LatchGuard structure(structure_latch());
            frame.MarkDirty(++dirty_gens_);
        }
    }
}

void DevicePageStore::UnpinFrame(PageId page_id) noexcept {
    // **The mirror of `PinFrame`'s order: the page latch is released first,
    // then the pin drops under the structure latch.** Dropping the pin first
    // would let a sweep evict the frame out from under the latch release
    // that follows it, which is the one interleaving this pair has to
    // exclude - and it is excluded by ordering rather than by holding both.
    Frame* frame = nullptr;
    {
        LatchGuard structure(structure_latch());
        auto found = frames_.find(page_id);
        if (found == frames_.end()) return;
        // Saturating rather than wrapping. An unpin with no pin is a defect
        // in the handle, not in the caller, and the two failure modes are
        // not symmetric: a floor leaves a frame resident forever (a leak,
        // visible in pinned_frames()), where an underflow makes it evictable
        // while somebody still holds it.
        if (found->second.pins == 0) return;
        frame = &found->second;
    }
    // The latch leaves with the pin: one handle, one hold of each. The word
    // knows whether this core is the exclusive owner, so no mode travels
    // here.
    // The same identity that took it, for the same reason: release under
    // `X` checks the owner field against the asking core, so acquire and
    // release must read `CurrentCore()` alike.
    if (latch_armed_) {
#ifndef NDEBUG
        // Erase one instance if this thread had a share on this page, and
        // before the release rather than after: the word is the authority
        // and this set only mirrors it. An exclusive hold was never
        // recorded, so the find misses and nothing is erased - and a thread
        // holding one page both shared and exclusively is the state the
        // detector aborts on, so it cannot be the case here.
        if (auto held = SharedHoldsHere().find(page_id); held != SharedHoldsHere().end()) {
            SharedHoldsHere().erase(held);
        }
#endif
        PageLatch::Release(frame->latch, CurrentCore());
    }
    {
        LatchGuard structure(structure_latch());
        // **The two decrements stay coupled**, as they were before this
        // stage split them apart and put them back. The gauge must move with
        // the pin or `kPinCeiling` stops meaning anything.
        //
        // Honest about how much that buys: the early return above already
        // filters `pins == 0`, so an uncoupled pair misbehaves only if two
        // unpins race at `pins == 1` - and `pins == 1` means one handle
        // exists, so only one unpin can be in flight. The window is
        // unreachable under correct handle usage, and
        // `am_s2_pin_protocol_test.cpp` passes with the pair split, which is
        // how that was established rather than assumed. This is the original
        // semantics kept because they are right, not a live defect fixed.
        UncountPin(*frame);
    }
}

void DevicePageStore::UncountPin(Frame& frame) noexcept {
    if (frame.pins != 0) {
        --frame.pins;
        // A pin uncounted on a thread that never counted it would let drain
        // mode write back under a held page - the shape it must not run in.
        assert(t_pins_here != 0 && "a pin released on a thread that did not take it");
        if (t_pins_here != 0) --t_pins_here;
        if (live_pins_ != 0) --live_pins_;
    }
}

bool DevicePageStore::TryAcquirePageLatchShared(PageId page_id, Frame& frame) noexcept {
#ifdef NDEBUG
    (void)page_id;
#endif
    if (!latch_armed_) return true;
    if (PageLatch::TryAcquire(frame.latch, PageLatchMode::kShared, CurrentCore()) ==
        PageLatchOutcome::kBusy) {
        return false;
    }
#ifndef NDEBUG
    SharedHoldsHere().insert(page_id);
#endif
    return true;
}

void DevicePageStore::MarkFrameDirty(PageId page_id) noexcept {
    // The same table walk and the same hold as `StampPageLsn` (AM-S2 step
    // 3e). Its miss is quieter - a dirty mark lost to a rehash is a page
    // that never reaches the device - which is why it goes in beside the
    // one that could be measured rather than waiting for a cell of its own.
    LatchGuard structure(structure_latch());
    auto it = frames_.find(page_id);
    if (it != frames_.end()) it->second.MarkDirty(++dirty_gens_);
}

Status DevicePageStore::LatchFrameForTest(PageId page_id, PinMode mode, std::uint32_t core) {
    auto it = frames_.find(page_id);
    if (it == frames_.end()) {
        return Status::NotFound("DevicePageStore: page " + std::to_string(page_id) +
                                " is not resident");
    }
    if (PageLatch::TryAcquire(it->second.latch, LatchModeFor(mode), core) !=
        PageLatchOutcome::kAcquired) {
        return Status::InvalidArgument("DevicePageStore: page " + std::to_string(page_id) +
                                       " is latched in a conflicting mode");
    }
    return Status::OK();
}

Status DevicePageStore::UnlatchFrameForTest(PageId page_id, std::uint32_t core) {
    auto it = frames_.find(page_id);
    if (it == frames_.end()) {
        return Status::NotFound("DevicePageStore: page " + std::to_string(page_id) +
                                " is not resident");
    }
    PageLatch::Release(it->second.latch, core);
    return Status::OK();
}

StatusOr<std::uint32_t> DevicePageStore::latch_word_for_test(PageId page_id) const {
    auto it = frames_.find(page_id);
    if (it == frames_.end()) {
        return Status::NotFound("DevicePageStore: page " + std::to_string(page_id) +
                                " is not resident");
    }
    return PageLatch::Load(it->second.latch);
}

bool DevicePageStore::IsPinnedClass(PageId page_id) const noexcept {
    // Half one: the reserved low ids. Needed because the fixed catalog pages
    // are formatted kHeap like any user relation, so the kind cannot tell
    // them apart - the finding recorded at the declaration and in
    // docs/inflight/in-progress/workplan-eviction.md EVT01.
    if (page_id < first_evictable_page_id_) return true;

    // Half two: the page kind, which is what EV3 actually specifies and what
    // works for a class that has one of its own. A Bound Cabin's pages carry
    // the aggregate an admission check reads, so reclaiming one would take a
    // *constraint* out of memory - the definition of a class that is never a
    // candidate.
    //
    // Only asked of a resident frame: a page that is not in the pool cannot
    // be a sweep candidate anyway, and reading a header off the device to
    // answer would turn a skip test into an I/O.
    // FM8: a bitmap page is never a reclaim candidate either, and its id
    // says so without a frame. Under D4(a) the maps are store-owned and
    // never enter the pool, so this is a guard against a future that puts
    // them there rather than a live case - which is exactly why it is
    // arithmetic and not a header read.
    if (IsMapPageId(page_id)) return true;

    auto it = frames_.find(page_id);
    return it != frames_.end() && IsPinnedClassFrame(it->second);
}

bool DevicePageStore::IsPinnedClassFrame(const Frame& frame) const noexcept {
    if (frame.page_id < first_evictable_page_id_ || IsMapPageId(frame.page_id)) return true;
    const PageHeaderFields header =
        ReadPageHeader(std::span<const std::byte, kPageSize>(*frame.bytes));
    return header.page_type == static_cast<std::uint8_t>(PageType::kCabinBound) ||
           header.page_type == static_cast<std::uint8_t>(PageType::kFreeMap) ||
           header.page_type == static_cast<std::uint8_t>(PageType::kHeaderlessMap);
}

std::vector<PageId> DevicePageStore::TakeDirtyEvictionQueue() {
    // Under the latch because the *writer* is now: `SweepLocked`
    // pushes an id here while holding it, and a swap racing that push is a
    // vector reallocating under a reader. Held across the swap only - the
    // caller's writeback is device I/O and happens on the returned copy,
    // which is exactly why this hands one back rather than a reference.
    LatchGuard structure(structure_latch());
    std::vector<PageId> out;
    out.swap(dirty_eviction_queue_);
    // Off the queue, so the sweep's next visit may queue one still dirty.
    for (const PageId id : out) {
        if (auto it = frames_.find(id); it != frames_.end()) it->second.queued = false;
    }
    return out;
}

void DevicePageStore::SetResidentLimit(PageId first_evictable_page_id) noexcept {
    // Additive only - see the declaration. A limit that could fall would let
    // a structure be declared un-evictable and then be evicted.
    if (first_evictable_page_id > first_evictable_page_id_) {
        first_evictable_page_id_ = first_evictable_page_id;
    }
}

std::size_t DevicePageStore::pinned_frames() const {
    // Under the hold since BE-S2: a walk of a table another core may be
    // publishing into, which BE-S1's census found unlatched.
    LatchGuard structure(structure_latch());
    std::size_t pinned = 0;
    for (const auto& [id, frame] : frames_) {
        if (frame.pins != 0) ++pinned;
    }
    return pinned;
}

std::size_t DevicePageStore::EvictColdFrames(std::size_t budget) {
    // **The third eraser's public door**, and all it does is take the latch
    // (the other two are `ReleaseScanSlot` and `FreePage`), which the
    // `Locked` body assumes.
    // Split rather than made re-entrant because `base/latch.hpp` says
    // plainly that a second acquisition on one thread hangs.
    //
    // Full laps: enough for the highest usage counter to be walked down to
    // zero and then collected. Nothing bumps a counter while the sweep
    // holds the latch, so one more lap than the cap is exactly sufficient.
    LatchGuard structure(structure_latch());
    return SweepLocked(budget, SlotCountLocked() * (kClockUsageCap + 1)).reclaimed;
}

DevicePageStore::SweepResult DevicePageStore::SweepLocked(std::size_t want,
                                                          std::size_t max_steps) {
    // **The hand walks slots** (BE-R1). A step is one slot - a hash probe for
    // an occupied one, since the frame is in the table's node - and the
    // hand keeps its place between sweeps, so the pass goes round the whole
    // array rather than re-punishing the low slots. Free and reserved slots
    // carry no page and are stepped over, and count as steps: a step bound
    // is a bound on the walk, whatever it finds.
    SweepResult out;
    const std::size_t slots = SlotCountLocked();
    if (want == 0 || slots == 0) return out;
    std::size_t& reclaimed = out.reclaimed;
    for (; out.steps < max_steps && reclaimed < want; ++out.steps) {
        const PageId owner = SlotOwner(clock_hand_);
        clock_hand_ = (clock_hand_ + 1) % slots;
        if (owner == kInvalidPageId) continue;
        // The frame is in the page table's node; the slot names its page.
        const auto found = frames_.find(owner);
        assert(found != frames_.end() && "a slot names a page the table does not hold");
        Frame& frame = found->second;

        // The three refusals, in the order they are cheapest to test. Each
        // is a guarantee something else depends on, not an optimization:
        //   pinned    - a live PageRef points into these bytes (EV01);
        //   resident  - the class is never a candidate at any pressure (EV3),
        //               which is what AST04's Bound Cabin rests on;
        //   dirty     - the flush it needs is WAL-gated and is EV04's, so
        //               dropping it here would lose a write (EV02's scope).
        if (frame.pins != 0) continue;
        if (IsPinnedClassFrame(frame)) continue;
        // A hold taken by another core: the shared pool's refusal.
        if (latch_armed_ && PageLatch::IsHeld(frame.latch)) continue;

        // §3.2's branches, in the specified order: a positive usage counter
        // is decremented and the frame survives this rotation.
        if (frame.usage != 0) {
            --frame.usage;
            out.progressed = true;
            continue;
        }

        // Usage zero and dirty: queued for writeback, **not** reclaimed
        // (§3.2's fourth branch, §4's queue). Reclaiming it would lose the
        // write. The bit is the queue's membership test.
        if (frame.dirty) {
            if (!frame.queued) {
                frame.queued = true;
                dirty_eviction_queue_.push_back(frame.page_id);
                dirty_queued_.Add();
            }
            continue;
        }

        ReleaseFrameLocked(frame);
        ++reclaimed;
    }

    if (reclaimed != 0 && log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("pagestore", "clock reclaimed " + std::to_string(reclaimed) +
                                     " frame(s) on core " + std::to_string(CurrentCore()) + ", " +
                                     std::to_string(frames_.size()) + " resident");
    }
    return out;
}

DevicePageStore::PoolCounters DevicePageStore::pool_counters() const {
    PoolCounters out;
    {
        LatchGuard structure(structure_latch());
        out.resident = frames_.size();
        out.slots = SlotCountLocked();
    }
    out.budget = capacity_;
    out.hits = hits_.Load();
    out.misses = misses_.Load();
    out.reclaimed_inline = reclaimed_inline_.Load();
    out.reclaimed_background = reclaimed_background_.Load();
    out.batches_inline = batches_inline_.Load();
    out.batches_background = batches_background_.Load();
    out.batches_partial = batches_partial_.Load();
    out.batch_steps = batch_steps_.Load();
    out.batch_steps_background = batch_steps_background_.Load();
    out.dirty_queued = dirty_queued_.Load();
    out.dirty_drained = dirty_drained_.Load();
    out.refused = refused_.Load();
    return out;
}

}  // namespace kds::storage
