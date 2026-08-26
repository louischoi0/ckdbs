#include "kds/storage/device_page_store.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "kds/storage/free_map.hpp"
#include "kds/storage/page_header.hpp"

namespace kds::storage {

namespace {

// "Never written" as the device shows it: every byte zero. The miss path
// and CreateAt ask the same question and must answer it the same way.
bool PageIsAllZero(const std::array<std::byte, kPageSize>& page) noexcept {
    return std::all_of(page.begin(), page.end(), [](std::byte b) { return b == std::byte{0}; });
}

}  // namespace

DevicePageStore::DevicePageStore(PageDevice& device, PageId first_new_page_id) noexcept
    : device_(device), next_new_page_id_(first_new_page_id) {}

const DevicePageStore::Page& DevicePageStore::AbsentRegionPage() noexcept {
    static const Page kZero{};
    return kZero;
}

void DevicePageStore::RecountAllocatedPages() noexcept {
    std::uint32_t total = 0;
    for (const auto& [region, pages] : map_regions_) {
        total += FreeMapCountAllocated(std::span<const std::byte, kPageSize>(pages.free_map));
    }
    allocated_pages_ = total;
}

bool DevicePageStore::maps_dirty() const noexcept {
    for (const auto& [region, pages] : map_regions_) {
        if (pages.dirty) return true;
    }
    return false;
}

Status DevicePageStore::LoadRegionIfPresent(std::uint32_t region) {
    const PageId free_id = FreeMapPageIdFor(FreeMapRegionBase(region));
    if (device_.page_capacity() <= free_id) return Status::OK();

    MapRegion pages;
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
            any_headerless_ = true;
        }
    }

    allocated_pages_ += FreeMapCountAllocated(std::span<const std::byte, kPageSize>(view));
    map_regions_.emplace(region, std::move(pages));
    return Status::OK();
}

StatusOr<std::span<std::byte, kPageSize>> DevicePageStore::EnsureHeaderlessMap(PageId page_id) {
    auto region = EnsureRegionResident(FreeMapRegionOf(page_id));
    if (!region.ok()) return region.status();
    if (region.value()->headerless_map == nullptr) {
        // The id is claimed *here*, as the page is placed, so the free map
        // never says a page exists whose bytes have not been written. It
        // cannot have been taken in the meantime: every allocation path
        // skips a bitmap id by arithmetic.
        const PageId headerless_id = HeaderlessMapPageIdFor(page_id);
        if (lease_ == nullptr) {
            if (Status s = device_.EnsureCapacity(headerless_id + 1); !s.ok()) return s;
            FreeMapAllocate(std::span<std::byte, kPageSize>(region.value()->free_map),
                            FreeMapBitIndexOf(headerless_id));
            ++allocated_pages_;
        }
        auto made = std::make_unique<Page>();
        FormatFreeMapPage(std::span<std::byte, kPageSize>(*made), PageType::kHeaderlessMap);
        region.value()->headerless_map = std::move(made);
        region.value()->dirty = true;
        any_headerless_ = true;
    }
    return std::span<std::byte, kPageSize>(*region.value()->headerless_map);
}

StatusOr<DevicePageStore::MapRegion*> DevicePageStore::EnsureRegionResident(
    std::uint32_t region) {
    if (auto it = map_regions_.find(region); it != map_regions_.end()) return &it->second;

    // **A leased store never touches the device for a map page.** Every
    // region it legitimately holds was loaded by Open(), before the lease
    // was installed (core_runtime.cpp orders it that way); a region reached
    // after that is one core 0 owns, is writing, and does not latch - so
    // reading it here would be an unsynchronised read of a live page, which
    // is the hazard RefreshFreeMapFromDevice exists to handle for region 0
    // and does not generalise.
    //
    // Refusing instead is worse than it looks: this path is reached from
    // CreateNewHeaderlessUnpinned when a peer's lease lies above region 0,
    // and the bit it wants to set is what stops StampIfHeadered stamping a
    // checksum over a headerless page's payload. That bit matters **in
    // memory** even though it can never be published - FlushMaps drops a
    // leased store's map writes, and always has.
    //
    // So a peer gets a private, empty, never-dirty region: exactly what its
    // region-0 copy already is, generalised. Durably recording a peer's
    // headerless pages is FM7's, under D5.
    if (lease_ != nullptr) {
        MapRegion pages;
        FormatFreeMapPage(std::span<std::byte, kPageSize>(pages.free_map));
        auto [it, inserted] = map_regions_.emplace(region, std::move(pages));
        return &it->second;
    }

    if (Status s = LoadRegionIfPresent(region); !s.ok()) return s;
    if (auto it = map_regions_.find(region); it != map_regions_.end()) return &it->second;

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
    ++allocated_pages_;
    (void)headerless_id;
    pages.dirty = true;

    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("pagestore", "free-map region " + std::to_string(region) +
                                     " created, covering ids " +
                                     std::to_string(FreeMapRegionBase(region)) + ".." +
                                     std::to_string(FreeMapRegionBase(region) + kFreeMapBitsPerPage - 1));
    }
    auto [it, inserted] = map_regions_.emplace(region, std::move(pages));
    return &it->second;
}

StatusOr<std::span<std::byte, kPageSize>> DevicePageStore::FreeMapBytesForRegion(
    std::uint32_t region) {
    auto pages = EnsureRegionResident(region);
    if (!pages.ok()) return pages.status();
    // Dirty on every take, not on every write: the taker is trusted to be
    // about to change bits, and the alternative - a clean map that a
    // reservation had already changed - is the PW3b defect.
    pages.value()->dirty = true;
    return std::span<std::byte, kPageSize>(pages.value()->free_map);
}

StatusOr<std::unique_ptr<DevicePageStore>> DevicePageStore::Open(PageDevice& device,
                                                                 PageId first_new_page_id) {
    auto store = std::unique_ptr<DevicePageStore>(new DevicePageStore(device, first_new_page_id));

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
    if (const char* budget = std::getenv("KDS_TEST_FRAME_BUDGET"); budget != nullptr) {
        const long parsed = std::strtol(budget, nullptr, 10);
        if (parsed > 0) store->SetFrameBudget(static_cast<std::size_t>(parsed));
    }
#endif
    return store;
}

