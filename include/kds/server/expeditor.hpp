#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/waker_table.hpp"
#include "kds/server/config_file.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/core_runtime.hpp"
#include "kds/server/load_placer.hpp"
#include "kds/exec/budget.hpp"
#include "kds/exec/cabin_optimizer_exec.hpp"
#include "kds/stats/cabin_optimizer.hpp"
#include "kds/stats/optimizer_signals.hpp"
#include "kds/server/mount_recovery.hpp"

#include "kds/server/stop_signal.hpp"
#include "kds/txn/lock_table.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/server/backup.hpp"
#include "kds/server/wal_archive.hpp"
#include "kds/server/page_reclaim.hpp"
#include "kds/server/statement_epoch.hpp"
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
// ---- The reactor and the WAL ----------------------------------------------
//
// Serve() runs a `sched::Scheduler` (sched.md section 2). TcpServer attaches
// its sockets to it; `system`-group timers run the checkpoint cadence
// (wal.md section 11) through PageStoreCheckpointTarget and the WAL drain
// that bounds a relaxed commit's loss window.
//
// Open() installs the store's WAL gate and hands every dispatcher a
// WalManager over the instance's one stream: every data mutation is logged
// (wal.md section 8; the free map is the exception), durable to its class
// before the reply, and no dirty page reaches the device ahead of its
// records. Open() runs mount recovery over the stream before any core
// serves (`mount_recovery.hpp`).

namespace kds::server {

// The pool's ceiling, checked at both doors (BE-R3): a config file without
// `buffer_pool_frames`, a `0`, and a server started with no config at all -
// whose `Config` carries the 0 it was built with - are each refused
// `InvalidArgument`. The key is required and has no default (operator
// ruling, `raft-marks-2026-10-07.md` §14). A free function beside the
// config for the same reason `CheckCoreCount` is one: testable without a
// server.
Status CheckBufferPoolFrames(std::size_t frames);

// `reactor_cpus`' spelling (BA-S2): CPU numbers separated by commas, spaces
// around each allowed. Refuses an empty item or a number that is not a u32.
StatusOr<std::vector<std::uint32_t>> ParseReactorCpus(std::string_view text);

// `reactor_cpus` against the core count: empty passes; otherwise exactly
// `cores` CPUs, no CPU twice, and none at or above `hardware_cpus` when that
// is known (non-zero). Free for the reason `CheckBufferPoolFrames` is.
Status CheckReactorCpus(const std::vector<std::uint32_t>& cpus, std::uint32_t cores,
                        unsigned hardware_cpus);

class Expeditor {
public:
    struct Config {
        std::string data_file = "kds.db";
        std::uint16_t port = 15432;
        // `listen_address`: the IPv4 address the default port binds, on
        // every core. Loopback unless set; `debug_text_port` and `kwp_port`
        // stay loopback whatever it says.
        std::string listen_address = kLoopbackAddress;

        // **Not a configuration key**: the test seam D19's fallback needs
        // (AT-S10c). Above one core, core 0 binds the port alone and hands
        // every accepted connection off (`connection_handoff.hpp`), as it
        // does on a platform that refuses `SO_REUSEPORT` - which Linux, the
        // one platform this engine builds for, never does. No config file
        // reaches it.
        bool force_listener_handoff = false;

        // **Not a configuration key**: a test seam over BL-S3's balancer.
        // Above one core every listener asks a placer at each transaction's
        // head which core that transaction runs on, and moves the connection
        // there (`transaction_placement.hpp`); unset, the placer is the
        // instance's `LoadPlacer`. Must outlive the run.
        TransactionPlacer* transaction_placer = nullptr;

        // The buffer pool's maximum, in 8 KiB frames, for the whole instance:
        // one pool every core shares (`eviction.md` §6). **Required**, with
        // no default - the 0 here is "not given", which `Open` refuses
        // (`CheckBufferPoolFrames`) - and it must cover the volume's
        // resident-class pages plus `DevicePageStore::kWorkingMinimumFrames`,
        // which `Open` checks once the volume is mounted (BE-Q6).
        std::size_t buffer_pool_frames = 0;

