#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/budget.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/txn/lock_table.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/stats/trail_recorder.hpp"
#include "kds/server/tcp_server.hpp"
#include "kds/server/mount_recovery.hpp"
#include "kds/server/remote_step_service.hpp"
#include "kds/server/range_alloc.hpp"
#include "kds/server/row_id_lease_service.hpp"
#include "kds/server/trx_id_lease_service.hpp"
#include "kds/server/remote_checkpoint_anchor.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/page_store_checkpoint_target.hpp"
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
//   1. **The catalog was read-only here until AT-S5.** Its fixed pages had
//      one writer, core 0, enforced by the store's `MayWrite`; a peer
//      faulted them read-only and shipped or refused every DDL. Every core
//      writes them now under the page latch, the DDL's relation `X` and the
//      schema word (`catalog.md` CT5), and this asymmetry is history.
//   2. **Allocation reaches the one free map**, under the structure latch.
//      It came from a per-core extent lease until AW-S1b, because a store a
//      core did not own could not reach that map at all.
//   3. **Waystone records nothing here.** `waystone_recording` is off on a
//      peer, and this is not a default anybody should change without
//      reading the next paragraph. **`access_statistics` left this
//      sentence at AT-S7**: every core writes `sys.access_stats` itself,
//      under the relation's root page latch, so the instance's one switch
//      arms it here exactly as on core 0 - CR7's fold-and-flush to core 0
//      and the ring kind that carried it are retired. **The Cabin left it
//      too** (AT-S7): one `stats::CabinStore` serves the instance, so a
//      peer observes into the same sets it serves from. AK-S2 had given
//      each core its own, on an invariant that AT-S5 and AT-S6 then
//      removed - a relation's one owner used to be exactly the core every
//      write to it landed on and every read of it ran on, so the owner's
//      store was the only one that could observe a value *and* stay a
//      superset through the writes that follow. The defect that left
//      behind (a set served a query short across cores) is closed; what
//      replaced the ownership argument is `cabin.md` §6's announce.
//
// ---- Why a peer records nothing (P6's known cost) -----------------------
//
// `sys.patterns` is a catalog page written on the **ordinary statement
// path** - `TrailRecorder::EnsurePattern` registers a shape seen twice.
// Under rule 1 above a peer could not write it, and the write could not be
// shipped to core 0 either: `RegisterPattern` returns a `PatternAccess*`
// the recorder uses immediately, so it needs an answer, and nothing here
// can wait for one.
//
// Waystone is advisory by construction (invariant 8), so a peer with it
// off returns **exactly the same rows**, more slowly, and contributes
// nothing to a replay.
//
// **`sys.access_stats` left this paragraph at AT-S7**: rule 1 is history,
// so a peer writes the relation where the statement ran and CR7's fold,
// its ring kind and CR8's permitted drop are all retired
// (`crosscore.md` CC13). Note what was *not* the fix, then or now:
// per-core statistics **relations**, which this paragraph used to name and
// which the ratification declined - they would have opened oid allocation,
// row migration and a core-count question, where the requirement was only
// that a peer's accesses be counted at all.
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
// once `Run()` has begun - **with one exception, and only one**:
// `scheduler().Stop()` and `WakerTable::Kick`, both of which are atomic and
// both of which core 0 uses on the way down (AU-S3). Stopping a peer was a
// `kShutdown` ring message until then, for the single reason that
// `Scheduler::stopped_` was a plain bool; the kind is struck and the
// exception is stated here rather than left to be inferred from the call.

namespace kds::server {

class CoreRuntime {
public:
    struct Config {
        std::uint32_t core_id = 0;
        // **No `wal_dir`** (AM-S4(d)). A peer opened no log file even
        // before this stage; the directory was still handed over because
        // `RecoverCoreAtMount` needed it to find a *coordinator's* stream
        // for the cross-stream prepared resolver. That resolver is gone,
        // and with it the last thing on this core that named a path.
        sched::MonoTimeNs checkpoint_interval_ns = 0;
        sched::MonoTimeNs wal_drain_interval_ns = 0;

        // Copied from the superblock core 0 already decoded, on the startup
        // thread and before any worker exists - so this is a plain copy,
        // not a cross-core read of a structure that belongs to core 0.
        std::uint32_t inline_cell_width = storage::kDefaultInlineCellWidth;
        std::uint32_t core_count = 1;