bool DevicePageStore::IsHeaderless(PageId page_id) const noexcept {
    // FM6 / D2(a). This predicate sits on the fault path, the write-back
    // path and the WAL gate, and for a database with no Waystone directory
    // - which has no headerless page anywhere, `waystone_dir.cpp` being the
    // engine's only creator of them - the answer is no, with no lookup.
    if (!any_headerless_) return false;
    // A map page's class is arithmetic, never a lookup. §3 of
    // docs/inflight/in-progress/workplan-multi-free-map.md needs this to be true rather than
    // merely convenient: this predicate sits on the fault path, the
    // write-back path and the WAL gate, so answering it by reading a map
    // would be a recursion if a map page could ever be the question. Both
    // bitmap classes are headered, so the answer is no.
    if (IsMapPageId(page_id)) return false;
    return FreeMapIsAllocated(headerless_map_bytes_for(page_id), FreeMapBitIndexOf(page_id)) &&
           IsAllocated(page_id);
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
    // seam, and pinning here would leak a pin no handle ever drops.
    auto created = CreateNewUnpinned();
    if (!created.ok()) return created.status();

    // Marked after the allocation succeeds and before the caller writes a
    // byte, so no flush can ever see the page headered. The region is
    // resident by construction: the id came from an allocation that had to
    // load or create its region to hand it out.
    const PageId headerless_page = created.value().first;
    auto map = EnsureHeaderlessMap(headerless_page);
    if (!map.ok()) return map.status();
    FreeMapAllocate(map.value(), FreeMapBitIndexOf(headerless_page));
    auto region = EnsureRegionResident(FreeMapRegionOf(headerless_page));
    if (!region.ok()) return region.status();
    region.value()->dirty = true;
    if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
        log_->Trace("pagestore",
                    "alloc headerless page=" + std::to_string(created.value().first));
    }
    return created;
}

Status DevicePageStore::FlushMaps() {
    if (!maps_dirty()) return Status::OK();

    // **A leased store never writes the maps** - SetCoreOwnership's rule in
    // as many words, and MayWrite's too: region 0's map pages sit below
    // `system_page_limit_`, which a peer may read and may never write. This
    // is the one write path that reaches `device_.WritePage` without asking
    // MayWrite, so the check has to be here.
    //
    // The bit that gets here is redo's: `CreateAt` marks the map at mount,
    // *before* the lease is installed (core_runtime.cpp orders it that way
    // deliberately), and until a peer had a checkpointer nothing on a peer
    // ever called FlushMaps. Publishing this core's copy would write back
    // the map as it stood when this store opened - reverting every
    // allocation and every extent reservation core 0 has made since, which
    // is silent reuse of live pages rather than a lost bit. Dropped instead:
    // the id redo re-created came out of an extent core 0 reserved, so core
    // 0's map already carries it and core 0's own flush makes it durable.
    if (lease_ != nullptr) {
        for (auto& [region, pages] : map_regions_) pages.dirty = false;
        return Status::OK();
    }

    // Ascending by region, which `std::map` gives for free. Regions are
    // independent of one another - a page's reachability rests on its own
    // region's map and nothing else - so the order across them is a
    // determinism choice, not a correctness one. Within a region it is
    // both, and the rule is the one the single-page map always followed.
    for (auto& [region, pages] : map_regions_) {
        if (!pages.dirty) continue;

        // The headerless map first, the free map second. Both orderings are
        // safe, but this one is safe for a reason worth writing down: the
        // free map is what makes a page id *exist*, so a crash between the
        // two leaves a headerless bit set for an id nothing allocated -
        // harmless, since IsHeaderless() also requires allocation. The
        // reverse order would publish an allocated headerless page whose
        // headerless bit had not landed, and the next read of it would
        // verify a checksum that was never written and call the page
        // corrupt.
        const PageId base = FreeMapRegionBase(region);
        if (pages.headerless_map != nullptr) {
            auto hbytes = std::span<std::byte, kPageSize>(*pages.headerless_map);
            StampPageChecksum(hbytes);
            if (Status s = device_.WritePage(HeaderlessMapPageIdFor(base), hbytes); !s.ok()) {
                if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                    log_->Error("pagestore", "headerless-map write failed for region " +
                                                 std::to_string(region) + ": " + s.message());
                }
                return s;
            }
        }

        auto fbytes = std::span<std::byte, kPageSize>(pages.free_map);
        StampPageChecksum(fbytes);
        if (Status s = device_.WritePage(FreeMapPageIdFor(base), fbytes); !s.ok()) {
            // The map is what makes a page reachable after a restart, so
            // losing this write loses pages whose bytes did land.
            if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                log_->Error("pagestore", "free-map write failed for region " +
                                             std::to_string(region) + ": " + s.message());
            }
            return s;
        }
        pages.dirty = false;
    }

    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("pagestore", "maps written, " + std::to_string(allocated_pages()) +
                                     " page(s) allocated across " +
                                     std::to_string(map_regions_.size()) + " region(s)");
    }
    return Status::OK();
}

Status DevicePageStore::PersistMaps() {
    if (Status s = FlushMaps(); !s.ok()) return s;
    return device_.Sync();
}

bool DevicePageStore::IsAllocated(PageId page_id) const noexcept {
    // FM3: the ceiling is the design ceiling now, not one bitmap page's
    // coverage. A region that does not exist reads as empty below it,
    // which is the same answer by a different route.
    if (page_id >= kMaxPageCount) return false;
    // A leased core's copy of the free map is the one it read at Open(),
    // and core 0 sets the bits for a lease when it *reserves* it - which
    // happens later, in core 0's copy. So this store's map cannot be asked
    // about this store's own ids, and the lease is the authority for them.
    //
    // Only an addition, never a subtraction: a bit the map does have still
    // counts. The two can only disagree in the direction of the map being
    // behind, because nothing ever frees.
    if (lease_ != nullptr && lease_->Owns(page_id)) return true;
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
    if (page_id < device_.page_capacity()) return Status::OK();
    return device_.EnsureCapacity(page_id + 1);
}

std::span<std::byte, kPageSize> DevicePageStore::InsertFrame(PageId page_id,
                                                             std::unique_ptr<Page> bytes,
                                                             bool dirty, bool warm) {
    std::span<std::byte, kPageSize> view(*bytes);
    Frame frame{std::move(bytes), dirty};
    // An ordinary miss starts warm (usage 1), not cold: the inline sweep
    // MG06 wires onto the fault path must never reclaim the page whose
    // fault triggered it, and one usage point is exactly one sweep rotation
    // of protection - the same grace a hit's bump buys. A *ring* fetch
    // starts cold, because a scan's touch is not heat (§5) and the ring's
    // own slot release depends on usage staying zero.
    frame.usage = warm ? std::uint8_t{1} : std::uint8_t{0};
    frames_.insert_or_assign(page_id, std::move(frame));
    return view;
}

