#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <utility>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/catalog/oid.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/ring_message.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/range_coalesce.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/wal/manager.hpp"

// AX — the two ring legs of a coalesce (`docs/spec/crosscore.md` §6c
// steps 0-4; `range_coalesce.hpp` is the work each leg performs).
//
// `index_build_service.hpp` is the shape this follows: core 0 owns the
// statement, sends, parks on a reply under a deadline, and the peer does
// the page half in its own stream. Two differences, both because a
// coalesce is smaller than an index build:
//
//   - **No `done` leg and no refusal window.** An index build needs one
//     because core 0's commit publishes a tree the owner must then
//     maintain; a coalesce publishes nothing the peer has to know about
//     beyond the catalog invalidation the contraction already broadcasts.
//     The peer's part is finished when it replies.
//   - **No definition on the wire.** Both requests carry a relation oid
//     and (for the quiesce) the one range's bounds. The receiver reads
//     `sys.ranges` itself, so nothing here can carry a page list - which
//     is what keeps the protocol clear of the 992-byte reply ceiling and
//     of `RelationWriteGrantPayload`'s six-page cap.
//
// ---- What a lost leg costs -------------------------------------------
//
// **Nothing that needs repair.** Every prefix of §6c's sequence is a
// state the engine serves: a quiesce that never ran leaves a range its
// owner still writes, and the merge is abandoned before any link; an
// absorb that never replied leaves pages moved and chains part-linked,
// which the intact directory still describes correctly because a range's
// walk is bounded by the range (§6c). So a deadline here fails the DDL
// and leaves the relation split, and the statement can be run again.
//
// ---- Concurrency ------------------------------------------------------
//
// Each handler runs as one `system` task on its own single-threaded
// reactor, and the whole of its work happens inside that task with no
// suspension - which is what quiesce means here: no statement of that
// core interleaves with the flush and the revoke.

namespace kds::server {

// The wire forms. POD, under ring_message.hpp's exception to the on-disk
// layout rules: they never leave the process.

// core 0 -> a departing range's owner. `absorber` is named so the owner
// can log its departure records against the receiving stream (PL §9 rule
// 1 puts them in the *giver's* stream), which is why the absorber must be
// chosen before this leg runs and not after it.
struct RangeQuiesceRequestPayload {
    std::uint64_t table_oid;
    std::uint64_t lo;
    std::uint64_t hi;
    std::uint32_t entry_page;
    std::uint32_t absorber;
};
static_assert(sizeof(RangeQuiesceRequestPayload) == 32);

// core 0 -> the absorber. The whole relation, because the absorber walks
// every range: the ones it already owns cost it nothing but a census.
struct RangeAbsorbRequestPayload {
    std::uint64_t table_oid;
};
static_assert(sizeof(RangeAbsorbRequestPayload) == 8);

// Both replies, one shape: `status_code` is a StatusCode, 0 is success,
// and `pages` is what the leg touched - the quiesce's flushed count, the
// absorb's acquired count. The message is the peer's failure truncated to
// the wire; the peer's log holds the whole of it.
inline constexpr std::size_t kRangeCoalesceReplyMessageBytes = 104;
struct RangeCoalesceReplyPayload {
    std::uint64_t table_oid;
    std::uint32_t status_code;
    std::uint32_t pages;
    char message[kRangeCoalesceReplyMessageBytes];  // NUL-terminated
};
static_assert(sizeof(RangeCoalesceReplyPayload) == 120);

// One deadline for both legs. Sized on the absorb, which is the long one:
// it faults, restamps and flushes every page of the relation, so it scales
// with the relation where the quiesce scales with one range's dirty
// frames. Deliberately the index build's number - the two are the same
// class of DDL-latency wait and a second constant would be a second thing
// to tune (`kIndexBuildReplyDeadlineNs`).
inline constexpr sched::MonoTimeNs kRangeCoalesceReplyDeadlineNs = 60ull * 1'000'000'000ull;

// ---- The peer's half ------------------------------------------------------

// Installed on every core, core 0 included: core 0 can be a departing
// range's owner (it owns the `lo = 0` range of every relation created on
// it) and can be the absorber, and a self-addressed message is an ordinary
// ring send to itself.
//
// The two seams into the runtime are the store, the WAL and the catalog it
// is built over; everything else it needs it reads from `sys.ranges`.
// `AdmitPages` is the runtime's `AdmitWritePages` - the acquisition
// sequence PL §9 rule 6 requires (fault, acquisition record, restamp,
// flush, grant) - passed in rather than reimplemented, because a second
// copy of that ordering is a second place for it to drift.
class RangeCoalesceServer {
public:
    using AdmitPagesFn = std::function<bool(std::span<const PageId>)>;

