#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/ring_transport.hpp"
#include "kds/server/config_file.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/core_runtime.hpp"
#include "kds/exec/budget.hpp"
#include "kds/exec/cabin_optimizer_exec.hpp"
#include "kds/stats/cabin_optimizer.hpp"
#include "kds/stats/optimizer_signals.hpp"
#include "kds/server/extent_lease_service.hpp"
#include "kds/server/assertion_build_service.hpp"
#include "kds/server/index_build_service.hpp"
#include "kds/server/mount_recovery.hpp"
#include "kds/server/row_id_lease_service.hpp"
#include "kds/server/trx_id_lease_service.hpp"
#include "kds/server/stop_signal.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/server/superblock_checkpoint_anchor.hpp"
#include "kds/server/kwp_load_server.hpp"
#include "kds/server/tcp_server.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/page_device.hpp"
#include "kds/storage/page_store_checkpoint_target.hpp"
#include "kds/wal/checkpointer.hpp"
#include "kds/wal/file_log_device.hpp"
#include "kds/wal/manager.hpp"

// The Expeditor: the KDS super process. One instance owns every subsystem
// of a running database - the page device, the page store above it, the
// superblock and catalog produced by bootstrap, the command dispatcher,
// and the listener - and owns their lifetime and start/stop order:
//
//   device -> store -> bootstrap (superblock + catalog) -> dispatcher
//   -> listener,  torn down in reverse, with a final Sync().
//
// Everything above holds references down the stack (Catalog into the
// store, CommandDispatcher into all three), so an Expeditor is pinned:
// non-copyable, non-movable, handed out as a unique_ptr by Open(). Moving
// one would leave those references pointing at a corpse.
//
// main() is the platform layer around this: it reads argv and the clock
// and prints, and does nothing else (rules.md #4).
//
// ---- The reactor, and why durability stopped depending on STOP ----------
//
// Serve() runs a `sched::Scheduler` (sched.md section 2), not a loop of its
// own. TcpServer attaches its sockets to it, and a `system`-group timer
// fires the checkpointer every `checkpoint_interval_ns`. Before this, the
// process sat blocked in accept()/read() for its whole life, so the only
// moments anything reached the platter were an explicit SYNC and the final
// Sync() after STOP - a server nobody typed SYNC into held every mutation
// in memory indefinitely.
//
// The checkpoint is what closes that: it flushes the store's dirty pages
// on a cadence (wal.md section 11-2) through PageStoreCheckpointTarget.
//
// ---- What the WAL covers, and what it does not --------------------------
//
// Open() installs the store's WAL gate and hands the dispatcher a
// WalManager, so INSERT is logged and ordered: its records are durable to
// the configured class before the reply, and no dirty page reaches the
// device ahead of them. A second `system`-group timer drains the log,
// which is what bounds a relaxed commit's loss window.
//
// Two gaps remain, and neither is small. **Recovery does not exist**
// (wal.md section 12): nothing reads the stream back, so a logged insert
// is recoverable in principle and not in practice - what protects a
// restart today is still the flush. And **INSERT is the only logged
// statement**; CREATE TABLE, UPDATE and the catalog rows under both still
// mutate pages outside the log, so for them the checkpoint remains what
// it was - a bound on the loss window, not a crash guarantee.

namespace kds::server {

// Refuses a nonzero instance frame budget that would give some core a
// share of zero - the message says why. Zero total stays legal and means
// unbounded by request. A free function beside the config for the same
// reason CheckCoreCount is one: testable without a server.
Status CheckFrameBudget(std::size_t frames, std::uint32_t cores);

// Refuses every `peer_listeners = on` pairing that cannot work: with TLS
// or SCRAM auth (both are built on core 0's stack; sharing them immutably
// is PW5's open half - refused truthfully rather than served insecurely),
// with `cores = 1` (no peer to listen, and SO_REUSEPORT on the only
// socket loses the exclusive bind), and with creating-core placement
// (every relation is core 0's, so a peer session could serve nothing). A
// free function for the same reason its two siblings above are.
Status CheckPeerListenerConfig(bool peer_listeners, bool tls, bool auth_scram,
                               std::uint32_t cores, catalog::PlacementPolicy placement);

// EV4 (docs/spec/eviction.md §6), under the operator invariant of
// 2026-08-24: the key is an instance total and every core's share is
// equal - `frames / min(cores, hardware cores)` as ratified, which is
// `frames / cores` on every instance that exists, because Expeditor::Open
// refuses to boot when `cores` exceeds the detectable hardware count and
// falls back to `cores` when the count is undetectable. The min() is
// therefore not carried as a parameter - its hardware arm can never be
// selected, and if it ever were, share * cores would exceed the total,
// the exact overcommit the invariant forbids. The remainder (at most
// cores-1 frames, ~504 KiB at the 64-core cap) is undistributed -
// equality is the invariant, and a remainder seat would break it. Never
// 0 for a nonzero total that passed CheckFrameBudget; `cores` must be
// nonzero (CheckCoreCount owns that).
std::size_t FrameBudgetShare(std::size_t frames, std::uint32_t cores) noexcept;

class Expeditor {
public:
    struct Config {
        std::string data_file = "kds.db";
        std::uint16_t port = 15432;