StatusOr<std::span<std::byte, kPageSize>> DevicePageStore::ResidentBytes(PageId page_id,
                                                                         bool mark_dirty,
                                                                         bool bump_usage) {
    // PW1c-7: a page outside every granted set is *claimed* from its stream
    // stamp before either check below can refuse - the stamp is the durable
    // form of ownership, the premise server/relation_grant_service.hpp
    // states once. Attempted only where the check that applies would
    // refuse (MayWrite implies MayFault for a leased store, so a write asks
    // one predicate, not two), so a leased or granted page pays nothing and
    // core 0 (no lease) pays the one pointer compare it always did. The
    // device read a claim makes is the miss path's own, handed down rather
    // than repeated.
    std::unique_ptr<Page> prefetched;
    if (lease_ != nullptr && page_id >= system_page_limit_ &&
        (mark_dirty ? !MayWrite(page_id) : !MayFault(page_id))) {
        TryClaimByStamp(page_id, prefetched);
    }
#ifndef NDEBUG
    // The shared-nothing check (workplan-crosscore.md P2, guideline 1),
    // debug builds only. It sits here rather than in Get()/GetForRead()
    // because *faulting* is the act that makes a page this core's business;
    // a frame already resident was faulted through this same test.
    //
    // A hard failure rather than an assert: the caller has a Status channel,
    // and a test can assert on the code where it could not on a SIGABRT.
    if (!MayFault(page_id)) {
        return Status::InvalidArgument(
            "DevicePageStore: core " + std::to_string(core_id_) + " may not fault page " +
            std::to_string(page_id) + "; it belongs to another core");
    }
#endif
    // The write half is enforced in **every** build for a leased store,
    // since PW1c-5: the interim peer-DML guard is gone, so this is what
    // stands between an unfunded peer write (a crashed publish, grants
    // lost to a restart) and a page whose next mount refuses with the
    // rule-5 stamp mismatch. Refused-retryably beats detected-later.
    // Dirtying a system page would make a peer the second writer of a
    // single-writer page; two messages below because MayWrite refuses for
    // two reasons, and the not-from-this-lease one is the common case.
    // Zero cost where it matters: core 0 has no lease, so MayWrite
    // returns at its first test, and this runs on the frame-load path,
    // never per row. Debug builds additionally get the fault check above.
    if (mark_dirty && !MayWrite(page_id)) {
        return Status::InvalidArgument(
            "DevicePageStore: core " + std::to_string(core_id_) + " may not write page " +
            std::to_string(page_id) +
            (page_id < system_page_limit_
                 ? "; the system range has one writer, the system core"
                 : "; it is not from this core's extent lease, carries no write grant, and "
                   "its stream stamp does not name this core (a relation fault grant "
                   "conveys read rights only)"));
    }

    if (auto it = frames_.find(page_id); it != frames_.end()) {
        // Never clears the flag: a frame already dirty from an earlier
        // mutation stays dirty however many readers touch it afterwards.
        if (mark_dirty) it->second.dirty = true;
        // §3.1-2: a saturating bump on every hit, including a read -
        // "recently used" is about access, not about mutation. A *ring*
        // fetch is the one exception (§5): a scan's touch is not heat.
        if (bump_usage && it->second.usage < kClockUsageCap) ++it->second.usage;
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

    // The claim attempt above may already hold the bytes, checksum-verified
    // there; otherwise this is the read.
    const bool verified_by_claim = prefetched != nullptr;
    std::unique_ptr<Page> bytes = std::move(prefetched);
    if (bytes == nullptr) {
        bytes = std::make_unique<Page>();
        if (Status s = device_.ReadPage(page_id, std::span<std::byte, kPageSize>(*bytes));
            !s.ok()) {
            return s;
        }
    }

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
        // first flush: an extent reserved for a peer's lease is allocated
        // whole in the map core 0 flushes at startup, while the peer writes
        // its pages lazily, so a crash between a page's PAGE_INIT and its
        // write-back leaves exactly this (found by PW1c-7's restart test:
        // a peer that crashed with one unflushed new page could not
        // remount). NotFound, not Corruption: nothing was damaged, and
        // redo's PAGE_INIT arm *creates* a page the store does not hold,
        // where its checksum arm can only poison one it holds wrong and
        // wait for a full page image that never comes (wal/redo.cpp). A
        // torn page with a zero header and a nonzero body is not all zero
        // and still fails the checksum below.
        //
        // Run even when the claim above verified the checksum, and
        // deliberately: the claim verifies *that* check, not this one, and
        // "an all-zero page cannot pass a checksum" is a fact about
        // CRC32C's value over 8192 zero bytes rather than anything this
        // code says. Skipping it on the claim path would make the store's
        // answer for a never-written page depend on that coincidence.
        if (PageIsAllZero(*bytes)) {
            return Status::NotFound("DevicePageStore: page " + std::to_string(page_id) +
                                    " is allocated but was never written (all zero)");
        }
        // The claim above verified these very bytes before it believed
        // their stamp, so this is the one check it does subsume.
        if (Status s = verified_by_claim
                           ? Status::OK()
                           : VerifyPageChecksum(std::span<const std::byte, kPageSize>(*bytes));
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
    auto view = InsertFrame(page_id, std::move(bytes), mark_dirty, bump_usage);
    // MG06: the on-demand trigger (EV5). Faulting past the budget sweeps
    // the excess inline - under a **temporary pin on the frame just
    // inserted**, because usage alone does not protect it: EvictColdFrames
    // makes up to kClockUsageCap+1 laps in one call, so it can decrement a
    // fresh frame's single usage point on one lap and reclaim it on the
    // next, freeing the exact bytes this function is about to return. (The
    // first version of this block claimed one usage point was enough; the
    // MG05 poisoner run found the freed frame within ten thousand ops.)
    // The pin is safe to take by hand here: pinned frames are never erased,
    // so the iterator stays valid across the sweep.
    if (frame_budget_ != 0 && frames_.size() > frame_budget_) {
        auto guard = frames_.find(page_id);
        ++guard->second.pins;
        EvictColdFrames(frames_.size() - frame_budget_);
        --guard->second.pins;
    }
    return view;
}

void DevicePageStore::ReleaseScanSlot(PageId page_id) noexcept {
    if (page_id == kInvalidPageId) return;
    auto it = frames_.find(page_id);
    if (it == frames_.end()) return;  // reclaimed by a sweep meanwhile: fine
    const Frame& frame = it->second;
    // The foreground got there: a dirty write must reach the device, a pin
    // is absolute, a usage bump means a foreground accessor touched it
    // (ring fetches never bump), and a pinned-class page is never dropped
    // by anyone. Each abandons the frame to ordinary pool life.
    if (frame.dirty || frame.pins > 0 || frame.usage > 0 || IsPinnedClass(page_id)) return;
    frames_.erase(it);
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
        // The scan is over: every slot the foreground did not claim goes
        // back to the device's keeping.
        for (const PageId id : slots_) store_.ReleaseScanSlot(id);
    }

    StatusOr<std::span<std::byte, kPageSize>> Fetch(PageId page_id) override {
        // In place when resident - the foreground's frame or one of this
        // ring's own slots - never bumping usage: §5's interaction rule in
        // one direction, and "a scan is not heat" in the other.
        if (auto it = store_.frames_.find(page_id); it != store_.frames_.end()) {
            return std::span<std::byte, kPageSize>(*it->second.bytes);
        }

        // Rotate: the slot's previous occupant is dropped unless the
        // foreground claimed it, then the new page faults in clean with
        // its usage untouched.
        store_.ReleaseScanSlot(slots_[hand_]);
        auto bytes = store_.ResidentBytes(page_id, /*mark_dirty=*/false, /*bump_usage=*/false);
        if (!bytes.ok()) return bytes.status();
        slots_[hand_] = page_id;
        hand_ = (hand_ + 1) % slots_.size();
        return bytes;
    }

private:
    DevicePageStore& store_;
    std::vector<PageId> slots_;
    std::size_t hand_ = 0;
};

std::unique_ptr<ScanFetcher> DevicePageStore::OpenScanRing(std::size_t frames) {
    return std::make_unique<ScanRing>(*this, frames);
}

bool DevicePageStore::MayFault(PageId page_id) const noexcept {
    // The system core owns every fixed structure, so it may reach anything.
    if (lease_ == nullptr) return true;
    // The fixed system range is readable by every core: the catalog lives
    // there, and a core that cannot read it cannot serve a statement (P6).
    if (page_id < system_page_limit_) return true;
    if (lease_->Owns(page_id)) return true;
    // CC7: pages of a relation the catalog assigns to this core, granted at
    // DDL publish - read rights only, MayWrite never consults them. And
    // what this core may write it may read: a write grant's exact pages
    // and PW1c-7's stamp claims.
    return HasFaultRight(page_id) || HasWriteRight(page_id);
}

void DevicePageStore::GrantFaultPages(Extent extent) {
    // D10(a): no ceiling. An extent may span regions - it is an id range
    // and nothing constrains it to one - so the loop creates each region's
    // bitmap as it reaches it. Only the design ceiling bounds it now, and
    // an extent cannot reach that (ExtentAllocator::Reserve refuses).
    for (PageId id = extent.first; id < extent.end(); ++id) {
        RightsRegion& rights = RightsFor(id);
        if (rights.fault == nullptr) rights.fault = std::make_unique<Page>();
        FreeMapAllocate(std::span<std::byte, kPageSize>(*rights.fault), FreeMapBitIndexOf(id));
    }
}

void DevicePageStore::GrantWritePages(std::span<const PageId> pages) {
    for (PageId id : pages) {
        RightsRegion& rights = RightsFor(id);
        if (rights.write == nullptr) rights.write = std::make_unique<Page>();
        FreeMapAllocate(std::span<std::byte, kPageSize>(*rights.write), FreeMapBitIndexOf(id));
    }
}

bool DevicePageStore::DeviceHoldsOnlyZeros(PageId page_id) const {
    // Not addressable is the strongest form of never written. A failed read
    // answers "in use": refusing a CreateAt is the safe error.
    if (page_id >= device_.page_capacity()) return true;
    auto bytes = std::make_unique<Page>();
    if (!device_.ReadPage(page_id, std::span<std::byte, kPageSize>(*bytes)).ok()) return false;
    return PageIsAllZero(*bytes);
}

void DevicePageStore::TryClaimByStamp(PageId page_id, std::unique_ptr<Page>& prefetched) {
    if (page_id >= kMaxPageCount) return;  // no bit could hold the claim
    // A headerless page carries no stamp at all, so the bytes at the stamp's
    // offset are payload - checked here rather than beside the device read,
    // because the resident branch below would otherwise believe whatever a
    // headerless body happened to spell there and hand out write rights for
    // it. Refused downstream exactly as today.
    if (IsHeaderless(page_id)) return;
    std::uint16_t stamp = 0;
    if (auto it = frames_.find(page_id); it != frames_.end()) {
        // Resident without rights: redo faulted it at mount, before the
        // lease existed (core_runtime.cpp orders it that way), and stamped
        // this stream's id onto it as it replayed.
        stamp = GetPageStreamStamp(std::span<const std::byte, kPageSize>(*it->second.bytes));
    } else {
        // A page the device cannot address is not a page to read.
        if (page_id >= device_.page_capacity()) return;
        auto bytes = std::make_unique<Page>();
        if (!device_.ReadPage(page_id, std::span<std::byte, kPageSize>(*bytes)).ok()) return;
        // Verified before the stamp is believed: a torn page could spell
        // any stamp. The miss path trusts this verification and skips its
        // own.
        if (!VerifyPageChecksum(std::span<const std::byte, kPageSize>(*bytes)).ok()) return;
        stamp = GetPageStreamStamp(std::span<const std::byte, kPageSize>(*bytes));
        prefetched = std::move(bytes);
    }
    // Only this stream's own stamp claims. A foreign stamp is another
    // core's page (a defect to reach, refused as before); 0 is a page no
    // stream has written since it was formatted - a creation page core 0
    // handed off but this core never acquired, which the grant path's
    // acquisition restamp (rule 6) settles, never a claim.
    if (stamp != StreamStampFor(core_id_)) return;
    RightsRegion& rights = RightsFor(page_id);
    if (rights.write == nullptr) rights.write = std::make_unique<Page>();
    FreeMapAllocate(std::span<std::byte, kPageSize>(*rights.write), FreeMapBitIndexOf(page_id));
    ++stamp_claims_;
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("pagestore", "core " + std::to_string(core_id_) + " claimed page " +
                                     std::to_string(page_id) + " from its stream stamp");
    }
}

