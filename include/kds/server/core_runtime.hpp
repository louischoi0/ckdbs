#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/budget.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/tcp_server.hpp"
#include "kds/server/assertion_build_service.hpp"
#include "kds/server/extent_lease_service.hpp"
#include "kds/server/index_build_service.hpp"
#include "kds/server/shipped_statement_executor.hpp"
#include "kds/server/statement_ship_service.hpp"
#include "kds/server/mount_recovery.hpp"
#include "kds/server/remote_step_service.hpp"
#include "kds/server/row_id_lease_service.hpp"
#include "kds/server/trx_id_lease_service.hpp"
#include "kds/server/remote_checkpoint_anchor.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/page_store_checkpoint_target.hpp"
#include "kds/storage/extent_lease.hpp"
#include "kds/storage/page_device.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/wal/checkpointer.hpp"
#include "kds/wal/file_log_device.hpp"
#include "kds/wal/manager.hpp"

// One core's stack (docs/inflight/in-progress/workplan-crosscore.md P2): the reactor and
// everything below it that is *not* shared.
//
// `page.md` §6 puts the intent in one line - "multi-core adds instances, not
// synchronization" - and this class is that sentence made a type. The
// single-core wiring `Expeditor` already had becomes the per-core wiring,
// instantiated N times.
//
// ---- What a non-system core has, and what it does with it ---------------
//
// As of P6 a peer has a full statement stack: its own `DevicePageStore` over
// the shared device, its own `Catalog`, transaction manager and
// `CommandDispatcher`. It can resolve a relation and run a statement.
//
// Three asymmetries against core 0 are deliberate and are the whole of P6's
// soundness:
//
//   1. **The catalog is read-only here.** The catalog's fixed pages have one
//      writer, core 0 (M5). A peer faults them read-only - the page store
//      enforces it (`MayWrite`) - and re-reads them when core 0 broadcasts
//      `kCatalogInvalidate` after a DDL. A peer that has not yet processed
//      the broadcast answers "table not found", which crosscore.md §5
//      already specifies as retryable.
//   2. **Allocation comes from a lease**, never from the free map, which is
//      also core 0's (storage/extent_lease.hpp).
//   3. **Nothing is recorded.** `waystone_recording` and
//      `access_statistics` are off on a peer, and this is not a default
//      anybody should change without reading the next paragraph.
//
// ---- Why a peer records nothing (P6's known cost) -----------------------
//
// `sys.patterns` and `sys.access_stats` are catalog pages written on the
// **ordinary statement path** - `TrailRecorder::EnsurePattern` registers a
// shape seen twice, and every successful statement records its access
// shapes. Under rule 1 above a peer cannot write them, and neither can be
// shipped to core 0: the access-stat write could be (it is explicitly
// best-effort), but `RegisterPattern` returns a `PatternAccess*` the
// recorder uses immediately, so it needs an answer, and nothing here can
// wait for one.
//
// Both features are advisory by construction - invariant 8 for Waystone,
// "a degraded statistic, not a degraded database" for the other - so a peer
// with them off returns **exactly the same rows**, more slowly, and
// contributes nothing to the optimizer's input. That is the honest cost of
// this phase. The fix is per-core statistics relations, which crosscore.md
// §2 already calls for ("no statistics cross cores") and which is a page
// layout change to three relations.
//
// Core 0 still owns the superblock, the free map, the catalog pages and the
// listener. Those live on `Expeditor` rather than here: they are the
// *database*, not a core's copy of anything.
//
// ---- Threading -----------------------------------------------------------
//
// A CoreRuntime is created on the startup thread and then handed to exactly
// one worker, which owns it for the rest of its life. Nothing in it is
// synchronized (rules.md #3), and nothing outside that worker may touch it
// once `Run()` has begun - including to stop it, which is why shutdown is a
// message (`RingMessageKind::kShutdown`) and not a method call.

namespace kds::server {

class CoreRuntime {
public:
    struct Config {
        std::uint32_t core_id = 0;
        std::string wal_dir;
        sched::MonoTimeNs checkpoint_interval_ns = 0;
        sched::MonoTimeNs wal_drain_interval_ns = 0;

