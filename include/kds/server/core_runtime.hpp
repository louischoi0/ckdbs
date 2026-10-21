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
#include "kds/catalog/instance_words.hpp"
#include "kds/exec/budget.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/txn/lock_table.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/stats/trail_recorder.hpp"
#include "kds/server/tcp_server.hpp"
#include "kds/server/mount_recovery.hpp"
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
// A peer has a full statement stack: its own `Catalog`, transaction manager
// and `CommandDispatcher` over the instance's one `DevicePageStore`, which
// it borrows from core 0 (AM-S2 step 3; `Config::shared_store`). It
// resolves a relation and runs a statement exactly as core 0 does.
//
// **No asymmetry against core 0 is left in what a statement may do**:
// every core writes the catalog's pages under the page latch, the DDL's
// relation `X` and the schema word (`catalog.md` CT5); every core reaches
// the one free map under its structure latch; and every core records and
// replays trails, writes `sys.access_stats` itself under the relation's
// root page latch, and observes into the instance's one
// `stats::CabinStore` (`cabin.md` §6's announce).
// The read-only peer catalog, the per-core extent lease and the per-core
// Cabin store retired (AT-S5, AW-S1b, AT-S7); their history is at cf889b63.
//
// The superblock, the free map, the catalog pages and the store are the
// instance's, not core 0's: every core reads and writes them under their
// latches. The log is core 0's to open and drain, and every core appends
// to it under the stream's latch (`Config::shared_stream`). They live on
// `Expeditor` rather than here because they are the *database*, not a
// core's copy of anything. Every core listens on
// the port since AT-S8; only D19's fallback has core 0 accept alone and
// hand each connection off (`tcp_server.hpp`).
//
// ---- Threading -----------------------------------------------------------
//
// A CoreRuntime is created on the startup thread and then handed to exactly
// one worker, which owns it for the rest of its life. Nothing in it is
// synchronized (rules.md #3), and nothing outside that worker may touch it
// once `Run()` has begun - **with one exception, and only one**:
// `scheduler().Stop()` and `WakerTable::Kick`, both of which are atomic and
// both of which core 0 uses on the way down (AU-S3). The exception is
// stated here rather than left to be inferred from the call.

namespace kds::server {

class CoreRuntime {
public:
    struct Config {
        std::uint32_t core_id = 0;
        // No `wal_dir`: the cross-stream prepared resolver that needed one
        // retired (AM-S4(d)); its history is at cf889b63.
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
        // serve every other. Null opens a frame table of this runtime's own
        // over the same device - a fixture's shape.
        //
        // The store's writeback gate is a `wal::WalDurability`, which is a
        // property of the *log*: every core's manager attaches to core 0's
        // stream, so any of them answers for all, and `SuperBlock::Decode`
        // refuses a volume with more than one stream (AM-S4(d)).
        storage::DevicePageStore* shared_store = nullptr;

        // The capacity of a store this runtime opens for itself - a
        // fixture's, where `shared_store` is null - and required there: 0 is
        // refused at `Open`, as everywhere (BE-R3). **Ignored where
        // `shared_store` is set**, which is every production core: the one
        // pool was opened at `buffer_pool_frames` by `Expeditor::Open`.
        std::size_t buffer_pool_frames = 0;

        // Settings a peer shares with core 0.
        wal::DurabilityClass durability = wal::DurabilityClass::kGroup;
        txn::IsolationLevel isolation = txn::IsolationLevel::kReadCommitted;
        exec::Budget budget;

        // The Cabin store's switch and caps, copied from core 0's like every
        // other shared setting (AK-S2). Read only where `cabins_store` below
        // is null - a fixture - since the store is the instance's (AT-S7).
        // On by default as `Expeditor::Config::cabins` is, so a fixture's
        // peer is built the way a served one is; the caps carry
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

        // **The statement limits core 0's dispatcher is given** (AT-S8),
        // copied like every other shared setting. A session runs on the
        // core that accepted it since every core listens, and a peer's
        // dispatcher with its own defaults would answer differently from
        // core 0 under the same config file. Each default is the
        // dispatcher's own, so a fixture's peer is built as an unconfigured
        // instance's is.
        StatementLimits statement_limits;