        // **The instance's one buffer pool** (AM-S2 step 3). Non-null means
        // this core does not open a store: it borrows core 0's, which is
        // what "shared pool" means and what makes a page faulted on one core
        // serve every other. Null keeps the arrangement every build had
        // before - a frame table per core over the same device.
        //
        // **It used to be conditional on one WAL stream, and that was not a
        // detail.** The store's writeback gate is a `wal::WalDurability`,
        // which is a property of the *log*: under AR0 M0 every core's
        // manager attaches to core 0's stream, so any of them answers for
        // all. A pre-M0 volume mounted per-core, and a shared store there
        // would check a page logged in core 1's stream against core 0's
        // watermark and could write it back ahead of the record describing
        // it. AM-S4(d) made `SuperBlock::Decode` refuse such a volume, so
        // `Expeditor` passes this unconditionally now.
        storage::DevicePageStore* shared_store = nullptr;

        // This core's share of the instance frame budget
        // (`buffer_pool_frames`, docs/spec/eviction.md §6: the key is a
        // total, divided evenly per core - EV4). 0 = unbounded, the same
        // meaning SetFrameBudget gives it. Core 0's share is applied by
        // `Expeditor::Open` at store open rather than through this struct,
        // and it is the **same** `frames / cores` every peer gets: this said
        // "the even part plus the division remainder", and `FrameBudgetShare`
        // (`expeditor.cpp`) is one division, so the remainder is dropped.
        //
        // **Ignored where `shared_store` is set**, and the division goes
        // with it: one pool takes the whole `buffer_pool_frames`, which
        // `Expeditor::Start` applies to that pool where it decides to share
        // it. That is what EV4 asked for, and dividing was standing in for a
        // pool that could not be shared.
        std::size_t buffer_pool_frames = 0;

        // Settings a peer shares with core 0. Recording is *not* among
        // them - see the header on why a peer records nothing.
        wal::DurabilityClass durability = wal::DurabilityClass::kGroup;
        txn::IsolationLevel isolation = txn::IsolationLevel::kReadCommitted;
        exec::Budget budget;

        // The Cabin store's switch and caps, copied from core 0's like every
        // other shared setting (AK-S2; rule 3 above says why a peer holds a
        // store). On by default as `Expeditor::Config::cabins` is, so a
        // fixture's peer is built the way a served one is; the caps carry
        // `CabinLimits`' own defaults rather than restating them.
        bool cabins = true;
        stats::CabinLimits cabin_limits;

        // The lock family's fault net, from core 0's
        // `lock_wait_fault_net_ms`; `txn/lock_table.hpp` carries what it
        // means. **Defaulted to the constant, not to 0**: 0 is a value an
        // operator may set - refuse at once instead of waiting - so a
        // field that used it for "nobody said" would turn every
        // unconfigured peer into that policy. The dispatcher's own member
        // carries the same default for the same reason.
        sched::MonoTimeNs lock_wait_fault_net_ns = txn::kLockWaitFaultNetNs;

        // RD5's `range_size_ids`, copied from core 0 like every other
        // shared setting. One number sizes both the row-id lease grant and
        // the range; the argument for both is `server/range_alloc.hpp`'s.
        //
        // **Zero here is a transport zero, not the shipped default**, the
        // same way `in_doubt_ceiling_ns` above is: the expeditor always
        // writes this field from `Expeditor::Config`, which carries DA1's
        // `kRangeSizeIdsDefault`. A `CoreRuntime` built by hand - which is
        // every unit test - therefore gets ranges off unless it says
        // otherwise, and says so at the site rather than inheriting an
        // instance-wide decision it is not modelling.
        std::uint64_t range_size_ids = kRangeSizeOff;

        // Whether this core records access shapes at all. **The
        // instance's own `access_statistics` setting**, passed down rather
        // than re-decided here - core 0 has always read it, and a peer had
        // it forced off because it had nowhere to write. It writes
        // `sys.access_stats` itself since AT-S7, so the operator's one
        // switch means the same thing on every core.
        bool access_statistics = true;

        // **Waystone's two switches, on the same terms** (AT-S7,
        // `waystone_recording` and `waystone_replay`). A peer recorded no
        // trail and replayed none, because `sys.patterns` is a catalog page
        // and a peer could not write one; every core writes them under the
        // page latch since AT-S5, and `Catalog::ClaimPatternWaystoneRoot`
        // is what keeps two cores that build one pattern's directory from
        // keeping two. Off leaves this core exactly as it was.
        bool waystone_recording = true;
        bool waystone_replay = true;

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