        // Copied from the superblock core 0 already decoded, on the startup
        // thread and before any worker exists - so this is a plain copy,
        // not a cross-core read of a structure that belongs to core 0.
        std::uint32_t inline_cell_width = storage::kDefaultInlineCellWidth;
        std::uint32_t core_count = 1;

        // This core's share of the instance frame budget
        // (`buffer_pool_frames`, docs/spec/eviction.md §6: the key is a
        // total, divided evenly per core - EV4). 0 = unbounded, the same
        // meaning SetFrameBudget gives it. Core 0's share - the even part
        // plus the division remainder - is applied by Expeditor::Open at
        // store open, not through this struct.
        std::size_t buffer_pool_frames = 0;

        // Settings a peer shares with core 0. Recording is *not* among
        // them - see the header on why a peer records nothing.
        wal::DurabilityClass durability = wal::DurabilityClass::kGroup;
        txn::IsolationLevel isolation = txn::IsolationLevel::kReadCommitted;
        exec::Budget budget;

        // This core's page-id lease, carved by core 0's ExtentAllocator
        // before the worker starts.
        storage::Extent lease;

        // This core's WAL anchor, copied out of core 0's superblock on the
        // startup thread - where recovery starts this stream's scan (RV1/RV2,
        // server/mount_recovery.hpp). It cannot be read from `superblock_`
        // below: that is a default-constructed copy whose anchor slots are
        // all zero, and a peer's checkpointer publishes its anchor *through*
        // core 0 (remote_checkpoint_anchor.hpp) rather than into its own.
        // A zeroed anchor is legal and means "no checkpoint yet, scan from
        // the head of the stream" - which is why the mistake would be silent.
        //
        // Value-initialized, and that brace is load-bearing:
        // `WalAnchorFields` is a plain on-disk layout with no member
        // initializers (superblock.hpp), so a caller that left this field
        // alone would otherwise hand recovery whatever was on the stack.
        // Which is not a subtle failure - it is `ScanLog: lsn 1065353216
        // names segment 15`, and a mount that refuses because of a
        // caller's stack contents.
        WalAnchorFields anchor{};

        // Core 0's durable transaction-id ceiling, copied on the startup
        // thread for the reason the anchor above is: this core's
        // `superblock_` is default-constructed, so the field reads 0 here
        // and a mount would compare its recovered stream against nothing.
        // Zero means "core 0 had none to give", which is only true before a
        // database exists (PW1, docs/inflight/in-progress/workplan-peer-writer.md).
        std::uint64_t next_trx_id = 0;
    };

    // Opens this core's WAL stream, page store, catalog and dispatcher, and
    // builds its reactor. `device` is the shared page device and must
    // outlive this runtime; the store built over it is this core's own.
    //
    // The WAL segment files are named `wal-<core_id>-<segment_no>.log`
    // (file_log_device.hpp), so N cores in one directory do not collide -
    // that naming predates multicore and is why it needed no change.
    static StatusOr<std::unique_ptr<CoreRuntime>> Open(Config config,
                                                       storage::PageDevice& device,
                                                       const sched::Clock& clock, Logger* log);

    CoreRuntime(const CoreRuntime&) = delete;
    CoreRuntime& operator=(const CoreRuntime&) = delete;

    // **The reactor is dropped first, ahead of everything it borrows.**
    // `scheduler_` is declared first because every member below borrows it,
    // which by the reverse-order rule would destroy it *last* - and the
    // scheduler owns coroutine frames (`sched::CoroTask` destroys a
    // suspended one), whose locals reach back into those members. A
    // pipeline stage parked at a credit gate when `kShutdown` arrives holds
    // a `txn::ReaderLease` on its frame, and that lease's destructor calls
    // `txn_manager_->UnregisterReader()` - on a manager the reverse order
    // has already destroyed. Safe here because nothing destroyed below
    // submits or polls; only the frames' own destructors run, and every
    // member they touch is still alive.
    //
    // One member breaks that last sentence and so goes *before* the
    // scheduler in the same body: `listener_` (PW5), whose `~TcpServer`
    // unregisters its fds from the reactor.
    ~CoreRuntime();

    // Attaches this core to the ring matrix and installs the handlers every
    // core needs - today just `kShutdown`, which is what lets core 0 stop
    // this one without touching its memory. `transport` must outlive this.
    Status AttachTransport(sched::RingTransport& transport);