        // **The borrow cap, per transaction** (AO-R10, AR2 E2; the
        // operator's mark of 2026-09-08 fixes the value at 65,536 and
        // calls it `[provisional]` until AO-S7 names it). One name for the
        // quantity, here and in `txn::kMaxLocksPerTxnDefault`, per
        // `CLAUDE.md`'s rule against a second spelling. Reaching it is a
        // refusal and never an escalation: the borrow model does not widen
        // a transaction's fine borrows into a coarse one, so a cap refuses
        // rather than truncating, and it is not retryable because a cap
        // cannot be waited out.
        std::size_t max_locks_per_txn = txn::kMaxLocksPerTxnDefault;

        // Direct TLS on the default (KWP) port (docs/spec/protocol.md §1):
        // with `tls = on` the first byte every client sends there is a
        // ClientHello, with no plaintext fallback and no STARTTLS-style
        // upgrade; `debug_text_port` never carries TLS. Off by default
        // because every listener binds loopback only.
        //
        // Both paths must be set when `tls` is (ApplyFile refuses
        // otherwise), must be PEM, and are read once at Serve(). On a
        // server built without TLS (KDS_WITH_TLS=OFF), `tls = on` is
        // refused with NotImplemented naming the build flag - a config
        // written for a capability this binary does not have fails
        // loudly, the physical_optimizer = on precedent.
        bool tls = false;
        std::string tls_cert_file;
        std::string tls_key_file;

        // Connection authentication (docs/spec/protocol.md D8's auth stage).
        // `auth = off` (default) admits every connection; `auth = scram`
        // requires every connection on the default port (KWP `C_AUTH`) and
        // on `debug_text_port` to complete a SCRAM-SHA-256 exchange before
        // its first statement - STOP included. It refuses `kwp_port`, whose
        // v0 load endpoint has no auth stage.
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

        // Directory the WAL segment files live in. Defaults to
        // `<data_file>.wal` when left empty.
        std::string wal_dir;

        // The WAL archive (BR-S4, `wal_archive.hpp`): on when one of its
        // two destinations is set.
        ArchiveSettings archive;

        // How often the `system`-group checkpoint task runs. Checkpoint
        // cadence is the RTO knob and an [OPEN] decision (wal.md sections
        // 11 and 13), so this is a parameter with a default, never a
        // constant anything may depend on. 0 disables the timer entirely,
        // which puts durability back on SYNC and STOP alone.
        sched::MonoTimeNs checkpoint_interval_ns = 5'000'000'000;  // 5 s

        // The server's default durability class (wal.md section 1): every
        // session starts at it (`ClientSetup::durability`), and KWP/1's
        // durability frame changes it per session (protocol.md).
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
        // It exists because a cooperative core yields mid-statement only in
        // a `SELECT`'s outermost walk (BA-S15): any other unbounded
        // statement holds that core against every other client on it.
        std::uint64_t max_rows_touched = exec::kDefaultRowTouchBudget;

        // BI3's per-statement row cap for a multi-row INSERT
        // (docs/spec/bulkinsert.md, `max_insert_rows`). A refusal, never a
        // truncation; must be at least 1.
        std::uint64_t max_insert_rows = parser::kDefaultMaxInsertRows;

        // KWP v0's load endpoint (`kwp_port`, docs/inflight/in-progress/workplan-kwp-load.md).
        // 0 - the default - opens no socket at all: the endpoint exists
        // only when asked for. Loopback only, like the text port.
        std::uint16_t kwp_port = 0;

        // **The newline text protocol's loopback debug port**
        // (`debug_text_port`, docs/spec/protocol.md §12, KW-D6). 0 - the
        // default - opens no socket at all.
        //
        // The main `port` speaks KWP/1 since the cut-over; this is where
        // the newline protocol survives, and §12 calls it what it is: "a
        // documented loopback debug surface", not a second production
        // surface. Loopback only, like every listener this server binds.
        //
        // **`STOP` lives here** (KW-D4): it is a line command, not a KWP
        // statement, so it is reachable only on this port, and a deployment
        // that stops its server by typing STOP needs this port open.
        std::uint16_t debug_text_port = 0;

        // ---- `isolation` (docs/spec/txn.md section 1) -----------------------
        //
        // The level a session starts at, and therefore the level an
        // autocommit statement runs at. **READ COMMITTED is the default**,
        // and the reason is specific to this engine rather than convention:
        // under first-updater-wins, REPEATABLE READ holds one read view
        // for a whole transaction and so converts more concurrent writes
        // into retryable aborts - and since M2 it also waits for fewer of
        // them (`docs/spec/txn.md` section 5). This is the server rung
        // of the same three-level chain `durability` uses - server, then
        // `SET ISOLATION LEVEL` per session, then `BEGIN ISOLATION LEVEL`
        // per transaction.
        txn::IsolationLevel isolation = txn::IsolationLevel::kReadCommitted;

