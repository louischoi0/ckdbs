#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/free_map.hpp"
#include "kds/storage/page_device.hpp"

// Page-id extents: how a core that does not own the free map allocates
// pages (docs/inflight/in-progress/workplan-crosscore.md M5 and P5, docs/spec/page.md §7).
//
// ---- Why leases exist, and why they are not an optimization -------------
//
// Core 0 owns the free map (M5), so every other core's allocation has to
// reach it. The obvious shape - a request/response message per allocation -
// **cannot be written today**, and the reason is structural rather than
// effortful: there are 22 synchronous allocation sites deep in the storage
// layer (btree split, heap chain growth, var-heap append, undo pages), and
// nothing in the engine can suspend mid-call. Task representation is an
// explicitly open decision (`docs/spec/sched.md` §3 and §10, CLAUDE.md), so
// converting those call sites into resumable state machines would settle it
// by precedent, in the worst possible place to settle it.
//
// A lease removes the question. A core reserves a **run of ids up front**
// and then allocates from it with no message, no suspension and no change to
// any of those 22 sites. This is exactly the "per-core prealloc batching"
// `docs/spec/page.md` §7 deferred; it lands with the fan-out because per-core
// page stores do not work without it.
//
// The same trade `txn::TrxIdSequence` already makes for transaction ids, and
// the same consequence: **ids are unique and monotonic per core, never
// gapless**. A crash, or a core that stops with a partly-spent lease, burns
// the remainder. That is what batching costs and it is cheap - page ids are
// not identities, unlike Keystone ids.
//
// ---- What is deliberately not here --------------------------------------
//
// **Nothing frees an extent.** There is no page reclamation anywhere in the
// engine (no purge, no compaction, no DROP TABLE), so a returned lease would
// be a mechanism with no caller and no way to test it against a real
// workload. When reclamation arrives it decides this too.
//
// **Refill is not a blocking call.** `LeasedIdSource` answers
// TxnConflict when spent and says nothing about how a new extent is
// obtained; the requesting core asks core 0 over the ring
// (`RingMessageKind::kExtentLease`) and should ask at `low_water()`, while it
// still has ids to hand out. A core that runs dry before the grant lands
// fails one statement retryably rather than blocking its reactor - the only
// answer compatible with a cooperative core that never waits.

namespace kds::storage {

// A contiguous, exclusively-owned run of page ids.
//
// Contiguous rather than a set because that is what makes handing out the
// next id a single increment, which is the whole point - and because a
// contiguous run keeps a relation's pages near each other on the device,
// which the heap chain walk and any future readahead both want.
struct Extent {
    PageId first = kInvalidPageId;
    std::uint32_t count = 0;

    bool empty() const noexcept { return count == 0; }

    // One past the last id. Safe to compute: Reserve() refuses a run that
    // would reach kMaxPageCount, so this never wraps - the guard moved with
    // the ceiling when the free map became multi-page (FM4, and
    // docs/inflight/in-progress/workplan-multi-free-map.md §4's first named boundary case).
    PageId end() const noexcept { return first + count; }

    bool Contains(PageId page_id) const noexcept {
        return count > 0 && page_id >= first && page_id < end();
    }
};

// The extent size is `storage::kDefaultExtentPages` (page_device.hpp), which
// page.md §5 already proposed at 64 pages / 512 KiB and marked `[OPEN: size]`.
// It is deliberately *not* redefined here: the growth unit of the device and
// the lease unit of a core are the same quantity, and two names for it would
// be two things to keep in step. Every function below takes it as a
// parameter, so nothing depends on the number.
//
class DevicePageStore;

// Carves extents out of the free map. **Core 0 only** - it is the one owner
// of that page (M5), and this class exists so that ownership has a name.
//
// It does not own the map: `DevicePageStore` holds the bytes and is the
// only thing that writes them back. **A reservation is therefore durable
// only once the store has flushed a map it knows is dirty, and this class
// has to be what tells it** (PW3b's finding, docs/inflight/in-progress/workplan-peer-writer.md):
// the store marks its map dirty when `free_map_bytes()` is *taken*, so an
// allocator that cached that span reserved into a map the next flush
// skipped as clean - every refill core 0 granted after its last flush
// reached the device only if something else dirtied the map. That was
// invisible while a crash could only burn an *unspent* extent; since a peer
// writes into its lease (PW1c) a lost reservation frees a run holding
// committed rows, for the next mount's allocator to hand out over them. So
// the production form is built over the store, every reservation marks the
// map, and `Persist()` lands it before a grant leaves core 0.
//
// **It holds no page.** Before the free map became multi-page it borrowed
// one span for its lifetime, which is also why one bitmap page's coverage
// and the whole id space were the same number. Since FM4 it holds the store
// and asks for a region at a time (docs/inflight/in-progress/workplan-multi-free-map.md).
class ExtentAllocator {
public:
    // Over the live map of the store whose ids are being carved - the
    // production form, per the paragraph above. `store` must outlive this
    // allocator.
    //
    // Constructing one no longer marks anything dirty. It used to, because
    // it took the map's span up front and taking the span is what marks it;
    // since FM4 it holds no span at all and asks per region per call, so
    // the spurious dirty at construction is gone while the guarantee that
    // motivated it - a reservation always dirties the region it wrote - is
    // unchanged. Worth naming because that side effect is what let the PW3b
    // defect's first regression test pass against the very implementation
    // it was written to condemn (that review's C1): a test that leans on
    // construction to dirty the map is testing nothing now.
    explicit ExtentAllocator(DevicePageStore& store, PageId first_new_page_id) noexcept;