    // PW5: binds `port` with SO_REUSEPORT and attaches the listener to
    // this core's reactor and dispatcher, on the startup thread before
    // the worker exists. STOP accepted here routes to the system core
    // (tcp_server.hpp's stop contract); the listener dies first at
    // teardown - see ~CoreRuntime.
    Status ListenAndAttach(std::uint16_t port);

    // BUG-4 ordering (the PW5 review): closes the listener - and with it
    // every accepted session, rolling back open transactions - while this
    // core's WAL can still be synced by the caller. Serve calls it for
    // every core before the final per-core Sync(), the same detach-then-
    // sync order core 0's own teardown has always had. Idempotent.
    void CloseListener() noexcept { listener_.reset(); }

    // Runs this core's reactor until a `kShutdown` message arrives. This is
    // the worker thread's whole body.
    void Run();

    // Drains and syncs this core's log. Called on the way down, after Run()
    // returns, so an acknowledged commit on this core survives the stop.
    Status Sync();

    // Runs one checkpoint to completion and publishes its anchor **through
    // core 0** (PW3, docs/inflight/in-progress/workplan-peer-writer.md; remote_checkpoint_anchor.hpp
    // carries why that send is one-way). A no-op on a core with no
    // checkpointer - core 0's is `Expeditor`'s, and a runtime with no
    // transport has nowhere to publish to.
    //
    // Public for the reason `GrantRelationFault` is: the cadence below calls
    // it, and a test drives it without a reactor.
    Status Checkpoint();

    // **The shutdown checkpoint** (PW3b, docs/inflight/in-progress/workplan-peer-writer.md) - the
    // third of core 0's three checkpoint points, which PW3 left a peer
    // without: a graceful restart replayed up to one `checkpoint_interval`
    // of every peer's stream. Flushes this core's pages, runs one checkpoint
    // and publishes its anchor **directly through `system_anchor`** - core
    // 0's `SuperBlockCheckpointAnchor` - rather than over the ring, because
    // after the worker join no reactor runs on either side to carry a send
    // (remote_checkpoint_anchor.hpp's last section holds the argument and
    // the rejected alternative).
    //
    // The page flush comes first for the reason core 0's final Sync()
    // precedes its checkpoint (expeditor.cpp): a checkpoint's redo start is
    // min(recLSN) over the dirty table it snapshots at BEGIN, so with the
    // table empty the redo start is the BEGIN LSN itself and the next mount
    // reads this checkpoint's own two records rather than everything since
    // the oldest dirty page. It runs under the WAL gate and Complete() makes
    // CHECKPOINT_END durable, so nothing here rests on the caller's sync.
    //
    // Startup thread, after Run() returned and the worker joined - the same
    // thread and moment as Sync(). On a core with no checkpointer the flush
    // still runs and nothing is published. Not fatal to a shutdown when it
    // fails: the data is durable through the syncs, and the cost is a slower
    // next mount, which the caller logs.
    Status ShutdownCheckpoint(wal::CheckpointAnchor& system_anchor);

    // Drops this core's cached view of the catalog - both the derived facts
    // and the page frames they came from. What the `kCatalogInvalidate`
    // handler calls; exposed so a test can drive it without a reactor.
    void InvalidateCatalog();

    // Asks the system core for another extent when this one crosses its
    // low-water mark. A no-op with no transport, and at most one request in
    // flight at a time.
    void MaybeRefillLease();

    // The same, for this core's transaction ids (PW1): a peer may not raise
    // the superblock's ceiling, so its windows are granted. Peers only -
    // core 0 carves its own and never leases from itself.
    void MaybeRefillTrxIds();

    // And for row ids (PW1b), which differ in what triggers them: a row-id
    // lease is per *relation*, so this asks for the neediest relation the
    // lease table knows about, and the table learns of one only when a
    // statement asks it for an id (catalog/row_id_lease.hpp).
    void MaybeRefillRowIds();

    // And for a relation's grants (PW1c-7, relation_grant_service.hpp):
    // sends the system core one re-delivery request per relation the
    // dispatcher's rights probe found unwritable since the last tick. On
    // the same tick as the leases; a no-op with no transport or no demand.
    void MaybeRequestRelationGrants();