        // **The instance's optimizer surface** (AT-S8, `OptimizerSurface`):
        // the relayout mode, the one collector, the one `CABIN_OPTIMIZER`
        // switch and the controller view. Default-constructed, the
        // dispatcher keeps its own - a fixture's shape.
        OptimizerSurface optimizer;

        // Whether this core records access shapes at all. **The
        // instance's own `access_statistics` setting**, passed down rather
        // than re-decided here. Every core writes `sys.access_stats` itself
        // (AT-S7), so the operator's one switch means the same thing on
        // every core.
        bool access_statistics = true;

        // **Waystone's two switches, on the same terms** (AT-S7,
        // `waystone_recording` and `waystone_replay`): the instance's
        // ceilings over the patterns `WAYSTONE ON` opted in (CF-S1b). No
        // core builds a directory at run time - the opt-in and the mount do
        // - so every core records into the one root a row names. Off leaves
        // this core exactly as it was.
        bool waystone_recording = true;
        bool waystone_replay = true;

        // The stream's WAL anchor (slot 0), copied out of core 0's superblock
        // on the startup thread. `Open` reads only its `redo_start_lsn`, as
        // the start of the assertion resume's scan on a runtime that owns
        // its registry (a fixture). A peer's checkpointer publishes into the
        // instance's anchor (`checkpoint_anchor` below), not into this copy.
        // A zeroed anchor is legal ("no checkpoint yet, scan from the head").
        //
        // Value-initialized, and that brace is load-bearing:
        // `WalAnchorFields` is a plain on-disk layout with no member
        // initializers (superblock.hpp), so a caller that left this field
        // alone would hand the scan whatever was on the stack.
        WalAnchorFields anchor{};

        // **The volume's superblock, and it is required** — `Open` refuses
        // without it. Copied wholesale into `superblock_` below, which is
        // what closes a class of bug rather than an instance of one.
        //
        // Background (PW1, AL-S1c, AL-S9): fields copied one at a time
        // kept leaving a peer answering a legal, silent, wrong zero off a
        // default-constructed `superblock_` (`SHOW META`'s `version`,
        // `create_time`, `last_mount_time`), against `crosscore.md` CC11.
        // So the peer gets the whole decoded image and the pointer is
        // mandatory. `anchor` above stays separate because it is a
        // *selection* - the fold's slot 0 - not a field the image lacks.
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
        // sync, touching the device itself never. `Open` refuses a null.
        //
        // Borrowed and outliving every peer.
        wal::WalStream* shared_stream = nullptr;
        wal::WalWriter* shared_writer = nullptr;

        // **The instance's visibility state** (AN-R1), borrowed from the
        // expeditor on the same terms as the two above: it records commit
        // order as a commit record's LSN, which one stream is what makes
        // comparable across cores. Every instance hands one over. Null is
        // every *fixture's* answer - a `CoreRuntime` built without one keeps
        // the per-core `ReadView`.
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

        // **The instance's four words** (`catalog.md` CT6, one struct since
        // CF-S1c), borrowed the same way: `Expeditor` owns them, and this
        // core's catalog takes the struct (`Catalog::SetInstanceWords`) and
        // its transaction manager the schema word. Null leaves the catalog
        // on its local copies (`Catalog::SetInstanceWords`), which is a
        // fixture's shape and never a server's.
        catalog::InstanceWords* instance_words = nullptr;

        // **The instance's Cabin store** (AT-S7), borrowed on the same
        // terms: `Expeditor` owns one and every core observes into it and
        // serves from it. Null builds this runtime its own, which is a
        // fixture's shape and the only place a per-core store remains.
        stats::CabinStore* cabins_store = nullptr;
        // **The instance's assertion registry** (AT-S5d), borrowed the same
        // way: every core's writes check and reserve into the one directory
        // each assertion has, and core 0's mount has already resumed it, so
        // a runtime handed one resumes nothing. Null builds this runtime's
        // dispatcher its own and resumes into it - a fixture's shape.
        exec::AssertionEnforcer* assertions = nullptr;