    // Over bare bytes, for tests that script a map. The caller owns the
    // bytes' durability, and Persist() has nothing to do. **One page, so
    // one region, and it is region 0**: this form cannot grow the map, and
    // a search that leaves region 0 reports OutOfSpace exactly as the
    // single-page allocator always did.
    explicit ExtentAllocator(std::span<std::byte, kPageSize> free_map,
                             PageId first_new_page_id) noexcept
        : bare_map_(free_map.data()), next_(first_new_page_id) {}

    // Reserves `count` **contiguous** free ids and marks every one of them
    // allocated immediately.
    //
    // Marking at reservation rather than at first use is what makes a run
    // exclusive: an id handed to core 2 must never also be found by core 0's
    // own CreateNew(), and the free map is the only place that fact can
    // live. The cost is that an unspent lease looks like allocated pages -
    // accurate, since nobody else may have them.
    //
    // **A reservation never crosses a free-map region** (D3(a) of
    // docs/inflight/in-progress/workplan-multi-free-map.md). A run that would straddle abandons
    // the tail of the region it started in and restarts in the next, which
    // costs at most count-1 ids per boundary - under 0.1% at the default
    // extent size, and permanently, since nothing frees. What it buys: one
    // dirty map page per reservation, one crash window, one resident
    // region. It also makes a region the longest possible run, which is
    // why a count above kFreeMapBitsPerPage is refused outright.
    //
    // Over a store, a search that walks off the end of the last region
    // **creates the next one** (FM5) - growth has no separate call and
    // nothing to keep in step with the search.
    //
    // Fails with InvalidArgument for count 0, and OutOfSpace when no
    // contiguous run that long remains below the design ceiling
    // (kMaxPageCount ids, 16 TiB).
    StatusOr<Extent> Reserve(std::uint32_t count);

    // Makes every reservation so far durable: the store writes its map back
    // and syncs the device (`DevicePageStore::PersistMaps`). The extent
    // grant's durability point (server/extent_lease_service.hpp) - called
    // before the grant leaves core 0, for the reason the class comment
    // gives. OK with nothing to do over bare bytes.
    Status Persist();

    // Extents handed out. Diagnostics and tests.
    std::uint64_t reservations() const noexcept { return reservations_; }

private:
    // One region's live bytes, fetched from the store per call so each
    // mutation marks that region dirty - the cached span is the raw-bytes
    // form's only. Fails where the store's does: a region a peer may not
    // create, a device that cannot be grown, a torn map page.
    StatusOr<std::span<std::byte, kPageSize>> MapBytes(std::uint32_t region);

    DevicePageStore* store_ = nullptr;
    // The raw-bytes form's single page, null over a store. A pointer and
    // not a span because a fixed-extent span has no empty state, and the
    // store form genuinely holds no page - it asks per region.
    std::byte* bare_map_ = nullptr;
    // Where the next search starts. Purely a hint - correctness rests on the
    // free map, and a stale value costs a longer scan and never a double
    // allocation.
    PageId next_;
    std::uint64_t reservations_ = 0;
};

// One core's private supply of page ids, handed out with no message and no
// suspension. Not thread-safe and not meant to be: it belongs to one
// reactor, like everything else on a core (rules.md #3).
class LeasedIdSource {
public:
    LeasedIdSource() noexcept = default;
    explicit LeasedIdSource(Extent extent) { Grant(extent); }

    // The next id, or TxnConflict when the lease is spent - the one code
    // the wire's `retryable` bit follows (status.hpp's IsRetryable says why).
    //
    // Exhaustion is a **retryable** condition, not a failure of the
    // database: a refill is a message away and the statement that hit the
    // boundary can be re-run. Callers must not treat it as OutOfSpace, which
    // means the device is genuinely full.
    StatusOr<PageId> Next();

    // Adds an extent and starts issuing from it. Any ids left in the
    // previous one are burned - see the header note on gaplessness.
    //
    // The old extent is **remembered** even though it is no longer issued
    // from, because the pages allocated out of it are still this core's and
    // it still faults them. Forgetting them would make the ownership check
    // below reject a core's own pages the moment it took a second lease,
    // which is a bug the check would report as somebody else's.
    //
    // A grant that begins exactly where the previous one ended is merged
    // into it rather than appended, which is the ordinary case: the
    // allocator carves sequentially, so a core that is the only one
    // allocating accumulates one range, not one per refill.
    void Grant(Extent extent);

    // Ids left before Next() starts failing.
    std::uint32_t remaining() const noexcept { return current_.count - issued_; }
    bool spent() const noexcept { return remaining() == 0; }

    // Whether a refill should be requested now. True once the lease is down
    // to a quarter of its ids, so the grant has time to arrive while the
    // core can still allocate. A quarter is a heuristic and nothing depends
    // on it - asking early costs one message.
    bool low_water() const noexcept { return remaining() <= current_.count / 4; }

    // Whether `page_id` falls in **any** extent this core has been granted,
    // spent or current. What the page store's ownership check asks.
    //
    // Linear over the granted list, which is why grants are merged: the
    // common case is one range. Called in **every** build since PW1c-5 -
    // IsAllocated and the leased store's always-on MayWrite both consult
    // it on the frame-load path - so the merge is what keeps the scan a
    // handful of compares, not a debug-only indulgence.
    bool Owns(PageId page_id) const noexcept;

    // The extent currently being issued from.
    const Extent& extent() const noexcept { return current_; }

    // Every extent granted, after merging. Diagnostics and tests.
    const std::vector<Extent>& granted() const noexcept { return granted_; }

    std::uint64_t issued_total() const noexcept { return issued_total_; }

private:
    Extent current_{};
    std::uint32_t issued_ = 0;
    std::uint64_t issued_total_ = 0;
    // Every range ever granted, merged where contiguous. Kept for Owns()
    // alone - issuing only ever reads current_.
    std::vector<Extent> granted_;
};

}  // namespace kds::storage