        // There is no `default_key_mode` field. The key is still recognized
        // by the loader, and refused there naming the removal rather than
        // falling to the unknown-key error - it decided what a `CREATE
        // TABLE` naming no key-mode word meant, and there is no longer a
        // mode to decide (docs/spec/heap-and-tuple.md §4.1).

        // Whether a successful SELECT of an **opted** shape records a
        // Waystone trail (`waystone_recording`, default on). Since CF-S1b
        // nothing records until an operator opts a shape in with `WAYSTONE
        // ON`, so this is the instance's ceiling over the opted shapes, not
        // a switch over every repeated SELECT; the boot log line and
        // `sys.meta`'s `waystone` section print it beside the opted count.
        // Off, an opt-in is accepted and gets no directory, and the mount
        // makes one once it is on.
        //
        // Off is a valid production setting, not a debug switch: recording
        // costs a page write per newly-hot opted instance. It is also the
        // switch that makes "results are identical either way" a thing an
        // operator can check on their own data.
        bool waystone_recording = true;

        // Whether an opted SELECT may be served from a trail a previous
        // execution recorded (`waystone_replay`, default on; the same
        // ceiling over the opted shapes). This is the half that
        // *repays* recording - and the first Waystone code that can be
        // asked to produce a row, which is why the advisory-contract suite
        // compares it against every other configuration byte for byte.
        bool waystone_replay = true;

        // Whether a successful SELECT records its access shapes into
        // `sys.access_stats` (`access_statistics`, default on).
        //
        // Independent of Waystone: it feeds `SHOW ACCESS` and CREATE CABIN's
        // no-filter warning (docs/spec/heap-and-tuple.md §7), not a trail.
        // Default on because a history that starts when someone remembers to
        // enable it is a history that does not exist when it is asked for.
        bool access_statistics = true;

        // ---- Decay score and Cabin controller (docs/spec/decay.md,
        // docs/spec/cabin-controller.md) ------------------------------------

        // R1's half-life: how long an untouched decay score takes to lose
        // half its weight (`decay_half_life`, in **seconds** in the file
        // because that is the unit an operator reasons about, held in ns
        // because that is the clock's). 600 s is the spec's `[PROPOSED]`
        // default, pinned here and nowhere else; nothing may depend on the
        // number, only on the rule — one half-life halves, exactly.
        // Consumed by the Cabin controller and its signal collector.
        sched::MonoTimeNs decay_half_life_ns = 600'000'000'000ULL;  // 600 s

        // ---- Cabin optimizer, docs/spec/cabin-controller.md §II.6 -------
        //
        // The controller's boot settings. `cabin_optimizer` seeds PO8's
        // switch (off by default - experimental, cabin-controller.md
        // §II.6's posture); the rest translate into
        // `stats::CabinOptimizerConfig` via CabinOptimizerSettings().
        // Thresholds are **percent integers** because the config file
        // parses no decimals: 300 means θ = 3.0. Validation enforces the
        // one rule the hysteresis stands on - θ_drop < 100 < θ_create -
        // and every number is `[PROPOSED]`.
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
        // own parameter since 2026-08-10 (formerly `2 x T_amort`, which the
        // default 128 preserves). Read the floor note on
        // `stats::CabinOptimizerConfig::cooldown_half_lives` before
        // lowering it: below a workload's longest quiet period it retires
        // live Cabins, not dead ones.
        std::uint32_t cabin_optimizer_cooldown_half_lives = 128;
        // 0 disables the cadence, the checkpoint_interval_ms precedent.
        std::uint64_t cabin_optimizer_snapshot_interval_ms = 10'000;

        // The boot settings assembled into the decision core's config.
        // T_amort is `cabin_optimizer_amort_windows` whole R1 half-lives
        // (cabin-controller.md §II.6).
        stats::CabinOptimizerConfig CabinOptimizerSettings() const;
        // The two caps as the store takes them - one place for the
        // positional pair, so core 0's store and a peer's (AK-S2) cannot
        // disagree on the order.
        stats::CabinLimits CabinLimitsOf() const noexcept {
            return stats::CabinLimits{cabin_max_values, cabin_max_entries_per_value};
        }