        // **The instance's checkpoint anchor and its run** (AT-S8), borrowed
        // from `Expeditor`. A runtime handed an anchor builds a checkpointer
        // at `Open`, runs the completion checkpoint there, and publishes
        // every later one into this anchor directly - the fold and page 0
        // are guarded by the superblock latch the anchor carries. Null
        // builds no checkpointer: a fixture, and a core-0 runtime, whose
        // checkpointer is `Expeditor`'s. The gate may be null where the
        // anchor is not, which runs ungated (`wal::CheckpointGate`).
        wal::CheckpointAnchor* checkpoint_anchor = nullptr;
        wal::CheckpointGate* checkpoint_gate = nullptr;

        // **The instance's transaction-id ceiling** (AT-S10b, AT-R4),
        // borrowed from `Expeditor`: the one `SuperBlock` every core's
        // sequence carves its window from, the superblock latch the carve
        // reads and raises it under, and what makes a raised ceiling
        // durable. Every core's `TrxIdSequence` is built over these three,
        // so two cores carving at once get disjoint blocks. **Required**,
        // as `superblock` is: `Open` refuses a null one. A null `persist`
        // raises the ceiling in memory only, a fixture's arrangement.
        struct TrxIdCeiling {
            SuperBlock* superblock = nullptr;
            Latch* latch = nullptr;
            std::function<Status()> persist;
        };
        TrxIdCeiling trx_id_ceiling;
    };

    // Attaches this core to the instance's WAL stream and page store
    // (`Config::shared_stream`, `Config::shared_store`), builds its catalog,
    // transaction stack and dispatcher, and builds its reactor. `device` is
    // the shared page device and must outlive this runtime; a store is
    // opened over it only where `shared_store` is null (a fixture).
    static StatusOr<std::unique_ptr<CoreRuntime>> Open(Config config,
                                                       storage::PageDevice& device,
                                                       const sched::Clock& clock, Logger* log);

    CoreRuntime(const CoreRuntime&) = delete;
    CoreRuntime& operator=(const CoreRuntime&) = delete;

    // **The reactor is left to declaration order** (`core_runtime.cpp`
    // says why its early reset went): `scheduler_` is declared above every
    // member that borrows it, so it is destroyed after them - and the
    // scheduler owns coroutine frames (`sched::CoroTask` destroys a
    // suspended one), so a frame it destroys at teardown may reach only
    // members declared above it, which is why `owned_locks_` is.
    //
    // One member is dropped by hand *before* the scheduler in the body:
    // `listener_` (PW5), whose `~TcpServer` unregisters its fds from the
    // reactor.
    ~CoreRuntime();

    // PW5, and the arrangement since AT-S8: binds `port` with SO_REUSEPORT
    // and attaches the listener to this core's reactor and dispatcher, on
    // the startup thread before the worker exists. STOP accepted here
    // routes to the system core (tcp_server.hpp's stop contract); the
    // listener dies first at teardown - see ~CoreRuntime.
    //
    // `setup` **must be the one every other listener on `port` has**
    // (`TcpServer::ClientSetup`): the kernel hands a connection to whichever
    // core it likes, so a peer speaking another framing would answer a KWP
    // client in lines, a peer without the TLS channel or the SCRAM gate would
    // serve the secured port in plaintext and unauthenticated, and two
    // identity sources minting from independent counters can issue one
    // session id twice. The text protocol is passed explicitly by the tests
    // that exercise the newline surface.
    Status ListenAndAttach(std::uint16_t port, const TcpServer::ClientSetup& setup = {},
                           const std::string& address = kLoopbackAddress);

    // **D19's fallback** (AT-S10c, `connection_handoff.hpp`): where the port
    // cannot be shared, core 0 alone binds it and this core runs the
    // connections core 0 hands it - a listener with no socket, attached and
    // stop-routed exactly as `ListenAndAttach`'s, whose reactor takes each
    // socket from its inbox when kicked. `setup` is that function's, for its
    // reasons; `handoff` must outlive this runtime.
    Status HostHandedConnections(ConnectionHandoff& handoff,
                                 const TcpServer::ClientSetup& setup = {});

    // **BL-S2**: this core's listener places each transaction by `placer`,
    // moving a connection through `handoff`, and hosts the inbox the
    // connections other cores move to it arrive in. After one of the two
    // above; both must outlive this runtime.
    Status AcceptMoves(ConnectionHandoff& handoff, TransactionPlacer* placer);

