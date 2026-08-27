#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/ring_message.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/sched/task.hpp"
#include "kds/server/core_affinity.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/manager.hpp"

// Building a peer-owned relation's index on the core that owns it
// (docs/inflight/in-progress/workplan-peer-writer.md §7c, decided 2026-08-25; PW1c-6b-2 is the
// owner's half and the ring, PW1c-6b-3 core 0's two phases).
//
// `CREATE INDEX` is core 0's statement - the catalog has one writer - but
// the tree it builds is pages, and a relation another core owns has its
// pages in that core's pool, stamped by that core's stream, holding rows
// core 0 cannot see: the owner's uncommitted writes, and every committed
// one core 0 never faulted. So the build moves, not the statement. Core 0
// prepares the definition (`exec::PrepareIndexDef`) and sends it here; the
// owner runs the page half `CREATE INDEX` always ran (`exec::BuildIndexTree`
// - root from its own lease, backfill from its own pool, the tree's images
// into its own stream), seeds its own anchor slot, and replies with the
// root. Core 0 writes the `sys.indexes` row with that root and no seed
// (`Catalog::AnchorSeed::kByOwner`), commits, and tells the owner `done`.
//
// ---- The refusal window --------------------------------------------------
//
// From the request's arrival until `done`, **writes to that relation on
// the owner are refused retryably** (`IndexBuildPending`,
// core_affinity.hpp). The owner's catalog shows no index until core 0's
// commit reaches it, so a row written in the window would be indexed by
// nobody - and an index missing a row is a wrong answer with a right
// answer's shape. `done` ends the window whichever way the statement went.
//
// The window is sound only because of what `Backfill` indexes: **every
// version** the relation holds, uncommitted and delete-marked included
// (docs/spec/index.md §10a). A rollback on the owner writes pages without
// passing the dispatcher's gate - compensation goes through the
// transaction manager, not a statement - so a window that refused
// statements alone would still see pages change under the build if the
// build indexed only what was visible. It does not: a row whose writer
// later rolls back is in the tree, and the probe's MVCC check drops it, the
// same way it drops it from every index today.
//
// ---- The order core 0 keeps, and why `done` may cross the broadcast ------
//
// The owner learns of the published index two ways: the catalog
// invalidation broadcast every DDL sends, and `done(committed)`, whose
// handler drops the owner's cache itself. Either may arrive first; both
// are sound because `InvalidateCatalog` **re-reads the catalog pages off
// the device**, and core 0 flushes them *before either message leaves*:
// the row write's own invalidation hook (`Catalog::BumpVersion` ->
// `Expeditor::BroadcastCatalogInvalidation`, flush then send) runs inside
// phase 2, ahead of the commit, and `done` is sent after the commit. An
// instance wired without that hook (a test over a bare catalog) has to
// flush by hand before `done`, or the owner re-reads the state before the
// row and keeps refusing on the shape gate rather than the window.
//
// `done(committed)` is sent when the commit record is *appended*, not when
// it is durable - the durability wait is the statement's, taken after
// (docs/spec/wal.md D2). A crash in between loses the commit: recovery rolls
// core 0's DDL back and retires the row, and the tree the owner built -
// logged under kNoTxnId in the owner's stream, redone regardless - is
// orphaned with the entries the owner's maintenance wrote into it
// meanwhile. Orphaned, not wrong: nothing names it any more.
//
// ---- Two clocks, one invariant ---------------------------------------------
//
// Core 0 parks on the reply under `kIndexBuildReplyDeadlineNs` and sends
// `done(aborted)` when it gives up; the owner keeps the window under
// `kIndexBuildPendingCeilingNs` when no `done` comes at all. The owner must
// never release while core 0 could still commit: a row written after the
// release and before the commit is exactly the missing row above. The
// static_assert below bounds the *reply* leg only - the ceiling exceeds
// the deadline. The commit leg, reply received to `done` sent, is one
// reactor turn on core 0 (phase 2 is synchronous: the row, the commit
// append, the send) plus however long a full ring holds the send-retry
// task; the 120 s between deadline and ceiling is the margin the design
// assumes for it, not one the code proves. A `done` that arrives after the
// window expired is ignored, so a commit that late leaves the owner's
// cache to the broadcast (and to the expiry's own invalidation) - the case
// PW1c-6b-4's gate lift has to reckon with.
//
// A reply that arrives after core 0 gave up matches no waiter: the receiver
// sends the owner `done(aborted)` itself, so the tree it names is orphaned
// and the window closes now rather than at the ceiling - otherwise a
// request the ring held past the deadline would cost the relation a
// 180 s write outage.
//
// ---- Under kNoTxnId, in the owner's stream --------------------------------
//
// The images and the anchor record carry no transaction. Core 0's DDL
// transaction lives in core 0's stream, and naming it here would mint a
// loser in this stream's analysis with nothing to roll back - the phantom
// a PAGE_HANDOFF with a transaction id is refused for. What a rollback
// costs is the tree, orphaned, and the anchor slot, which stays (PW2-3's
// named debt, one more occupant).