        // ---- Cabin (docs/spec/cabin.md) ---------------------------------

        // Whether Cabins may be built and served (`cabins`, default on).
        //
        // On by default because it does **nothing** until a `CREATE CABIN`:
        // no Cabin exists on a fresh database, so the cost of the switch
        // being on is one empty `cabin_columns` test per write and per compiled
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

        // Whether a walk consults its scan kernel (BM-Q5 (a); exec/budget.hpp).
        bool scan_kernel = true;

        // `join_order`: `planned` lets the join planner choose a block's
        // order, `written` keeps the order the statement wrote (CB-S2).
        bool plan_join_order = true;

        // **How long a statement may wait before the engine calls it a
        // fault** (AO-R8, the retired in-doubt ceiling's quantity,
        // re-scoped at AT-S6). Milliseconds in the file,
        // because the quantity an operator reasons about is a
        // client-visible stall and microseconds would be noise on it. The
        // semantics have one home, at `kLockWaitFaultNetNs`
        // (txn/lock_table.hpp), which also carries the derivation.
        sched::MonoTimeNs lock_wait_fault_net_ns = txn::kLockWaitFaultNetNs;

        // `tcp_keepalive_s` (CN-10 U7), for every listener: seconds in the
        // file, the unit `TCP_KEEPIDLE` takes. The semantics have one home,
        // `ConfigureAcceptedSocket` (tcp_server.hpp).
        std::uint32_t tcp_keepalive_s = kDefaultTcpKeepaliveS;

        // `txn_lifetime_ceiling_ms` (AN-R14), for every listener:
        // milliseconds in the file, for `lock_wait_fault_net_ms`'s reason.
        // 0 is refused. The semantics have one home, `TcpServer`'s
        // `ExpireOverdueTransactions` (tcp_server.hpp).
        sched::MonoTimeNs txn_lifetime_ceiling_ns = kDefaultTxnLifetimeCeilingNs;


        // How often the `system`-group WAL drain runs. It is what makes a
        // kRelaxed commit durable within its interval and what resolves a
        // kGroup batch nobody is waiting on; a drain with nothing pending
        // does no I/O (manager.hpp), so this is cheap to run often. 0
        // disables it, which leaves kRelaxed commits unsynced until the
        // next checkpoint or shutdown.
        sched::MonoTimeNs wal_drain_interval_ns = 1'000'000;  // 1 ms

        // How long a `relaxed` commit may sit unsynced - D3's loss window
        // (docs/spec/wal.md). Enforced by core 0's drain, which hands the
        // sync to the WAL writer thread (`WalManager::DrainOnce`), so no
        // statement pays for it where a writer runs.
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

        // How many reactor cores this instance runs. **Not pinned since
        // AT-S9**: a mount records the running count in the superblock, one
        // WAL stream (AM-S4(d)) and no `owner_core` (D17) having left nothing
        // on the volume that names a core.
        //
        // Bounded above by server::kMaxCores (every per-core array is
        // indexed by core_id) and validated against
        // std::thread::hardware_concurrency() at startup: overcommitting
        // pinned reactors to fewer physical cores is not a configuration
        // this engine's cooperative, never-blocking task model survives.
        std::uint32_t cores = 1;

        // **The CPU each reactor is pinned to** (BA-S2, BA-R0's host map):
        // reactor k runs on `reactor_cpus[k]`, core 0 included. Empty keeps
        // the default - peer k on CPU k, core 0 unpinned - which puts
        // reactors 0 and 1 on one physical core's SMT siblings on a host
        // that numbers siblings adjacently. When given it names exactly
        // `cores` distinct CPUs, each one this machine has.
        std::vector<std::uint32_t> reactor_cpus;

        // Diagnostic log (base/log.hpp). `log_dir` empty means "next to
        // wherever the process runs"; the two are joined into one path, so
        // an absolute `log_file` is honoured on its own.
        std::string log_dir;
        std::string log_file = "kdb.log";
        LogLevel log_level = LogLevel::kInfo;

        // Resolved destination: log_dir joined with log_file. Empty only
        // when log_file is empty, which means "do not log to a file".
        std::string LogPath() const;