    // The receive side of CC7's flush-then-grant handoff (workplan P6b):
    // fault rights over a relation's page range, granted by core 0 at DDL
    // publish. What the `kRelationFaultGrant` handler calls; exposed so a
    // test can drive it without a reactor, InvalidateCatalog's pattern.
    void GrantRelationFault(storage::Extent extent);

    // The receive side of PW1c-4's exact-page write grant, and the home of
    // PL §9 rule 6's **acquisition restamp**: each granted page is faulted
    // (read rights - the fault grant precedes this on the same FIFO edge),
    // restamped to this stream (stamp := own, page_lsn := this stream's
    // current end LSN, via the StampPageLsn funnel) and flushed durable,
    // and only then admitted to MayWrite. A failure leaves the page
    // unwritable and logs it - the relation stays refused retryably, the
    // publish hook's own stance. What the `kRelationWriteGrant` handler
    // calls; exposed for the reason GrantRelationFault is.
    void GrantRelationWrite(std::span<const PageId> pages);

    // This core's row-id leases and refill state (P5's shape). Exposed for
    // the same reason GrantRelationFault is: a test drives the grant
    // without a reactor, and diagnostics read the counters.
    catalog::RowIdLeaseTable& row_id_leases() noexcept { return row_id_leases_; }
    RowIdRefill& row_id_refill() noexcept { return row_id_refill_; }

    // PW1c-7's demand, exposed for the same reason: a test reads that the
    // dispatcher's probe recorded a relation, then drives the tick.
    const RelationGrantDemand& relation_grant_demand() const noexcept { return grant_demand_; }

    // PW1c-6b-2's window and the service that keeps it, exposed for the
    // same reason. Null on core 0 and before AttachTransport.
    const PendingIndexBuilds& pending_index_builds() const noexcept {
        return pending_index_builds_;
    }
    IndexBuildServer* index_builds() noexcept {
        return index_builds_.has_value() ? &*index_builds_ : nullptr;
    }

    // PW1c-6c's owner half, exposed for the same reason: a test drives a
    // build and reads what the owner did. Null on core 0 and before
    // AttachTransport.
    AssertionBuildServer* assertion_builds() noexcept {
        return assertion_builds_.has_value() ? &*assertion_builds_ : nullptr;
    }

    // This core's half of statement shipping (SS3), exposed for the same
    // reason: a test drives a shipped statement and reads what the owner
    // did with it. Null before AttachTransport.
    ShippedStatementExecutor* shipped_statements() noexcept {
        return shipped_executor_.has_value() ? &*shipped_executor_ : nullptr;
    }
    StatementShipClient* statement_ship() noexcept {
        return statement_ship_client_.has_value() ? &*statement_ship_client_ : nullptr;
    }

    // This core's transaction-id lease, exposed for the first of those two
    // reasons only: a test drives a grant without a reactor.
    txn::TrxIdLease& trx_id_lease() noexcept { return trx_id_lease_; }

    std::uint32_t core_id() const noexcept { return config_.core_id; }
    sched::Scheduler& scheduler() noexcept { return *scheduler_; }
    wal::WalManager& wal() noexcept { return *wal_; }
    catalog::Catalog& catalog() noexcept { return *catalog_; }
    CommandDispatcher& dispatcher() noexcept { return *dispatcher_; }

    storage::DevicePageStore& store() noexcept { return *store_; }

    // What this core's mount did (RV1/RV2 at Open, RC08's completion
    // checkpoint at AttachTransport) - `Expeditor::recovery()`'s counterpart,
    // and what this core's `SHOW META` recovery block reads (PW3b: kept
    // rather than discarded, so a peer's stop can be checked to have bounded
    // its next mount by the same field core 0's is).
    const MountRecovery& recovery() const noexcept { return recovery_; }

private:
    CoreRuntime(Config config, Logger* log) noexcept
        : config_(config), log_(log), lease_(config.lease) {}

    Config config_;
    Logger* log_ = nullptr;
    sched::RingTransport* transport_ = nullptr;
    // Filled at Open, read by the dispatcher below for the rest of this
    // core's life - so it is declared above everything that borrows it.
    MountRecovery recovery_;