        // **`anchors` - every core's, for R6-4 - is gone** (AM-S4(d)). It
        // fed one thing: the cross-stream resolver, which read a
        // *coordinator's* own stream and needed that core's anchor to bound
        // the scan honestly. With one stream a prepare's verdict is in core
        // 0's own mount pass, so there is no second stream to scan and no
        // second anchor to bound it.

        // **The volume's superblock, and it is required** — `Open` refuses
        // without it. Copied wholesale into `superblock_` below, which is
        // what closes a class of bug rather than an instance of one.
        //
        // Three fields used to live here individually — the transaction-id
        // ceiling (PW1), the log topology (AL-S1c) and, before them, the
        // WAL anchor — each added after a peer was caught answering a
        // *legal, silent, wrong zero* off a default-constructed
        // `superblock_`. The AL-S9 review found the next three already
        // live: `version`, `create_time` and `last_mount_time`, which a
        // peer's `SHOW META` printed as `0` and as the epoch under
        // `peer_listeners = on`, contradicting `crosscore.md` CC11's *every
        // core reads with the same authority*. Field-by-field was never
        // going to end, because nothing listed which fields a peer was
        // entitled to answer and nothing checked.
        //
        // So the peer gets the whole decoded image and the pointer is
        // mandatory: a `CoreRuntime` cannot be constructed without being
        // told what volume it is on. `anchor` above stays separate because
        // it is a *selection* — the fold's slot 0 — not a field the image
        // is missing.
        //
        // **A borrowed pointer, read once and not retained.** `Open` copies
        // through it in its first statement, on the startup thread, after
        // core 0 has finished raising its own ceiling and before any peer
        // worker runs — the same single-threaded discipline `anchor`
        // relies on. It is not a handle on core 0's live image and must not
        // become one: that would be shared mutable state with no
        // declaration (`docs/rules/rules.md` §3).
        const SuperBlock* superblock = nullptr;

        // **The instance's log, and it is required** (AR0 M0, AL-R1/AL-S1c;
        // AM-S4(d)). Core 0 owns them and this core attaches: it appends
        // through the shared stream's latch and asks the writer for every
        // sync, touching the device itself never. `Open` refuses a null,
        // because the arm that opened a device of this core's own left with
        // the topology that had one.
        //
        // Borrowed and outliving every peer, the way the ring transport is.
        wal::WalStream* shared_stream = nullptr;
        wal::WalWriter* shared_writer = nullptr;

        // **The instance's visibility state** (AN-R1), borrowed from the
        // expeditor on the same terms as the two above: it records commit
        // order as a commit record's LSN, which one stream is what makes
        // comparable across cores. Every instance hands one over since
        // AM-S4(d), where the per-core arm that could not have supplied one
        // went. Null is still every *fixture's* answer - a `CoreRuntime`
        // built without one keeps the per-core `ReadView`, which at AN-S1
        // is what every core is still reading anyway.
        txn::InstanceVisibility* visibility = nullptr;

        // **This core's reactor configuration** (AV-S1). `Expeditor` leaves
        // it default-constructed, which is what every core ran on before the
        // field existed; the two-core rig raises `max_idle_block_ms` so that
        // a reactor which times out instead of waking fails a cell rather
        // than passing it slowly - the same thing `scheduler_test.cpp`'s
        // wake cells do on a bare `Scheduler`, and the only thing about a
        // reactor a rig has to be able to say. The existing struct rather
        // than one field copied out of it: `rules.md`'s second-name rule,
        // and the shape an operator knob would take if one ever lands.
        sched::SchedulerConfig scheduler;

        // **The instance's lock table** (AO-S5), borrowed the way
        // `visibility` is: `Expeditor` builds one for every core and hands
        // it here, so a decide on one core flips - and kicks - a waiter
        // queued on another (AU-S2). Null builds this runtime its own at
        // `core_count == 1`, which is a fixture's shape (`core_runtime_test`'s
        // core-0 runtimes), and none above it.
        txn::LockTable* locks = nullptr;

        // **The instance's schema version word** (AT-S2), borrowed the same
        // way: `Expeditor` owns the atomic and every core's catalog reads
        // it at each task boundary. Null leaves the catalog revalidating
        // nothing, which is a fixture's shape.
        std::atomic<std::uint64_t>* schema_word = nullptr;