namespace kds::server {

// The wire forms. POD, under ring_message.hpp's exception to the on-disk
// rules: they never leave the process.

// core 0 -> owner: the definition, verbatim from `exec::PrepareIndexDef` -
// the widths it computed, the columns in declared order, the oid it
// issued. The name rides along so the owner's `CheckIndexDef` refuses by
// the rules core 0's did, one implementation.
struct IndexBuildRequestPayload {
    std::uint64_t table_oid;
    std::uint64_t index_oid;
    std::uint16_t key_width;
    std::uint16_t entry_width;
    std::uint8_t nkeys;
    std::uint8_t ncovered;
    std::uint8_t flags;
    std::uint8_t reserved0;
    std::uint16_t key_cols[catalog::kMaxIndexKeyColumns];
    std::uint16_t covered_cols[catalog::kMaxIndexCoveredColumns];
    char name[catalog::kCatalogNameMax];  // NUL-padded, as the catalog stores it
};
static_assert(sizeof(IndexBuildRequestPayload) == 112);

// owner -> core 0: the root, or why not. `status_code` is a StatusCode; 0
// is success and only then is `root_page_id` meaningful. The message is
// the owner's failure, truncated to the wire: the client on core 0 sees
// it, and the owner's log holds the whole.
inline constexpr std::size_t kIndexBuildReplyMessageBytes = 112;
struct IndexBuildReplyPayload {
    std::uint64_t index_oid;
    std::uint32_t root_page_id;
    std::uint32_t status_code;
    char message[kIndexBuildReplyMessageBytes];  // NUL-terminated
};
static_assert(sizeof(IndexBuildReplyPayload) == 128);

// core 0 -> owner: the statement's end. `committed` = 1 publishes the
// tree; 0 orphans it. Either closes the window.
struct IndexBuildDonePayload {
    std::uint64_t index_oid;
    std::uint8_t committed;
    std::uint8_t reserved0[7];
};
static_assert(sizeof(IndexBuildDonePayload) == 16);

inline constexpr sched::MonoTimeNs kIndexBuildReplyDeadlineNs = 60ull * 1'000'000'000ull;
inline constexpr sched::MonoTimeNs kIndexBuildPendingCeilingNs = 180ull * 1'000'000'000ull;
static_assert(kIndexBuildPendingCeilingNs > kIndexBuildReplyDeadlineNs,
              "the owner must outwait core 0's reply deadline: a window released before core 0 "
              "gives up admits rows the published index would miss (the commit leg past the "
              "deadline is the margin between the two, see the header)");

// The encode core 0 sends. **Refuses** rather than truncates, since a
// truncated definition would be checked by the owner as something other
// than what was declared. Two refusals, and only the first is a rule this
// engine already keeps: the column caps are `CheckIndexDef`'s
// refuse-never-truncate (spec §11), so a definition that reached here is
// already within them and this is the guard for a caller that did not go
// through `exec::PrepareIndexDef`. The **name** length is stricter than
// the catalog, which has no name rule at all - `Catalog::CreateIndex`
// truncates through `SetName` - so a name past kCatalogNameMax - 1 is
// refused on this arm and silently shortened on the local one. That
// divergence is real and named here rather than papered over; closing it
// belongs in `CheckIndexDef`, where both arms would see it.
StatusOr<IndexBuildRequestPayload> IndexBuildRequestOf(const catalog::Catalog::IndexDef& def);
// The owner's decode of a request `IndexBuildServer::OnRequest` has
// already bounded.
catalog::Catalog::IndexDef IndexDefOf(const IndexBuildRequestPayload& request);

// Every message of this protocol travels through `sched::SubmitSendPod` on
// the `system` group, with **`session_core` the constant 0 on every leg** -
// not an echo of the request's, as the lease services do it, because core 0
// owns the statement in both directions and nothing on this protocol reads
// the field. It is named at each call site rather than defaulted, so a
// reader of a captured header sees it was decided. `request_id` is 0 for
// `done`, which belongs to no waiter.

// ---- The owner's half ------------------------------------------------------

class IndexBuildServer {
public:
    // Runs on `done(committed)` and on an expiry - the runtime drops its
    // catalog cache here, so the published index is seen. The one seam
    // that reaches back into its owner; the sends and the build's task go
    // straight to the reactor and the ring, as the client's do.
    using OnCommittedFn = std::function<void()>;