    RangeCoalesceServer(catalog::Catalog& catalog, storage::DevicePageStore& store,
                        wal::WalManager* wal, std::uint32_t core_id, sched::Scheduler& scheduler,
                        sched::RingTransport& transport, AdmitPagesFn admit,
                        Logger* log = nullptr) noexcept
        : catalog_(catalog),
          store_(store),
          wal_(wal),
          core_id_(core_id),
          scheduler_(scheduler),
          transport_(transport),
          admit_(std::move(admit)),
          log_(log) {}

    // §6c steps 0-2 for one range: walk it, flush it, log a departure
    // `PAGE_HANDOFF` per page naming the absorber, wait for durability,
    // revoke this core's write rights, drop the frames. Replies either
    // way - core 0 is parked on one.
    void OnQuiesce(const sched::MessageHeader& header, std::span<const std::byte> payload);

    // §6c step 4: plan the relation from this core's own catalog, acquire
    // every page it does not already hold, link the chains in `lo` order,
    // flush. Replies either way.
    void OnAbsorb(const sched::MessageHeader& header, std::span<const std::byte> payload);

    std::uint64_t quiesced_ranges() const noexcept { return quiesced_ranges_; }
    std::uint64_t absorbed_relations() const noexcept { return absorbed_relations_; }
    std::uint64_t pages_moved() const noexcept { return pages_moved_; }

private:
    void Reply(sched::RingMessageKind kind, std::uint32_t requester, std::uint64_t request_id,
               std::uint64_t table_oid, std::uint32_t pages, const Status& status);

    catalog::Catalog& catalog_;
    storage::DevicePageStore& store_;
    wal::WalManager* wal_;
    std::uint32_t core_id_;
    sched::Scheduler& scheduler_;
    sched::RingTransport& transport_;
    AdmitPagesFn admit_;
    Logger* log_;
    std::uint64_t quiesced_ranges_ = 0;
    std::uint64_t absorbed_relations_ = 0;
    std::uint64_t pages_moved_ = 0;
};

// ---- Core 0's half --------------------------------------------------------

// What a reply lands in, addressed by the leg's request id.
struct RangeCoalesceOutcome {
    bool arrived = false;
    Status status;
    std::uint32_t pages = 0;
    sched::MonoTimeNs deadline_ns = 0;
};

// Core 0's side: the waiters, the deadline, the two sends and the two
// reply receivers. A map for its stable addresses, `IndexBuildClient`'s
// reason - the receiver writes into an entry while other entries come and
// go, and a vector would move it.
class RangeCoalesceClient {
public:
    RangeCoalesceClient(sched::Scheduler& scheduler, sched::RingTransport& transport,
                        const sched::Clock& clock, Logger* log = nullptr) noexcept
        : scheduler_(scheduler), transport_(transport), clock_(clock), log_(log) {}

    // Installs both reply receivers. The handlers capture `this` and there
    // is no unregister, so the client must outlive every pump of that
    // scheduler.
    Status RegisterReplyReceivers();

    Status Quiesce(std::uint32_t owner_core, std::uint64_t request_id, catalog::Oid rel_oid,
                   std::uint64_t lo, std::uint64_t hi, PageId entry_page, std::uint32_t absorber);
    Status Absorb(std::uint32_t absorber, std::uint64_t request_id, catalog::Oid rel_oid);

    // The parked driver's predicate: the reply arrived, the deadline
    // passed, or the waiter is gone.
    bool Settled(std::uint64_t request_id) const;
    const RangeCoalesceOutcome* Find(std::uint64_t request_id) const;
    void Close(std::uint64_t request_id);

    std::size_t waiting() const noexcept { return waiting_.size(); }

private:
    void Open(std::uint64_t request_id);
    void OnReply(const sched::MessageHeader& header, std::span<const std::byte> payload);

    sched::Scheduler& scheduler_;
    sched::RingTransport& transport_;
    const sched::Clock& clock_;
    Logger* log_;
    std::map<std::uint64_t, RangeCoalesceOutcome> waiting_;
};

}  // namespace kds::server