Status DevicePageStore::RefreshFreeMapFromDevice() {
    if (lease_ == nullptr) {
        return Status::InvalidArgument(
            "DevicePageStore: the system core's free map is the authority; nothing to refresh");
    }
    // Scratch first, validate whole, then merge - three defects of the
    // first form, each fixed here (the 25059bf review's C-2): reading into
    // the live copy destroyed it on a failed validate; core 0 flushes this
    // page concurrently with no latch, so a torn read is an ordinary
    // event, answered by keeping the old copy and retrying at the next
    // grant; and "the device is only ever ahead" is false - redo's
    // CreateAt sets bits in this copy at mount that core 0's map may never
    // have flushed, so replacement would subtract a page this store's own
    // recovery rebuilt. Union is what makes "strictly forward" a
    // constructed property. ValidateFreeMapPage, not the checksum half
    // alone: Open()'s whole rule.
    //
    // D5(a): **every resident region**, not region 0 alone. A peer loads
    // every region the device holds at Open - which runs before the lease
    // is installed, core_runtime.cpp ordering it that way - so every one of
    // them goes stale from that moment, and refreshing only the first left
    // the rest frozen at mount. The scratch-validate-union discipline is
    // per region and unchanged; a region that fails to read or validate
    // leaves its copy intact and stops the refresh, which is the retry the
    // next grant performs.
    //
    // A region created privately after the lease was installed (see
    // EnsureRegionResident) is skipped: the device holds no such page, so
    // there is nothing to union and reading would find only zeros.
    //
    // **Two things this deliberately does not adopt**, stated because the
    // rest of the comment reads as "the device's truth" and it is only
    // half of it:
    //
    //   - a region **not resident here at all** stays absent, and
    //     free_map_bytes_for answers such a region as all zeroes. Loading
    //     one is AdoptDeviceMapOnMiss's, at the seam that knows which id
    //     is wanted; doing it here would mean sweeping the whole device
    //     for regions on every grant.
    //   - the **headerless** bitmap of each region, which stays a
    //     mount-time snapshot. Unreachable today - the only creator is
    //     the Waystone directory (`stats/waystone_dir.cpp`), whose pages
    //     a peer can reach through neither a relation grant nor a stamp
    //     claim - but if it ever becomes reachable the asymmetry bites
    //     the wrong way: an adopted free-map bit over a stale headerless
    //     bit makes ResidentBytes verify a checksum that was never
    //     written, so the answer degrades from NotFound to Corruption.
    //     Union it here when that gate lifts.
    auto fresh = std::make_unique<Page>();
    for (auto& [region, pages] : map_regions_) {
        const PageId free_id = FreeMapPageIdFor(FreeMapRegionBase(region));
        if (device_.page_capacity() <= free_id) continue;

        auto view = std::span<std::byte, kPageSize>(*fresh);
        if (Status s = device_.ReadPage(free_id, view); !s.ok()) return s;
        if (RawPageType(view) == static_cast<std::uint8_t>(PageType::kInvalid)) continue;
        if (Status s = ValidateFreeMapPage(std::span<const std::byte, kPageSize>(*fresh));
            !s.ok()) {
            return s;
        }
        for (std::size_t i = kPageBodyOffset; i < kPageSize; ++i) {
            pages.free_map[i] |= (*fresh)[i];
        }
    }
    RecountAllocatedPages();
    return Status::OK();
}