    // BUG-4 ordering (the PW5 review): closes the listener - and with it
    // every accepted session, rolling back open transactions - while this
    // core's WAL can still be synced by the caller. Serve calls it for
    // every core before the final per-core Sync(), the same detach-then-
    // sync order core 0's own teardown has always had. Idempotent.
    void CloseListener() noexcept { listener_.reset(); }
    // Whether this core holds a client listener - every peer an instance
    // opens does since AT-S8.
    bool listening() const noexcept { return listener_.has_value(); }

    // Runs this core's reactor until another thread sets its stop flag
    // (`scheduler().Stop()`, AU-S3). This is the worker thread's whole body.
    // **The stop is sticky**: `Scheduler::Run` no longer clears the flag on
    // entry, so a stop that lands before this is called returns immediately
    // rather than being erased, and a CoreRuntime is run exactly once.
    void Run();

    // Drains and syncs this core's log. Called on the way down, after Run()
    // returns, so an acknowledged commit on this core survives the stop.
    Status Sync();

    // Runs one checkpoint to completion and publishes its anchor into the
    // instance's (`Config::checkpoint_anchor`), under the instance's gate: a
    // call that finds another core's checkpoint running skips and returns OK
    // (AT-S8, `wal::CheckpointGate`). A no-op on a core with no checkpointer
    // - core 0's is `Expeditor`'s, and a runtime handed no anchor has
    // nowhere to publish to.
    //
    // Public because the cadence below calls it and a test drives it
    // without a reactor.
    Status Checkpoint();

    // **The shutdown checkpoint** (PW3b) - the third of core 0's three
    // checkpoint points, which a peer once lacked: a graceful restart
    // replayed up to one `checkpoint_interval_ms` of every peer's work.
    // Flushes this core's pages, then runs `Checkpoint()`.
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
    Status ShutdownCheckpoint();

    // Abandons this core's cadence run, if one is queued or parked
    // (`wal::Checkpointer::Abandon`, BI-R4). The shutdown path calls it for
    // every core before the first shutdown checkpoint, with this core's
    // reactor joined.
    void AbandonCheckpoint() noexcept;

    // The access-stats flush (AT-S7) and CC7's grant receivers (AW-S1b)
    // retired; their history is at cf889b63.

    std::uint32_t core_id() const noexcept { return config_.core_id; }
    // What this core was opened with. Immutable after `Open`, so readable
    // from any thread - a test's view of what `Expeditor` handed a peer.
    const Config& config() const noexcept { return config_; }
    sched::Scheduler& scheduler() noexcept { return *scheduler_; }

    // **How a peer stops the instance** (AU-S3). A client's `STOP` accepted
    // on this core must stop the *instance*, not this reactor - a stopped
    // peer still takes its kernel share of new connections while core 0
    // reports healthy. The stop flag is atomic, so this is a direct call
    // plus a kick.
    //
    // One callback rather than a pair of pointers to core 0's scheduler and
    // to the waker table: the peer has no business knowing either, and what
    // it actually needs is the single verb "stop the instance".
    void set_instance_stop(std::function<void()> stop) { instance_stop_ = std::move(stop); }
    wal::WalManager& wal() noexcept { return *wal_; }
    catalog::Catalog& catalog() noexcept { return *catalog_; }
    // This core's transaction-id sequence: its own window over the
    // ceiling it was handed (`Config::trx_id_ceiling`), exposed for tests.
    txn::TrxIdSequence& trx_ids() noexcept { return *trx_ids_; }
    // This core's transaction manager, exposed for tests: a cell asks one
    // core what it answers about another core's transaction (AX-S1).
    txn::TransactionManager& transactions() noexcept { return *txn_manager_; }
    CommandDispatcher& dispatcher() noexcept { return *dispatcher_; }
    // **The instance's Cabin store** since AT-S7, or this runtime's own
    // where nobody handed it one (a fixture), or null under `cabins = off`.
    stats::CabinStore* cabins() noexcept {
        if (cabins_ != nullptr) return cabins_;
        return cabin_store_ ? &*cabin_store_ : nullptr;
    }