        // The statement limits above as the one value every core's
        // dispatcher is handed (AT-S8, `StatementLimits`).
        StatementLimits Limits() const {
            return StatementLimits{indexes, max_insert_rows,
                                   exec::AggregateLimits{aggregate_max_groups,
                                                         aggregate_max_distinct},
                                   sort_max_rows, join_build_max_rows, scan_kernel,
                                   plan_join_order};
        }

        // Every key a config file may set (config_file.hpp). Kept next to
        // the fields it fills so adding a field and forgetting the key is
        // one edit away from being noticed.
        static std::vector<std::string> KnownConfigKeys();

        // Keys this build refuses by name, each with the message that says
        // what replaced it. `ApplyFile` checks them before the unknown-key
        // check, so the message is what an operator sees.
        static std::vector<std::pair<std::string, std::string>> RetiredConfigKeys();

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

    // Out of line because `running_` below points at a type this header does
    // not define.
    ~Expeditor();

    // Binds the port and serves clients until a STOP command, then syncs.
    // `Start()` then `RunUntilStopped()`, and what production calls.
    Status Serve();

    // Assembles the instance and returns with it running: the reactors of
    // cores 1..N-1 are on their own threads and the listeners are bound,
    // but **core 0's reactor has not been entered**, so no client statement
    // has run.
    //
    // **That gap is the reason for the split** (AM-S0(a)): it makes the
    // multi-core assembly - who opens the log, who attaches, what a peer is
    // handed - reachable from `tests/expeditor_test.cpp`.
    //
    // On failure nothing is left running: every thread this call started is
    // stopped and joined before it returns, so a caller may drop the
    // `Expeditor` on an error without a second call. Calling it twice is
    // refused - `InvalidArgument`, the "simply wrong" code - because the
    // second call would bind a bound port and spawn a second set of workers.
    Status Start();

    // Runs core 0's reactor until something stops it, then takes the
    // shutdown path whole - the enrolled rollback, the peer broadcast and
    // join, each core's final sync and shutdown checkpoint, the listener
    // detach, this core's sync and checkpoint. Refused if `Start()` did not
    // run, because there would be no reactor to run and the shutdown tail
    // would then report a clean stop of an instance that never started.
    Status RunUntilStopped();

    // Writes everything back to stable storage: what SYNC and the shutdown
    // path call, beside the checkpoint cadence.
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

    // Writes the superblock page and syncs it, the log first. The mount's
    // raise of the transaction-id ceiling calls it; a carve calls
    // `PersistTrxIdCeiling` below, which skips the log drain.
    //
    // A full Sync() - the log and the pool - which the mount's raise, its
    // one caller, runs once before the completion checkpoint. A carve needs
    // only page 0 made durable, which is `PersistTrxIdCeiling`.
    Status PersistSuperBlock();

    // **What every core's transaction-id carve persists through** (AT-S10b):
    // the same encode, then page 0 alone made durable (BA-S13,
    // `DevicePageStore::PersistPage`) - callable from any core, because the
    // encode is under the superblock latch and the writeback takes the
    // page's claim. Page 0 is unlogged, so its writeback waits on no log
    // record and on no other page.
    Status PersistTrxIdCeiling();

    // Runs one checkpoint to completion: snapshot the dirty table, flush
    // it, log CHECKPOINT_END, publish the superblock anchor. This is what
    // the interval timer calls, exposed so a test can drive the cadence
    // deterministically instead of waiting on wall time.
    Status Checkpoint();

    // **One step of DROP TABLE page reclamation** (BF-R8): at most
    // `kReclaimBatchPages` frees, gated on the durable redo start. What core
    // 0's system tick calls, exposed so a test drives it deterministically.
    Status ReclaimStep();
    void SetReclaimCutForTest(std::function<void(const char*)> hook) {
        reclaimer_->SetCutForTest(std::move(hook));
    }
    const ReclaimCounters& reclaim_counters() const noexcept { return reclaim_counters_; }

    const SuperBlock& superblock() const noexcept { return database_->superblock; }
    catalog::Catalog& catalog() noexcept { return database_->catalog; }
    storage::DevicePageStore& store() noexcept { return *store_; }
    // What BP-S3's arming cells read: which instance structures latch.
    const txn::InstanceVisibility& visibility() const noexcept { return *visibility_; }
    const stats::CabinStore* cabin_store() const noexcept {
        return cabin_store_ ? &*cabin_store_ : nullptr;
    }
    const StatementEpochs& statement_epochs() const noexcept { return *statement_epochs_; }
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
    BackupControl& backup() noexcept { return *backup_; }
    WalArchiver* archiver() noexcept { return archiver_ ? &*archiver_ : nullptr; }