bool DevicePageStore::MayWrite(PageId page_id) const noexcept {
    if (lease_ == nullptr) return true;
    // Read-only for a peer, deliberately: one writer per catalog page is
    // what makes a peer's stale view a retryable "not found" rather than a
    // torn read. The system check stays first: a write grant names
    // relation creation pages, never a system page, and keeping the order
    // makes that a structural fact rather than a convention.
    if (page_id < system_page_limit_) return false;
    if (lease_->Owns(page_id)) return true;
    // PW1c-4: the exact pages core 0 formatted for this core's relations,
    // granted after their handoff records went durable (GrantWritePages);
    // PW1c-7: the pages this stream's stamp claimed (TryClaimByStamp).
    return HasWriteRight(page_id);
}

StatusOr<std::span<std::byte, kPageSize>> DevicePageStore::CreateAtUnpinned(PageId page_id) {
    if (lease_ != nullptr) {
        // Placing a page at a *chosen* id is a claim on the free map, and
        // this store does not own it (see SetCoreOwnership). Every caller of
        // CreateAt is bootstrap or a fixed system page, all of which are
        // core 0's by M5 - so this is unreachable rather than restrictive,
        // and it is here so that it stays that way.
        return Status::InvalidArgument(
            "DevicePageStore: core " + std::to_string(core_id_) +
            " may not place a page at a chosen id; the free map belongs to the system core");
    }
    if (page_id >= kMaxPageCount) {
        return Status::OutOfRange("DevicePageStore: page id " + std::to_string(page_id) +
                                  " is beyond the " + std::to_string(kMaxPageCount) +
                                  "-page design ceiling");
    }
    // An allocated id is in use unless the device proves it was never
    // written - the page redo re-creates after a PAGE_INIT outran its first
    // flush (ResidentBytes answers NotFound for the same page, and says why
    // the map can be ahead of the bytes). Decided by a resident frame or
    // the device's bytes, never by the map alone: the map bit is exactly
    // what is true of both a live page and a never-written one.
    // The two maps live in this object, not in a frame, and may not have
    // reached the device yet - in use by definition.
    // Any region's bitmaps, not just region 0's: IsMapPageId is the
    // arithmetic form of the two fixed ids this check used to name.
    if (IsMapPageId(page_id) ||
        (IsAllocated(page_id) &&
         (frames_.count(page_id) != 0 || !DeviceHoldsOnlyZeros(page_id)))) {
        return Status::AlreadyExists("page id already in use");
    }
    if (Status s = EnsureAddressable(page_id); !s.ok()) return s;

    auto region = EnsureRegionResident(FreeMapRegionOf(page_id));
    if (!region.ok()) return region.status();
    auto free_map = std::span<std::byte, kPageSize>(region.value()->free_map);
    const std::uint32_t bit = FreeMapBitIndexOf(page_id);
    if (!FreeMapIsAllocated(std::span<const std::byte, kPageSize>(region.value()->free_map),
                            bit)) {
        ++allocated_pages_;
    }
    FreeMapAllocate(free_map, bit);
    region.value()->dirty = true;

    auto bytes = std::make_unique<Page>();
    bytes->fill(std::byte{0});
    if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
        log_->Trace("pagestore", "alloc page=" + std::to_string(page_id) + " (allocated=" +
                                     std::to_string(allocated_pages()) + ")");
    }
    // A brand-new page exists only in this frame until it is written back,
    // so it is dirty by definition.
    return InsertFrame(page_id, std::move(bytes), /*dirty=*/true);
}

StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> DevicePageStore::CreateNewUnpinned() {
    if (lease_ != nullptr) {
        // A leased core takes its id from the run core 0 already reserved
        // for it, and touches no shared state to do it. The free-map bits
        // were set at reservation, so there is nothing to mark here - which
        // is exactly why this path needs no message and no suspension
        // (extent_lease.hpp).
        auto id = lease_->Next();
        if (!id.ok()) return id.status();
        if (Status s = EnsureAddressable(id.value()); !s.ok()) return s;

        auto bytes = std::make_unique<Page>();
        bytes->fill(std::byte{0});
        if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
            log_->Trace("pagestore", "alloc page=" + std::to_string(id.value()) + " from core " +
                                         std::to_string(core_id_) + "'s lease (" +
                                         std::to_string(lease_->remaining()) + " left)");
        }
        return std::make_pair(id.value(),
                              InsertFrame(id.value(), std::move(bytes), /*dirty=*/true));
    }

    // FM3/FM5: the search crosses regions, and creates the next one when
    // it runs off the end of the last. Core 0 has no lease
    // (expeditor.cpp grants leases to cores 1..N-1 only), so this is the
    // whole of a single-core deployment's allocation and the hint keeps it
    // to one region's map in the steady state.
    PageId page_id = kInvalidPageId;
    for (PageId candidate = next_new_page_id_; candidate < kMaxPageCount;) {
        const std::uint32_t region = FreeMapRegionOf(candidate);
        auto bytes = FreeMapBytesForRegion(region);
        if (!bytes.ok()) return bytes.status();
        auto found = FreeMapFindFirstFree(bytes.value(), FreeMapBitIndexOf(candidate));
        if (found.has_value()) {
            const PageId id = FreeMapRegionBase(region) + *found;
            // A bitmap id is not a free page even when its bit is clear:
            // under FM6 the headerless map's id carries no bit until the
            // page is placed. Arithmetic rather than a reserved bit, so
            // this cannot go wrong by a bit being set late.
            if (IsMapPageId(id)) {
                candidate = id + 1;
                continue;
            }
            page_id = id;
            break;
        }
        candidate = FreeMapRegionBase(region + 1);
    }
    if (page_id == kInvalidPageId) {
        return Status::OutOfSpace("DevicePageStore: no free page id at or above " +
                                  std::to_string(next_new_page_id_));
    }
    // The raw sibling, not the pinned base accessor, for the reason
    // CreateNewHeaderlessUnpinned() states: this *is* the raw seam, and
    // pinning here takes a pin no handle asked for - balanced only by the
    // temporary's destructor, and counted against the debug ceiling in
    // between.
    auto created = CreateAtUnpinned(page_id);
    if (!created.ok()) return created.status();

    next_new_page_id_ = page_id + 1;
    return std::make_pair(page_id, created.value());
}