        // Resident-frame budget for the **whole instance**, divided evenly
        // per core with the remainder to core 0 (docs/spec/eviction.md §6
        // EV4 - built 2026-08-24; docs/workplan-pageref.md MG06); 0 =
        // unbounded. The sweep arms only on a core whose share is nonzero,
        // and a nonzero total below `cores` is refused at boot
        // (CheckFrameBudget) rather than rounded to an unbounded share.
        std::size_t buffer_pool_frames = 0;

        // Direct TLS on the text port (docs/spec/protocol.md §1, decided
        // 2026-08-13): with `tls = on` the first byte every client sends
        // is a ClientHello, and there is no plaintext fallback and no
        // STARTTLS-style upgrade on the same port. Off by default because
        // the port is loopback-only today; TLS is the precondition for
        // that ever changing, not a decoration on loopback.
        //
        // Both paths must be set when `tls` is (ApplyFile refuses
        // otherwise), must be PEM, and are read once at Serve(). On a
        // server built without TLS (KDS_WITH_TLS=OFF), `tls = on` is
        // refused with Unsupported naming the build flag - a config
        // written for a capability this binary does not have fails
        // loudly, the physical_optimizer = on precedent.
        // Per-core listeners (workplan-peer-writer.md PW5; the operator
        // story is in kds.conf.sample, the socket mechanics in
        // tcp_server.hpp). Off by default; the workable pairings are
        // CheckPeerListenerConfig's to police.
        bool peer_listeners = false;

        bool tls = false;
        std::string tls_cert_file;
        std::string tls_key_file;

        // Connection authentication (docs/spec/protocol.md D8's auth stage,
        // on the text protocol until KWP P07). `auth = off` (default)
        // admits every connection, today's behaviour; `auth = scram`
        // requires every connection to complete a SCRAM-SHA-256
        // exchange before its first statement - STOP included.
        //
        // `users_file` is required when auth is on: the flat verifier
        // store (auth.hpp), loaded whole at startup, provisioned with
        // `kds_server --add-user`. Same build story as TLS: a server
        // built with KDS_WITH_TLS=OFF refuses `auth = scram` at startup
        // naming the flag.
        //
        // Independent of `tls` on purpose - SCRAM never puts the
        // password on the wire, so auth-without-TLS is coherent on
        // loopback - but a non-loopback future requires both, and the
        // authorization model (who may do what once in) remains an Open
        // Decision this key does not touch.
        bool auth_scram = false;
        std::string users_file;

        // Directory the per-core WAL segment files live in. Defaults to
        // `<data_file>.wal` when left empty.
        std::string wal_dir;

        // How often the `system`-group checkpoint task runs. Checkpoint
        // cadence is the RTO knob and an [OPEN] decision (wal.md sections
        // 11 and 13), so this is a parameter with a default, never a
        // constant anything may depend on. 0 disables the timer entirely,
        // which puts durability back on SYNC and STOP alone.
        sched::MonoTimeNs checkpoint_interval_ns = 5'000'000'000;  // 5 s

        // Durability class applied to every logged statement (wal.md
        // section 1). Per-transaction in the design and per-server here,
        // because the newline protocol has nowhere to carry it - KWP/1
        // makes it a protocol field (protocol.md), and this key retires
        // into that default when it does.
        //
        // kGroup is the default because it is the one that is both durable
        // and able to amortize: with one connection it costs the same sync
        // per commit as kStrict, and it starts paying the moment there is
        // more than one committer. kRelaxed trades a bounded loss window
        // for not waiting at all.
        wal::DurabilityClass durability = wal::DurabilityClass::kGroup;