    // **The peers, and the window to ask in is between `Start()` and
    // `RunUntilStopped()`.** Empty at `cores = 1` and before `Start()`;
    // cleared on the way down, so a caller that asks afterwards sees no
    // cores rather than dangling ones. Core 0 is not in it - core 0's
    // runtime is this object.
    const std::vector<std::unique_ptr<CoreRuntime>>& cores() const noexcept { return cores_; }
    // What places each transaction above one core (BL-S3): the test's seam
    // or the instance's `LoadPlacer`; null at one core, after `Start()`.
    const TransactionPlacer* transaction_placer() const noexcept { return placer_; }
    // The inboxes a connection crosses cores through: D19's fallback's
    // accepts, and BL-S2's moves where a placer is set. Null where neither.
    const ConnectionHandoff* connection_handoff() const noexcept {
        return handoff_.has_value() ? &*handoff_ : nullptr;
    }

    // **The instance's one wake registry** (AU-S1b, `sched/waker_table.hpp`),
    // null at `cores = 1`, where it is not built, and **the instance's lock
    // table** (AO-S5). Exposed for the same reason `wal()` and `store()`
    // are: whether the lock table kicks through *this* registry, and whether
    // every core was handed *this* table, are wiring facts whose failure is
    // a waiter that sleeps out its block rather than a wrong answer.
    const txn::LockTable* locks() const noexcept { return locks_.get(); }
    const sched::WakerTable* wakers() const noexcept {
        return wakers_.has_value() ? &*wakers_ : nullptr;
    }

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

    // Encodes the in-memory superblock into page 0 under the superblock
    // latch - the half `PersistSuperBlock` and `PersistTrxIdCeiling` share.
    Status EncodeSuperBlock();

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

    // **The instance's one Cabin store** (AT-S7), holding every observed
    // value's entry set; every peer borrows it
    // (`CoreRuntime::Config::cabins_store`). Empty when `cabins = off`. It outlives the
    // dispatcher that borrows it, which is the whole of its lifetime
    // contract - the sets are memory-resident by design (§9: a crash
    // declares every Cabin unobserved and traffic rebuilds it).
    // The cabin optimizer's signal collector (workplan PHY01): S1/S2 fed
    // by the dispatcher per successful SELECT, S3 forwarded by the Cabin
    // store. Declared **before** both of its feeders - each holds a
    // pointer to it, so it must be destroyed after them.
    std::optional<stats::OptimizerSignals> optimizer_signals_;
    // The join planner's statistics, one store for the instance
    // (docs/spec/plan-stats.md); its steward runs on core 0's tick.
    std::optional<stats::PlanStats> plan_stats_;

    std::optional<stats::CabinStore> cabin_store_;

    // ---- The transaction stack (docs/spec/txn.md) ---------------------------
    //
    // Declared before the dispatcher, which holds a pointer into it, and
    // torn down after it - the same construction-order rule every other
    // member here follows. `ids_` writes the superblock through a callback
    // rather than owning the page, because what "durable" means for the
    // superblock belongs to this class (Sync()), not to the sequence.
    // The instance's shared visibility state (AN-R1), borrowed by core 0's
    // own manager and by every peer's. Declared **first** in this block and
    // therefore destroyed last, after `cores_` and after `txn_manager_` -
    // the same rule `wal_` follows for the stream peers append through.
    //
    // **Always engaged** (AM-S4(d)): it records commit order as a commit
    // record's LSN, and `SuperBlock::Decode` refuses a volume with more
    // than one stream, so every mount has one order and records it.
    std::optional<txn::InstanceVisibility> visibility_;

    // **The instance's lock table** (AO-R2, built here at AO-S5): one for
    // every core, 64 partitions at any core count, latchless at one. Core 0's
    // manager takes it below and every peer borrows it through
    // `CoreRuntime::Config::locks`, so a decide on any core releases into
    // the one table and wakes - and kicks, through `wakers_` - a waiter
    // queued from any other (AU-S2). Declared above the manager that holds
    // it, so it outlives it. Core 0's dispatcher takes it too, so AO-S4a's
    // deadlock detector's graph spans every core.
    std::unique_ptr<txn::LockTable> locks_;