    storage::DevicePageStore& store() noexcept { return *store_; }

    // What this core's mount did (RV1/RV2 and RC08's completion
    // checkpoint, both at Open) - `Expeditor::recovery()`'s counterpart,
    // and what this core's `SHOW META` recovery block reads (PW3b: kept
    // rather than discarded, so a peer's stop can be checked to have bounded
    // its next mount by the same field core 0's is).
    const MountRecovery& recovery() const noexcept { return recovery_; }

private:
    CoreRuntime(Config config, Logger* log) noexcept
        : config_(config), log_(log) {}

    // What `ListenAndAttach` and `HostHandedConnections` share: `server`
    // becomes this core's listener, configured, attached and stop-routed.
    Status AttachListener(TcpServer server, const TcpServer::ClientSetup& setup);

    Config config_;
    Logger* log_ = nullptr;
    // Filled at Open, read by the dispatcher below for the rest of this
    // core's life - so it is declared above everything that borrows it.
    MountRecovery recovery_;

    // Declared in construction order and torn down in reverse, the same
    // discipline Expeditor's members follow: the reactor holds the io
    // backend, the WAL manager holds the log device, and the dispatcher
    // holds references into everything below it.
    // **Declared above the scheduler, so it outlives every frame that
    // releases into it** - `expeditor.hpp`'s rule for its own table: a
    // parked frame's borrow (`read_borrow.hpp`) releases in its destructor,
    // which runs when the scheduler drops the frame at teardown.
    // The pointer is the table this core uses, owned or borrowed (AO-S5).
    std::unique_ptr<txn::LockTable> owned_locks_;
    txn::LockTable* locks_ = nullptr;

    std::unique_ptr<sched::IoBackend> io_backend_;
    std::optional<sched::Scheduler> scheduler_;
    // **No `log_device_`** (AM-S4(d)): the device is core 0's and this core
    // reaches it only through the borrowed stream.
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

    // This core's trail recorder, built where `waystone_recording` is on
    // (AT-S7 - it was core 0's alone). Declared ahead of the dispatcher,
    // which holds a pointer to it.
    std::optional<stats::TrailRecorder> trail_recorder_;
    // **The instance's store, borrowed** (AT-S7), and null wherever
    // `Config::cabins` was: then `cabin_store_` below is this runtime's
    // own, which only a fixture builds now.
    stats::CabinStore* cabins_ = nullptr;
    // Declared ahead of its borrower, the dispatcher further down, so
    // reverse destruction ends it before the store it points into.
    std::optional<stats::CabinStore> cabin_store_;

    // The target this core's checkpointer flushes through (PW3), built at
    // `Open` when the runtime is handed an anchor. Declared below `store_`,
    // which it holds a reference to.
    std::optional<storage::PageStoreCheckpointTarget> checkpoint_target_;

    // The statement stack. A peer's `SuperBlock` is a **copy** taken on the
    // startup thread: the dispatcher needs one for SHOW-class commands, and
    // the live instance belongs to core 0. Nothing here reaches the page.
    //
    // No transaction id is carved from it: `trx_ids_` below carves from the
    // instance's image, handed in `Config::trx_id_ceiling` (AT-S10b).
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
    // catches the cycle it could close (AO-S4a). The edgeless parks, the
    // shipped statement's and the FK probe's, retired (AT-S6, AT-S5f);
    // their history is at cf889b63.
    std::optional<txn::TransactionManager> txn_manager_;
    std::optional<CommandDispatcher> dispatcher_;

    // The client listener this core accepts on (PW5, AT-S8). It borrows
    // the scheduler and the dispatcher, and
    // `~TcpServer` calls back into the scheduler to unregister its fds - so
    // it is dropped explicitly at the top of `~CoreRuntime`, before the
    // body discards the scheduler's queued tasks, as core 0 detaches its
    // listener first.
    std::optional<TcpServer> listener_;

    // Last, because it borrows every one of them: the WAL, the target and
    // anchor above, `txn_manager_`, and the dispatcher's assertion
    // enforcer. Reverse-order destruction therefore takes it first.
    std::optional<wal::Checkpointer> checkpointer_;
};

}  // namespace kds::server