        // Ceiling on the tuples one statement may decode before it is
        // refused with ResourceExhausted (exec/budget.hpp). 0 is
        // unlimited.
        //
        // It exists because nothing suspends mid-statement on a
        // cooperative core: an unbounded statement holds that core against
        // every other client on it, so a bounded failure is the kinder
        // answer than a connection that never replies.
        std::uint64_t max_rows_touched = exec::kDefaultRowTouchBudget;

        // BI3's per-statement row cap for a multi-row INSERT
        // (docs/spec/bulkinsert.md, `max_insert_rows`). A refusal, never a
        // truncation; must be at least 1.
        std::uint64_t max_insert_rows = parser::kDefaultMaxInsertRows;

        // KWP v0's load endpoint (`kwp_port`, docs/inflight/in-progress/workplan-kwp-load.md).
        // 0 - the default - opens no socket at all: the endpoint exists
        // only when asked for. Loopback only, like the text port.
        std::uint16_t kwp_port = 0;

        // ---- `isolation` (docs/spec/txn.md section 1) -----------------------
        //
        // The level a session starts at, and therefore the level an
        // autocommit statement runs at. **READ COMMITTED is the default**,
        // and the reason is specific to this engine rather than convention:
        // under first-updater-wins with no waiting, REPEATABLE READ holds
        // one read view for a whole transaction and so converts more
        // concurrent writes into retryable aborts. This is the server rung
        // of the same three-level chain `durability` uses - server, then
        // `SET ISOLATION LEVEL` per session, then `BEGIN ISOLATION LEVEL`
        // per transaction.
        txn::IsolationLevel isolation = txn::IsolationLevel::kReadCommitted;

        // There is no `default_key_mode` field. The key is still recognized
        // by the loader, and refused there naming the removal rather than
        // falling to the unknown-key error - it decided what a `CREATE
        // TABLE` naming no key-mode word meant, and there is no longer a
        // mode to decide (docs/spec/heap-and-tuple.md §4.1).

        // Whether a successful SELECT records a Waystone trail
        // (`waystone_recording`, default on).
        //
        // Off is a valid production setting, not a debug switch: recording
        // costs a page write per newly-hot instance and buys nothing until
        // replay exists (workplan P11). It is also the switch that makes
        // "results are identical either way" a thing an operator can check
        // on their own data rather than take on trust.
        bool waystone_recording = true;

        // Whether a SELECT may be served from a trail a previous execution
        // recorded (`waystone_replay`, default on). This is the half that
        // *repays* recording - and the first Waystone code that can be
        // asked to produce a row, which is why the advisory-contract suite
        // compares it against every other configuration byte for byte.
        bool waystone_replay = true;

        // Whether a successful SELECT records its access shapes into
        // `sys.access_stats` (`access_statistics`, default on).
        //
        // Independent of Waystone: this is the physical optimizer's input
        // (docs/spec/heap-and-tuple.md §7), not a trail. Default on because a
        // history that starts when someone remembers to enable it is a
        // history that does not exist when the optimizer arrives.
        bool access_statistics = true;

        // ---- Physical optimizer (docs/spec/physical-optimizer.md) -------

        // R3's mode: `off` or `shadow` (default). Shadow costs nothing at
        // rest - the planner is pull-only, computed when `SHOW RELAYOUT`
        // asks - which is why on-by-default is safe where a background
        // optimizer would not be. `on` is **refused at startup naming §6's
        // gates**: a config written for the future fails loudly today
        // instead of silently under-delivering.
        PhysicalOptimizerMode physical_optimizer = PhysicalOptimizerMode::kShadow;

        // R1's half-life: how long an untouched decay score takes to lose
        // half its weight (`decay_half_life`, in **seconds** in the file
        // because that is the unit an operator reasons about, held in ns
        // because that is the clock's). 600 s is the spec's `[PROPOSED]`
        // default, pinned here and nowhere else; nothing may depend on the
        // number, only on the rule — one half-life halves, exactly.
        // Consumed by the planner (workplan PX05); until then the key
        // parses, validates, and steers nothing, which is stated in the
        // spec rather than discovered.
        sched::MonoTimeNs decay_half_life_ns = 600'000'000'000ULL;  // 600 s