    // **The instance's four words** (`catalog.md` CT6; one struct since
    // CF-S1c, `catalog/instance_words.hpp`): the schema version word
    // (AT-S2; AR0-5 §2.1, D21), the object-oid sequence and the delete-mark
    // count (AT-S5b), and the Waystone opted count (CF-S1c). Memory-resident
    // and never persisted: the schema word and the mark count start at zero
    // with every cache empty, the oid sequence is seeded from the pages by
    // the first catalog that needs an oid, and the opted count is set from
    // the chain by the mount's renewal. Core 0's catalog takes them through
    // `Catalog::SetInstanceWords` in `Open`, every peer's through
    // `CoreRuntime::Config::instance_words`, and core 0's transaction
    // manager borrows the schema word; no DDL is broadcast to a core.
    //
    // **The declaration order is not what makes this safe.** `database_`
    // is declared *above* the words, so core 0's `Catalog` outlives them;
    // what makes that harmless is that the struct is trivially destructible
    // (`instance_words.hpp` asserts it) and `~Catalog` reads none of it. A
    // word whose destructor did anything would have to move above
    // `database_`.
    catalog::InstanceWords instance_words_;

    // **The instance's assertion registry** (AT-S5d, AT-R15): the one
    // directory per assertion every core's writes check and reserve into,
    // `LockTable`'s idiom - this class owns it, core 0's dispatcher is
    // pointed at it in `Open` before the mount resumes it, and every peer is
    // handed it through `CoreRuntime::Config`. Declared above the dispatcher,
    // the cores and the checkpointer, all of which borrow it, so it outlives
    // every one of them. Optional only for the in-place build: its latch
    // makes it immovable, and whether it is armed is the config's.
    std::optional<exec::AssertionEnforcer> assertions_;

    // **Page 0's two writers since AT-S8, and the one run** - declared above
    // everything that borrows them. `superblock_latch_` is taken around every
    // mutation of `database_->superblock` and the encode that follows it:
    // every core's transaction-id carve (AT-S10b) and the checkpoint
    // anchor's fold from any core (`SuperBlockCheckpointAnchor::SetLatch`). `checkpoint_gate_` is
    // what lets at most one of the instance's checkpointers run
    // (`wal::CheckpointGate`); every peer is handed both.
    //
    // **Acquisition order** (`rules.md` §3): the superblock latch is taken
    // holding nothing and is **outer to page 0's page latch** - both writers
    // take it, then `Get(0)` - and it is released before the store sync, so
    // it is never held across device I/O and nothing reached from a sync
    // takes it. The gate is not a latch: a failed attempt skips, so it
    // orders against nothing.
    Latch superblock_latch_{LatchKind::kSuperblock};

    // **The optimizer surface's shared state** (AT-S8, `OptimizerSurface`):
    // the `CABIN_OPTIMIZER` switch every core's `SET` flips and the cadence
    // reads; the collector's latch, armed above one core; and the view latch
    // core 0's cadence holds across a tick while any core's
    // `SHOW CABIN_OPTIMIZER` reads the controller. Order: the view latch is
    // outer to the collector's. **The tick holds the view latch across I/O**
    // - a `CreateCabin` and the whole seeded build's walk - so a
    // `SHOW CABIN_OPTIMIZER` on a peer sleeps that peer's reactor for a
    // build (`known-gaps.md`); a reader never holds it across I/O.
    std::atomic<bool> cabin_optimizer_on_{false};
    Latch optimizer_signals_latch_{LatchKind::kOptimizer};
    Latch cabin_view_latch_{LatchKind::kOptimizer};
    wal::CheckpointGate checkpoint_gate_;

    std::optional<txn::TrxIdSequence> trx_ids_;
    std::optional<txn::UndoLog> undo_log_;
    std::optional<txn::TransactionManager> txn_manager_;