        // **The instance's Cabin store** (AT-S7), borrowed on the same
        // terms: `Expeditor` owns one and every core observes into it and
        // serves from it. It was this core's own until AT-S7, on the rule
        // that a relation's owner was the core every write to it landed on
        // and every read of it ran on - which AT-S5 ended for the write and
        // AT-S6 for the read, leaving each store holding the half its core
        // happened to write. Null builds this runtime its own, which is a
        // fixture's shape and the only place a per-core store remains.
        stats::CabinStore* cabins_store = nullptr;
        // **The instance's object-oid sequence** (AT-S5b), borrowed the same
        // way; null leaves this core's catalog on its own counter, which is
        // a fixture's shape and never a server's.
        std::atomic<catalog::Oid>* oid_sequence = nullptr;
        // **The instance's delete-mark counter** (AT-S5b), borrowed the same
        // way; the purge runs on one core and this is what lets that core
        // see a peer's marks.
        std::atomic<std::uint64_t>* mark_counter = nullptr;
        // **The instance's assertion registry** (AT-S5d), borrowed the same
        // way: every core's writes check and reserve into the one directory
        // each assertion has, and core 0's mount has already resumed it, so
        // a runtime handed one resumes nothing. Null builds this runtime's
        // dispatcher its own and resumes into it - a fixture's shape.
        exec::AssertionEnforcer* assertions = nullptr;
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
    // pipeline stage parked at a credit gate when the reactor stops holds
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
    // core needs - catalog invalidation, the CC7 grants, and the services
    // below. **Not a stop handler**: since AU-S3 core 0 stops this reactor
    // with `scheduler().Stop()` plus a kick, so shutdown reaches a core that
    // never attached a transport at all. `transport` must outlive this.
    Status AttachTransport(sched::RingTransport& transport);

    // PW5: binds `port` with SO_REUSEPORT and attaches the listener to
    // this core's reactor and dispatcher, on the startup thread before
    // the worker exists. STOP accepted here routes to the system core
    // (tcp_server.hpp's stop contract); the listener dies first at
    // teardown - see ~CoreRuntime.
    // `protocol` **must match every other listener on `port`**: peers
    // share one port through SO_REUSEPORT, so the kernel hands a
    // connection to whichever core it likes and a peer speaking a
    // different framing would answer a KWP client in lines. KWP by
    // default, like core 0's; the text protocol is passed explicitly by
    // the tests that exercise the newline surface.
    // `identity` mints this listener's session ids and cancel keys. It
    // **must be the same source core 0 uses**, and that is a correctness
    // statement rather than tidiness: peers share one port through
    // SO_REUSEPORT, so the kernel decides which core accepts a connection,
    // and two listeners minting from two independent counters can issue one
    // session id twice. Unset falls back to `TcpServer`'s counter, which is
    // distinct per listener and not unique across them.
    Status ListenAndAttach(std::uint16_t port, Protocol protocol = Protocol::kKwp,
                           wal::DurabilityClass durability = wal::DurabilityClass::kGroup,
                           TcpServer::IdentitySource identity = {});

    // BUG-4 ordering (the PW5 review): closes the listener - and with it
    // every accepted session, rolling back open transactions - while this
    // core's WAL can still be synced by the caller. Serve calls it for
    // every core before the final per-core Sync(), the same detach-then-
    // sync order core 0's own teardown has always had. Idempotent.
    void CloseListener() noexcept { listener_.reset(); }

    // Runs this core's reactor until another thread sets its stop flag
    // (`scheduler().Stop()`, AU-S3). This is the worker thread's whole body.
    // **The stop is sticky**: `Scheduler::Run` no longer clears the flag on
    // entry, so a stop that lands before this is called returns immediately
    // rather than being erased, and a CoreRuntime is run exactly once.
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

    // The same, for this core's transaction ids (PW1): a peer may not raise
    // the superblock's ceiling, so its windows are granted. Peers only -
    // core 0 carves its own and never leases from itself.
    void MaybeRefillTrxIds();

    // Asks this core's manager whether it should burn its unspent id block
    // to stop pinning the instance's commit-order floor (AN-R13), and turns
    // a "yes, but I have no block" into the grant request `MaybeRefillTrxIds`
    // sends. Runs on the same `system` tick, immediately before it.
    void MaybeBurnIdleTrxIdBlock();