        // ---- Cabin optimizer, §II.6 (workplan PHY05) --------------------
        //
        // The controller's boot settings. `cabin_optimizer` seeds PO8's
        // switch (off by default - experimental, §II.6's posture, and the
        // opposite default from Part I's shadow because a controller that
        // *acts* is not a report); the rest translate into
        // `stats::CabinOptimizerConfig` via CabinOptimizerSettings().
        // Thresholds are **percent integers** because the config file
        // parses no decimals: 300 means θ = 3.0. Validation enforces the
        // one rule the hysteresis stands on - θ_drop < 100 < θ_create -
        // and every number is `[PROPOSED]`, consumed by PHY04's task when
        // it exists and by nothing until then, stated plainly.
        bool cabin_optimizer = false;
        std::uint64_t cabin_optimizer_page_budget = 1024;
        std::uint32_t cabin_optimizer_theta_create_pct = 300;
        std::uint32_t cabin_optimizer_theta_drop_pct = 50;
        std::uint32_t cabin_optimizer_theta_swap_pct = 200;
        std::uint32_t cabin_optimizer_theta_extend_pct = 20;
        std::uint32_t cabin_optimizer_theta_heal_pct = 10;
        std::uint32_t cabin_optimizer_confirm_snapshots = 3;
        // T_amort in whole decay half-lives (never 0) - the build-cost
        // amortization window; the derivation lives on
        // `stats::CabinOptimizerConfig::amort_windows`.
        std::uint32_t cabin_optimizer_amort_windows = 64;
        // T_cooldown in whole decay half-lives - the DECAYING dwell, its
        // own parameter since 2026-08-10 (it was `2 x T_amort`). 128 is
        // what that expression yielded at the shipped window, so the
        // default changed no behaviour. Read the floor note on
        // `stats::CabinOptimizerConfig::cooldown_half_lives` before
        // lowering it: below a workload's longest quiet period it retires
        // live Cabins, not dead ones.
        std::uint32_t cabin_optimizer_cooldown_half_lives = 128;
        // 0 disables the cadence, the checkpoint_interval_ms precedent.
        std::uint64_t cabin_optimizer_snapshot_interval_ms = 10'000;

        // The boot settings assembled into the decision core's config.
        // T_amort stays the R1 half-life (§II.6's own default) with no key
        // of its own until a measured workload argues for one.
        stats::CabinOptimizerConfig CabinOptimizerSettings() const;

        // ---- Cabin (docs/spec/cabin.md) ---------------------------------

        // Whether Cabins may be built and served (`cabins`, default on).
        //
        // On by default because it does **nothing** until a `CREATE CABIN`:
        // no Cabin exists on a fresh database, so the cost of the switch
        // being on is one `cabin_mask == 0` test per write and per compiled
        // equality. Off is the configuration the contract suite compares
        // against - "identical replies with cabins on and off" is the
        // property the whole feature has to keep, and an operator has to be
        // able to check it on their own data.
        bool cabins = true;

        // Whether a secondary index may be *read* (`indexes`, default on).
        //
        // A read-path switch only. Off makes an index step take the walk it
        // would have taken had the index not existed, and the compiled chain
        // is unchanged either way - so "identical replies with indexes on
        // and off" compares execution rather than compilation, which is the
        // sharper of the two tests.
        //
        // **There is deliberately no switch for maintenance.** An index that
        // stops being maintained is *wrong* rather than slow, and a config
        // key that can produce a wrong answer is not a config key. It is
        // also why `DROP INDEX` exists: turning the cost off is a catalog
        // act, not a configuration one.
        bool indexes = true;

        // §8's budget, which the spec leaves `[OPEN]`. Both `[PROPOSED]`,
        // both held here rather than compiled in so that every option the
        // spec lists stays viable. Nothing may depend on either number; what
        // the code depends on is the rule they enforce - a cap **refuses to
        // observe** a value and never truncates its entry set, because a
        // truncated set marked authoritative is a wrong answer.
        //
        // 0 is meaningful for the first: no value may be observed, which
        // keeps the catalog objects while switching the behaviour off.
        std::size_t cabin_max_values = 4096;
        std::size_t cabin_max_entries_per_value = 4096;