    // What core 0's recovery did at mount (RV1). Kept rather than logged and
    // dropped, because two later steps of Open() read it: the store's
    // allocation floor owes `page_floor` (RC04's obligation 1), and
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
    // so is `wakers_` - G2's zero overhead is kept literally, by not
    // building one.
    //
    // Core 0's reactor is *not* here: it is the one Serve() runs on the
    // calling thread, with the listener and dispatcher attached, so the
    // single-core path is this object alone.
    //
    // AU-S3: how core 0 reaches a peer's reactor, and since AT-S10d how any
    // core reaches any other (AR0-6-R1). Sized at open and registered into
    // by each core, so a `Stop()` from this thread is followed by a kick
    // that ends the block the peer is in.
    std::optional<sched::WakerTable> wakers_;
    // D19's fallback (AT-S10c): the inboxes core 0's listener hands
    // connections into, built only where the port cannot be shared. After
    // `wakers_`, which it kicks through, and before `cores_` and
    // `running_`, whose listeners hold it - so it outlives both.
    std::optional<ConnectionHandoff> handoff_;
    // BL-S3: the balancer every core asks, above one core where no test
    // placer is set (`load_placer.hpp`).
    LoadPlacer load_placer_;
    TransactionPlacer* placer_ = nullptr;
    // AU-S3: what a peer calls when a client says STOP on it. Built once
    // core 0's scheduler and the waker table both exist, and handed to every
    // peer so none of them needs to know about either.
    std::function<void()> instance_stop_;

    // Core 0's checkpoint target, and **the instance's anchor**, which every
    // peer's checkpointer publishes into since AT-S8 - so declared above
    // `cores_`: reverse-order destruction takes the peers first, and the
    // teardown sequence that clears `cores_` early is no longer what keeps a
    // peer from outliving the anchor it holds.
    std::optional<storage::PageStoreCheckpointTarget> checkpoint_target_;
    std::optional<SuperBlockCheckpointAnchor> checkpoint_anchor_;

    // DROP TABLE page reclamation (BF-R8): the tombstones this mount found
    // pending, collected after the completion checkpoint, and the counters
    // every core's `SHOW META` prints.
    ReclaimCounters reclaim_counters_;
    std::optional<PageReclaimer> reclaimer_;
    // The instance's backup control (BR): its pin bounds the recycler and
    // the reclaim gate, and every core's dispatcher runs BACKUP through it.
    // After the anchor it reads `D` from, so it is destroyed - its log copy
    // joined - before the anchor.
    std::optional<BackupControl> backup_;
    // The WAL archive (BR-S4), when configured: its floor bounds the
    // recycler beside the backup's pin. Stopped before the log it reads.
    std::optional<WalArchiver> archiver_;
    // BF-R9's statement-epoch slots, one per core.
    std::optional<StatementEpochs> statement_epochs_;
    // Where the mount's recovery scan ended (BF-R4): every record that names
    // a pending tombstone's pages lies below it, so once the durable redo
    // start reaches it no mount can replay one. Set once `RecoverCoreAtMount`
    // returns, before anything reads it.
    wal::Lsn reclaim_gate_ = 0;

    std::vector<std::unique_ptr<CoreRuntime>> cores_;

    // Sets every peer's stop flag and kicks it awake (AU-S3: write, then
    // kick). Called on the way down, from core 0's thread, before the
    // workers are joined. A no-op at `cores = 1`, where no waker table was
    // built and there is no peer to stop.
    void BroadcastShutdown();

    // The failure half of `Start()`: stops and joins whatever core threads
    // it had already spawned, then drops the cores. Not the shutdown tail -
    // an instance that never entered core 0's reactor has nothing to sync
    // and nothing to checkpoint.
    void StopStartedCores();

    // The optimizer surface every core's dispatcher is handed (AT-S8,
    // `OptimizerSurface`): the collector, the switch, and
    // the controller view where a Cabin store exists. The view latch is
    // armed above one core only.
    OptimizerSurface Optimizer();

    // Core 0's checkpointer; the live set it records comes from the
    // transaction manager, which implements wal::ActiveTransactions.
    std::optional<wal::Checkpointer> checkpointer_;

    // **The running instance's own state**: the io backend, the
    // TLS context and credential store, the three listeners, core 0's
    // reactor, the reactor-borrow guard, and the peer threads. Defined in
    // `expeditor.cpp`, which is what its field order and its destructor are
    // about and why `~Expeditor` is out of line - the type would otherwise
    // drag the epoll backend, OpenSSL's context and `<thread>` into every
    // translation unit that names an `Expeditor`, and put a
    // `#if KDS_WITH_TLS` member in a public header.
    //
    // **Declared last, so it is destroyed before every member it borrows
    // from** - `dispatcher_` holds its reactor.
    struct ServeRuntime;
    std::unique_ptr<ServeRuntime> running_;
};

}  // namespace kds::server