    // `scheduler` is this core's reactor - the build runs on it as a
    // `system` task, its clock dates the window, and every reply leaves
    // through it as a send-retry task on `transport`. Both outlive this.
    IndexBuildServer(catalog::Catalog& catalog, storage::PageStore& store, wal::WalManager* wal,
                     std::uint32_t core_id, PendingIndexBuilds& pending,
                     sched::Scheduler& scheduler, sched::RingTransport& transport,
                     OnCommittedFn on_committed = {}, Logger* log = nullptr) noexcept
        : catalog_(catalog),
          store_(store),
          wal_(wal),
          core_id_(core_id),
          pending_(pending),
          scheduler_(scheduler),
          transport_(transport),
          on_committed_(std::move(on_committed)),
          log_(log) {}

    // The kIndexBuildRequest handler: bound the counts, check the owner,
    // open the window, build (as a `system` task), seed, reply. Every
    // refusal is a reply - core 0 is parked on one.
    void OnRequest(const sched::MessageHeader& header, std::span<const std::byte> payload);
    // The kIndexBuildDone handler: closes the window; `committed` also
    // drops the catalog cache. A `done` naming no open window is ignored.
    void OnDone(const sched::MessageHeader& header, std::span<const std::byte> payload);
    // The tick: closes every window older than the ceiling, each logged as
    // the `done` that never came, and drops the cache in case it was a
    // commit whose `done` was lost.
    void Expire(sched::MonoTimeNs now);

    // Builds attempted. Diagnostics and tests.
    std::uint64_t builds() const noexcept { return builds_; }

private:
    void Build(std::uint32_t requester, std::uint64_t request_id,
               const IndexBuildRequestPayload& request);
    void Reply(std::uint32_t requester, std::uint64_t request_id, std::uint64_t index_oid,
               PageId root, const Status& status);

    catalog::Catalog& catalog_;
    storage::PageStore& store_;
    wal::WalManager* wal_;
    std::uint32_t core_id_;
    PendingIndexBuilds& pending_;
    sched::Scheduler& scheduler_;
    sched::RingTransport& transport_;
    OnCommittedFn on_committed_;
    Logger* log_;
    std::uint64_t builds_ = 0;
};

// ---- Core 0's half ---------------------------------------------------------

// What a reply lands in, addressed by the statement's request id.
// `arrived` is what the statement parks on; `deadline_ns` is when it
// stops parking.
struct IndexBuildOutcome {
    bool arrived = false;
    Status status;
    PageId root_page_id = kInvalidPageId;
    sched::MonoTimeNs deadline_ns = 0;
};

// Core 0's side of the protocol (PW1c-6b-3): the waiters, the deadline,
// the two sends and the reply receiver. The dispatcher's foreign
// `CREATE INDEX` arm is its one client - phase 1 calls `Request`, the
// parked statement re-tests `Settled` once per reactor turn, phase 2 reads
// `Find`, then `Close`s and says `Done`. A map, for its stable addresses:
// the receiver writes into an entry while other entries come and go, and a
// vector would move it.
class IndexBuildClient {
public:
    IndexBuildClient(sched::Scheduler& scheduler, sched::RingTransport& transport,
                     const sched::Clock& clock, Logger* log = nullptr) noexcept
        : scheduler_(scheduler), transport_(transport), clock_(clock), log_(log) {}

    // Installs the reply receiver on the scheduler this was built over.
    // The handler captures `this` and there is no unregister, so **the
    // client must outlive every pump of that scheduler** - it cannot
    // outlive the scheduler itself, since it is built from it, and the
    // owner of both is what keeps a reply from landing on a dead client.
    // A reply matching no waiter is core 0 having given up (the deadline,
    // or a statement with no reactor to park on): a successful one is
    // answered with `done(aborted)` so the owner's window closes now; a
    // refusal needs nothing, the owner closed its own.
    Status RegisterReplyReceiver();

    // Phase 1's send: opens the waiter under the deadline, then sends. A
    // definition the wire refuses (`IndexBuildRequestOf`) opens nothing.
    Status Request(std::uint32_t owner_core, std::uint64_t request_id,
                   const catalog::Catalog::IndexDef& def);
    // The parked statement's predicate: the reply arrived, the deadline
    // passed, or the waiter is gone. One clock read per reactor turn.
    bool Settled(std::uint64_t request_id) const;
    // The outcome, or null once closed. `arrived` false after `Settled`
    // answered true is the deadline.
    const IndexBuildOutcome* Find(std::uint64_t request_id) const;
    void Close(std::uint64_t request_id);
    // Phase 2's end, either way: publishes or orphans the owner's tree
    // and closes its window.
    void Done(std::uint32_t owner_core, std::uint64_t index_oid, bool committed);

    std::size_t waiting() const noexcept { return waiting_.size(); }

private:
    sched::Scheduler& scheduler_;
    sched::RingTransport& transport_;
    const sched::Clock& clock_;
    Logger* log_;
    std::map<std::uint64_t, IndexBuildOutcome> waiting_;
};

}  // namespace kds::server