        // AG11's caps (docs/spec/aggregate.md §6), both `[PROPOSED]` and
        // both held here for the reason the Cabin pair above are: nothing
        // may depend on either number, only on the rule they enforce - a
        // cap **fails the statement**, never truncates the group set and
        // never spills, because a truncated set is a wrong answer with a
        // right answer's shape.
        //
        // `aggregate_max_distinct` is summed over every DISTINCT set in one
        // statement rather than applied per set: the resource being bounded
        // is the statement's memory, and a hundred small sets cost what one
        // large one does.
        //
        // 0 is meaningful for the first, as it is for `cabin_max_values`:
        // no group may be founded, which refuses every aggregated statement
        // while leaving the grammar in place.
        std::size_t aggregate_max_groups = 65536;
        std::size_t aggregate_max_distinct = 1048576;

        // How many rows one `ORDER BY` may hold (`[PROPOSED]`,
        // docs/workplan-order-by.md OB4). Counted in rows and not bytes,
        // for the reason `aggregate_max_groups` counts groups: an entry
        // count is the number an operator can reason about against a row
        // width they already know.
        //
        // A sort refuses when it hits this, and does not spill or truncate
        // - it has no fallback to decline to, the way a Cabin does. Under a
        // `LIMIT` a sort holds only `offset + limit` rows, so this bounds
        // the unlimited case, which is the one that can grow with the
        // relation.
        std::size_t sort_max_rows = exec::kDefaultSortMaxRows;

        // How many rows one statement-local inner build may hold (workplan
        // JB5; the semantics - the Cabin's fall-back refusal, 0 as the
        // outright off-switch - live at the constant's declaration,
        // exec/budget.hpp).
        std::size_t join_build_max_rows = exec::kDefaultJoinBuildMaxRows;

        // How often the `system`-group WAL drain runs. It is what makes a
        // kRelaxed commit durable within its interval and what resolves a
        // kGroup batch nobody is waiting on; a drain with nothing pending
        // does no I/O (manager.hpp), so this is cheap to run often. 0
        // disables it, which leaves kRelaxed commits unsynced until the
        // next checkpoint or shutdown.
        sched::MonoTimeNs wal_drain_interval_ns = 1'000'000;  // 1 ms

        // How long a `relaxed` commit may sit unsynced - D3's loss window
        // (docs/spec/wal.md). Exposed because it is not only a durability dial:
        // the sync that enforces it runs on the reactor thread, so whatever
        // statement is in flight when it fires pays the device's full
        // latency. Measured on an EBS volume, the default produces one
        // ~2.2 ms statement every 12 ms and is the whole of `relaxed`'s tail
        // (bench/results-latency-matrix.md). Raising it lengthens the loss
        // window and thins the stalls out; it cannot remove them while the
        // sync is on this thread.
        sched::MonoTimeNs relaxed_flush_interval_ns = 10'000'000;  // 10 ms

        // How many bytes every variable-width value occupies inside a
        // tuple (docs/spec/heap-and-tuple.md section 3.3). It is read from
        // configuration exactly once - at the bootstrap of a *new*
        // database, which pins it into the superblock; every mount after
        // that validates this value against the pinned one and refuses to
        // start on a disagreement, because on-disk tuple layout is computed
        // from it.
        //
        // The default is **[PROPOSED], not settled** (CLAUDE.md's open
        // decisions): 64 is sized so common OLTP strings never spill, and
        // the number is to be measured against real target-schema string
        // lengths. Nothing may depend on it - it is a configured value
        // threaded into catalog::RowLayout, never a compiled-in constant.
        std::uint32_t inline_cell_width = storage::kDefaultInlineCellWidth;

        // How many reactor cores this instance runs
        // (docs/inflight/in-progress/workplan-crosscore.md M6). Pinned into the superblock at
        // the bootstrap of a *new* database and validated against it on
        // every mount after, exactly as `inline_cell_width` is - and for a
        // reason of the same weight: WAL streams are per core, so the count
        // decides how many streams the database has, and recovery under a
        // changed count is [OPEN] (wal.md §3).
        //
        // **1 is the default and today it is also the only count that does
        // anything.** The multicore workplan's P0 and P1 record ownership
        // and build the cross-core transport; nothing spawns a second
        // reactor yet (P2), so a value above 1 pins the superblock and
        // sizes the ring matrix and changes nothing else. It is a real
        // configuration rather than a placeholder because the pinning has
        // to happen at bootstrap - a database created single-core cannot
        // later be told it has four streams.
        //
        // Bounded above by server::kMaxWalCores (the superblock's anchor
        // table is indexed by core_id) and validated against
        // std::thread::hardware_concurrency() at startup: overcommitting
        // pinned reactors to fewer physical cores is not a configuration
        // this engine's cooperative, never-blocking task model survives.
        std::uint32_t cores = 1;