    // And for row ids (PW1b), which differ in what triggers them: a row-id
    // lease is per *relation*, so this asks for the neediest relation the
    // lease table knows about, and the table learns of one only when a
    // statement asks it for an id (catalog/row_id_lease.hpp).
    void MaybeRefillRowIds();

    // **`MaybeFlushAccessStats` rode this tick and is gone** (AT-S7). It
    // sent CR7's folded access shapes to core 0 at `wal_drain_interval_ns`,
    // because a peer could not write `sys.access_stats`; it writes the row
    // where the statement ran now, so there is nothing to flush and no
    // cadence to pick.

    // **CC7's grant receivers went with the grants** (AW-S1b):
    // `GrantRelationFault`, `GrantRelationWrite` and the `AdmitWritePages`
    // that carried PL §9 rule 6's acquisition restamp. Each answered "may
    // this core reach that page", which a frame table shared by every core
    // does not ask.

    // This core's row-id leases and refill state (P5's shape). Exposed so a
    // test drives the grant without a reactor, and diagnostics read the
    // counters.
    catalog::RowIdLeaseTable& row_id_leases() noexcept { return row_id_leases_; }
    RowIdRefill& row_id_refill() noexcept { return row_id_refill_; }

    // This core's transaction-id lease, exposed for the first of those two
    // reasons only: a test drives a grant without a reactor.
    txn::TrxIdLease& trx_id_lease() noexcept { return trx_id_lease_; }

    std::uint32_t core_id() const noexcept { return config_.core_id; }
    sched::Scheduler& scheduler() noexcept { return *scheduler_; }

    // **How a peer stops the instance** (AU-S3). A client's `STOP` accepted
    // on this core must stop the *instance*, not this reactor - a stopped
    // peer still takes its kernel share of new connections while core 0
    // reports healthy. That used to route a `kShutdown` to the system core
    // so that core 0's own thread would flip its stop flag; the flag is
    // atomic now, so this is a direct call plus a kick.
    //
    // One callback rather than a pair of pointers to core 0's scheduler and
    // to the waker table: the peer has no business knowing either, and what
    // it actually needs is the single verb "stop the instance".
    void set_instance_stop(std::function<void()> stop) { instance_stop_ = std::move(stop); }
    wal::WalManager& wal() noexcept { return *wal_; }
    catalog::Catalog& catalog() noexcept { return *catalog_; }
    // This core's transaction-id sequence, exposed for one caller: a rig
    // whose core 0 is a `CoreRuntime` registers production's own grant
    // handler over it (`trx_id_lease_service.hpp`), which `Expeditor` does
    // over its own sequence. Two sequences over one superblock would issue
    // one id twice, so a rig must carve a peer's block from this one.
    txn::TrxIdSequence& trx_ids() noexcept { return *trx_ids_; }
    CommandDispatcher& dispatcher() noexcept { return *dispatcher_; }
    // **The instance's Cabin store** since AT-S7, or this runtime's own
    // where nobody handed it one (a fixture), or null under `cabins = off`.
    stats::CabinStore* cabins() noexcept {
        if (cabins_ != nullptr) return cabins_;
        return cabin_store_ ? &*cabin_store_ : nullptr;
    }

    storage::DevicePageStore& store() noexcept { return *store_; }

    // What this core's mount did (RV1/RV2 at Open, RC08's completion
    // checkpoint at AttachTransport) - `Expeditor::recovery()`'s counterpart,
    // and what this core's `SHOW META` recovery block reads (PW3b: kept
    // rather than discarded, so a peer's stop can be checked to have bounded
    // its next mount by the same field core 0's is).
    const MountRecovery& recovery() const noexcept { return recovery_; }

private:
    CoreRuntime(Config config, Logger* log) noexcept
        : config_(config), log_(log) {}

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
    // **Declared above the scheduler, so it outlives every frame that
    // releases into it** - `expeditor.hpp`'s rule for its own table. Since
    // AT-S1 a parked producer's or consumer's borrow (`read_borrow.hpp`)
    // releases in its destructor, which runs when the scheduler drops the
    // frame at teardown; with the table declared below the scheduler that
    // release would reach a freed table. Unreachable today - an owned table
    // exists only at one core, where no transport and so no producer exists
    // - and the order is what keeps it that way rather than a coincidence.
    // The pointer is the table this core uses, owned or borrowed (AO-S5).
    std::unique_ptr<txn::LockTable> owned_locks_;
    txn::LockTable* locks_ = nullptr;