Status DevicePageStore::RaiseAllocationFloor(PageId first_allocatable_page_id) {
    if (lease_ != nullptr) {
        return Status::Unsupported(
            "DevicePageStore: core " + std::to_string(core_id_) +
            " allocates from an extent lease, whose floor this store does not own; raising it "
            "here would change nothing");
    }
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
    return Resolve(page_id, /*mark_dirty=*/true);
}

StatusOr<std::span<std::byte, kPageSize>> DevicePageStore::GetForReadUnpinned(PageId page_id) {
    return Resolve(page_id, /*mark_dirty=*/false);
}

StatusOr<std::span<std::byte, kPageSize>> DevicePageStore::Resolve(PageId page_id,
                                                                  bool mark_dirty) {
    if (!IsAllocated(page_id) && !AdoptDeviceMapOnMiss(page_id)) {
        return NotAllocated(page_id);
    }
    return ResidentBytes(page_id, mark_dirty);
}

bool DevicePageStore::AdoptDeviceMapOnMiss(PageId page_id) {
    // Core 0's copy **is** the free map, so a miss there is an absence and
    // there is nothing to adopt.
    if (lease_ == nullptr) return false;
    // Above the design ceiling no bit exists on any device either, so the
    // device read below could only confirm what this returns.
    if (page_id >= kMaxPageCount) return false;
    ++map_refreshes_on_miss_;

    // A region that was **not resident at this core's mount** is the same
    // defect one level up, and the FM series is what made it reachable:
    // RefreshFreeMapFromDevice walks resident regions only, and
    // free_map_bytes_for answers an absent region as all zeroes - so a page
    // core 0 placed in a region created after this core started could not
    // be adopted at all, not even one this core was explicitly granted.
    // Loaded here, and **only when absent**, because LoadRegionIfPresent
    // adds the region's allocated count while its emplace would not
    // overwrite a resident one - calling it on a region already held would
    // double-count. It does not create a region for a never-written id.
    const std::uint32_t region = FreeMapRegionOf(page_id);
    if (FindRegion(region) == nullptr) {
        if (Status s = LoadRegionIfPresent(region); !s.ok()) {
            LogAdoptionFailure(page_id, s);
            return false;
        }
        if (IsAllocated(page_id)) return true;
    }

    if (Status s = RefreshFreeMapFromDevice(); !s.ok()) {
        LogAdoptionFailure(page_id, s);
        return false;
    }
    return IsAllocated(page_id);
}

void DevicePageStore::LogAdoptionFailure(PageId page_id, const Status& why) const {
    // A refresh that keeps failing is the difference between "this id
    // really is not allocated" and "this core cannot find out", and only
    // the log separates them - the caller's refusal names the page either
    // way. The copy is left intact by the scratch-validate-union rule, so
    // the next miss retries.
    if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
        log_->Error("pagestore", "free-map adoption on miss of page " +
                                     std::to_string(page_id) + " failed: " + why.message());
    }
}

Status DevicePageStore::NotAllocated(PageId page_id) const {
    // redo.cpp:322 learned this on its own path: "page id not found" alone
    // says nothing about *which* id, and on a leased core nothing about
    // which authority answered.
    std::string msg = "page id " + std::to_string(page_id) + " not found";
    if (lease_ != nullptr) {
        msg += " (core " + std::to_string(core_id_) +
               ", leased: not in this core's extent lease and not set in its free-map copy)";
    }
    return Status::NotFound(std::move(msg));
}

Status DevicePageStore::StampPageLsn(PageId page_id, std::uint64_t lsn) {
    if (lsn == wal::kNoLsn) {
        return Status::InvalidArgument(
            "DevicePageStore: page_lsn 0 means 'never logged' and cannot be stamped");
    }
    auto it = frames_.find(page_id);
    if (it == frames_.end()) {
        return Status::NotFound("DevicePageStore: page " + std::to_string(page_id) +
                                " is not resident, so its page_lsn cannot be stamped");
    }

    // This is the one dirtying path that never asks MayWrite, and that is
    // deliberate, not an oversight (the 25059bf review's C-6): rule 6's
    // acquisition restamp must dirty a page *before* the write grant is
    // installed - the restamp is what makes granting sound. Every other
    // caller reached its frame through the checked accessor first.
    SetPageLsn(std::span<std::byte, kPageSize>(*it->second.bytes), lsn);
    // PW1c-3, PL §9 rule 4: the stream that last wrote the page. Rides
    // the LSN stamp because the two answer one question - *whose* offset
    // is page_lsn - and a page stamped by one and not the other is what
    // rule 5 calls Corruption.
    SetPageStreamStamp(std::span<std::byte, kPageSize>(*it->second.bytes),
                       StreamStampFor(core_id_));
    it->second.dirty = true;
    // First record since the frame was last written back wins: recLSN is
    // the *oldest* LSN redo must replay to make the page whole, so a later
    // record must never overwrite it (wal.md section 11-1).
    if (it->second.rec_lsn == wal::kNoLsn) it->second.rec_lsn = lsn;
    return Status::OK();
}