        // Relation placement (workplan P6c, `placement` config key):
        // `creating` (default) pins every relation to the creating core;
        // `rotate` spreads user relations over the non-system cores. Until
        // cross-core dispatch exists a rotated relation's statements are
        // refused retryably by the affinity check, so rotate is for
        // exercising CC7's handoff, not for serving traffic.
        catalog::PlacementPolicy placement = catalog::PlacementPolicy::kCreatingCore;

        // Diagnostic log (base/log.hpp). `log_dir` empty means "next to
        // wherever the process runs"; the two are joined into one path, so
        // an absolute `log_file` is honoured on its own.
        std::string log_dir;
        std::string log_file = "kdb.log";
        LogLevel log_level = LogLevel::kInfo;

        // Resolved destination: log_dir joined with log_file. Empty only
        // when log_file is empty, which means "do not log to a file".
        std::string LogPath() const;

        // Every key a config file may set (config_file.hpp). Kept next to
        // the fields it fills so adding a field and forgetting the key is
        // one edit away from being noticed.
        static std::vector<std::string> KnownConfigKeys();

        // Overlays `file` onto this config. Keys absent from the file leave
        // the current value alone, which is what makes the precedence chain
        // (defaults, then file, then command line) a matter of call order.
        //
        // Fails on an unknown key, an unparseable value, or a value out of
        // range - a config file that is quietly half-applied is worse than
        // one that refuses to start.
        Status ApplyFile(const ConfigFile& file);
    };

    // Opens (creating if absent) the data file, brings a database up on it,
    // and persists the result before returning, so a fresh database that
    // dies before its first client cannot come back looking fresh again.
    // Does not bind the port - Serve() does that.
    static StatusOr<std::unique_ptr<Expeditor>> Open(Config config,
                                                     std::uint64_t now_unix_seconds);

    Expeditor(const Expeditor&) = delete;
    Expeditor& operator=(const Expeditor&) = delete;

    // Binds the port and serves clients until a STOP command, then syncs.
    Status Serve();

    // Writes everything back to stable storage. Still what SYNC and the
    // shutdown path call; no longer the *only* thing that persists, now
    // that the checkpoint timer runs (see the file comment).
    //
    // The log goes first, and unconditionally: the store's WAL gate only
    // syncs the log as far as the pages it is about to write require, so a
    // relaxed commit whose pages are already clean would otherwise still
    // be in the ring when the process exits. A drain here is what makes
    // "stopped cleanly" mean every acknowledged commit survived.
    Status Sync() {
        if (wal_ != nullptr) {
            if (Status s = wal_->SyncAll(); !s.ok()) return s;
        }
        return store_->Sync();
    }

    // Writes the superblock page and syncs it. What `txn::TrxIdSequence`
    // calls when it raises the transaction-id ceiling: the sequence hands
    // out a block of ids from memory, and this is what makes that block's
    // ceiling survive a restart so the ids are never reissued.
    //
    // A full Sync() rather than a page write, because a superblock in the
    // page cache is a ceiling that a crash still loses - which is the whole
    // thing the durable write is bought for.
    Status PersistSuperBlock();

    // Runs one checkpoint to completion: snapshot the dirty table, flush
    // it, log CHECKPOINT_END, publish the superblock anchor. This is what
    // the interval timer calls, exposed so a test can drive the cadence
    // deterministically instead of waiting on wall time.
    Status Checkpoint();

    const SuperBlock& superblock() const noexcept { return database_->superblock; }
    catalog::Catalog& catalog() noexcept { return database_->catalog; }
    storage::DevicePageStore& store() noexcept { return *store_; }
    CommandDispatcher& dispatcher() noexcept { return *dispatcher_; }
    const Config& config() const noexcept { return config_; }
    const wal::CheckpointStats& checkpoint_stats() const noexcept {
        return checkpointer_->stats();
    }
    wal::WalManager& wal() noexcept { return *wal_; }