    // Declared in construction order and torn down in reverse, the same
    // discipline Expeditor's members follow: the reactor holds the io
    // backend, the WAL manager holds the log device, and the dispatcher
    // holds references into everything below it.
    std::unique_ptr<sched::IoBackend> io_backend_;
    std::optional<sched::Scheduler> scheduler_;
    std::unique_ptr<wal::FileLogDevice> log_device_;
    std::unique_ptr<wal::WalManager> wal_;

    // This core's own supply of page ids, and the store that allocates from
    // it. Declared before the store, which holds a pointer to it.
    storage::LeasedIdSource lease_;
    std::unique_ptr<storage::DevicePageStore> store_;

    // The refill this core is waiting on, if any. It outlives the coroutine
    // that waits on it, which is `WaitFor`'s one requirement - a flag on the
    // coroutine's own frame would be gone the moment it suspended.
    ExtentRefill refill_;
    // One refill in flight at a time. Without this the low-water check
    // would submit a fresh request on every tick until the first grant
    // landed, and every one of them would be answered - burning an extent
    // per tick for a core that needed one.
    bool refill_in_flight_ = false;

    // Row-id leases (P5's shape): the per-relation blocks this core issues
    // Keystone ids from, installed into the catalog on every non-zero
    // core, and the refill state the kRowIdLease receiver releases.
    catalog::RowIdLeaseTable row_id_leases_;
    RowIdRefill row_id_refill_;

    // The transaction-id lease this core issues from (PW1), and the refill
    // waiting on a grant. Declared before `trx_ids_` below, which holds a
    // pointer to the lease.
    txn::TrxIdLease trx_id_lease_;
    TrxIdRefill trx_id_refill_;
    // One refill in flight at a time, `refill_in_flight_`'s rule and for
    // its reason: without it every tick before the first grant lands would
    // submit another request, and every one of them would be answered.
    bool trx_id_refill_in_flight_ = false;
    // One row-id refill in flight at a time, for the same reason - and it is
    // per core rather than per relation, so a second needy relation waits one
    // tick rather than racing the first.
    bool row_id_refill_in_flight_ = false;

    // The relations this core owns and found itself unable to write
    // (PW1c-7): written by the dispatcher's rights probe, drained by
    // MaybeRequestRelationGrants. Peers only; core 0's dispatcher is never
    // given it.
    RelationGrantDemand grant_demand_;
    // One re-delivery request in flight per core (the PW1c-7 review's C4):
    // each request makes core 0 run a whole publish - a catalog scan, an
    // extent flush, three appends and one fsync on its reactor - so a client
    // retrying an ungrantable relation at the 1 ms drain cadence must not
    // become a thousand fsyncs a second on core 0. Cleared when a write
    // grant is admitted; expires after kRelationGrantRequestTicks ticks so
    // a request core 0 dropped (a failed publish, a relation it does not
    // grant) can be asked again by the next refused statement.
    bool grant_request_in_flight_ = false;
    std::uint32_t grant_request_age_ticks_ = 0;
    static constexpr std::uint32_t kRelationGrantRequestTicks = 1000;  // ~1 s at 1 ms

    // The remote step server (P4b), armed at AttachTransport: this core
    // answers STEP_OPENs for relations it owns.
    std::optional<RemoteStepServer> remote_steps_;

    // PW1c-6b-2 (index_build_service.hpp): the window the dispatcher's
    // gate reads - declared before the dispatcher, which holds a pointer
    // - and the owner's half of a peer-owned relation's CREATE INDEX,
    // armed at AttachTransport on peers. It borrows the catalog, store
    // and WAL declared below; references only, and its destructor
    // touches none of them.
    PendingIndexBuilds pending_index_builds_;
    std::optional<IndexBuildServer> index_builds_;

    // PW1c-6c (assertion_build_service.hpp): the owner's half of a
    // peer-owned relation's CREATE ASSERTION, armed at AttachTransport on
    // peers. No window beside it, where the index build has one - the
    // owner adopts inside its own build task, so there is nothing to hold
    // off. It borrows the catalog, store, WAL, transaction manager and the
    // dispatcher's registry, all declared below; references only.
    std::optional<AssertionBuildServer> assertion_builds_;