Status DevicePageStore::AwaitWalGate(std::span<const PageId> page_ids) {
    if (wal_gate_ == nullptr) return Status::OK();

    // One EnsureDurable for the batch maximum, not one per page: the call
    // is a no-op once the watermark is past, so the highest page_lsn in
    // the batch subsumes every other.
    wal::Lsn highest = wal::kNoLsn;
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
    if (highest == wal::kNoLsn) return Status::OK();  // nothing logged in this batch

    if (Status s = wal_gate_->EnsureDurable(highest); !s.ok()) {
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

StatusOr<std::size_t> DevicePageStore::WriteBack(std::span<const PageId> page_ids) {
    // Ascending and unique: id order is file order (page.md section 13),
    // and the queue this drains may name a page twice across sweeps.
    std::vector<PageId> ordered(page_ids.begin(), page_ids.end());
    std::sort(ordered.begin(), ordered.end());
    ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());

    // (1) durable: one gate call for the batch maximum, before any byte
    // moves - the whole of flush-before-evict.
    if (Status s = AwaitWalGate(ordered); !s.ok()) return s;

    std::size_t written = 0;
    std::vector<std::byte> scratch;
    for (std::size_t i = 0; i < ordered.size();) {
        auto it = frames_.find(ordered[i]);
        if (it == frames_.end() || !it->second.dirty) {
            ++i;  // evicted, or already written by someone else: not ours
            continue;
        }

        // Extend the run while the next ids are consecutive, resident and
        // dirty - the shape one WritePageRun can take.
        std::size_t run = 1;
        while (run < kWritebackRunPages && i + run < ordered.size() &&
               ordered[i + run] == ordered[i] + run) {
            auto next = frames_.find(ordered[i + run]);
            if (next == frames_.end() || !next->second.dirty) break;
            ++run;
        }

        // (2) checksum, the last thing that touches a page before it goes
        // out (page.md section 8) - skipped for a headerless page, which
        // has no field to put one in.
        for (std::size_t k = 0; k < run; ++k) {
            auto& frame = frames_.find(ordered[i + k])->second;
            StampIfHeadered(ordered[i + k], std::span<std::byte, kPageSize>(*frame.bytes));
        }

        // (3) write: one device call for a run, per page otherwise. The
        // run copies into scratch because frames are separate heap
        // allocations - bounded by kWritebackRunPages, and best-effort by
        // spec §4: a device without a real scatter write still sees the
        // pages land in file order.
        Status wrote = Status::OK();
        if (run > 1) {
            scratch.resize(run * kPageSize);
            for (std::size_t k = 0; k < run; ++k) {
                const auto& frame = frames_.find(ordered[i + k])->second;
                std::memcpy(scratch.data() + k * kPageSize, frame.bytes->data(), kPageSize);
            }
            wrote = device_.WritePageRun(ordered[i], static_cast<std::uint32_t>(run),
                                         std::span<const std::byte>(scratch));
        } else {
            const auto& frame = frames_.find(ordered[i])->second;
            wrote = device_.WritePage(ordered[i],
                                      std::span<const std::byte, kPageSize>(*frame.bytes));
        }
        if (!wrote.ok()) {
            if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                log_->Error("pagestore", "write failed for page " +
                                             std::to_string(ordered[i]) + " (run of " +
                                             std::to_string(run) + "): " + wrote.message());
            }
            return wrote;
        }

        // (4) clean, only now: a failure above leaves the frame dirty and
        // its recLSN intact, so the next writeback retries it.
        for (std::size_t k = 0; k < run; ++k) {
            auto& frame = frames_.find(ordered[i + k])->second;
            frame.dirty = false;
            frame.rec_lsn = wal::kNoLsn;  // clean: nothing to replay into it
            if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
                log_->Trace("pagestore", "wrote page=" + std::to_string(ordered[i + k]));
            }
        }
        written += run;
        i += run;
    }
    return written;
}

StatusOr<std::size_t> DevicePageStore::DrainDirtyEvictionQueue() {
    const std::vector<PageId> queued = TakeDirtyEvictionQueue();
    if (queued.empty()) return std::size_t{0};
    auto written = WriteBack(queued);
    if (written.ok() && written.value() > 0 && log_ != nullptr &&
        log_->enabled(LogLevel::kDebug)) {
        log_->Debug("pagestore", "writeback drained " + std::to_string(written.value()) +
                                     " queued dirty page(s)");
    }
    return written;
}

std::size_t DevicePageStore::MaintainFreeReserve(std::size_t pool_frames,
                                                 std::size_t watermark) {
    std::size_t reclaimed_total = 0;
    for (;;) {
        const std::size_t resident = frames_.size();
        const std::size_t free_frames = pool_frames > resident ? pool_frames - resident : 0;
        if (free_frames >= watermark) break;

        const std::size_t reclaimed = EvictColdFrames(watermark - free_frames);
        reclaimed_total += reclaimed;

        // Queued dirt is written clean here so the *next* rotation can
        // reclaim it - §4's "reclaim happens on the sweep's next visit".
        // A drain failure ends the loop rather than the world: the pages
        // stay dirty and queued facts are re-derived by the next sweep.
        auto drained = DrainDirtyEvictionQueue();
        const std::size_t cleaned = drained.ok() ? drained.value() : 0;

        if (reclaimed == 0 && cleaned == 0) break;  // a full rotation yielded nothing
    }
    return reclaimed_total;
}

Status DevicePageStore::Flush() {
    std::vector<PageId> dirty;
    dirty.reserve(frames_.size());
    for (const auto& [page_id, frame] : frames_) {
        if (frame.dirty) dirty.push_back(page_id);
    }

    auto written = WriteBack(dirty);
    if (!written.ok()) return written.status();

    if (Status s = FlushMaps(); !s.ok()) return s;
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
    std::vector<PageId> dirty;
    dirty.reserve(frames_.size());
    for (const auto& [page_id, frame] : frames_) {
        if (frame.dirty) dirty.push_back(page_id);
    }
    std::sort(dirty.begin(), dirty.end());
    return dirty;
}

std::vector<std::pair<PageId, wal::Lsn>> DevicePageStore::DirtyPagesWithRecLsn() const {
    std::vector<std::pair<PageId, wal::Lsn>> dirty;
    dirty.reserve(frames_.size());
    for (const auto& [page_id, frame] : frames_) {
        if (frame.dirty) dirty.emplace_back(page_id, frame.rec_lsn);
    }
    std::sort(dirty.begin(), dirty.end());
    return dirty;
}

Status DevicePageStore::EvictClean(std::span<const PageId> page_ids) {
    // Checked before anything is dropped, so a bad call leaves the store
    // exactly as it was rather than half-evicted.
    for (const PageId id : page_ids) {
        auto it = frames_.find(id);
        if (it == frames_.end()) continue;
        if (it->second.dirty) {
            return Status::InvalidArgument(
                "DevicePageStore: page " + std::to_string(id) +
                " is dirty; evicting it would discard a write");
        }
        // A pinned frame is one somebody holds a live `PageRef` into, so
        // dropping it here is the use-after-free the handle exists to
        // prevent (docs/inflight/in-progress/workplan-eviction.md EV01). This path predates pins
        // and its callers - a peer dropping stale catalog pages - never hold
        // one, so the check guards against a future caller rather than
        // against normal operation, exactly as the dirty check above does.
        if (it->second.pins != 0) {
            return Status::InvalidArgument(
                "DevicePageStore: page " + std::to_string(id) + " is pinned by " +
                std::to_string(it->second.pins) +
                " reference(s); evicting it would dangle them");
        }
    }
    for (const PageId id : page_ids) {
        frames_.erase(id);
    }
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("pagestore", "evicted " + std::to_string(page_ids.size()) +
                                     " page(s) for re-read on core " + std::to_string(core_id_));
    }
    return Status::OK();
}