    // What recovery did to this core's stream at mount. Zeroed on a database
    // whose log held nothing, which is the common case and a fact worth
    // being able to assert rather than assume.
    const MountRecovery& recovery() const noexcept { return recovery_; }

    // The `SIGTERM`/`SIGINT` descriptor the platform layer installed
    // (`server/stop_signal.hpp`). `Serve()` registers it with its reactor, so a
    // process-manager stop takes the same path a client's `STOP` does - the
    // final sync and the shutdown checkpoint included.
    //
    // Handed in rather than installed here, because blocking those signals has
    // to happen before **any** thread exists and this class starts the WAL
    // writer in `Open()`. `main.cpp` owns that ordering; this owns the reactor.
    // Unset means "no signal handling", which is what every in-process caller
    // and every test wants.
    void set_stop_signal(StopSignal* stop) noexcept { stop_signal_ = stop; }

    // The server's log. Always non-null: with no file configured it is a
    // Logger over a null sink, so call sites never test for one.
    Logger& log() noexcept { return *logger_; }

private:
    Expeditor(Config config, std::unique_ptr<storage::PageDevice> device,
              std::unique_ptr<storage::DevicePageStore> store) noexcept;

    Status OpenLog();

    Config config_;
    sched::SystemClock clock_;
    SystemWallClock wall_clock_;

    // Declared before everything it might report on, and destroyed after
    // it - so a subsystem's teardown can still log.
    std::unique_ptr<FileLogSink> log_sink_;
    std::optional<Logger> logger_;

    // Declared in construction order, and torn down in reverse: the
    // checkpointer holds references into the WAL manager, the target, and
    // the anchor; the anchor holds one into the superblock and the store.
    std::unique_ptr<storage::PageDevice> device_;
    std::unique_ptr<storage::DevicePageStore> store_;
    std::optional<bootstrap::BootstrapResult> database_;
    // Declared before the dispatcher, which holds a pointer to it: the
    // recorder has to outlive every statement that reports to it.
    std::optional<stats::TrailRecorder> trail_recorder_;

    // The core-local Cabin store: one per core, holding every observed
    // value's entry set. Empty when `cabins = off`. It outlives the
    // dispatcher that borrows it, which is the whole of its lifetime
    // contract - the sets are memory-resident by design (§9: a crash
    // declares every Cabin unobserved and traffic rebuilds it).
    // The cabin optimizer's signal collector (workplan PHY01): S1/S2 fed
    // by the dispatcher per successful SELECT, S3 forwarded by the Cabin
    // store. Declared **before** both of its feeders - each holds a
    // pointer to it, so it must be destroyed after them.
    std::optional<stats::OptimizerSignals> optimizer_signals_;

    std::optional<stats::CabinStore> cabin_store_;

    // ---- The transaction stack (docs/spec/txn.md) ---------------------------
    //
    // Declared before the dispatcher, which holds a pointer into it, and
    // torn down after it - the same construction-order rule every other
    // member here follows. `ids_` writes the superblock through a callback
    // rather than owning the page, because what "durable" means for the
    // superblock belongs to this class (Sync()), not to the sequence.
    std::optional<txn::TrxIdSequence> trx_ids_;
    std::optional<txn::UndoLog> undo_log_;
    std::optional<txn::TransactionManager> txn_manager_;

    // What core 0's recovery did at mount (RV1). Kept rather than logged and
    // dropped, because two later steps of Open() read it: the extent
    // allocator's search hint owes `page_floor` (RC04's obligation 1), and
    // the transaction ceiling is applied from it before the id sequence that
    // caches one is built. Default-constructed means "recovery has not run
    // yet", which is only true before that call.
    MountRecovery recovery_;

    // Not owned: the platform layer installs it before this object exists and
    // outlives Serve().
    StopSignal* stop_signal_ = nullptr;

    // The cabin optimizer's decision core and executor (workplan PHY04).
    // After everything they hold references into - the catalog (via
    // database_), the store, the Cabin store, the txn manager - and before
    // the dispatcher, whose kill switch the cadence lambda reads. Both
    // exist only when Cabins do: a controller over a store that cannot
    // hold a Cabin would decide about nothing.
    std::optional<stats::CabinOptimizer> cabin_controller_;
    std::optional<exec::CabinOptimizerExecutor> cabin_executor_;