    // The two objects this core's checkpointer borrows (PW3). Built at
    // `AttachTransport`, not at `Open`: the anchor publishes over the ring,
    // so it cannot exist before the ring does. Declared below `scheduler_`
    // and `store_`, which they hold references to.
    std::optional<storage::PageStoreCheckpointTarget> checkpoint_target_;
    std::optional<RemoteCheckpointAnchor> checkpoint_anchor_;

    // The statement stack. A peer's `SuperBlock` is a **copy** taken on the
    // startup thread: the dispatcher needs one for SHOW-class commands, and
    // the live instance belongs to core 0. Nothing here reaches the page.
    //
    // Since PW1 the copy carries one field that is not merely decorative:
    // `Config::next_trx_id`, core 0's transaction-id ceiling, is applied to
    // it at `Open` so the mount check has a real bound and `trx_ids_` below
    // caches a real one. It is still a copy and still unpersisted - a peer's
    // *raise* of that ceiling comes from a grant, never from here.
    SuperBlock superblock_;
    std::optional<catalog::Catalog> catalog_;
    std::optional<txn::TrxIdSequence> trx_ids_;
    std::optional<txn::UndoLog> undo_log_;
    std::optional<txn::TransactionManager> txn_manager_;
    std::optional<CommandDispatcher> dispatcher_;

    // **Statement shipping, both halves, on every core** (SS1's rule: an
    // owner with no request handler and an arrival core with no reply
    // receiver each cost a shipped statement a full deadline and a false
    // `UnknownOutcome`, and from the arrival core the two are
    // indistinguishable from a slow owner). Armed at AttachTransport,
    // peer or not, because shipping runs in both directions - core 0's
    // client ships to a peer's server and a peer's client ships to core
    // 0's.
    //
    // **Declaration order is load-bearing**, and in the opposite direction
    // from the usual: the server holds the executor's `Seam()`, which
    // captures the executor, so the server must be destroyed *first* and is
    // therefore declared *last* of the two. Both go below `dispatcher_`,
    // which the executor borrows, and the reactor that owns their tasks is
    // dropped ahead of every member by `~CoreRuntime`'s body.
    std::optional<ShippedStatementExecutor> shipped_executor_;
    std::optional<StatementShipServer> statement_ship_server_;
    std::optional<StatementShipClient> statement_ship_client_;
    // The client listener this core accepts on, when per-core listeners are
    // configured (PW5). It borrows the scheduler and the dispatcher, and
    // `~TcpServer` calls back into the scheduler to unregister its fds - so
    // it is dropped explicitly at the top of `~CoreRuntime`, ahead of the
    // scheduler. Declaration order alone would not do it: that destructor's
    // *body* drops the scheduler before any member destructor runs.
    std::optional<TcpServer> listener_;

    // Last, because it borrows every one of them: the WAL, the target and
    // anchor above, `txn_manager_`, and the dispatcher's assertion
    // enforcer. Reverse-order destruction therefore takes it first.
    std::optional<wal::Checkpointer> checkpointer_;
};

// The extent-aligned page range covering a relation's fixed roots (the
// desc/root page and the var-heap root when one exists) - what core 0
// grants the owner at DDL publish (crosscore.md CC7). Extent-aligned
// because the store's ownership unit is the extent; the alignment may
// cover pages of other core-0 relations, the superset assertion CC7
// accepts, since the enforced mechanism is statement dispatch and never
// this check. The production send lands with P6c, when a non-creating
// owner first becomes possible; until then the contract test drives it.
storage::Extent RelationFaultExtentOf(const catalog::SysTableRow& row,
                                      std::uint32_t extent_pages);

// The send-side half of PW1c-4's publish, extracted so it is testable
// without a whole Expeditor (the 95b45e8 review's S1 - the untested
// lambda is where C1 hid): appends a PAGE_HANDOFF per formatted page
// into the giver's stream, makes them durable, and returns the
// exact-page write-grant payload. A failed status means **withhold the
// write grant** - the relation stays fault-readable and its writes
// refused retryably. Refuses more pages than the payload holds rather
// than truncating (the capacity check PW1c-6 will lean on). The caller
// owns the flush *before* this and the sends after it.
StatusOr<RelationWriteGrantPayload> PrepareRelationHandoff(wal::WalManager* wal,
                                                           std::uint32_t owner_core,
                                                           std::span<const PageId> pages);

}  // namespace kds::server