    std::unique_ptr<sched::IoBackend> io_backend_;
    std::optional<sched::Scheduler> scheduler_;
    // **No `log_device_`** (AM-S4(d)): the device is core 0's and this core
    // reaches it only through the borrowed stream. The member was always
    // null under one stream and only ever written by the per-core arm.
    std::unique_ptr<wal::WalManager> wal_;

    // AU-S3. Empty on a single-core instance and in every fixture that
    // wires a listener with no instance around it, where a peer's STOP has
    // nothing to route to and stopping this reactor is the whole instance.
    std::function<void()> instance_stop_;

    // **Owned, or borrowed** (AM-S2 step 3). `owned_store_` is empty where
    // `Config::shared_store` named the instance's pool; `store_` points at
    // whichever it is and is what every member and every caller uses, so
    // nothing below this line knows the difference. The destruction order
    // matters and is why these are a pair rather than a `variant`: a
    // borrowed store outlives this object and must not be destroyed here,
    // while an owned one is destroyed with everything else that reaches
    // back into it (see the destructor's note on reverse order).
    std::unique_ptr<storage::DevicePageStore> owned_store_;
    storage::DevicePageStore* store_ = nullptr;

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

    // Set by the burn check when this core should burn its unspent id block
    // and has no granted one parked to install (AN-R13). Makes
    // `MaybeRefillTrxIds` ask despite a full window, which is the one thing
    // that inverts the parking rule `low_water()` states.
    bool burn_requested_ = false;
    // One row-id refill in flight at a time, for the same reason - and it is
    // per core rather than per relation, so a second needy relation waits one
    // tick rather than racing the first.
    bool row_id_refill_in_flight_ = false;

    // This core's trail recorder, built where `waystone_recording` is on
    // (AT-S7 - it was core 0's alone). Declared ahead of the dispatcher,
    // which holds a pointer to it.
    std::optional<stats::TrailRecorder> trail_recorder_;
    // **The instance's store, borrowed** (AT-S7), and null wherever
    // `Config::cabins` was: then `cabin_store_` below is this runtime's
    // own, which only a fixture builds now.
    stats::CabinStore* cabins_ = nullptr;
    // Declared ahead of every borrower - the step server just below, the
    // dispatcher and the probe server further down - so reverse
    // destruction ends them before the store they point into.
    std::optional<stats::CabinStore> cabin_store_;

    // The remote step server (P4b), armed at AttachTransport: this core
    // answers STEP_OPENs for relations it owns.
    std::optional<RemoteStepServer> remote_steps_;

    // **And the client half** (R4-R/RR2), armed beside it: before it every
    // core could *serve* a fan-in stage and only core 0 could *open* one,
    // so which reads a session could answer depended on which core
    // `SO_REUSEPORT` had accepted it on. Declared above `dispatcher_`,
    // which borrows it.
    std::optional<SessionStepClient> remote_reads_;

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
    // **The lock family's table** (AO-R2): the instance's, borrowed through
    // `Config::locks` (AO-S5), or this runtime's own at `core_count == 1`
    // when none was handed over - `owned_store_`/`store_`'s shape, and a
    // fixture's case. The manager takes it on every core, so a decide here
    // releases its borrows into the one table and wakes - and kicks - a
    // waiter queued from any core (AU-S2).
    //
    // **The dispatcher takes it on every core since AO-S4b.** Its use of
    // the table is the wait-for graph and the admission it gates: with a
    // table, a transaction holding rows may wait because the detector
    // catches the cycle it could close (AO-S4a). Across cores that was not
    // complete at AO-S5(a) - a cycle could pass through a wait that
    // registered no edge, the shipped-statement park - so the dispatcher
    // kept AO-S3's narrow rule above one core. AO-S4b records that edge
    // where it can be recorded, on the owner at enrolment
    // (`docs/spec/cross-owner-txn.md, retired`), and lifts the rule. The FK probe
    // park registers none and waits on nothing yet; AO-S5(b) owes both.
    std::optional<txn::TransactionManager> txn_manager_;
    std::optional<CommandDispatcher> dispatcher_;

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

}  // namespace kds::server