    std::optional<CommandDispatcher> dispatcher_;

    std::unique_ptr<wal::FileLogDevice> log_device_;
    std::unique_ptr<wal::WalManager> wal_;
    // ---- The fan-out (docs/inflight/in-progress/workplan-crosscore.md P2) --------------------
    //
    // Cores 1..N-1, each on its own pinned thread. Empty at `cores = 1`, and
    // so is `transport_` - guideline 2's "the ring layer contributes zero
    // messages and zero allocations" is kept literally, by not building one.
    //
    // Core 0's reactor is *not* here: it is the one Serve() runs on the
    // calling thread, with the listener and dispatcher attached, exactly as
    // it was before this existed. That asymmetry is deliberate - it is what
    // makes the single-core path unchanged rather than merely equivalent.
    std::optional<sched::RealRingTransport> transport_;

    // The session side of remote reads (workplan P4c), armed with the
    // transport: core 0 ships eligible single-step reads to owning cores.
    std::optional<SessionStepClient> remote_reads_;

    // Core 0's own step server (workplan P4d-4b-3): a pipeline stage whose
    // relation core 0 owns is served here, exactly as a peer serves its
    // own. Registered *with* remote_reads_ under one fan-by-tag lambda per
    // shared message kind, because a scheduler holds one handler per kind
    // and both consumers discard unmatched tags silently.
    std::optional<RemoteStepServer> remote_steps_;

    // Core 0's side of a peer-owned relation's CREATE INDEX (PW1c-6b-4,
    // index_build_service.hpp): the dispatcher's foreign arm parks on it
    // between the request it sends the owner and the sys.indexes row it
    // then commits. Armed with the transport, beside remote_reads_ and for
    // its reason - a reply must never beat its receiver. It captures the
    // Serve() scheduler exactly as remote_reads_' send lambda does, and is
    // used only while that scheduler runs; nothing pumps it after Serve
    // returns.
    std::optional<IndexBuildClient> index_builds_;

    // Core 0's side of a peer-owned relation's CREATE ASSERTION (PW1c-6c,
    // assertion_build_service.hpp), on the same terms as `index_builds_`
    // above and armed beside it.
    std::optional<AssertionBuildClient> assertion_builds_;

    // **Core 0's two halves of statement shipping** (SS1/SS3), armed with
    // the transport for `index_builds_`' reason. Core 0 is an owner like
    // any other - a peer's client ships it the statements it owns - and an
    // arrival core like any other, so it needs both.
    //
    // Declared **after** `cores_` would be wrong and **before** it is not
    // enough to think about: what matters is that the server holds the
    // executor's seam, so the server is declared last of the two and
    // destroyed first. Both borrow `dispatcher_`, declared above.
    std::optional<ShippedStatementExecutor> shipped_executor_;
    std::optional<StatementShipServer> statement_ship_server_;
    std::optional<StatementShipClient> statement_ship_client_;

    std::vector<std::unique_ptr<CoreRuntime>> cores_;

    // Core 0's page-id allocator (M5): the only thing that carves the free
    // map. A member rather than a local in Serve() because the grant handler
    // registered on core 0's reactor borrows it and outlives the statement
    // that installed it.
    std::optional<storage::ExtentAllocator> extents_;

    // Sends every peer a kShutdown. Called on the way down, from core 0's
    // thread, before the workers are joined.
    void BroadcastShutdown(sched::Scheduler& core0_scheduler);

    // Flushes the catalog pages and tells every peer to drop its cache.
    // Hooked to `Catalog::BumpVersion()`, the single DDL choke point, so a
    // DDL added later broadcasts without knowing this exists.
    void BroadcastCatalogInvalidation(sched::Scheduler& core0_scheduler);

    std::optional<storage::PageStoreCheckpointTarget> checkpoint_target_;
    std::optional<SuperBlockCheckpointAnchor> checkpoint_anchor_;
    // The live set a checkpoint records now comes from the transaction
    // manager, which implements wal::ActiveTransactions. `NoActiveTransactions`
    // was correct only while nothing could be live.
    std::optional<wal::Checkpointer> checkpointer_;
};

}  // namespace kds::server