Status DevicePageStore::FlushPages(std::span<const PageId> page_ids) {
    // The checkpointer's route through the one writeback primitive - §4's
    // "consumer of the machinery, not a parallel implementation".
    auto written = WriteBack(page_ids);
    if (!written.ok()) return written.status();
    bool wrote_any = written.value() > 0;

    // The maps go out with them, and after them: a page is only reachable
    // once the map says its id is allocated, so publishing the map first
    // would let a crash expose a page whose bytes never landed.
    if (maps_dirty()) {
        if (Status s = FlushMaps(); !s.ok()) return s;
        wrote_any = true;
    }

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

void DevicePageStore::PinFrame(PageId page_id) noexcept {
    // Called by the base pinned accessors immediately after the raw fetch
    // made the frame resident, on the same single-threaded core - so the
    // find can only miss if something is deeply wrong, and a miss is left
    // as a no-op pin rather than an abort: the failure it produces (a frame
    // evictable while a handle lives) is the one the poisoner (MG05)
    // detects deterministically.
    auto it = frames_.find(page_id);
    if (it == frames_.end()) return;
    ++it->second.pins;
    ++live_pins_;
    if (live_pins_ > pin_high_water_) pin_high_water_ = live_pins_;
#ifndef NDEBUG
    // MG04's ceiling, asserted rather than logged: a workload that holds
    // more pins than the audit derived is either a new Shape-B site missing
    // its bound or a leak, and both should fail the test that reaches them.
    if (live_pins_ > kPinCeiling) {
        std::fprintf(stderr,
                     "DevicePageStore: %zu live pins exceeds kPinCeiling %zu "
                     "(docs/spec/page.md §3)\n",
                     live_pins_, kPinCeiling);
        std::abort();
    }
#endif
}

void DevicePageStore::UnpinFrame(PageId page_id) noexcept {
    auto it = frames_.find(page_id);
    if (it == frames_.end()) return;
    // Saturating rather than wrapping. An unpin with no pin is a defect in
    // the handle, not in the caller, and the two failure modes are not
    // symmetric: a floor leaves a frame resident forever (a leak, visible in
    // pinned_frames()), where an underflow makes it evictable while somebody
    // still holds it.
    if (it->second.pins != 0) {
        --it->second.pins;
        if (live_pins_ != 0) --live_pins_;
    }
}

void DevicePageStore::MarkFrameDirty(PageId page_id) noexcept {
    auto it = frames_.find(page_id);
    if (it != frames_.end()) it->second.dirty = true;
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
    if (it == frames_.end()) return false;
    const PageHeaderFields header =
        ReadPageHeader(std::span<const std::byte, kPageSize>(*it->second.bytes));
    return header.page_type == static_cast<std::uint8_t>(PageType::kCabinBound) ||
           header.page_type == static_cast<std::uint8_t>(PageType::kFreeMap) ||
           header.page_type == static_cast<std::uint8_t>(PageType::kHeaderlessMap);
}

std::vector<PageId> DevicePageStore::TakeDirtyEvictionQueue() {
    std::vector<PageId> out;
    out.swap(dirty_eviction_queue_);
    return out;
}

void DevicePageStore::SetResidentLimit(PageId first_evictable_page_id) noexcept {
    // Additive only - see the declaration. A limit that could fall would let
    // a structure be declared un-evictable and then be evicted.
    if (first_evictable_page_id > first_evictable_page_id_) {
        first_evictable_page_id_ = first_evictable_page_id;
    }
}

std::size_t DevicePageStore::pinned_frames() const noexcept {
    std::size_t pinned = 0;
    for (const auto& [id, frame] : frames_) {
        if (frame.pins != 0) ++pinned;
    }
    return pinned;
}

std::size_t DevicePageStore::EvictColdFrames(std::size_t budget) {
    if (budget == 0 || frames_.empty()) return 0;

    // The sweep order. `frames_` is an unordered_map, so "where the hand is"
    // cannot be an iterator - a rehash would invalidate it - and is instead a
    // page id the pass re-finds by ordering. That costs a sort per sweep and
    // is why page.md §16-7 has the frame table becoming open-addressed; it is
    // deliberately not fixed here, because a sweep nothing calls yet (EV7) is
    // not where to spend that change.
    std::vector<PageId> order;
    order.reserve(frames_.size());
    for (const auto& [id, frame] : frames_) order.push_back(id);
    std::sort(order.begin(), order.end());

    // Resume where the last pass stopped, so the hand advances around the
    // whole set rather than re-punishing the low ids every time.
    auto start = std::lower_bound(order.begin(), order.end(), clock_hand_);
    const std::size_t first = static_cast<std::size_t>(start - order.begin());

    std::size_t reclaimed = 0;
    // Enough laps for the highest usage counter to be walked down to zero
    // and then collected. Nothing bumps a counter while the sweep runs, so
    // one more lap than the cap is exactly sufficient and no rotation past
    // that can reclaim anything a previous one did not.
    const std::size_t steps = order.size() * (kClockUsageCap + 1);
    for (std::size_t step = 0; step < steps && reclaimed < budget; ++step) {
        const PageId id = order[(first + step) % order.size()];
        auto it = frames_.find(id);
        if (it == frames_.end()) continue;  // reclaimed earlier in this pass

        Frame& frame = it->second;

        // The three refusals, in the order they are cheapest to test. Each
        // is a guarantee something else depends on, not an optimization:
        //   pinned    - a live PageRef points into these bytes (EV01);
        //   resident  - the class is never a candidate at any pressure (EV3),
        //               which is what AST04's Bound Cabin rests on;
        //   dirty     - the flush it needs is WAL-gated and is EV04's, so
        //               dropping it here would lose a write (EV02's scope).
        if (frame.pins != 0) continue;
        if (IsPinnedClass(id)) continue;

        // §3.2's branches, in the specified order: a positive usage counter
        // is decremented and the frame survives this rotation.
        if (frame.usage != 0) {
            --frame.usage;
            continue;
        }

        // Usage zero and dirty: queued for writeback, **not** reclaimed
        // (§3.2's fourth branch, §4's queue). Reclaiming it would lose the
        // write, and the writeback that would clean it is EVT03's.
        if (frame.dirty) {
            if (std::find(dirty_eviction_queue_.begin(), dirty_eviction_queue_.end(), id) ==
                dirty_eviction_queue_.end()) {
                dirty_eviction_queue_.push_back(id);
            }
            continue;
        }

#ifndef NDEBUG
        // MG05's poisoner: a caller that kept a raw span into this frame
        // reads 0xEF, deterministically, instead of whatever the allocator
        // does next. ASan turns the same mistake into a hard stop; this
        // makes it visible in a plain Debug build too.
        std::memset(frame.bytes->data(), 0xEF, kPageSize);
#endif
        frames_.erase(it);
        ++reclaimed;
        clock_hand_ = id + 1;
    }

    if (reclaimed != 0 && log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("pagestore", "clock reclaimed " + std::to_string(reclaimed) +
                                     " frame(s) on core " + std::to_string(core_id_) + ", " +
                                     std::to_string(frames_.size()) + " resident");
    }
    return reclaimed;
}

}  // namespace kds::storage
