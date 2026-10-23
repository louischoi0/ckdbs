#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kds/base/function_ref.hpp"
#include "kds/base/latch.hpp"
#include "kds/base/log.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/aggregate.hpp"
#include "kds/exec/query_layer.hpp"
#include "kds/exec/sort.hpp"
#include "kds/exec/assertion_check.hpp"
#include "kds/exec/transition.hpp"
#include "kds/exec/budget.hpp"
#include "kds/exec/plan_memo.hpp"
#include "kds/exec/step_compiler.hpp"
#include "kds/stats/plan_stats.hpp"
#include "kds/exec/fk_check.hpp"
#include "kds/exec/index_maintain.hpp"
#include "kds/exec/set_expr.hpp"
#include "kds/exec/plan_printer.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/cabin_ddl.hpp"
#include "kds/exec/cabin_expr.hpp"
#include "kds/exec/cabin_optimizer_exec.hpp"
#include "kds/exec/index_maintain.hpp"
#include "kds/parser/ast.hpp"
#include "kds/stats/access_stats.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/stats/trail_recorder.hpp"
#include "kds/stats/trail_store.hpp"
#include "kds/stats/lock_wait_log.hpp"
#include "kds/stats/relation_stats.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/coro.hpp"


#include "kds/server/result_sink.hpp"
#include "kds/server/sys_views.hpp"
#include "kds/exec/catalog_view.hpp"
#include "kds/server/session.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wire/error_registry.hpp"

// Command dispatch: turns one client statement into an action against the
// running database and a response. Pure engine logic - no sockets, no
// syscalls, no clock reads (rules.md #4) - which is why it is split from the
// platform-layer listener (TcpServer, tcp_server.hpp) and unit-tested
// directly. The SQL statements are parsed by src/parser and executed here;
// column types resolve through `Catalog::ResolveTypeByName()` against
// sys.types.
//
// CREATE TABLE keeps a bare-name form with no parens, which asks for a
// zero-column table and therefore always errors (every relation needs a
// Keystone tuple key column); a '(' after the name selects the full form.
//
// Newline protocol: one command per line, case-insensitive keyword; a
// response is exactly one line with no embedded newline - the listener
// appends the terminator.
//
// ---- WAL: every mutation is logged --------------------------------------
//
// Given a WalManager, every write path - INSERT, UPDATE, DELETE, PURGE and
// the catalog rows under DDL - appends the records that describe it, and a
// commit is not answered until durable to its class (wal.md sections 1 and
// 5.2). An INSERT logs, in order: its spills, its index entries, then the
// row - a BTREE_INSERT, or the one BTREE_SPLIT carrying every page a leaf
// split wrote (BD-R12).
//
// ---- Every user relation is a clustered B+ tree ----------------------------
//
// Since BZ (`instructions/v3.2.0/workorder-bz-heap-hidden-key.md`) a heap is
// a btree whose tuple key is the engine's hidden column 0, so no handler
// here branches on storage. A btree **leaf is a heap page**: the row codec,
// `PageView` reads/overwrites and the `SHOW PAGE` dump work on it as on any
// slotted page. `SELECT`/`UPDATE ... WHERE id = <n>` descends the tree,
// which is **authoritative** - a miss means the row does not exist, and no
// scan follows. `INSERT` may split a leaf and grow the tree a level, in
// which case the relation's root is repointed before the client is
// answered.
//
// ---- Ordering: an insert's records are appended under its holds (AT-S21) --
//
// The storage call writes the tuple and hands back the pages it changed,
// still held (`InsertPlacement::held`: the leaf and the parents a split
// secured); index maintenance logs each entry under the
// index tree's hold; every spill keeps its var-heap page held. `LogInsert`
// appends and stamps under those holds, then drops them - before an own
// transaction's commit, whose durability wait must not run under a latch
// (wal.md section 8-1). Ground: a write runs on every core (since AT-S5), so
// unheld, another core could move the row the record names - divide its
// leaf, or place a row below it and shift it a slot (BD-R2) - write back a
// parent whose split was not yet logged, or shift the index entry the record
// was re-read from (`insert_log_crash_rig_test.cpp`).

namespace kds::sched {
// Only the pointer is held here; `set_scheduler_view` below says what for.
class Scheduler;
}  // namespace kds::sched

namespace kds::server {

struct ReclaimCounters;  // page_reclaim.hpp
class BackupControl;     // backup.hpp
class WalArchiver;       // wal_archive.hpp
class PageReclaimer;     // page_reclaim.hpp
class StatementEpochs;   // statement_epoch.hpp

// What the mount's recovery did (`server/mount_recovery.hpp`), reported by
// SHOW META. Forward-declared rather than included: only the pointer is held
// here, and the definition drags in the WAL and catalog headers that every
// consumer of this file would then pay for.
struct MountRecovery;


// A `SELECT` whose walk runs on after its head returns (BA-S15); defined
// in `command_dispatcher.cpp`.
class SelectRun;

// A relation-X build - `CREATE INDEX`, `REINDEX INDEX`, `CREATE ASSERTION`,
// `CREATE TRANSITION` - whose walk runs on after its head returns (A1 stage
// 2); defined in `command_dispatcher.cpp`.
class BuildRun;

struct DispatchOutcome {
    std::string response;
    bool should_stop = false;

    // **How many rows the statement affected**, for a caller that needs the
    // number rather than the sentence. `S_COMPLETE{tag, rows_affected}`
    // (docs/spec/protocol.md §7) is the one such caller today.
    //
    // Carried rather than parsed out of `response`: the DML replies do not
    // share a spelling (`UPDATED 7`, `INSERTED oid=.. id=..`, a bulk
    // insert's `rows=`). Set where the count is known; 0 everywhere else,
    // the truthful answer for a read, a DDL and a session statement.
    std::uint64_t rows_affected = 0;

    // **Why the statement failed, as a `Status`.** OK on every success and
    // on the handful of refusals built from a bare message.
    //
    // Carried because a typed client switches on the error's *category*
    // (protocol.md §11), and `StatusFromErrorReply` on the rendered line
    // folds every code but four into `InvalidArgument`.
    Status status = Status::OK();

    // **The wire detail this failure carries**, or `kNoDetail`. A `Status`
    // has no room for one - `protocol.md` §11's details live on the wire
    // error, which the session builds - so a refusal that knows *which*
    // limit it hit has to carry the answer up here beside the status. Only
    // the borrow cap sets it today (`wire::ResourceDetail::kLockCap`): the
    // client's fix differs from every other `ResourceExhausted` - a shorter
    // transaction, not a smaller statement - which is why it has a number.
    std::uint16_t resource_detail = wire::kNoDetail;

    // A write this core is holding back because something it needs is held
    // by a transaction that **has not decided yet** - or since AY-S3 (B6)
    // has decided and not yet released a unit whose wake this statement
    // holds (AO-S3).
    //
    // Two things set it: `CheckWriteConflictBlocking` when the row's last
    // writer is in flight, and the foreign-key forward resolution when the
    // *parent* row's writer is. `NoteBlockingWriter` is the single gate on
    // both and holds the conditions.
    //
    // Set only where a re-run could get past the refusal: the statement
    // wrote nothing before the conflict. `DispatchAsync` parks on the doubt
    // and re-runs; a statement that had written rows is not re-runnable (a
    // second `SET v = v + 1`) - it parks mid-walk instead (`parked_mid_walk`).
    //
    // **The synchronous `Dispatch()` never waits on one**: a path with no
    // reactor cannot park, so it answers the retryable conflict itself.
    //
    // **AX-S2b: where the refused ask handed back a slot, the wait is on it.**
    // `slot` is the wake the refused unit ask registered (`BorrowChain`),
    // flipped by the holder's release from whichever core releases and
    // followed by a kick to this core - so a waiter asleep in its idle block
    // is woken, and its re-run comes after the release rather than between
    // the holder's decide and its release. A wake is not a grant: a third
    // writer can take the unit first, and the re-run is refused and parks
    // again under the same deadline. Since AY-S2 a cross-unit refusal - a
    // declared range met by a row's `X`, a row under a range fence - hands
    // back a slot too, registered on the unit that refused it, which `key`
    // names. Null where the refusal handed back none - an assertion's
    // group, a dispatcher with no lock table, a holder only the header
    // names - and there the wait polls `IsInFlight`.
    struct WriteBlock {
        std::uint64_t trx_id = 0;  // the holder being waited for
        std::uint64_t tuple_key = 0;      // the row it holds
        txn::LockKey key{};        // the unit `slot` is registered on
        std::shared_ptr<txn::LockWaitSlot> slot;
        // The mode the refused ask wanted, for `sys.lock_waits`; meaningful
        // only beside a `slot`.
        txn::LockMode mode = txn::LockMode::kExclusive;
    };
    std::optional<WriteBlock> write_block = std::nullopt;

    // **The unit this statement's refused ask is registered on** (AO-S6e-b)
    // - the relation it asked for, or since AY-S2 the fence a foreign-key
    // check's parent row sits under - or nullopt. Asked by a DDL's relation `X` (`DROP TABLE`,
    // `CREATE`/`DROP INDEX`, a `CREATE ASSERTION`'s build) against a
    // writer's `IX` or a reader's `IS`, and since AT-S5e by a writer's first
    // `IX` against a DDL's `X`. The wait is on the table's own slot and not
    // on the holder's decide, because the release flips it and kicks this
    // core wherever the holder runs, and a read borrow's holder is no
    // transaction `TransactionManager::IsInFlight` knows.
    //
    // The slot is what a release flips (`lock_table.hpp`, AU-S2's
    // write-then-kick). `holder` names the refusal and draws the wait-for
    // edge where both ends are transactions - a waiter inside an explicit
    // transaction holds what that transaction has written
    // (`AwaitRelationLock`); a read borrow is never an edge's end, and a
    // holder of 0 is a stale-memo re-run (`BorrowChain`) that waits on
    // nobody.
    struct LockWait {
        txn::LockKey key;
        std::uint64_t holder = 0;
        std::shared_ptr<txn::LockWaitSlot> slot;
        // Whether the refusal that ends the wait badly poisons an explicit
        // transaction - false for a statement that is not the transaction's,
        // a `CREATE ASSERTION`'s build (AT-S5e).
        bool poisons = true;
        // The mode the refused ask wanted (`sys.lock_waits`). Every site
        // names it; the default is only aggregate initialization's.
        txn::LockMode mode = txn::LockMode::kExclusive;
    };
    std::optional<LockWait> lock_wait = std::nullopt;

    // **A `PURGE` judged a key it may not free yet** (BH PU4, PU5): its
    // latest writer is undecided, or its committed deleter is still
    // invisible to some reader. Nothing was written. `DispatchAsync` polls
    // until the doubt clears, re-runs the statement whole, and refuses it
    // `TxnConflict` at `exec::kPurgeHorizonWaitNs`; the synchronous path
    // answers with that refusal at once, which is `response`. A poll,
    // because no release or horizon advance kicks it (`known-gaps.md`).
    //
    // `PURGE DROPPED COLUMNS` (CD-S2) waits the same way for the same two
    // things, and for a third: `epoch` non-zero is the schema word every
    // core's running statements must have revalidated at or after - none may
    // still hold a layout in which a dropped column is live.
    struct PurgeWait {
        std::uint64_t tuple_key = 0;
        std::uint64_t trx_id = 0;  // the writer waited for, or the deleter
        bool horizon = false;      // waiting for every reader, not a decide
        std::uint64_t epoch = 0;   // waiting for every statement epoch, not a writer
    };
    std::optional<PurgeWait> purge_wait = std::nullopt;

    // **The walk stopped inside the statement and the scope is still open**
    // (AO-S3b). `write_block` says which row and holder; this says the
    // statement is *resumable* rather than re-runnable - it has written rows,
    // a re-run would write them twice and there are no savepoints, so it
    // resumes where it stopped. `Session::parked_write` carries what the
    // resume needs; this flag tells the handler's tail not to end the scope.
    bool parked_mid_walk = false;
    WalkCursor walk_cursor = {};

    // The commit this statement staged, when the client may not be told
    // about it until the log is durable (`durability = group`, docs/spec/wal.md
    // D2). `kNoLsn` means there is nothing to wait for - every relaxed
    // statement, every read, and every strict commit, which synced on its
    // own stack before returning.
    //
    // **The wait is deliberately not taken where the commit happens**: an
    // inline sync holds the core while the device works and serializes every
    // other connection (a group batch of exactly 1). The caller decides:
    // `Dispatch()` waits inline, having no scheduler; `DispatchAsync()`
    // parks, so the next connection's commit joins the same sync.
    wal::Lsn pending_lsn = wal::kNoLsn;

    // The `DispatchAsync` call this outcome belongs to, carried so a wait's
    // re-run - inside `AwaitStatementWaits`, which has only the outcome -
    // opens its window as the same statement (`MayParkScope`). 0 outside
    // `DispatchAsync`.
    std::uint64_t statement = 0;

    // **A `SELECT`'s walk, not yet run** (BA-S15, BA-R12). Set only under
    // `DispatchAsync`, on the plain path's reply - the one shape whose walk
    // is unbounded - after the head has parsed, compiled and described the
    // statement: the walk runs on after the head returns, yielding its core
    // every `SlicePolicy::pages_per_slice` pages, and `DispatchAsync`
    // awaits it before it reads anything else here. It owns everything the
    // walk reads, holds this core's statement epoch at the word the head
    // revalidated against and pins the catalog memo, and gives all three
    // back when it is destroyed. Null on every other reply, and always
    // null once `DispatchAsync` returns.
    std::shared_ptr<SelectRun> select_run = nullptr;

    // **A relation-`X` build, not yet walked** (A1 stage 2, `sched.md` §3).
    // Set only under `DispatchAsync`, on a `CREATE INDEX`, `REINDEX INDEX`,
    // `CREATE ASSERTION` or `CREATE TRANSITION` reply whose head took the
    // relation `X` and answered every refusal: the walk runs on after the
    // head returns, yielding its core every `SlicePolicy::pages_per_slice`
    // leaves, and its tail publishes and decides. It owns the `X`'s holder,
    // the build and the progress registration, holds this core's statement
    // epoch and pins the catalog memo. Null on every other reply, and always
    // null once `DispatchAsync` returns.
    std::shared_ptr<BuildRun> build_run = nullptr;

    // **The poison `EndWrite` withheld for a structural re-run** (BA-S14),
    // carried with the statement rather than read back off the dispatcher:
    // since BA-S15 another statement of this core can run between this
    // statement's head and the end of `DispatchAsync`, and would overwrite
    // a member.
    bool poison_withheld = false;

    // **The statement's shape for `sys.statement_stats`** (BO-S5): its
    // fingerprint's `fetch_id`, 0 for a kind that carries none (only
    // `SELECT`, `INSERT` and `UPDATE` do). Carried for `poison_withheld`'s
    // reason: another statement of this core can run before this one ends.
    std::uint64_t fetch_id = 0;

    // **The relation `rows_affected` counts rows of** (BO-S6), for
    // `sys.relation_stats`: set beside a write's count, 0 everywhere else.
    catalog::Oid written_oid = 0;

    // **The reply carries rows besides its tag** (RETURNING, BU-Q17): the
    // rows went out through the session's sink, and `response` is still the
    // DML's tag, which a KWP `S_COMPLETE` carries in place of `SELECT`.
    bool returning = false;
};

// The one spelling of an error reply on the newline protocol (docs/spec/txn.md
// §5, docs/spec/protocol.md §11): `ERR <TOKEN> retryable=<b> <message>` for the
// codes a client library switches on - TXN_CONFLICT, FK_VIOLATION,
// ASSERTION_VIOLATION - and `ERR <message>` for everything else. Every
// dispatcher path reports through it, which is what keeps the shape from
// drifting between them; declared here so the spellings, a compatibility
// surface, can be pinned by a test that owns no socket and no dispatcher.
std::string ErrorReply(const Status& status);

// **The inverse**, for the paths that hold a rendered `ERR` line and need
// the *code* back: the KWP session and the KWP load server, as the fallback
// where `DispatchOutcome::status` is OK on a refusal built from a bare
// message. Exact for the four spellings a client switches on; every other
// code renders as a bare `ERR <message>` and comes back as one, and
// `ErrorReply(StatusFromErrorReply(line)) == line` for every line
// `ErrorReply` produces (its test asserts it).
//
// A line that is not an error reply is a bare `kOk`; the line itself is not
// carried, since the caller already holds it.
//
// The classification is prefix-shaped: a *success* line beginning `ERR `
// would read as a refusal. The replies keep that unreachable - a DML answer
// opens with a fixed keyword (`INSERTED`/`UPDATED`/`DELETED`/`OK`), and a
// SELECT's header line is comma-joined identifiers - so a reply shape added
// later that can lead with free text breaks it silently.
Status StatusFromErrorReply(std::string_view reply);

// **The deadlock victim's refusal, one sentence for both sites** (AO-R7):
// the dispatcher loop and the owner-side arm. `TxnConflict`, the one
// retryable code - the survivor will have released by the time this one
// comes back - with a message naming deadlock, so an operator looks for a
// lock-order bug rather than contention. The statement is aborted; the
// transaction is poisoned and still holds its rows. `waited_for` names what
// the victim waited on: a row and its holder, or a core's transaction.
Status DeadlockVictim(const std::string& waited_for);

// **The statement limits a config file sets, as one value** (AT-S8). Every
// core accepts sessions, so every core's dispatcher runs under the same
// ones: built once from the config, applied by one call on every core. Each
// default is the dispatcher's own.
struct StatementLimits {
    bool indexes = true;
    std::uint64_t max_insert_rows = parser::kDefaultMaxInsertRows;
    exec::AggregateLimits aggregate;
    std::size_t sort_max_rows = exec::kDefaultSortMaxRows;
    std::size_t join_build_max_rows = exec::kDefaultJoinBuildMaxRows;
    bool scan_kernel = true;
    // `join_order = planned` (true) or `written` (CB-S2).
    bool plan_join_order = true;
};

// **The Cabin controller's surface, as one value** (AT-S8, on the
// operator's word). The pieces are the instance's - one
// signal collector (latched above one core), one switch, one controller and
// executor - and every core's dispatcher is handed the same value.
// `view_latch` is what core 0's cadence holds across a tick; a view read
// from another core takes it too. A null switch leaves the dispatcher's
// own; every other member is taken as given.
struct OptimizerSurface {
    stats::OptimizerSignals* signals = nullptr;
    // The join planner's statistics (docs/spec/plan-stats.md), the
    // instance's one store.
    stats::PlanStats* plan_stats = nullptr;
    std::atomic<bool>* cabin_optimizer_on = nullptr;
    const stats::CabinOptimizer* controller = nullptr;
    const exec::CabinOptimizerExecutor* executor = nullptr;
    Latch* view_latch = nullptr;
};

// A fingerprinted SELECT's two identities, both from the parse (AP-R2,
// instructions/v3.0.0/workorder-ap-function-catalog-fetch-id.md).
//
// `trail` is what the Waystone keys on - `{fetch_id, arg_hash}`, so two
// statements differing only in their select list share a trail.
// `pattern_id` is the statement's own shape, which the cabin optimizer
// counts by and `ANALYZE` prints: a `SUM(x)` and an `x` over one fetch are
// one trail and two plans.
struct StatementIdentity {
    stats::InstanceKey trail;
    std::uint64_t pattern_id = 0;
};

class CommandDispatcher {
public:
    // `log` and `clock` are optional and independently so: a null logger
    // disables every diagnostic below, and a null clock only drops the
    // duration from the ones that report one. Both default to off so the
    // socket-free unit tests stay socket- *and* clock-free.
    //
    // The clock is the reason this class is no longer strictly free of
    // injectable interfaces (see the note above): reporting how long a
    // query took needs a monotonic reading, and taking one directly would
    // be the std::chrono call rules.md section 4 forbids.
    // `wal` is optional too, and null means INSERT mutates pages without
    // logging them - the unlogged path, which the socket-free
    // unit tests and the catalog-level tests still run on.
    CommandDispatcher(SuperBlock& superblock, catalog::Catalog& catalog,
                       storage::PageStore& page_store, Logger* log = nullptr,
                       const sched::Clock* clock = nullptr, wal::WalManager* wal = nullptr,
                       wal::DurabilityClass durability = wal::DurabilityClass::kGroup,
                       exec::Budget budget = exec::Budget(),
                       stats::TrailRecorder* recorder = nullptr,
                       bool replay_enabled = false,
                       bool access_statistics = true,
                       stats::CabinStore* cabins = nullptr,
                       txn::TransactionManager* txn = nullptr,
                       txn::IsolationLevel isolation =
                           txn::IsolationLevel::kReadCommitted,
                       std::uint32_t core_id = 0, bool indexes = true,
                       std::uint64_t max_insert_rows =
                           parser::kDefaultMaxInsertRows) noexcept
        : superblock_(superblock),
          catalog_(catalog),
          page_store_(page_store),
          log_(log),
          clock_(clock),
          wal_(wal),
          durability_(durability),
          budget_(budget),
          recorder_(recorder),
          replay_enabled_(replay_enabled),
          access_stats_enabled_(access_statistics),
          cabins_(cabins),
          txn_(txn),
          core_id_(core_id),
          indexes_enabled_(indexes),
          max_insert_rows_(max_insert_rows),
          // Last two, matching their declaration order below. Order-free:
          // every initializer here reads a constructor parameter, and none
          // reads another member.
          default_isolation_(isolation),
          autocommit_session_(isolation) {
        // The one place a catalog and a manager are known to belong
        // together, which is why DT9's wiring is here and not in each
        // server's startup: a new construction site - a test fixture
        // especially - cannot forget it. Two dispatchers over one catalog
        // leave the later manager installed, the same relation the single
        // `catalog_` reference already has.
        if (txn != nullptr) catalog.SetTransactionManager(txn);
        // The declaration loader (BV-S2, AQ-S1), here for the manager's
        // reason: a construction site cannot forget it, so every serving
        // catalog fills `TableAccess::transitions` and every expression
        // Cabin's `F`. It captures the catalog and the store, never the
        // dispatcher, which a test may destroy before them.
        catalog.SetDeclarationLoader(
            [&catalog, &page_store](catalog::TableAccess& access) {
                if (Status s = exec::LoadTransitions(catalog, page_store, access); !s.ok()) {
                    return s;
                }
                return exec::LoadExprCabins(catalog, page_store, access);
            });
        ArmAssertionRecordBudget();
    }

    // Parses and executes one line. Never fails outward: a malformed or
    // unrecognized line produces an "ERR ..." response rather than any
    // kind of error return - a bad line from one client must never be
    // able to bring the dispatcher (or the server driving it) down.
    //
    // Recognized commands (case-insensitive):
    //   PING                  -> "PONG"
    //   STOP                  -> "OK bye" and should_stop = true
    //   SYNC                  -> "OK synced" or "ERR ...". Writes the page
    //                            store back to stable storage.
    //   SHOW META             -> superblock stats, one line
    //   SHOW TABLES           -> space-separated table names
    //   SHOW PATTERNS         -> `SELECT * FROM sys.patterns` (the show
    //                            sugar): one row per shape ever opted in
    //                            with `WAYSTONE ON`, its `fetch_id` in
    //                            decimal, and a `waystone` column (1 or 0)
    //                            that replaced `origin` and `pinned` at
    //                            CF-S1b - 1 only for a current-version row
    //                            with the opt-in bit. `use_count` and
    //                            `last_seen` read 0 since BL-S4's ruling 1
    //                            deleted their writer (rows.hpp). Rows from
    //                            older fingerprint revisions are listed
    //                            too, `waystone` 0, their
    //                            `fingerprint_version` saying why.
    //   SHOW ACCESS           -> "access_shapes=<n>", then one "\n"-escaped
    //                            section per recorded access shape:
    //                            "kind=<Lookup|Probe|Range|FilterScan|Scan>
    //                             rel=<s> columns=[<s>,...] uses=<n>
    //                             last_seen=<n>". Access statistics
    //                            (docs/spec/heap-and-tuple.md §7), keyed
    //                            by *columns* and never by values - so
    //                            `WHERE flag = 1` and `WHERE flag = 2` are
    //                            one shape, which is what keeps the list
    //                            bounded by the schema rather than by the
    //                            data.
    //   SHOW PAGE <page_id> [VALUES]
    //                         -> page dump: header + slot directory for a
    //                            heap page or a B+ tree leaf, or level +
    //                            separator array for a B+ tree internal
    //                            node.
    //                            Still exactly one wire line (never a raw
    //                            newline byte), but sections are joined
    //                            with the literal two-character escape
    //                            "\n" for a readable multi-line render on
    //                            the client side (tools/ckdbs_cli.py
    //                            unescapes it before printing). Development/
    //                            inspection only - not part of any
    //                            transactional read path. The optional
    //                            VALUES keyword additionally hex-encodes
    //                            each live slot's tuple payload (hex, not
    //                            raw text, since a payload can contain any
    //                            byte including '\n' - see HexEncode()'s
    //                            comment in the .cpp).
    //   DESCRIBE <name>       -> a summary line
    //                            "oid=<n> root_page_id=<n>
    //                             clustered_type=<HEAP|BTREE> next_id=<n>
    //                             columns=<n>" (plus height=<n> leaves=<n>
    //                             for a BTREE relation), then one "\n"-escaped
    //                            (see SHOW PAGE above) section per column:
    //                            "pos=<n> name=<s> type=<s> len=<n>
    //                             notnull=<yes|no> tuple_key=<yes|no>
    //                             autoincrement=<if-omitted|no>". DESC is
    //                            accepted as a synonym.
    //   CREATE TABLE <name>   -> the bare, pre-parser form: a zero-column
    //                            table. Now always "ERR ...", because
    //                            every relation's first column is its
    //                            mandatory Keystone tuple key
    //                            (heap-and-tuple.md section 4) and a
    //                            zero-column relation cannot have one.
    //                            Kept only so the failure names the
    //                            reason; use the column-list form.
    //   CREATE TABLE <name> (<col> <type> [, ...])
    //       [HEAP | BTREE] [EXPLICIT]
    //                         -> same CREATED/EXISTS response as above,
    //                            but with real columns: parsed via
    //                            src/parser, types resolved through
    //                            Catalog::ResolveTypeByName(). The storage
    //                            word: BTREE (default) is a clustered B+
    //                            tree on the first column, the Keystone
    //                            tuple key; HEAP is one on a hidden tuple
    //                            key the engine adds (BZ-R1). EXPLICIT is
    //                            accepted and does nothing; ASSIGNED is
    //                            refused. Either order, each at most once.
    //                            See src/exec/row_codec.hpp for the
    //                            supported column type set.
    //   INSERT INTO <name> [(<col> [, ...])] VALUES (<val> [, ...]) [, (...)]*
    //                         -> "INSERTED oid=<n> id=<n> page=<n> slot=<n>"
    //                            for one row, "INSERTED oid=<n> rows=<n>
    //                            first_id=<n> last_id=<n>" for several, or
    //                            "ERR ...". Without a column list, positional:
    //                            per row, `n` values name the tuple key first
    //                            and `n - 1` have it issued (heap-and-tuple.md
    //                            §4.1, invariant 11). With one (BW-S6), one
    //                            value per listed column; the tuple key left
    //                            out or `NULL` is issued, and a nullable
    //                            column left out is NULL.
    //   SELECT * FROM <name> [WHERE <cond> [AND <cond>]*]
    //                         -> a full ordered scan of the relation,
    //                            WHERE-filtered; a bare `WHERE <tuple key> = <n>`
    //                            instead takes the point path (a tree
    //                            descent, on a btree relation).
    //                            One wire line: "col1,col2,..." then one
    //                            "\n"-escaped (see SHOW PAGE above) section
    //                            per matching row, comma-joined values.
    //                            No rows matching -> just the header line.
    //   UPDATE <name> SET <col> = <expr> [, ...] [WHERE ...]
    //                         -> "UPDATED <n>" (n = rows touched) or
    //                            "ERR ..." (types.md §3.2b). DELETE, PURGE,
    //                            BACKUP, the transaction statements and the
    //                            rest route through `DispatchAndStage`.
    //
    // `session` carries the connection's transaction state (session.hpp);
    // **null means autocommit through a private session**. A cache-dropping
    // boundary, as `DispatchAndStage` below states.
    DispatchOutcome Dispatch(std::string_view line, Session* session = nullptr);

    // `Dispatch` for a statement **the engine issues itself** - a listener's
    // ROLLBACK at a transaction's lifetime ceiling or a client's disconnect,
    // a KWP load's own BEGIN, COMMIT and ROLLBACK. It runs exactly as
    // `Dispatch` does and is no client's statement, so `sys.statement_stats`
    // does not count it (BO-S5); `Dispatch` is this, timed and counted.
    DispatchOutcome DispatchInternal(std::string_view line, Session* session);

    // The statement path without the durability wait: it runs the statement
    // and reports any commit it staged through `DispatchOutcome::pending_lsn`.
    // Both entry points above go through it; they differ only in how they
    // wait.
    //
    // **A statement boundary, and since AT-S2 a cache-dropping one**: its
    // head revalidates this core's catalog against the schema word, which
    // frees every cached `TableAccess` when the word has moved. Every public
    // entry - `Dispatch`, `DispatchAsync`, this - is therefore one: hold no
    // `const TableAccess*` and no `Schema&` borrowed from the catalog across
    // a call to any of them; copy out what you need first
    // (`kwp_load_server.cpp`'s load begin is the shape).
    DispatchOutcome DispatchAndStage(std::string_view line, Session* session);

    // The suspendable form. Writes its reply through `out` - which the
    // **caller** owns and must keep alive across suspension - and finishes
    // with a Status describing the dispatch itself, not the statement (a
    // failed statement is an "ERR ..." in `out`, exactly as it is for the
    // synchronous form).
    //
    // ---- Why both forms exist -------------------------------------------
    //
    // This one can suspend: it parks on a statement's waits (a held row, a
    // refused lock, a `PURGE`'s horizon, a `group` commit's sync) and runs a
    // `SELECT`'s walk in slices (`select_run`). The synchronous `Dispatch()`
    // stays for every caller with no reactor - the socket-free tests, the
    // rollback on connection close - waiting inline for a commit's sync and
    // answering every other wait's refusal at once. Both run
    // `DispatchAndStage`, so there is one dispatch path.
    //
    // **`line` is not copied.** A coroutine's parameters live in its frame,
    // but a `string_view` parameter copies the view and not the bytes - and
    // the parser's tokens are themselves views into this buffer
    // (parser-v2.md's zero-copy tokens). The caller must keep the statement
    // text alive until the coroutine finishes, which is why `TcpServer`
    // copies each line out of its inbox before dispatching one.
    //
    // A cache-dropping boundary, as `DispatchAndStage` states.
    sched::Coro DispatchAsync(std::string_view line, Session* session, DispatchOutcome* out);

    // The level a fresh session starts at (`isolation`). TcpServer stamps
    // it on each connection's session at accept.
    txn::IsolationLevel default_isolation() const noexcept { return default_isolation_; }

    // **A test seam on the insert path** (AT-S21): runs
    // once per `INSERT` row, after the row is placed and its indexes,
    // reservation and undo are written, immediately before the row's own
    // record is appended. A two-core cell puts another core's split (a
    // divide or an append), a forced writeback or another core's index insert
    // there - the three windows the insert's logging bug entry named (AT-S21
    // closed it).
    // Unset in production, where it costs one empty-function test per row.
    void SetBeforeInsertLogForTest(std::function<void()> hook) {
        before_insert_log_for_test_ = std::move(hook);
    }

    // **A test seam at the instant a row's id is fixed** (BB-S2): runs once
    // per id `InsertOneRow` fixes for a row - on a btree's omitted tuple key once
    // per draw, so a draw that collides with a named key and is burned
    // (BD-S3's re-draw) fires it again with the next id - once its id is
    // borrowed and its row encoded. It sits before the descent and, for a
    // named key, before its admission (BD-R6), so another core's insert does
    // not block on it. Unset in production.
    void SetAfterRowIdFixedForTest(std::function<void(std::uint64_t)> hook) {
        after_row_id_fixed_for_test_ = std::move(hook);
    }

    // **A test seam between an omitted tuple key's issue and its borrow** (BD-S3's
    // review): runs once per issue on a btree, with the id drawn. A cell that
    // stops here lets a named key place the same id first - the one
    // collision BD-R6 leaves, which the insert answers by drawing again.
    // Unset in production.
    void SetAfterRowIdIssuedForTest(std::function<void(std::uint64_t)> hook) {
        after_row_id_issued_for_test_ = std::move(hook);
    }

    // **A test seam between a row's placement and its first record** (BD-S1,
    // `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md` E5): runs
    // once per `INSERT` row, after the row is placed and before its index
    // maintenance. A non-OK status fails the row exactly as a failed index
    // maintenance does - the placement left with no trail entry and no
    // record, the case E5 names. Unset in production.
    void SetAfterPlacementForTest(std::function<Status()> hook) {
        after_placement_for_test_ = std::move(hook);
    }

    // **A test seam on CREATE ASSERTION** (AZ-S2): runs once per create,
    // after the build's publish run is logged and immediately before the
    // `sys.assertions` row - where a checkpoint on another core, a writer
    // on another core, or a racing create of the same name is put. Unset
    // in production.
    void SetAfterAssertionPublishRunForTest(std::function<void()> hook) {
        after_assertion_publish_run_for_test_ = std::move(hook);
    }

    // **A test seam on a plain `SELECT`'s walk** (CE-S1): runs on the
    // reactor after each row the walk emits, with the count emitted so far
    // in this run. A non-OK status ends the walk with it, as a row's own
    // refusal would - after rows have reached the sink, which is what the
    // re-run and streaming cells need. Unset in production.
    void SetSelectRowEmittedForTest(std::function<Status(std::uint64_t)> hook) {
        select_row_emitted_for_test_ = std::move(hook);
    }

private:
    // AZ-S3: the registry refuses at admission a group no log record of this
    // dispatcher's stream can carry. Every core's stream is the instance's
    // one, so the value is the same from every dispatcher.
    void ArmAssertionRecordBudget() noexcept {
        if (wal_ != nullptr) enforcer_->SetRecordBudget(wal_->usable_payload_bytes());
    }

    std::function<void()> before_insert_log_for_test_;
    std::function<void(std::uint64_t)> after_row_id_fixed_for_test_;
    std::function<Status()> after_placement_for_test_;
    std::function<void(std::uint64_t)> after_row_id_issued_for_test_;
    std::function<void()> after_assertion_publish_run_for_test_;
    std::function<Status(std::uint64_t)> select_row_emitted_for_test_;

    // ---- Transaction control (docs/spec/txn.md sections 1, 6) ----------------
    DispatchOutcome HandleBegin(std::string_view args, Session& session);
    DispatchOutcome HandleCommit(Session& session);
    DispatchOutcome HandleRollback(Session& session);
    DispatchOutcome HandleUse(std::string_view line, std::string_view args, Session& session);
    DispatchOutcome HandleSetIsolation(std::string_view args, Session& session);
    // `SET DURABILITY {STRICT|GROUP|RELAXED}` (protocol-wp.md P03,
    // docs/spec/protocol.md §9). The session rung of the same chain
    // `HandleSetIsolation` sets, and deliberately the same shape: a
    // session statement the dispatcher routes, not a `parser::Statement`
    // arm - see the note at its definition.
    DispatchOutcome HandleSetDurability(std::string_view args, Session& session);
    DispatchOutcome HandleSetPriority(std::string_view line, std::string_view args,
                                      Session& session);

    // The read view a statement reads through. In autocommit it is minted
    // fresh here and belongs to no transaction; inside an explicit one it
    // is the transaction's, re-minted per statement under READ COMMITTED
    // and held since BEGIN under REPEATABLE READ.
    //
    // Returns a snapshot that sees everything when no TransactionManager
    // was given - the pre-MVCC engine, exactly.
    StatusOr<txn::LeasedSnapshot> SnapshotFor(Session& session);

    // ---- The write scope (section 6's failure atomicity) ----------------
    //
    // A write statement runs inside a transaction whether or not the client
    // asked for one. `owned` says which: in autocommit this scope began the
    // transaction and must end it, and inside an explicit transaction it
    // borrows the session's and ends nothing.
    struct WriteScope {
        txn::Transaction* txn = nullptr;
        bool owned = false;
        // The session this write belongs to. Carried here rather than
        // threaded separately through every *Inner() because the scope
        // already *is* this write's transaction context, and the home-core
        // binding (crosscore.md CC3) is part of that context.
        Session* session = nullptr;
        bool ok() const noexcept { return txn != nullptr; }
    };

    // Fails only if a transaction cannot be started. A dispatcher with no
    // manager returns an empty scope, and the write path stamps
    // kBootstrapXid.
    StatusOr<WriteScope> BeginWrite(Session& session);

    // Ends what BeginWrite began. `result` is the statement's outcome: OK
    // commits an owned scope, anything else aborts it. Inside an explicit
    // transaction a failure **poisons the session** rather than unwinding -
    // rows already written stay, and the client must ROLLBACK (section 6).
    Status EndWrite(Session& session, WriteScope& scope, const Status& result);

    // A write handler's tail: `EndWrite` under the reply's verdict, then the
    // rows the write reported counted into `sys.relation_stats` as `kind`
    // (BO-S6) - `out.written_oid`'s, once the statement has answered.
    DispatchOutcome FinishWrite(Session& session, WriteScope& scope, DispatchOutcome out,
                                stats::RelationWrite kind);

    // **The one way an owned scope ends without committing** (AO-S6d,
    // AO-0 item 15). Three arms of `EndWrite` reach it: the statement
    // failed, its assertion entries could not be cleared, or its commit
    // record could not be written. Ground: `TransactionManager::Commit`
    // fails only *before* `PublishCommit`, so the transaction is still
    // active and `Release` refuses it; without `Abort` it stayed live, was
    // counted in flight by every later view, and held its borrows until
    // every later writer of its rows hit the fault net.
    //
    // `reported` is what the client is owed: the commit's own failure
    // where there was one, so the answer names what actually went wrong
    // rather than the compensation that followed it. `Status::OK()` means
    // the caller has no answer of its own and wants the unwind's.
    Status AbortOwnedScope(WriteScope& scope, const Status& reported);

    // The trx_id a write stamps: the scope's transaction, or
    // kBootstrapXid when there is no manager.
    static std::uint64_t WriterId(const WriteScope& scope);

    // First-updater-wins, plus **who** the conflicting writer is.
    // `TransactionManager::CheckWriteConflict` still decides the verdict;
    // this records `trx` as the holder the statement waits for (AO-S3), one
    // function for every call site so they cannot disagree about which
    // conflicts are waitable. It records only when the wait is both **safe**
    // and **capable of a different answer**:
    //
    //   - a reactor to park on (`may_park_`), and a holder in flight - or,
    //     since AY-S3 (B6), one decided but not yet released whose wake this
    //     statement holds;
    //   - **safe**: with a lock table the wait-for edge is recorded before
    //     the park and a waiter that would close a cycle is refused
    //     (AO-S4a); without one, only a waiter holding nothing may wait, so
    //     no cycle can form;
    //   - **capable**: a `kRepeatableRead` waiter is not parked where the
    //     re-run is `kFutile` - its view, minted at `BEGIN`, cannot see the
    //     holder's commit, so the re-run would be refused on the same
    //     ground. Conservative: an *aborted* holder would have admitted it.
    //
    // `RepeatableReadWait` is asked per wait, at each recording site (AO-S6d,
    // AO-0 item 17): a blocker may hold a coarser unit over the key - a range
    // fence, a `WHERE`-less write's relation unit - having written no version
    // of it, and an `INSERT`'s verdict does not go through the view at all (a
    // named key's uniqueness is a **physical** descent, an issued one the
    // mark). If such a blocker writes the row meanwhile, the re-run is
    // refused first-updater-wins (the operator's ruling of 2026-09-09).
    //
    // `waiter` is the transaction the statement runs in, or null in
    // autocommit before one is opened - which holds nothing and re-mints its
    // view by construction.
    enum class RepeatableReadWait {
        // The re-run answers what it answered before: the blocker wrote
        // this row's current version, so a commit is invisible to a view
        // minted at `BEGIN` and the refusal is the one already owed. Also
        // the answer wherever the site cannot tell - the table reports
        // *who* refused a borrow and not *what* they hold, so a statement
        // that declared a coarse unit and was refused keeps the exclusion
        // rather than guessing.
        kFutile,
        // The re-run may answer differently: the blocker wrote no version
        // of this key, or the statement's verdict is not a function of the
        // waiter's view.
        kCapable,
    };
    void NoteBlockingWriter(const txn::Transaction* waiter, std::uint64_t trx, std::uint64_t tuple_key,
                            RepeatableReadWait rerun);

    // Ends a parked write with `refused`, on every channel a client reads.
    // Both endings a park can have - the deadlock victim and the fault net
    // - are this call plus their own sentence, because writing them out
    // twice is what let the `status` half be forgotten in both.
    //
    // **`response` is not enough.** `KwpSession::OnStatementComplete`
    // prefers `out.status` over `StatusFromErrorReply(out.response)`, and
    // KWP is the default port, so a refusal that sets only the rendered
    // line reaches the debug arm and nobody else.
    void RefuseParkedWrite(DispatchOutcome& out, Session& session, const Status& refused,
                           bool poison = true);

    // **The write-block wait** (AO-S6d, AO-0 item 16). Parks until
    // `out->write_block`'s holder decides and then re-runs `line` whole,
    // ending at a grant, at the deadlock verdict, or at the fault net - and
    // on every one of those `out->write_block` is empty on return, so a
    // caller's test of it *after* the call means "the re-run opened a new
    // wait". `AwaitStatementWaits` is its caller.
    //
    // `statement_deadline_ns` is in-out and zero means "not taken yet":
    // the fault net bounds the *statement*, so a second entry inherits the
    // first one's deadline instead of starting a fresh one.
    // `parser::Parse`, setting `statement_fetch_id_` from the parse (BO-S5).
    StatusOr<parser::Statement> ParseKeepingShape(std::string_view line);
    // One statement's `sys.statement_stats` count, at its end (BO-S5):
    // `began_ns` is when it entered, re-runs and waits included.
    void RecordStatement(std::string_view line, const DispatchOutcome& out,
                         sched::MonoTimeNs began_ns) const;

    sched::Coro AwaitWriteBlock(std::string_view line, Session* session, DispatchOutcome* out,
                                sched::MonoTimeNs* statement_deadline_ns);
    // A `sys.lock_waits` row for a wait on `key` (BO-S3), stamped with this
    // core and the wall instant; the caller sets its outcome or times it.
    stats::LockWaitRecord LockWaitRow(const txn::LockKey& key, txn::LockMode mode,
                                      std::uint64_t waiter, std::uint64_t holder) const;

    // **The statement waits for the unit it was refused, then runs again
    // whole** (AO-S6e-b). It polls the table's slot - one atomic load per
    // reactor iteration, never a partition latch - under the same statement
    // deadline as the write-block wait: both are the lock family's fault
    // net, and a second bound would be a second name for one quantity.
    //
    // The registration is dropped before the re-run, whichever way the wait
    // ended: the re-run asks under a new transaction id, so the record left
    // by this one is addressable only by its slot.
    //
    // On return `out->lock_wait` is empty unless the re-run asked again,
    // which the loop that calls this is what handles.
    sched::Coro AwaitRelationLock(std::string_view line, Session* session, DispatchOutcome* out,
                                  sched::MonoTimeNs* statement_deadline_ns);

    // **BA-R1c**: the session's last commit LSN while no snapshot minted now
    // would cover it, else `wal::kNoLsn` - consuming a covered one, since
    // the ceiling never falls back below it. What `DispatchAsync` parks on
    // and `Dispatch` spins on before the statement runs.
    wal::Lsn UncoveredCommit(Session* session);

    // Both waits, looped until neither is set, through one function so the
    // two cannot diverge (AT-S5e): each wait's re-run is a whole fresh
    // statement and can meet what the other waits for.
    sched::Coro AwaitStatementWaits(std::string_view line, Session* session, DispatchOutcome* out,
                                    sched::MonoTimeNs* statement_deadline_ns);

    // `cur` is the row's own writer, from the tuple header, and decides the
    // **MVCC verdict** (first-updater-wins, `txn.md` §5) - a writer this
    // view cannot see refuses the write whether or not anybody holds a
    // lock, and a *committed* one is a refusal no wait can get past.
    //
    // `blocker` is in-out, and it is who to **wait** for.
    //
    // *In*, whoever refused this write's borrow, from the lock table
    // (AO-S6c-b). Zero when the table refused nobody: the borrow was
    // granted, or a coarse unit already covers the row, or no table exists.
    //
    // *Out*, the holder this write is blocked by: the table's where the
    // table named one, `cur` where it did not. A **union, not a cutover** -
    // the table names holders the header never could (a fence over a key
    // nobody has written), and the header names writers the table does not
    // (a dispatcher with no lock table). Zero out means nothing to wait for
    // and the refusal is the plain conflict, which the write sites test.
    Status CheckWriteConflictBlocking(const WriteScope& scope, std::uint64_t cur,
                                      std::uint64_t tuple_key, std::uint64_t& blocker);

    // The refusal a borrow the table declined produces, in
    // `CheckWriteConflict`'s own shape - `txn/manager.cpp` calls that
    // message part of the wire contract rather than a diagnostic, so the
    // three sites that can now raise it must not each invent one. It names
    // the **holder** rather than the row's writer, because that is who the
    // client is waiting for and, for a range fence, the two are not the
    // same transaction.
    static Status HeldByHolder(std::uint64_t tuple_key, std::uint64_t holder);

    // **The borrow, and the refusal a unit somebody else holds produces.**
    // `nullopt` means the unit is this statement's to write. A `Status`
    // means it is not - and the holder has already been recorded where
    // `DispatchAsync` can park on it, so the caller has only to render it.
    //
    // **A refusal naming nobody is not one of these.** The cap comes back as
    // a `Status` (AO-S6c-c), so the bool means grant or conflict only - and
    // the `blocker` test stays, so a refusal is never read as transaction 0
    // holding a row.
    //
    // The message is rendered from the **unit**, because a fence is not a
    // row: a run that borrowed `[lo, hi)` and was refused must not tell the
    // client that `lo` is held, since the descendant that conflicted may be
    // any key in the window.
    //
    // `rerun` is `NoteBlockingWriter`'s question and is the caller's to
    // answer, because the unit asked for does not settle it: an `INSERT`
    // asks for a tuple and may be refused by a fence, and a declared range
    // may be refused by a tuple holder that wrote the row.
    //
    // **`before_wait`, when set, is asked once a refusal is in hand and
    // before it becomes a wait.** A non-OK answer is returned in the wait's
    // place and nothing of the refusal is kept: no blocker is recorded, so
    // the wake `BorrowChain` registered is dropped at the statement's end.
    // Asked for a narrower unit only - a refusal at the relation stays a
    // wait. A named-key insert borrows before it is admitted, and asks
    // through this whether the key can ever be placed - so a key that never
    // can is refused, not left waiting on a fence it could never write past
    // (AO-S6c-c's rule): a duplicate whose writer decided or a committed row
    // under an undecided writer (BD-R5). Asked only on a refusal, so a
    // granted borrow pays nothing for it.
    std::optional<Status> BorrowOrWait(const WriteScope& scope, const txn::LockKey& unit,
                                       RepeatableReadWait rerun,
                                       txn::LockMode mode = txn::LockMode::kExclusive,
                                       FunctionRef<Status()> before_wait = {});

    // **The unique keys one row writes** (BX), each with the index it
    // belongs to. Encoded under the row's span; checked with it released.
    using UniqueKeysOfRow =
        std::vector<std::pair<const catalog::TableAccess::IndexRef*, exec::UniqueKey>>;

    // The key units a row's unique checks took, given back when the row's
    // write ends - its entries appended, or nothing of it written (BX-Q2
    // (a)). Every unit is given back before the next row's check, so none
    // is held across two.
    class UniqueKeyHolds {
    public:
        UniqueKeyHolds(txn::LockTable* locks, txn::Transaction* txn) noexcept
            : locks_(locks), txn_(txn) {}
        UniqueKeyHolds(const UniqueKeyHolds&) = delete;
        UniqueKeyHolds& operator=(const UniqueKeyHolds&) = delete;
        ~UniqueKeyHolds() { Release(); }
        void Add(const txn::LockKey& unit) { units_.push_back(unit); }
        void Release();

    private:
        txn::LockTable* locks_;
        txn::Transaction* txn_;
        std::vector<txn::LockKey> units_;
    };

    // `ta`'s unique keys the write moves, from `values` as `MaintainIndexes`
    // takes them; empty for a relation with no unique index, and for a row
    // one of whose keys does not encode (the row's own encode refuses it).
    UniqueKeysOfRow UniqueKeysOf(const catalog::TableAccess& ta,
                                           std::span<const parser::AstValue> values,
                                           std::uint16_t first_col_pos,
                                           std::span<const parser::AstValue> previous);

    // BX-R1..R4: for each key, its unit `X` (a refusal is a wait, as any
    // refused borrow), then the latest-state check - held is the row's
    // refusal at the written value's byte, busy a wait on the holder's
    // decide. Holding no page: the check descends the clustered tree. `self`
    // is an UPDATE's own row. `nullopt` when every key is free, its unit in
    // `holds`.
    std::optional<Status> CheckUniqueKeys(const WriteScope& scope,
                                          const catalog::TableAccess& ta,
                                          const UniqueKeysOfRow& keys, std::uint64_t self,
                                          UniqueKeyHolds& holds);
    // One key of `CheckUniqueKeys`: its unit `X` into `holds`, then the
    // latest-state verdict, free or held; busy is the wait it answers.
    StatusOr<exec::UniqueVerdict> CheckUniqueKeyLatest(const WriteScope& scope,
                                                       const catalog::TableAccess& ta,
                                                       const catalog::TableAccess::IndexRef& ix,
                                                       const exec::UniqueKey& key,
                                                       std::uint64_t self, UniqueKeyHolds& holds);

    // Is `cond` a non-negative integer literal compared against `access`'s
    // tuple key, and if so which id? The shared half of the test
    // `TupleKeyEqualityTarget` and `DeclaredWriteBorrow` both need.
    //
    // **`kind` before `op` and before `val`, which is why this is one
    // function.** `ast.hpp` records that both default to a value reading as
    // a legal equality - a `kBetween`'s low bound once made an UPDATE write
    // one row and answer `UPDATED 1`. Answers `nullopt` for every kind that
    // is not a `kCompareValue` against a literal, leaving `kBetween` to the
    // caller that knows it carries two bounds.
    std::optional<std::uint64_t> TupleKeyLiteral(const catalog::TableAccess& access,
                                           const parser::Condition& cond) const;

    // **The unit a write declares before it walks** (AO-0 item 14, marked
    // by the operator on 2026-09-08), or `nullopt` for the per-row borrow.
    // A write whose predicate covers a key window borrows that window
    // **once** instead of one entry per row, which keeps a bulk write inside
    // `max_locks_per_txn`, a cap that refuses rather than truncates.
    //
    // **A declaration, never an escalation.** The unit is read off the
    // predicate at dispatch, before a row is touched, so it does not depend
    // on how many rows match; the mark of 2026-09-08 §1 item 4 forbids
    // *widening* accumulated tuple borrows into a coarse one. Choosing
    // coarsely up front is what AR2 §3 does for DDL, a mover's key interval
    // and an FK reverse check with no covering structure.
    //
    // **What it costs, both ways.** A coarse borrow blocks writers the
    // per-row borrow admitted: a `DELETE FROM t` with no `WHERE` holds the
    // relation against every other writer for the transaction's length. And
    // two coarse borrows must still meet - `WHERE id > 1` and `WHERE id
    // BETWEEN 2 AND 3` overlap at row 2 - or detection would be *weaker*
    // than per-row, a wrong answer rather than a cost;
    // `LockTable::ConflictingOverlap` makes them meet.
    //
    // **What it does not reach**: a predicate naming no tuple key window - `WHERE
    // name = 'x'` - stays per-row and is the one shape the cap can refuse.
    std::optional<txn::LockKey> DeclaredWriteBorrow(
        const catalog::TableAccess& access, const std::vector<parser::Condition>& where) const;

    // **The tuple key window an AND-list bounds** - the one recognizer of it (BH-R2),
    // shared by `DeclaredWriteBorrow` and `PURGE`. `[lo, hi)` is folded from
    // every conjunct that compares the tuple key with a non-negative integer literal
    // in one of PU1's six forms; `bounded` says at least one did. `unfolded`
    // is the first conjunct that did not, which the two callers answer in
    // opposite ways and both soundly: a borrow skips it, because an AND-list
    // only removes rows and the window is a superset of what is written; a
    // `PURGE` refuses it, because skipping one would *widen* what it frees
    // (`id > 0 AND id <> 5` would free 5).
    struct TupleKeyWindow {
        std::uint64_t lo = 0;
        std::uint64_t hi = 0;
        bool bounded = false;
        const parser::Condition* unfolded = nullptr;
    };
    TupleKeyWindow FoldTupleKeyWindow(const catalog::TableAccess& access,
                          const std::vector<parser::Condition>& where) const;

    // **The DDL's relation `X`** (AO-S6e-b; AR2 §3's DDL row). Empty when
    // it was taken - and it is taken for the DDL transaction, so its decide
    // is what releases it. A conflict answers the refusal and, where there
    // is a reactor to park on, leaves `lock_wait_` set so `DispatchAsync`
    // waits for the holder to release and runs the statement again.
    //
    // `DROP TABLE`, the index DDL and an assertion's build take the relation
    // `X`; `CREATE TABLE` takes each referenced parent's `IS` through `mode`
    // (BF-Q11 (a)), so a parent's drop and a child's declaration exclude
    // each other.
    std::optional<Status> BorrowRelationForDdl(txn::Transaction* holder, catalog::Oid oid,
                                               bool poisons = true,
                                               txn::LockMode mode = txn::LockMode::kExclusive);

    // **The borrow a write takes, before the header it judges is
    // interpreted** (AO-S6a, on the operator's choice of 2026-09-08) - one
    // chain for every unit, the declared one before the walk and a tuple
    // per row where nothing was declared. The intention first - relation
    // `IX`/`IS`, then the unit - because a unit borrowed without it is
    // invisible to a relation-level ask; a refused intention takes nothing
    // under it. A relation unit has no intention above it.
    //
    // **Non-queueing**: takes the unit when free, else answers `false`
    // with `*blocker` set and, where the dispatcher may park, a wake
    // registered on the refusing unit (`TakeLockWait`). The only non-OK
    // status is the cap's, a refusal no wait gets past (AO-R10, AR2-R4).
    //
    // A dispatcher with no lock table, or a null transaction, borrows
    // nothing and answers granted.
    //
    // `mode` is the unit's: `X` for a writer's row, range or fence, `S` for
    // a foreign key's parent row (D9(a), AY-S5). The intention asked above
    // it follows - `IX` over `X`, `IS` over `S`.
    StatusOr<bool> BorrowChain(const WriteScope& scope, const txn::LockKey& unit,
                               std::uint64_t* blocker = nullptr,
                               txn::LockMode mode = txn::LockMode::kExclusive);

    // **One wait per statement, and the registration of the one it drops
    // goes with it.** A statement can reach two asks that each want to
    // park: since AT-S5f the forward foreign-key check takes the parent
    // row's at the dispatch fork, and the relation's `IX` is asked after
    // it. `DispatchOutcome` carries one, so the second install replaces the
    // first - and a wake registration is the caller's to remove
    // (`lock_table.hpp`), with nothing else that will: an entry is erased
    // only when it has neither holder nor waiter, so one left behind keeps
    // its entry, its waiter and its per-release kick for the life of the
    // instance. `into` is the statement's relation wait (`lock_wait_`) or
    // its row wake (`blocking_wake_`, AX-S2b), on the same terms.
    void TakeLockWait(std::optional<DispatchOutcome::LockWait>& into,
                      DispatchOutcome::LockWait wait);


    // `{CREATE | DROP} NAMESPACE <name>` (AF-T3). One handler for both, as
    // the parser has one production - see `NamespaceStmt` (ast.hpp).
    DispatchOutcome HandleNamespace(std::string_view line, Session& session);

    // Both take the session so a `CREATE TABLE` inside an explicit
    // transaction stamps its catalog rows with that transaction's id and
    // registers them for rollback (ddl-transactional.md DT3b).
    DispatchOutcome HandleCreateTable(std::string_view args, Session& session);
    DispatchOutcome HandleCreateTableSql(std::string_view line, Session& session);

    // The duplicate-name check's answer, for **both** CREATE TABLE forms:
    // `EXISTS oid=` when a relation of `name` is on the page in
    // `namespace_oid` (a name is unique within its namespace, namespace.md
    // NS6), nullopt when the name is free there. Unfiltered, deliberately
    // (`ddl-transactional.md`'s duplicate check), and **not the answer that
    // binds**: `Catalog::CreateTable` asks again under `sys.objects`' held
    // root page (AT-S17), and a create that loses the name there comes back
    // through this so it answers as if it had arrived second.
    std::optional<DispatchOutcome> ExistingRelationReply(catalog::Oid namespace_oid,
                                                         std::string_view name);

    // The DDL half of a transaction: the id a catalog row should carry,
    // and where to put the rows it wrote so `ROLLBACK` can retire them.
    // Answers `kBootstrapXid` and a null sink outside an explicit
    // transaction, which is every pre-DT3b caller.
    struct DdlScope {
        std::uint64_t trx_id = catalog::kBootstrapXid;
        std::vector<catalog::CatalogRowRef> written;
        txn::Transaction* txn = nullptr;
        std::vector<catalog::CatalogRowRef>* sink() {
            return txn != nullptr ? &written : nullptr;
        }
    };
    // RV3-3: the scope-based sibling every DDL handler now uses. The
    // transaction comes from the WriteScope - explicit or the implicit
    // one BeginWrite opened (D2: autocommit DDL is a real transaction) -
    // and installing the catalog's undo hook happens here, so a handler
    // cannot write catalog rows a crash loser could not roll back. The
    // hook is uninstalled by FinishDdlStatement, every exit - and by
    // `InDdlStatement` before an index build it hands on first yields.
    DdlScope DdlScopeFor(WriteScope& scope);
    // `DdlScopeFor`'s hook alone, for `txn`: an index build's tail
    // installs it again, since its head's was uninstalled before the first
    // yield - the hook is the dispatcher's, and another session's statement
    // runs on this core while the build is away.
    void InstallDdlUndoHook(txn::Transaction* txn);
    // The one shape a DDL route may have: BeginWrite, the body, then
    // FinishDdlStatement on every exit - structural, so no route can
    // install the undo hook and leave it armed (review S4).
    template <typename Fn>
    DispatchOutcome InDdlStatement(Session& session, Fn&& body);
    // The tail every DDL route runs: uninstalls the undo hook, resolves
    // the write scope (commit/abort for an owned one), and for an owned
    // scope runs the DDL-resolution seam - cache invalidation and the §5d
    // purge - that explicit COMMIT/ROLLBACK reaches through EndDdlScope.
    void FinishDdlStatement(Session& session, WriteScope& scope, DispatchOutcome& out);
    // D1/D2's promise for the transactionless DDL statements (pattern,
    // assertion, cabin, ALTER): their records sync before the
    // acknowledgement - they have no commit record for the durability
    // class to ride on. Every route that writes without a WriteScope owes
    // this call on its success path; the .cpp says why D2 syncs rather
    // than batching.
    Status AwaitDdlDurability();
    // EndDdlScope's core, keyed by id: the session-based wrapper serves
    // explicit COMMIT/ROLLBACK, this serves an implicit DDL transaction
    // whose resolution EndWrite performed.
    //
    // `committed_lsn` is the commit's LSN on a commit arm and `kNoLsn` on
    // every other: a drop the transaction made is queued for reclamation
    // only on the first (BF-R9), and forgotten on either.
    void EndDdlScopeById(std::uint64_t txn_id, wal::Lsn committed_lsn = wal::kNoLsn);
    // BY-R2: records what an index DDL may orphan, settled at its decide.
    void NoteOwedIndex(const WriteScope& scope, catalog::Oid rel_oid, catalog::Oid index_oid,
                       bool created);
    // An index DDL's `sys.indexes` delete-mark, and any DDL's written
    // catalog row, handed to the transaction so a rollback undoes them;
    // each a no-op when nothing was written or the scope has no transaction.
    void NoteIndexRowDropped(DdlScope& ddl, const catalog::CatalogRowChange& change);
    void NoteRowWritten(DdlScope& ddl, const catalog::CatalogRowRef& row);

    // The view a statement resolves relation names under
    // (workplan-ddl-transactional.md DT3c), or `nullopt` for "see
    // everything" - which is the *fast* path and the common one.
    //
    // **A view is minted only while some transaction holds uncommitted
    // DDL.** With none in flight every catalog row is either a bootstrap
    // row or a committed one, so an unfiltered read is correct for every
    // reader - and a filtered read would cost a catalog page scan per
    // statement, because a filtered lookup deliberately bypasses the
    // shared cache (DT3). That is ddl-transactional.md §6's cache
    // decision, taken: pay for isolation only where isolation is at
    // stake. **Or while one was open when the statement head read the
    // count**, before it revalidated: one that closed since may have left
    // an entry the head did not drop.
    std::optional<txn::ReadView> ViewFor(Session& session);
    // `ViewFor`'s minting half, whatever the count says.
    txn::ReadView MintResolveView(Session& session);

    // **Every name resolution goes through this** (`ddl-transactional.md`
    // §4): `resolve(view)` runs under `ViewFor`'s answer, and once more under
    // a minted view when the fast path was taken and a DDL opened anywhere
    // meanwhile (`InstanceVisibility::DdlGeneration` moved) - the unfiltered
    // read may then have found the row that DDL wrote. A DDL moves the
    // generation before its first catalog write, so a lookup that saw the
    // row sees the move. With no session there is no view to resolve under.
    template <typename Fn>
    auto ResolveNames(Session* session, Fn&& resolve) {
        if (session == nullptr) return resolve(nullptr);
        const std::uint64_t generation =
            txn_ != nullptr ? txn_->visibility().DdlGeneration() : 0;
        std::optional<txn::ReadView> view = ViewFor(*session);
        if (after_view_for_test_) after_view_for_test_();
        auto answer = resolve(view.has_value() ? &*view : nullptr);
        if (!view.has_value() && txn_ != nullptr &&
            txn_->visibility().DdlGeneration() != generation) {
            view = MintResolveView(*session);
            answer = resolve(&*view);
        }
        return answer;
    }
    template <typename Fn>
    auto ResolveNames(Session& session, Fn&& resolve) {
        return ResolveNames(&session, std::forward<Fn>(resolve));
    }

    // **The statement boundary, taken exactly once per statement.**
    // Under READ COMMITTED a transaction re-mints its view at each
    // statement (`txn.md` §1); every route - `DESCRIBE`, `SHOW TABLES`,
    // `ALTER`, `DROP TABLE`, the FK parent lookup included - resolves under
    // it (DT3c). Latched, reset per statement, so two resolutions in one
    // statement never see different views.
    Status EnsureStatementBoundary(Session& session);
    bool statement_boundary_taken_ = false;

    // Registers everything `written` holds on the transaction's trail.
    // Called **even when the DDL failed**: rows written before the failure
    // are on the page either way, and a rollback that skipped them would
    // leave the half-built relation this feature exists to prevent.
    // The rollback trail for a DDL route that changes rows rather than
    // inserting them (`DROP TABLE`, `DROP NAMESPACE`). No-op outside an
    // explicit transaction.
    void NoteCatalogRowChanges(DdlScope& scope,
                               const std::vector<catalog::CatalogRowChange>& changed);

    void NoteDdlRows(DdlScope& scope);

    // This core's transactions that opened a DDL scope and have not been
    // decided. Each is counted once in the instance's open-DDL count
    // (`OpenDdl`), which is what `ViewFor` asks. Entries are removed when the
    // transaction resolves, by `EndDdlScope`.
    std::vector<std::uint64_t> ddl_txns_;
    void EndDdlScope(const Session& session, wal::Lsn committed_lsn = wal::kNoLsn);

public:
    // **The delete-mark purge's trigger** (BP-H11, ddl-transactional.md
    // §5d): core 0's 50 ms `system` tick. A sweep write-fetches every
    // catalog page even when it retires nothing, so it runs only while a
    // mark is pending and only when the pending count or the settlement
    // (`TransactionManager::SettlementNow`) moved since the last sweep. Core
    // 0 only, a placement. A failed sweep is logged; its marks stay exactly
    // as reachable as before.
    void SweepSettledDeleteMarks();

private:
    // Delete-marked catalog rows retired since mount by the horizon-gated
    // purge (ddl-transactional.md §5d), and the sweeps that ran. SHOW META
    // prints both; `catalog_marks_finalized` beside them counts the
    // previous mount's leftovers - these are this mount's own.
    std::uint64_t catalog_marks_purged_ = 0;
    std::uint64_t catalog_purge_sweeps_ = 0;
    // What the last sweep saw: a sweep runs again only when the settlement
    // or the pending count differs (`SweepSettledDeleteMarks`).
    struct PurgeSeen {
        txn::TransactionManager::Settlement settlement;
        std::uint64_t marks;
        bool operator==(const PurgeSeen&) const = default;
    };
    std::optional<PurgeSeen> purge_seen_;
    // Records that this transaction now holds uncommitted catalog rows, so
    // the schema word moves at its decide. Its DDL scope opened it already
    // (`DdlScopeFor`, `OpenDdl`).
    void MarkHoldsDdl(txn::Transaction& txn);
    // Adds the transaction to `ddl_txns_` and, the first time, to the
    // instance's open-DDL count, which is what turns on every core's
    // `ViewFor` filtering. `EndDdlScopeById` closes both.
    void OpenDdl(std::uint64_t txn_id);

    // `CREATE CABIN` / `DROP CABIN` (docs/spec/cabin.md §10). One handler
    // for both: they share a parse and a reply shape, and differ only in
    // which catalog call they reach. Takes the whole statement line rather
    // than a suffix, because the parser is what resolves the two
    // identifiers.
    DispatchOutcome HandleCabin(std::string_view line, Session& session);
    // Both statements' catalog half and reply, under whatever hold
    // `HandleCabin` took.
    DispatchOutcome RunCabinDdl(const parser::CabinStmt& stmt, catalog::Oid default_namespace);

    // `ALTER TABLE ... RENAME TO | RENAME COLUMN` (docs/spec/alter.md,
    // workplan ALT03). One handler for both forms, for HandleCabin's
    // reason; the AL4 assertion RESTRICT and the AL7 system-relation
    // refusal live here, before the catalog write.
    DispatchOutcome HandleAlter(std::string_view line, Session& session);
    // `ALTER TABLE ... DROP COLUMN` (BW-R6..R8): under the relation `X`, in
    // the statement's DDL transaction.
    DispatchOutcome HandleDropColumn(const parser::AlterStmt& stmt, Session& session);
    // `ALTER TABLE ... ADD COLUMN` (BW-R4, BW-R5, BW-R7): a nullable column
    // in the lowest free slot, under the relation `X`, in the statement's
    // DDL transaction.
    DispatchOutcome HandleAddColumn(const parser::AlterStmt& stmt, Session& session);
    // Every ALTER form's head: the relation resolved under the session's
    // view and its qualifier, a system relation refused (AL7).
    StatusOr<catalog::Oid> ResolveAlterTarget(const parser::AlterStmt& stmt, Session& session);
    // The two column forms' head: `ResolveAlterTarget`, then the relation `X`
    // taken for `scope`'s transaction (BW-R7).
    StatusOr<catalog::Oid> AlterColumnRelation(const parser::AlterStmt& stmt, Session& session,
                                               WriteScope& scope);
    // A parsed column definition as its `sys.columns` row at `pos`, or the
    // refusal (CREATE TABLE's and ADD COLUMN's one resolver).
    std::optional<DispatchOutcome> ColumnRowOf(const parser::ColumnDef& col, std::uint32_t pos,
                                               catalog::SysColumnRow& row);
    // BW-R8: the first object that depends on column `pos` of `oid`, named,
    // or nullopt. Asked under the relation `X`.
    StatusOr<std::optional<std::string>> DropColumnRestriction(catalog::Oid oid,
                                                               std::uint16_t pos);
    // What names `pos` of `oid` by position - an index key or covered
    // column, a foreign key's child column, a Transition, a column Cabin, an
    // expression Cabin's key: `DropColumnRestriction` less its assertion arm,
    // which resolves names and so cannot name a position. Asked by every
    // ADD COLUMN of the position it takes (CD-S4).
    StatusOr<std::optional<std::string>> PositionDependant(catalog::Oid oid, std::uint16_t pos);
    // Drops every column Cabin of `oid` on one of `positions` - each a
    // position no live column holds, so no statement can name it to drop it
    // (CD-S0 P5) - transactionless and durable, and returns their ids. The
    // clean's sweep of its dropped columns and an ADD's of the position it
    // takes (CD-S3, CD-S4).
    StatusOr<std::vector<std::uint64_t>> SweepColumnCabins(
        catalog::Oid oid, std::span<const std::uint16_t> positions);

    // `DROP TABLE <name>` (docs/spec/drop-table.md, workplan DT03). The
    // DT3 RESTRICT gate lives here - a referencing foreign key and an
    // assertion each refuse naming the blocker - before the catalog's
    // tombstone-and-retire; the in-memory Cabin sets are forgotten after.
    DispatchOutcome HandleDropTable(std::string_view line, Session& session);


    // Emits one INDEX_INSERT per index mutation, or one BTREE_SPLIT of the
    // pages a split restructured, with the anchor when it grew a root.
    // Called **before** the HEAP_INSERT or HEAP_OVERWRITE the entries point
    // at (docs/spec/index.md §12.1): a dangling entry is dropped by
    // verification, a row with no entry is lost.
    Status LogIndexWrites(const std::vector<exec::IndexWrite>& writes, std::uint64_t txn_id);
    Status LogIndexWrite(const exec::IndexWrite& write, std::uint64_t txn_id);

    // What `MaintainIndexes` hands each write to (AT-S21). With a manager
    // owning the transaction the write is logged there and then, under the
    // index tree's hold, with the transaction's id; without one - the
    // own-transaction configuration, whose `TXN_BEGIN` `LogInsert` writes -
    // it is collected into `pending` for `LogInsert`, as before. Null with
    // no log.
    exec::IndexWriteLog IndexWriteLogFor(const WriteScope& scope,
                                         std::vector<exec::IndexWrite>& pending);

    // `VarHeapSink::on_append` for a row `tuple_key` of `rel_oid` (AT-S21's
    // review): with a manager owning the transaction, the spill's undo record
    // (`NoteSpills`) and then its records (`exec::LogSpill`), under the
    // var-heap page's hold. Null without a manager, whose spills are
    // collected and logged by `LogInsert`.
    std::function<Status(const exec::AppendedSpill&)> SpillLogFor(const WriteScope& scope,
                                                                  std::uint32_t rel_oid,
                                                                  std::uint64_t tuple_key);

    // `CREATE INDEX` / `DROP INDEX` (docs/spec/index.md §10). One handler
    // for both, for HandleCabin's reason. Both take the relation `X` before
    // their first catalog write and build where the session is (AT-S5e).
    DispatchOutcome HandleIndex(std::string_view line, Session& session);

    // `CREATE ASSERTION` / `DROP ASSERTION` (docs/spec/assertion.md §3,
    // workplan AST03). One handler for both, for HandleCabin's reason.
    //
    // Validates the declaration against the catalog (§3.1), builds the
    // Bound Cabin (AST06), publishes the row and adopts the live directory
    // into the instance's registry, which is what makes the reply's
    // `enforcing=1` true rather than a claim about a row.
    //
    // **Built where the session is** (AT-S5d): the registry is one and
    // every core writes. Non-transactional DDL (`ddl-transactional.md` §5): the
    // session is read only to tell its own transaction's `IX` from another
    // writer's when the build's relation `X` is refused (AT-S5e).
    DispatchOutcome HandleAssertion(std::string_view line, Session& session);

    // `CREATE TRANSITION` / `DROP TRANSITION` (BV-S2,
    // `instructions/v3.2.0/workorder-bv-transition-constraints.md`): under
    // the relation `X` of a transaction of the statement's own, as
    // `CREATE ASSERTION`'s build is (BV-R4), and committed at the statement
    // like it - never with the session's transaction. `SHOW TRANSITIONS` is
    // `SELECT * FROM sys.transitions` (BO's SV1).
    DispatchOutcome HandleTransition(std::string_view line, Session& session);

    // **The relation `X` a declaration's build holds** (AT-S5e, AT-0 item
    // 13), shared by `HandleAssertion` and `HandleTransition` - and by
    // `HandleAlter`'s two renames (`alter.md` AL4a): a
    // transaction of the statement's own that writes nothing and is rolled
    // back when the lock goes out of scope, so a refused build poisons
    // nothing (a cancelled one fails an open transaction, as any cancelled
    // statement does) - a transaction rather than a minted id because a
    // waiter names its holder. No manager or lock table: no lock, which is the pre-MVCC
    // engine every socket-free test runs on. Returns the refusal, if any.
    struct DdlBuildLock {
        txn::TransactionManager* manager = nullptr;
        txn::Transaction* txn = nullptr;
        // The build's report to its waiters' fault net, registered before
        // the ask, so no waiter parks on this holder before it. A member,
        // so it is withdrawn after the destructor's release: a waiter never
        // finds the holder silent while it still holds.
        std::optional<txn::HolderProgress::Scope> progress;
        DdlBuildLock() = default;
        DdlBuildLock(const DdlBuildLock&) = delete;
        DdlBuildLock& operator=(const DdlBuildLock&) = delete;
        ~DdlBuildLock();
        // The release, then the registration withdrawn; idempotent. A build
        // handed on ends it in its tail (A1 stage 2), the rest at their
        // scope's end.
        void End();
    };
    // Latest settled state, the view a declaration's build and its name
    // resolve under: `MintCheckView` with no writer, or everything with no
    // manager (the pre-MVCC engine every socket-free test runs on).
    txn::ReadView SettledView() const;
    std::optional<Status> TakeDdlBuildLock(Session& session, catalog::Oid relation,
                                           std::string_view relation_name, std::string_view verb,
                                           DdlBuildLock& out);
    DispatchOutcome HandleSetCabinOptimizer(std::string_view rest);


    // ---- Foreign-key checks (docs/spec/foreign-keys.md §§2-4) -----------
    //
    // The write paths' three entry points. They live here rather than in
    // `exec/` because they are what turns a verdict into a *reply* - which
    // needs relation names, the access-statistics switch, and the retryable
    // spelling - while the verdicts themselves are `exec::fk_check`'s, so
    // there is exactly one implementation of each check.

    // A read view of **now**, for a constraint check. See §4: not the
    // statement's snapshot, because a check reads latest state.
    txn::ReadView CheckView(const WriteScope& scope);

    // A refused parent-row hold's status, restated as the foreign key's
    // (a conflict names the child and parent relations; any other code
    // stands).
    Status ParentRowHeld(const catalog::TableAccess& child, catalog::Oid parent_rel,
                         std::uint64_t tuple_key, const Status& held);

    // The forward check for one foreign key and one written value (§2),
    // **answered from what the extraction pass already resolved** (§2a,
    // AH-T1). OK when the value is not an id at all - the row codec has the
    // better error for that. A violation gives back the `S` of every parent
    // the statement resolved as absent, whoever took it (AZ-S5, AZ-R5 as
    // amended 2026-10-02; `foreign-keys.md` §2c).
    Status CheckForeignKeyOnWrite(const catalog::TableAccess& child,
                                  const catalog::ForeignKeyRef& fk, const parser::AstValue& value,
                                  const exec::FkParentVerdicts& held, const WriteScope& scope);

    // The extraction pass (§2a, AH-R1): resolves every parent tuple key one row's
    // body names, into `into`, deduplicated by (parent relation, tuple key) so a
    // statement naming one parent from a thousand rows descends once.
    //
    // Called **before any row work** - at the dispatch fork for a statement
    // whose rows are all known there, and once per row otherwise. Nothing it
    // does may depend on a row having been written, which is what makes it
    // legal to run early - and why no foreign key references its own
    // relation (`catalog::RefuseSelfReference`).
    // **Each parent is held before it is descended** (D9(a), AY-S5): `IS`
    // on its relation and `S` on its row, into `scope`'s transaction until
    // it decides, and a refused `S` is a wait. The descent reads a view
    // minted after the grant.
    Status ResolveForeignKeyParents(const catalog::TableAccess& child,
                                     const std::vector<parser::AstValue>& body,
                                     exec::FkParentVerdicts& into, const WriteScope& scope);


    // The body `ResolveForeignKeyParents` and the FK checks index into: the
    // columns after the tuple key, which is the shape every downstream
    // consumer takes - a positional row's and a column-listed row's alike,
    // since `NormalizeInsertRows` maps a listed row onto it first (BW-S6).
    // Mirrors `InsertOneRow`'s arity split without repeating its refusals -
    // a row of neither legal length yields an empty body here and is
    // refused there, in the order it always was.
    static std::vector<parser::AstValue> InsertBodyOf(
        const catalog::TableAccess& ta, const std::vector<parser::AstValue>& values);

    // The reverse check for every foreign key pointing at `parent` (§3),
    // run per row about to be delete-marked.
    // Every child is walked here: the answer is this core's for the whole
    // relation, so there is nothing resolved elsewhere to read.
    //
    // Called with the parent row's `X` held; the check view is minted
    // inside, after that grant (the body says why).
    //
    // A busy answer naming a child writer records a wait on it (AY-Q8,
    // `WaitForChildRowWriter`), which the caller parks its walk on.
    Status CheckNoChildrenBeforeDelete(const catalog::TableAccess& parent, std::uint64_t parent_tuple_key,
                                       const WriteScope& scope);

    // **The reverse check's wait** (AY-Q8): the child row `tuple_key` of
    // `child_rel` is being written by `holder`, which holds no `S` on the
    // parent. Registers a wake on that row where a table and a reactor exist,
    // and records the block.
    void WaitForChildRowWriter(const WriteScope& scope, catalog::Oid child_rel, std::uint64_t tuple_key,
                               std::uint64_t holder);

    // One access shape, recorded by hand because a check is not a step
    // (FK-M4). Never fails a write.
    void RecordFkAccess(exec::AccessKind kind, catalog::Oid rel_oid, std::uint64_t column_mask);

    // The namespace a `CREATE TABLE ns.t` names, or the session's (`USE`)
    // for an unqualified name (namespace.md NS7): it decides the namespace
    // the relation is created in. An unknown namespace is refused with its
    // byte and is **not** created - see `ast.hpp`'s namespace-qualifier
    // rule.
    StatusOr<catalog::Oid> ResolveCreateNamespace(std::string_view qualifier,
                                                  std::uint32_t byte_offset,
                                                  const Session& session,
                                                  const txn::ReadView* view);

    // A relation's name for a human-readable reply, or `oid=<n>` when it
    // cannot be resolved. Inspection surfaces only: catalog rows store oids
    // so they stay fixed width, and printing one is where the name is
    // needed. Never called from an execute path - resolving a name during
    // execution is what parser-v2.md I11 forbids.
    std::string RelationNameOf(catalog::Oid oid);

    DispatchOutcome HandleInsert(std::string_view line, Session& session);

    // The statement itself, inside a write scope the wrapper opened and
    // will close. Split so that every early return below is an ordinary
    // return rather than one that has to remember to end a transaction.
    DispatchOutcome InsertInner(std::string_view line, WriteScope& scope);

    // Where one row landed, for the reply.
    struct InsertRowResult {
        std::uint64_t id = 0;
        PageId page_id = kInvalidPageId;
        std::uint16_t slot = 0;
    };

    // Whether a btree slot keyed `id` makes a named key a duplicate now
    // (BD-R5): `AlreadyExists` at `byte` when one is there, except one an
    // undecided insert of a transaction other than `own` wrote, whose
    // rollback frees it (BD-Q9, a wait); OK when absent. `own`'s own version
    // is a duplicate whatever it is: no decide of another frees it.
    Status NamedKeyVerdict(const catalog::TableAccess& ta, std::uint64_t id, std::uint32_t byte,
                           const txn::Transaction* own);
    // ON CONFLICT DO NOTHING's question (BU-Q18): is `id` placed? Takes the
    // key's row unit as a named-key insert does, waiting as it waits, and
    // keeps it only when the answer is no.
    StatusOr<bool> TupleKeyPlaced(const catalog::TableAccess& ta, std::uint64_t id,
                                  std::uint32_t byte, WriteScope& scope);

    // **What an `ON CONFLICT` reacts to** (CG-Q1 (b)): the tuple key
    // (`index` null), or the one column of a unique index.
    struct ConflictArbiter {
        const catalog::TableAccess::IndexRef* index = nullptr;
    };
    StatusOr<ConflictArbiter> ResolveConflictArbiter(const catalog::TableAccess& ta,
                                                     const parser::InsertStmt& written);

    // **`ON CONFLICT DO UPDATE`, and `DO NOTHING` on a unique index** (CG):
    // each row's verdict - insert, update the holder, skip, or refuse - taken
    // and acted on before the next row's, so a unique key's unit is never
    // held across two rows (`UniqueKeyHolds`). The insert arm is
    // `InsertOneRow`; the update arm is `UpdateParsed` on the holder, under
    // its row `X`, with `EXCLUDED.<col>` bound to the proposed row.
    DispatchOutcome InsertOnConflict(catalog::Oid oid, const catalog::TableAccess*& ta,
                                     const parser::InsertStmt& written,
                                     const parser::InsertStmt& shaped, ConflictArbiter arbiter,
                                     WriteScope& scope);
    // Whether `tuple_key`'s slot holds a live row whose `ix` key is `key` -
    // the update arm's re-read under the row's `X` (CG-S0 reading 1b).
    StatusOr<bool> RowHoldsUniqueKey(const catalog::TableAccess& ta,
                                     const catalog::TableAccess::IndexRef& ix,
                                     std::uint64_t tuple_key, const exec::UniqueKey& key);

    // The per-row write pipeline, verbatim and in order (bulkinsert.md
    // §4, BI2): arity, FK forward check, assertion admission, id, encode +
    // spill, placement, Cabin witness, index maintenance, reservation,
    // rollback trail, WAL, root repoint. **A refactor of InsertInner's
    // body, not a second write path** - there is exactly one place a row
    // becomes durable state, and it is this one for one row and for a
    // thousand. Returns the full error reply on failure (spellings intact -
    // ErrorReply's leading tokens are a compatibility surface, which is why
    // the bulk loop appends its row ordinal rather than prefixing it).
    //
    // `ta` is a live borrow the callee may *refresh*: a btree level growth
    // repoints the relation's root, which invalidates the catalog cache -
    // harmless on the last row, fatal to the next one, so the pointer is
    // re-borrowed before returning.
    //
    // `fk_held` is what the caller's extraction pass resolved over **every**
    // row of the statement (§2a): this function answers from it and starts
    // no descent of its own, which is what makes a bulk insert against one
    // parent cost one.
    //
    // A refusal comes back as its `Status`, rendered by the caller (BB-S3):
    // the code it carries reaches the wire, not only its line.
    std::optional<Status> InsertOneRow(catalog::Oid oid, const catalog::TableAccess*& ta,
                                       const std::vector<parser::AstValue>& values,
                                       WriteScope& scope, const exec::FkParentVerdicts& fk_held,
                                       InsertRowResult& out);

    // The already-parsed half of InsertInner: everything after the parse -
    // cap, manager guard, resolution, affinity, the row loop.
    // Split out for the KWP load session (docs/inflight/in-progress/workplan-kwp-load.md KW5),
    // whose rows arrive binary and never had text - BI2's "same write
    // path" made literal, since this IS the path a T1 statement takes.
    // **No `line`**: an INSERT reaches this path the way a KWP load chunk
    // always did, with no text.
    DispatchOutcome InsertParsed(const parser::InsertStmt& stmt, WriteScope& scope);

public:
    // KW5's public seam: run one parsed INSERT under `session` exactly as
    // HandleInsert runs a textual one - same write scope, same verdict
    // rule, same atomicity. The load session synthesizes an InsertStmt per
    // chunk and calls this.
    DispatchOutcome ExecuteInsert(const parser::InsertStmt& stmt, Session& session);
    // The load's bind (`C_LOAD_BEGIN`): `qualifier.name`, or a bare `name` in
    // the session's namespace, resolved under `session`'s view through
    // `ResolveNames`, so a relation the session cannot see is not answered
    // READY.
    StatusOr<catalog::Oid> ResolveRelationName(Session& session, std::string_view qualifier,
                                               std::string_view name);

    // For the sibling platform layers (the KWP load endpoint's schema
    // reads). The catalog's own discipline applies unchanged.
    catalog::Catalog& catalog() noexcept { return catalog_; }

private:
    // `analyze` switches the reply from rows to the compiled plan plus
    // the per-step counters the run produced. Everything before that -
    // parse, compile, execute - is the same code on the same statement
    // text, which is the point: an ANALYZE that took a different path
    // would describe a run nobody performed.
    //
    // `line` is always the *stripped* statement, never the ANALYZE-
    // prefixed text. Dispatch() strips the keyword before anything sees
    // the line, so a fingerprint taken anywhere below here is the same
    // one the unprefixed statement would produce - which is what keeps
    // `sys.patterns` and a Waystone trail from splitting in two over a
    // diagnostic prefix.
    //
    // `pinned`, when given, is the snapshot the SELECT reads at instead of one it
    // mints: a scalar subquery of an UPDATE reads at the UPDATE's own (BJ-S7),
    // so the statement sees one state of the database. No lease comes with it -
    // the transaction the UPDATE holds keeps the horizon beneath it for as long
    // as the subquery runs. `context` is pinned with it: the subquery's
    // `NOW()` is the UPDATE's.
    DispatchOutcome HandleSelect(std::string_view line, Session& session, bool analyze = false,
                                 const txn::Snapshot* pinned = nullptr,
                                 const exec::StatementContext* context = nullptr);

    // Everything `HandleSelect` does once `run` holds its snapshot and its
    // parsed statement: the compile, the trail, the walk or its hand-off.
    // `line` is the statement's text, empty for a query layer walk, which
    // has no text of its own - and so no parser, no fingerprint and no trail
    // (BU-Q12) - and is never handed back to the layer.
    DispatchOutcome SelectBody(const std::shared_ptr<SelectRun>& run, Session& session,
                               bool analyze, const exec::StatementContext* context,
                               std::string_view line);

    // The query layer's host (`exec::LayerHost`): a relation's description
    // and its walks, at the statement's snapshot.
    class LayerHostImpl;

    // **The query layer's door** (BU-S4, BU-Q9): `line` as the query
    // grammar reads it, run by `exec::RunQuery` at `snap`, its reply emitted
    // through the session's sink. nullopt when the query grammar refuses
    // the text too, and the caller's native refusal is then the answer.
    std::optional<DispatchOutcome> TryQueryLayer(std::string_view line, Session& session,
                                                 const txn::Snapshot& snap,
                                                 const exec::StatementContext* context);
    // RETURNING (BU-Q17): when `line` - an INSERT, UPDATE or DELETE that
    // answered `out` - has a RETURNING list, the rows this statement wrote,
    // found on its transaction's trail since the statement's mark and read
    // back by tuple key, emitted through the session's sink. Read before
    // the write scope ends, so a failure here fails the statement.
    DispatchOutcome WithReturning(std::string_view line, Session& session, WriteScope& scope,
                                  DispatchOutcome out);
    StatusOr<parser::InsertStmt> InsertSelectRows(const catalog::TableAccess& ta,
                                                  const parser::InsertStmt& written,
                                                  WriteScope& scope);
    // Runs a query-layer query whose text is `text` at `snap`, under this
    // dispatcher's row and group caps (BU-Q10): TryQueryLayer's and
    // INSERT ... SELECT's one way in.
    StatusOr<exec::RowSet> RunLayerQuery(const parser::Query& query, std::string_view text,
                                         Session& session, const txn::Snapshot& snap,
                                         const exec::StatementContext& context);

    // One relation's walk for the query layer: `stmt` - a single-relation
    // `SELECT *` with native conjuncts - run through `SelectBody` at `snap`,
    // its rows captured as values.
    StatusOr<std::vector<std::vector<parser::AstValue>>> WalkForLayer(
        parser::SelectStmt stmt, Session& session, const txn::Snapshot& snap,
        const exec::StatementContext& context);

    // Runs the walk a `DispatchAndStage` reply left in `out->select_run`
    // and writes the reply over `out` (BA-S15). `DispatchAsync` only.
    sched::Coro CompleteSelect(std::string_view line, DispatchOutcome* out);

    // Runs the walk and the tail a `DispatchAndStage` reply left in
    // `out->build_run` and writes the reply over `out` (A1 stage 2).
    // `DispatchAsync` only.
    sched::Coro CompleteBuild(std::string_view line, DispatchOutcome* out);

    // `DispatchAndStage`'s statement frame around `body(session)`: the
    // per-statement members cleared, the statement epoch published, this
    // core declared, and what the body left in the members carried onto its
    // outcome. Also the frame a relation-X build's tail (`BuildRun`) runs
    // in, after its
    // head's has closed (`BuildRun`).
    template <typename Body>
    DispatchOutcome StageStatement(Session* session, Body&& body);

    // `DispatchInner`'s per-statement stamps - the boundary not yet taken,
    // the commit's durability class - which a relation-X build's tail
    // (`BuildRun`) takes again, since other statements of this core ran since its head.
    void StampStatement(Session& session);

    // The statement log line `DispatchAndStage` writes, split out so a
    // `SELECT` whose walk ends after its head is logged when it ends.
    void LogStatement(std::string_view line, const DispatchOutcome& outcome,
                      sched::MonoTimeNs started_ns) const;

    // The ANALYZE reply: run the chain for its counters, print the plan
    // beside them. Split out so HandleSelect's row-formatting path and
    // this one visibly share everything above the sink.
    //
    // `trail` and `replay` are the same two halves an ordinary execution
    // gets. ANALYZE takes them because its contract is that the run it
    // describes is the run that actually happened: a diagnostic that
    // skipped replay would report descents no real execution performs.
    //
    // It takes no statement text: the `pattern_id` and `fetch_id` it
    // prints come from `identity`, which the caller got from the parse.
    //
    // `borrow` is the statement's read borrow, taken at the bind by
    // `HandleSelect` (AT-R1) rather than constructed here: a borrow made at
    // execution would be made after the compile it exists to protect.
    DispatchOutcome RunAnalyze(const exec::StepChain& chain, exec::TrailCollector* trail,
                               const exec::TrailReplay* replay,
                               const std::optional<StatementIdentity>& identity,
                               const txn::Snapshot& snapshot, exec::PositionSink& borrow,
                               exec::OutputSort& sorter);

public:
    // AG11's caps, from `aggregate_max_groups` / `aggregate_max_distinct`.
    //
    // A setter rather than a fifteenth constructor parameter: the ceiling
    // is read once at boot and never varies per statement, so it does not
    // need to be threaded through every test's construction - and the
    // defaults are the spec's `[PROPOSED]` numbers, so a dispatcher that is
    // never told behaves exactly as the documented configuration does.
    void set_aggregate_limits(exec::AggregateLimits limits) noexcept {
        aggregate_limits_ = limits;
    }

    // `sort_max_rows`, from the config. A setter for the same reason.
    void set_sort_max_rows(std::size_t rows) noexcept { sort_max_rows_ = rows; }
    // Set on the statement budget template directly: the knob rides
    // `Budget` into every runner and sub-chain (exec/budget.hpp), so the
    // dispatcher needs no member of its own for it.
    void set_join_build_max_rows(std::size_t rows) noexcept {
        budget_.set_join_build_max_rows(rows);
    }
    // `join_order` (CB-S2): false keeps every join in its written order.
    void set_plan_join_order(bool planned) noexcept { plan_join_order_ = planned; }
    // The join planner's statistics store, or null - every hand-built
    // dispatcher, which then plans data-free and folds nothing.
    void set_plan_stats(stats::PlanStats* plan_stats) noexcept {
        plan_stats_ = plan_stats;
        plan_source_ = stats::PlanStatsSource(plan_stats);
    }

    // All of them at once, as an instance hands them to each core (AT-S8,
    // `StatementLimits`).
    void set_statement_limits(const StatementLimits& limits) noexcept {
        indexes_enabled_ = limits.indexes;
        max_insert_rows_ = limits.max_insert_rows;
        set_aggregate_limits(limits.aggregate);
        set_sort_max_rows(limits.sort_max_rows);
        set_join_build_max_rows(limits.join_build_max_rows);
        budget_.set_scan_kernel(limits.scan_kernel);
        set_plan_join_order(limits.plan_join_order);
    }



    // The instance's reclaim counters (BF-R11), which `SHOW META` prints on
    // every core; null - every hand-built dispatcher - prints none of the
    // reclaim half. `counters` must outlive this.
    void SetReclaimCounters(const ReclaimCounters* counters) noexcept {
        reclaim_counters_ = counters;
    }

    // **Reclaim within the run** (BF-R9): the instance's statement-epoch
    // slots, which every statement head on this core publishes into, and
    // the reclaimer a committed drop is queued with. Null - every
    // hand-built dispatcher - publishes nothing and queues nothing, so a
    // drop is reclaimed at the next mount (BF-R8). Both must outlive this.
    void SetStatementEpochs(StatementEpochs* epochs) noexcept { epochs_ = epochs; }
    void SetReclaimQueue(PageReclaimer* reclaimer) noexcept { reclaim_queue_ = reclaimer; }

    // **A statement head** (BF-R9): publishes `kEntering` in this core's
    // slot, revalidates the catalog, then publishes the word the cache was
    // revalidated at; the slot goes idle when the scope ends. A nested head
    // - a statement run from inside another's - leaves the outer one's
    // publication standing. Every `DispatchAndStage` is one; the KWP load
    // endpoint's two handlers, which bind outside it, take one each.
    class StatementEpochScope {
    public:
        explicit StatementEpochScope(CommandDispatcher& dispatcher);
        ~StatementEpochScope();
        StatementEpochScope(const StatementEpochScope&) = delete;
        StatementEpochScope& operator=(const StatementEpochScope&) = delete;

    private:
        CommandDispatcher& d_;
    };

    // **A seam in the statement head** (BF-R12), at one of three points:
    // inside `Revalidate`, just after it reads the word - the point that
    // tells "published before the read" from "after" -, between
    // `Revalidate` and the publish - the window a reclaim must treat as a
    // statement holding an old memo, seen as `kEntering` - or after the
    // publish, where the slot holds the word that memo was built at. Set
    // and cleared while no statement runs; empty in production.
    enum class EpochSeam { kAfterRead, kBeforePublish, kAfterPublish };
    void SetEpochSeamForTest(EpochSeam at, std::function<void()> hook) {
        epoch_seam_at_ = at;
        epoch_seam_for_test_ = std::move(hook);
    }

    // **A seam in `PURGE`** (BH-S4), between its judging and its writing:
    // what another core does there is the race PU12 settles under the
    // leaf's latch - a named `INSERT`, another `PURGE`, a purge and a
    // re-insert of the judged key. Set and cleared while no statement runs;
    // empty in production.
    void SetPurgeSeamForTest(std::function<void()> hook) {
        purge_seam_for_test_ = std::move(hook);
    }

    // **A seam in `DROP TABLE`** (BF-Q11 (a)), between its first RESTRICT
    // ask and its `X` request: a foreign key created there must still
    // refuse the drop, through the second ask. Empty in production.
    void SetBeforeDropExclusiveForTest(std::function<void()> hook) {
        before_drop_exclusive_for_test_ = std::move(hook);
    }

    // **A seam in `DROP COLUMN`** between its RESTRICT ask and the catalog
    // door's write: a RENAME COLUMN landing there must not move the drop
    // onto another column. Empty in production.
    void SetBeforeDropColumnWriteForTest(std::function<void()> hook) {
        before_drop_column_write_for_test_ = std::move(hook);
    }

    // **A seam after an `UPDATE` wrote a row** (BJ-R9), handed the row's tuple key:
    // the application count a non-idempotent `SET` makes load-bearing. A
    // literal `SET` is idempotent, so a path that applied it twice to one
    // row was invisible; the cell counts the calls instead of reading a
    // value. The hook runs on the reactor of the statement, inside its walk.
    // Empty in production.
    void SetAfterUpdateRowAppliedForTest(std::function<void(std::uint64_t)> hook) {
        after_update_row_applied_for_test_ = std::move(hook);
    }
    // **A seam between `ViewFor`'s answer and the resolution it serves**
    // (`ResolveNames`): where another core's DDL opening lands in the
    // window the generation re-check closes. Empty in production.
    void SetAfterViewForTest(std::function<void()> hook) {
        after_view_for_test_ = std::move(hook);
    }

    // The access-batch setters (AT-S7) and R6-5's in-doubt ceiling (AT-S6)
    // retired; their history is at cf889b63. A writer's wait is the lock
    // family's, under `lock_wait_fault_net_ms`, and the old key is refused
    // at startup naming it.
    // **The instance lock table** (AO-R2), borrowed and null until an
    // expeditor hands one over. Every borrow, wait and wait-for edge this
    // dispatcher takes is in it; null - a fixture's shape - borrows nothing,
    // and only a transaction holding nothing waits (AO-S3's guard), so no
    // cycle can form without a detector.
    void set_locks(txn::LockTable* locks) noexcept { locks_ = locks; }
    // The statements `max_locks_per_txn` refused since this dispatcher was
    // built - a refusal, never a truncation (AO-S6c-c).
    std::uint64_t borrow_cap_stops() const noexcept { return borrow_cap_stops_; }

    // **Relations declared** (AO-S6e-b; at the bind since AT-S1): one per
    // relation per statement that was granted the relation `IS` the read
    // borrow is made of - a join counts two, and a write counts its own
    // relation. Not a wire counter and not a `SHOW META` block - it is what
    // a cell reads to say the borrow was taken at all, since a local read
    // is synchronous and nothing else can observe it while it runs. A
    // relation refused is read on and not counted, which is the
    // distinction that matters: this counts declarations granted, not
    // reads performed.
    std::uint64_t read_borrows() const noexcept { return read_borrows_; }
    // Which table this dispatcher records its edges in, so an assembly cell
    // can name it (AO-S4b).
    const txn::LockTable* locks() const noexcept { return locks_; }

    // **The lock family's fault net**, from `lock_wait_fault_net_ms`: the
    // retired in-doubt ceiling's quantity, re-scoped (AT-0 item 7).
    sched::MonoTimeNs LockWaitFaultNetNs() const noexcept { return lock_wait_fault_net_ns_; }
    void set_lock_wait_fault_net_ns(sched::MonoTimeNs ns) noexcept {
        lock_wait_fault_net_ns_ = ns;
    }


    // The cabin optimizer's signal collector (workplan PHY01), a setter
    // for `set_aggregate_limits`'s reason. Null - every existing construction site,
    // and any configuration without the collector - records nothing and
    // costs one predicate per successful SELECT.
    void set_optimizer_signals(stats::OptimizerSignals* signals) noexcept {
        optimizer_signals_ = signals;
    }

    // PO8's switch, boot half (workplan PHY05): the config key seeds it,
    // SET CABIN_OPTIMIZER flips it at runtime, SHOW META reports it. The
    // consumer is PHY04's cadence task, which reads it at every batch
    // boundary.
    void set_cabin_optimizer_enabled(bool enabled) noexcept {
        cabin_optimizer_on_->store(enabled, std::memory_order_relaxed);
    }
    bool cabin_optimizer_enabled() const noexcept {
        return cabin_optimizer_on_->load(std::memory_order_relaxed);
    }

    // Every piece of the optimizer surface at once, as an instance hands it
    // to each core (AT-S8, `OptimizerSurface`).
    void set_optimizer_surface(const OptimizerSurface& surface) noexcept {
        set_optimizer_signals(surface.signals);
        set_plan_stats(surface.plan_stats);
        if (surface.cabin_optimizer_on != nullptr) cabin_optimizer_on_ = surface.cabin_optimizer_on;
        set_cabin_optimizer_view(surface.controller, surface.executor);
        cabin_optimizer_view_latch_ = surface.view_latch;
    }


    // What the mount's recovery did, for `SHOW META` (RC09). A pointer into
    // the report the mount owns - `Expeditor::recovery_`, which outlives this
    // dispatcher - and null everywhere that mounts nothing, where SHOW META
    // then omits the block rather than printing zeroes that read as "recovery
    // ran and found nothing".
    void set_recovery(const MountRecovery* recovery) noexcept { recovery_ = recovery; }

    // The instance's backup control (BR-S2), which `BACKUP` runs through and
    // `SHOW META` reads. `Expeditor`'s, outliving every dispatcher; null
    // where nothing mounts a volume, and `BACKUP` is then refused.
    void set_backup(BackupControl* backup) noexcept { backup_ = backup; }

    // The instance's WAL archive (BR-S4), null when archiving is off: its
    // counters go into `SHOW META`, and while it is failing with the WAL
    // filesystem near full every statement that writes the log is refused
    // (BR-Q2 (a)).
    void set_archiver(const WalArchiver* archiver) noexcept { archiver_ = archiver; }

    // This core's reactor, for `SHOW META`'s group-accounting block
    // (`docs/spec/sched.md` §4's last bullet). It outlives this dispatcher on
    // every core. Null wherever no reactor runs the dispatcher - every
    // socket-free test - and the block is then omitted rather than printed
    // as zeroes, the rule the recovery block already follows.
    void set_scheduler_view(const sched::Scheduler* scheduler) noexcept {
        scheduler_view_ = scheduler;
    }

    // The assertion registry, exposed for the two things only a mount does:
    // refilling it after recovery (RC07's `ResumeAssertionsAfterRecovery`) and
    // handing it to the checkpointer as AS6a's snapshot source. Every other
    // caller reaches assertions through the write paths on this class, which is
    // why this is the only accessor and why it is not const.
    exec::AssertionEnforcer& assertions() noexcept { return *enforcer_; }

    // **The instance's registry** (AT-S5d, AT-R15), borrowed the way the lock
    // table is: `Expeditor` owns one for every core and hands it here, so a
    // write on any core is checked against the one directory each assertion
    // has. Never called leaves this dispatcher its own, which is a fixture's
    // shape and a one-core instance's. `enforcer` must outlive the dispatcher,
    // and the swap happens before the first statement - nothing reserved into
    // the registry it replaces.
    void set_assertions(exec::AssertionEnforcer* enforcer) {
        if (enforcer != nullptr) {
            owned_enforcer_.reset();
            enforcer_ = enforcer;
        } else if (owned_enforcer_ == nullptr) {
            owned_enforcer_ = std::make_unique<exec::AssertionEnforcer>();
            enforcer_ = owned_enforcer_.get();
        }
        ArmAssertionRecordBudget();
    }


    // The view's two sources (workplan PHY06), a setter for
    // `set_optimizer_signals`'s reason. Both null - every construction
    // site without the controller - and `SHOW CABIN_OPTIMIZER` then
    // reports the surface as absent rather than printing zeros wearing a
    // fresh face (SHOW ASSERTIONS' rule).
    void set_cabin_optimizer_view(const stats::CabinOptimizer* controller,
                                  const exec::CabinOptimizerExecutor* executor) noexcept {
        cabin_controller_ = controller;
        cabin_executor_ = executor;
    }

private:
    // The aggregated SELECT path (docs/spec/aggregate.md AG1): the same
    // execution, with an `Aggregator` in the sink and the fold's output
    // emitted after it. `header` is the column-heading line the caller
    // already built.
    //
    // A sibling of RunAnalyze rather than a branch inside the row loop: the
    // two differ only in what consumes the rows, and a per-row `if` would
    // be paid on every row of every statement.
    // `os` is the caller's buffer, already holding the column-heading line,
    // so no second `std::ostringstream` is built (measured as most of the
    // fold's per-statement overhead, AP03).
    // `sink` is where the rows go; `text_sink` is the same object when
    // nothing else was installed, and is what the reply is taken from.
    // `borrow` as on `RunAnalyze` above (AT-R1). The sort and the counters
    // each takes are the statement's pooled scratch (`SelectScratch`).
    DispatchOutcome RunAggregated(ResultSink& sink, TextResultSink& text_sink,
                                  const exec::StepChain& chain, exec::TrailCollector* trail,
                                  const exec::TrailReplay* replay,
                                  const std::optional<StatementIdentity>& identity,
                                  const txn::Snapshot& snapshot, exec::PositionSink& borrow,
                                  exec::ExecStats& stats);

    // **The success-path recording point.** Three collectors observe the
    // same moment - a completed execution - and they are called from one
    // place so a fourth cannot be added to two of the three sites. Every
    // caller reaches here only after the execution succeeded; there is
    // deliberately no failure-path form (see RecordTrail).
    // Folds a planned block's complete run into the planner's statistics
    // (docs/spec/plan-stats.md PS2, PS3).
    void RecordPlanStats(const exec::StepChain& chain, const exec::ExecStats& stats);
    void RecordExecution(const std::optional<StatementIdentity>& identity,
                         exec::TrailCollector* trail, const exec::StepChain& chain,
                         const exec::ExecStats& stats);

    // Hands a successful execution's trail to the recorder. Shared by the
    // row-returning path and ANALYZE so the two cannot come to disagree
    // about when a trail is written.
    void RecordTrail(const std::optional<StatementIdentity>& identity,
                     exec::TrailCollector* trail);

    // Counts one execution of every step's access shape. Shared by the
    // row-returning path and ANALYZE, for the reason RecordTrail is: two
    // call sites that could disagree about when a statistic is written
    // would make the statistic mean two things.
    void RecordAccessShapes(const exec::StepChain& chain);

    // The cabin optimizer's S1/S2 (cabin-controller.md §II.2,
    // workplan PHY01): one decayed touch per successful fingerprinted
    // SELECT, carrying the statement's page count. Beside RecordTrail and
    // RecordAccessShapes because it is the same moment - a completed
    // execution - observed by a third collector.
    void RecordOptimizerSignals(const std::optional<StatementIdentity>& identity,
                                const exec::StepChain& chain, const exec::ExecStats& stats);

    // ---- The Cabin write hook (docs/spec/cabin.md §5) --------------------
    //
    // **This is what "observed ⇒ complete" costs**, and the whole reason a
    // Cabin can be authoritative where a Waystone trail cannot: absence has
    // a witness, and this is the witness. One directory probe per cabined
    // column per write - in-memory, O(1), and skipped entirely by the
    // empty `cabin_columns` test for a relation with no Cabin.
    //
    // Every mandatory action is an **append**. Nothing is ever removed here:
    // an older snapshot may still be entitled to match a row through the
    // undo chain, so eager removal is *incorrect* and not merely
    // unnecessary. The surplus is subtracted at read time by verification.
    //
    // `values[i]` is the value of column `first_col_pos + i`: INSERT passes
    // the row's body (`first_col_pos` 1, after the tuple key) and UPDATE the whole
    // decoded row (0). `tuple_key`, `page_id` and `slot` are the tuple's identity
    // and location, already in hand at both sites, so C6's hints cost
    // nothing to produce.
    //
    // `previous`, when non-empty, is the row **before** the write, indexed
    // the same way. It is what implements §5's third row - an UPDATE that
    // did not touch the key column does nothing - and it is not an
    // optimization: appending on every write is correct (the set stays a
    // superset) but unbounded, so a relation updated often enough would
    // grow one value's set until the cap un-observed it. INSERT passes
    // nothing, having no previous row.
    //
    // Never fails: a Cabin that cannot witness a write un-observes the value
    // instead, which returns it to the authoritative scan path (§1's
    // corollary) and is always legal.
    // Whether any Cabin witnesses `access`'s writes: `NoteCabinWrite`'s own
    // early return, and the UPDATE loop's once-per-statement skip of it -
    // one test, so the skip cannot drift from what the call would do.
    bool CabinsWitness(const catalog::TableAccess& access) const noexcept {
        return cabins_ != nullptr && (!access.cabin_columns.empty() || !access.expr_cabins.empty());
    }
    void NoteCabinWrite(const catalog::TableAccess& access,
                        std::span<const parser::AstValue> values, std::uint16_t first_col_pos,
                        std::uint64_t tuple_key, PageId page_id, std::uint16_t slot,
                        std::span<const parser::AstValue> previous = {});
    DispatchOutcome HandleUpdate(std::string_view line, Session& session);

    // `DELETE FROM <t> [WHERE ...]` (docs/spec/txn.md sections 4.3, 6).
    //
    // A **delete-mark**, never a physical removal: the slot keeps its bytes
    // and gains kSlotFlagDeleted, and the deleter's id goes in the tuple's
    // writer field. That pair is the whole of DELETE in the no-xmax model,
    // and it is why an older snapshot still reads the row - it steps back
    // over the kDeleteMark undo record and finds the tuple's own payload
    // unchanged.
    //
    // **The Cabin write hook is deliberately not called here.** By
    // cabin.md section 5 removal is forbidden: an older snapshot may
    // still be entitled to match the row through the undo chain, so
    // dropping its entry would break the superset invariant. The surplus is
    // subtracted at read time, which now includes the visibility predicate.
    DispatchOutcome HandleDelete(std::string_view line, Session& session);
    // `PURGE FROM t WHERE <tuple key window>` (BH): one whole attempt - judge the
    // window, then write it key by key - under an owned transaction of its
    // own, so each re-drive after a wait begins with a fresh view (BH-R5)
    // and holds no unit while it waits. A key still pending - an undecided
    // writer, or a deleter some reader cannot see yet - is returned as
    // `purge_wait` and nothing is written.
    DispatchOutcome HandlePurge(std::string_view line, Session& session);
    // `WAYSTONE ON <select>`, `WAYSTONE OFF <select>` and `WAYSTONE OFF
    // <fetch_id>` (CF-S1b): opts a shape in to trail recording and replay, by
    // example, or out of it. Autocommit only; answers once the state it
    // reports is as durable as the session's rung promises a commit.
    DispatchOutcome HandleWaystone(std::string_view line, Session& session);
    // `PURGE DROPPED COLUMNS FROM t` (CD-S2): one whole attempt under an
    // owned DDL transaction and the relation's `IX` - the statement-epoch
    // gate, then the relation walked in batches, each dirty row cleaned in
    // place by `exec::CleanDroppedCells`. A row whose writer is not resolved
    // for every reader, or a statement still on an older schema word, is
    // returned as `purge_wait`; the rows cleaned before it stay clean.
    DispatchOutcome HandlePurgeDroppedColumns(const parser::PurgeStmt& stmt, Session& session);
    // `resume_from` is where a parked walk of this statement stopped
    // (AO-S3b); an inactive cursor is a first run and starts at the head,
    // which is what every caller but `DispatchAsync`'s resume passes. Not
    // defaulted, for `VisitRelation`'s `span` reason alone: a caller that
    // starts from the head says so.
    //
    // The snapshot is the *same* one the first run minted - the resumed
    // statement must not re-mint it, or the rows it already wrote and the
    // rows it has yet to reach would be read under two views.
    DispatchOutcome DeleteInner(std::string_view line, WriteScope& scope,
                                const txn::Snapshot& snapshot, WalkCursor resume_from,
                                const exec::StatementContext& context);
    //
    // `subqueries` carries the statement's scalar-subquery answers (BJ-S7): empty
    // on the first run, which fills it, and handed back on a resume, which reads
    // it, so a subquery is run once for the whole statement however often it
    // is compiled.
    DispatchOutcome UpdateInner(std::string_view line, WriteScope& scope,
                                const txn::Snapshot& snapshot, WalkCursor resume_from,
                                const exec::StatementContext& context,
                                std::vector<exec::SubqueryResult>& subqueries);
    // `UpdateInner` past its parse. `arm_target`, when set, is the one row an
    // `ON CONFLICT DO UPDATE` arm writes (CG): it replaces the WHERE's tuple
    // key lookup, the WHERE must be empty, and a lookup that cannot answer
    // is refused rather than walked - a walk under an empty WHERE would
    // write every row.
    DispatchOutcome UpdateParsed(parser::UpdateStmt& stmt, WriteScope& scope,
                                 const txn::Snapshot& snapshot, WalkCursor resume_from,
                                 const exec::StatementContext& context,
                                 std::vector<exec::SubqueryResult>& subqueries,
                                 std::optional<std::uint64_t> arm_target);

    // **A scalar subquery in an UPDATE's SET value** (BJ-S7, BJ-Q6): run once,
    // before the walk, through the SELECT path under the session's view, and
    // answered as a single typed value (NULL for no row). More than one row or
    // column is refused, and so is a correlated one - a per-row sub-chain under
    // the write walk's exclusive leaf would be the page-latch cycle
    // `rules.md` §3's page latch row forbids, and the view would show it this
    // statement's own earlier rows.
    StatusOr<exec::SubqueryResult> RunScalarSubquery(const parser::Expr& node,
                                                     const parser::UpdateStmt& stmt,
                                                     WriteScope& scope,
                                                     const txn::Snapshot& snapshot,
                                                     const exec::StatementContext& context);
    DispatchOutcome HandleSync();
    // BR-Q2 (a)'s refusal while the WAL archive fails and its filesystem is
    // near full, or nothing; every path that writes the log asks it.
    std::optional<DispatchOutcome> ArchiveRefusal() const;
    // BACKUP START '<label>' | COPY TO '<dir>' | STOP TO '<dir>' | ABORT |
    // STATUS (BR-S2, S5).
    DispatchOutcome HandleBackup(std::string_view line, Session& session);



    // `SELECT ... FROM sys.<view>`. Answered without the compiler: a
    // catalog view is materialized from the catalog's typed readers, not
    // walked out of pages, so it is not a relation a step can read
    // (exec/catalog_view.hpp).
    DispatchOutcome HandleCatalogView(const parser::SelectStmt& stmt, Session& session);

    // `SHOW <target> [<arg>]` and `DESCRIBE <t>`: one `SELECT` over one
    // view each (SV1), built as an AST so an argument keeps its byte.
    DispatchOutcome HandleShow(std::string_view line, std::string_view target,
                               std::string_view arg, Session& session);

    // ---- The `sys` views (src/server/sys_views.cpp) ----------------------
    //
    // One builder per view; `ReadSysView` dispatches on the registry entry.
    // `key` is the view's key literal where the statement wrote one
    // (`sys_views.hpp`), null otherwise; a schema-object view reads under
    // `view` (BO-Q2), null for unfiltered; `byte` is the view name's, which a
    // refusal that is not about a literal names.
    StatusOr<exec::CatalogView> ReadSysView(const SysViewSpec& spec, Session& session,
                                            const parser::AstValue* key, std::uint32_t byte);
    StatusOr<exec::CatalogView> TablesView(const txn::ReadView* view);
    StatusOr<exec::CatalogView> ObjectsView(const txn::ReadView* view);
    StatusOr<exec::CatalogView> ColumnsView(const txn::ReadView* view,
                                            const parser::AstValue* rel_name,
                                            catalog::Oid default_namespace);
    StatusOr<exec::CatalogView> TypesView();
    StatusOr<exec::CatalogView> PatternsView();
    StatusOr<exec::CatalogView> NamespacesView(const txn::ReadView* view);
    StatusOr<exec::CatalogView> IndexesView(const txn::ReadView* view);
    StatusOr<exec::CatalogView> FkeysView(const txn::ReadView* view);
    StatusOr<exec::CatalogView> AccessStatsView();
    StatusOr<exec::CatalogView> RelationStatsView();
    StatusOr<exec::CatalogView> CabinsView();
    StatusOr<exec::CatalogView> AssertionsView();
    StatusOr<exec::CatalogView> TransitionsView();
    StatusOr<exec::CatalogView> CabinOptimizerView(std::uint32_t byte);
    StatusOr<exec::CatalogView> LockWaitsView();
    StatusOr<exec::CatalogView> StatementStatsView();
    StatusOr<exec::CatalogView> CheckpointsView();
    StatusOr<exec::CatalogView> PageSlotsView(const parser::AstValue& page_id);
    StatusOr<exec::CatalogView> MetaView();
    // The clustered root a relation's anchor names, or its descriptor page.
    StatusOr<PageId> CurrentRootOf(const catalog::SysTableRow& row);
    // `sys.meta`'s fields: each section's `key=value` tokens.
    std::vector<std::pair<std::string_view, std::string>> MetaSections();

    // A full ordered scan of the relation, whichever storage it uses. Both
    // walk sibling/next links left to right, so the row order is identical.
    //
    // `page_access` must be kWrite whenever `fn` modifies a tuple - UPDATE
    // and DELETE scan through here - and kRead otherwise, which keeps a
    // SELECT from dirtying every page it reads (page_store.hpp). `fn`
    // returns storage::VisitControl: kStop ends the scan successfully.
    //
    // `cursor` is the resume position (AO-S3b): null for a walk that cannot
    // park (every read walk), otherwise in **and** out - read to start
    // where a parked walk stopped, written when this walk stops. Not
    // defaulted: a caller that cannot park says so by passing null.
    Status VisitRelation(
        const catalog::TableAccess& access, storage::PageAccess page_access,
        const std::function<StatusOr<storage::VisitControl>(PageId, heap::PageView&,
                                                            std::uint16_t)>& fn,
        WalkCursor* cursor);

    // The btree walk behind `VisitRelation`, and its resume shape: a resume
    // descends by key rather than returning to a remembered leaf, which is
    // what makes it safe against a split that happened while the statement
    // was parked (`WalkCursor`, `session.hpp`).
    Status WalkBtreeLeaves(
        PageId root, storage::PageAccess page_access,
        const std::function<StatusOr<storage::VisitControl>(PageId, heap::PageView&,
                                                            std::uint16_t)>& fn,
        WalkCursor* cursor);

    // Gives every spilled value a rollback: one undo record per spill, in
    // the writing transaction's chain, plus the trail entry a live Abort
    // reads. Both compensate by releasing the slot.
    //
    // **Called before whatever logs the VARHEAP_APPENDs**, because an
    // UNDO_WRITE must precede the record it can undo - RV3's rule for
    // catalog writes, for the same reason: redo alone must never resurrect
    // an append the undo phase has no record to release.
    //
    // A scope with no transaction records nothing (a dispatcher built
    // without a manager, and the `kNoTxnId` writes `LogChainInsert` makes
    // for the assertion catalog); those spills leak on rollback.
    Status NoteSpills(const WriteScope& scope, std::uint32_t rel_oid, std::uint64_t tuple_key,
                      const std::vector<exec::AppendedSpill>& spills);

    // Logs an insert **under the holds its placement handed out** (AT-S21,
    // `wal.md` §8-1), released here once the row's record is stamped - and
    // before an own transaction's commit, whose durability wait must not run
    // under a latch. The record set is the file header's: the spills, the
    // index entries, then the row (BTREE_INSERT, or one BTREE_SPLIT carrying
    // it; BD-R3 E2, BD-R12) -
    // spills before the row so replay never reaches a pointer that resolves
    // to nothing (rule-fixed-length-tuple.md §5). With no WAL it logs
    // nothing and a split's grown root is still published.
    //
    // `own_txn` false means a TransactionManager owns the transaction and
    // logged TXN_BEGIN; this emits only the page records and leaves
    // TXN_COMMIT and its durability wait to EndWrite(). `owner_oid` (page.md
    // §2a) rides every PAGE_INIT so redo re-stamps what the live path did.
    // `spills` is the manager-less path's: with a manager each spill was
    // noted and logged at its append (`SpillLogFor`).
    //
    // A failure is reported to the client and the tuple stays in the frame:
    // a lost write on a crash, not a wrong answer now.
    Status LogInsert(storage::InsertPlacement& placed,
                     std::span<const std::byte> tuple, std::uint64_t trx_id,
                     std::uint64_t owner_oid, PageId anchor_page_id,
                     const std::vector<exec::AppendedSpill>& spills,
                     const std::vector<exec::IndexWrite>& index_writes = {},
                     bool own_txn = true);

    // A clustered split's one BTREE_SPLIT record and, when it grew the tree
    // a level, the root's publication **inside that record**: the anchor's
    // slot is staged under the split's holds and imaged with its pages, then
    // adopted in memory once the record is appended. A log cut therefore
    // keeps the split and its root together or neither. With no log the
    // slot is still written and adopted.
    Status LogSplit(storage::InsertPlacement& placed, std::uint64_t txn_id, catalog::Oid oid,
                    PageId anchor_page_id);

    // The record both trees' splits take: `pages` as one BTREE_SPLIT and,
    // when `new_root` is set, the anchor staged into it and the root
    // adopted after the append (`index_oid` 0 is the clustered root).
    // `anchor_pin` is the split's pin on the anchor, released once staged;
    // `held` the pages the split still holds, imaged through those holds.
    Status LogTreeSplit(std::span<const PageId> pages, std::span<const storage::PageRef> held,
                        std::uint64_t txn_id,
                        catalog::Oid rel_oid, std::uint64_t index_oid, PageId new_root,
                        PageId anchor_page_id, storage::UnlatchedPin* anchor_pin);

    // One page's full image, logged and the page stamped behind it - a
    // wrapper over `storage::LogFullPageImage`, the one helper every site
    // uses so none can omit the stamp (`redo.cpp` gates every record on
    // `page_lsn`). No-op with no WAL attached.
    Status LogFullPageImage(PageId page_id, std::uint64_t txn_id);

    // What a `WHERE id = <const>` statement should do instead of scanning.
    // The three cases are distinct because the *authority* of the answer
    // differs:
    //
    //   kScan    no shortcut: the descent failed (a corrupt page). Scan; the
    //            scan reaches the leaves by their sibling links and is
    //            authoritative.
    //   kAt      look at this (page, slot) - a btree descent, which is
    //            authoritative - through `at.leaf`, held **exclusive**:
    //            every caller writes the slot (btree.hpp `Location`).
    //   kAbsent  **no such row**, on authority.
    struct TupleKeyLookup {
        enum class Kind { kScan, kAt, kAbsent };
        Kind kind = Kind::kScan;
        btree::Location at;
    };
    TupleKeyLookup LocateByTupleKey(const catalog::TableAccess& access, std::uint64_t tuple_key);

    // The row-relocation callback a rollback needs when a leaf division has
    // moved rows this transaction wrote (txn/manager.hpp's RowLocator).
    // Built per abort, never stored on the manager - see the definition.
    txn::TransactionManager::RowLocator RowLocatorForRollback();

    // The tuple key value a WHERE clause is a *bare* equality against, or nullopt
    // if it is anything else - no WHERE, more than one condition, a non-tuple-key
    // column, a non-equality operator, or a non-integer or negative
    // literal.
    //
    // Shared by SELECT and UPDATE so the two cannot disagree about which
    // predicates take the point path. Duplicating this check is how one
    // path ends up descending for a query the other correctly scans.
    std::optional<std::uint64_t> TupleKeyEqualityTarget(
        const catalog::TableAccess& access,
        const std::vector<parser::Condition>& where) const;

    // Diagnostics. Levels are chosen so the default (info) is quiet under
    // load: DDL and SYNC are Info because they are rare and consequential,
    // a completed query is Debug, and the per-tuple heap events are Trace.
    // Enabling trace on a busy server costs a write() per tuple - it is a
    // development tool, not an operating mode.
    // Dispatch() wraps this to time it and log the outcome once, in one
    // place, rather than at every return of every handler.
    DispatchOutcome DispatchInner(std::string_view line, Session& session);

    bool logging(LogLevel level) const noexcept {
        return log_ != nullptr && log_->enabled(level);
    }
    sched::MonoTimeNs NowNs() const noexcept { return clock_ == nullptr ? 0 : clock_->Now(); }

    SuperBlock& superblock_;
    catalog::Catalog& catalog_;
    storage::PageStore& page_store_;


    // Whether the statement running right now can park (set by
    // `DispatchAsync`, never by `Dispatch`). One statement runs at a time
    // per core (sched.md §3), which is what makes a member the right place
    // for it - the same argument `pending_commit_lsn_` makes one line up.
    bool may_park_ = false;

    // Whether the running statement's entry point re-runs a structural
    // refusal without parking (`Dispatch`'s blocking BA-S14 loop, under
    // `RerunScope`), so `EndWrite` keeps the note for it as it does under
    // `may_park_`.
    bool reruns_structural_ = false;

    // **`EndWrite` withheld an explicit transaction's poison for a
    // structural re-run** (BA-S14). Read where the re-runs give up: only a
    // statement whose poison was withheld is poisoned there - a read never
    // poisons, so its refusal at the deadline must not either. Which
    // statements re-run at all is `EndWrite`'s call, made by clearing
    // `storage::StructuralRefusalNoted()` for any it ends otherwise.
    bool structural_poison_withheld_ = false;

    // **`strict` commits parked before their publish** (BA-S7 part 2,
    // BA-Q3 (b)). A commit that may park stages its record for the writer
    // (`TransactionManager::CommitDeferred`), holds its marker, and leaves
    // its second half here; the statement then parks on durability like a
    // `group` commit. Whoever first finds the record durable runs the
    // second half - the parked statement on its kick, or `Dispatch`'s
    // synchronous wait on its own core's marker, which could otherwise wait
    // for a statement that cannot run until it returns. In LSN order, since
    // the writer's watermark only rises. This core's alone.
    //
    // **Each entry names the statement that staged it** (BA-S15): another
    // statement of this core can run inside a `SELECT`'s walk, or inside any
    // park, and stage its own; `DispatchAsync` parks on its own entry only.
    struct DeferredCommit {
        wal::Lsn lsn = wal::kNoLsn;
        txn::Transaction* txn = nullptr;
        std::uint64_t statement = 0;
    };
    std::vector<DeferredCommit> deferred_commits_;

    // Both commit arms' deferred tail: the statement parks on `lsn`, and
    // `FinishDeferredCommits` owns `txn` from here.
    void DeferCommit(txn::Transaction* txn, wal::Lsn lsn);

    // Whether a commit of `txn` here takes the deferred path: `strict`, a
    // statement that may park, a writer that kicks (above one core - at one
    // core the commit syncs inline, as before), and no catalog write in the
    // transaction, whose second half `EndDdlScope` owns.
    bool DefersCommit(std::uint64_t txn_id) const;

    // Runs the second half of every deferred commit whose record is
    // durable, in order. Once the log is stopped, every one whose record is
    // not durable is aborted instead: D1 never publishes a commit that is
    // not durable, and a failed sync aborts on both paths (txn.md §6). The
    // statement answers with the refusal (`DispatchAsync`).
    void FinishDeferredCommits();

    // **The allowance, taken and given back structurally** (AO-S6d): a
    // hand-placed set/clear pair is silently wrong the day a `co_await` or
    // an early `co_return` lands between them, and the statement's dispatch
    // and re-runs sit beside suspension points. Restores rather than clears.
    class MayParkScope {
    public:
        // `statement` is the `DispatchAsync` call this window belongs to,
        // which a deferred commit staged inside it is stamped with.
        MayParkScope(CommandDispatcher& owner, bool allowed, std::uint64_t statement) noexcept
            : owner_(owner), saved_(owner.may_park_), saved_statement_(owner.parking_statement_) {
            owner_.may_park_ = allowed;
            owner_.parking_statement_ = statement;
        }
        ~MayParkScope() {
            owner_.may_park_ = saved_;
            owner_.parking_statement_ = saved_statement_;
        }
        MayParkScope(const MayParkScope&) = delete;
        MayParkScope& operator=(const MayParkScope&) = delete;

    private:
        CommandDispatcher& owner_;
        bool saved_;
        std::uint64_t saved_statement_;
    };
    // `reruns_structural_`, taken and given back the same way.
    class RerunScope {
    public:
        explicit RerunScope(CommandDispatcher& owner) noexcept
            : owner_(owner), saved_(std::exchange(owner.reruns_structural_, true)) {}
        ~RerunScope() { owner_.reruns_structural_ = saved_; }
        RerunScope(const RerunScope&) = delete;
        RerunScope& operator=(const RerunScope&) = delete;

    private:
        CommandDispatcher& owner_;
        bool saved_;
    };
    // BA-S14's re-run test, shared by both entry points: a structural
    // refusal of a statement `EndWrite` kept the note for, inside the
    // statement's one deadline (`deadline_ns`, set at the first refusal).
    // Counts the re-run it admits, and rewinds the session's result sink for
    // it (CE-A1); declines one whose rows a client may already have read
    // (CE-R4).
    bool StructuralRerunDue(const DispatchOutcome& out, Session* session,
                            sched::MonoTimeNs& deadline_ns);
    // The `DispatchAsync` call whose synchronous window is running, and the
    // last one issued (`MayParkScope`, `DeferredCommit::statement`).
    std::uint64_t parking_statement_ = 0;
    std::uint64_t last_statement_ = 0;
    Logger* log_;
    const sched::Clock* clock_;
    wal::WalManager* wal_;
    // The **server's** class - the bottom rung of §9's chain. Never read
    // by a commit path directly: `effective_durability_` below is what a
    // commit is owed, and it is this one only for a session that overrode
    // nothing.
    wal::DurabilityClass durability_;

    // The class the statement now in flight commits under
    // (`Session::EffectiveDurability`), stamped once at the top of
    // `DispatchInner` and read by every ack-timing site beneath it.
    //
    // A member because one statement runs at a time on a core (sched.md
    // §3), so `LogInsert` and `AwaitDdlDurability` need no `Session&` and
    // no second copy of the precedence chain. Initialised to the server's
    // class.
    wal::DurabilityClass effective_durability_ = durability_;

    // The per-statement work ceiling, from `max_rows_touched`. Held by
    // value and handed to each execution, which takes its own copy - so
    // one statement's spend never carries into the next.
    exec::Budget budget_;
    // `join_order = planned` (CB-S2).
    bool plan_join_order_ = true;
    // `set_plan_stats`': the instance's store, and this core's view of it.
    stats::PlanStats* plan_stats_ = nullptr;
    stats::PlanStatsSource plan_source_;
    // This core's join order memo (CC-S4).
    exec::PlanMemo plan_memo_;

    // AG11's caps, handed to every fold this dispatcher runs, from the
    // `aggregate_max_groups` / `aggregate_max_distinct` config keys (spec
    // §6's defaults when unset). Held by value for `budget_`'s reason.
    exec::AggregateLimits aggregate_limits_;

    // The fold, **reused rather than constructed per statement** (AP03):
    // building one per statement cost about 4 us of server CPU on a tuple key
    // lookup, nearly all of it buffer allocation.
    //
    // `Reset` points it at each statement's spec and labels, which live on
    // that statement's chain - so between statements it holds pointers that
    // are not valid, and nothing may read it there. That is the same
    // contract the pooled trail collector has with `Clear()`.
    exec::Aggregator aggregator_;

    // `sort_max_rows` - how many rows one sort may hold before the
    // statement is refused. A cap, not a budget: it never truncates.
    std::size_t sort_max_rows_ = exec::kDefaultSortMaxRows;

    // Where a successful SELECT reports the tuples it found, or null when
    // nothing is recording - which is a valid production configuration
    // (`waystone_recording = off`) and the default here, so every
    // socket-free unit test stays recorder-free too.
    //
    stats::TrailRecorder* recorder_ = nullptr;

    // Whether a SELECT may be served from a previously recorded trail
    // (`waystone_replay`). Independent of `recorder_`: replaying trails
    // while recording no new ones is a legitimate configuration, and it is
    // one of the five the advisory-contract suite compares.
    //
    // **Turning this on cannot change a reply.** A trail supplies only a
    // location, which is then read and filtered by the same code a
    // descent's location would have been, and every entry is validated
    // first (invariants 8, 9). Defaults off.
    bool replay_enabled_ = false;

    // **A `SELECT`'s scratch, pooled rather than owned** (BA-S15). The
    // trail collector reserves a whole trail's worth of room - an 8 KB
    // malloc per query if built per statement - and the replay index, the
    // output sort and the per-step counters keep their buffers the same
    // way. They were one set of dispatcher members while one statement ran
    // at a time on a core; since BA-S15 several `SELECT` walks of one core
    // can be suspended at once, so each `SelectRun` borrows a set here and
    // gives it back when it ends. The pool grows to the most walks this
    // core has had suspended together and keeps them.
    //
    // The output sort (OB4) and the counters are here for the same reason:
    // `Reset` points the sort at one statement's keys, and between
    // statements it holds a buffer nothing may read; a statement that wrote
    // no `ORDER BY`, or whose order the compiler elided, leaves it inactive.
    struct SelectScratch {
        exec::TrailCollector trail{stats::kMaxTrailEntries};
        exec::TrailReplay replay;
        exec::OutputSort sorter;
        exec::ExecStats stats;
    };
    std::vector<std::unique_ptr<SelectScratch>> select_scratch_pool_;
    friend class SelectRun;
    friend class SuspensionHold;
    friend class BuildRun;
    friend class IndexBuildRun;
    friend class DeclarationBuildRun;
    template <typename Build>
    friend class DeclarationBuildRunOf;

    // Whether a successful SELECT records its access shapes
    // (`access_statistics`). Defaults **on**: a history that starts when
    // someone enables it does not exist when `SHOW ACCESS` or CREATE
    // CABIN's no-filter warning asks for it (docs/spec/heap-and-tuple.md §7).
    bool access_stats_enabled_;
    stats::AccessStatsCounters access_counters_;

    // The instance's Cabin store, or null when cabins are switched off
    // (`cabins = off`, and the default here), so "identical replies with
    // cabins on and off" is a property of the structure rather than of the
    // test data.
    stats::CabinStore* cabins_ = nullptr;

    // `SetReclaimCounters`'; null outside an assembled instance.
    const ReclaimCounters* reclaim_counters_ = nullptr;
    // BF-R9's two, null outside an assembled instance.
    StatementEpochs* epochs_ = nullptr;
    PageReclaimer* reclaim_queue_ = nullptr;
    // Statement heads open on this core: only the outermost publishes.
    int epoch_depth_ = 0;
    // Whether a DDL was open anywhere when the last statement head read the
    // count, before it revalidated (`ViewFor`).
    bool ddl_open_at_head_ = false;
    EpochSeam epoch_seam_at_ = EpochSeam::kBeforePublish;
    std::function<void()> epoch_seam_for_test_;
    std::function<void()> before_drop_exclusive_for_test_;
    std::function<void()> before_drop_column_write_for_test_;
    std::function<void(std::uint64_t)> after_update_row_applied_for_test_;
    std::function<void()> after_view_for_test_;
    std::function<void()> purge_seam_for_test_;
    // **What an open transaction's drops owe** (BF-R9): recorded when the
    // drop's catalog write succeeds, queued by `EndDdlScopeById` at the
    // transaction's commit, and forgotten at its other endings.
    struct OwedDrop {
        std::uint64_t txn_id = 0;
        catalog::Oid oid = 0;
        catalog::PendingRoots roots;
    };
    std::vector<OwedDrop> owed_drops_;
    // **What an open transaction's index DDL may orphan** (BY-R2): a
    // `DROP INDEX`'s tree, queued at the transaction's commit, and a
    // `CREATE INDEX`'s logged tree, queued at any other ending. A create that
    // failed after its slot went down records nothing; the next mount's
    // owed-slot scan finds it (BY-R1).
    struct OwedIndex {
        std::uint64_t txn_id = 0;
        catalog::Oid rel_oid = 0;
        PageId anchor = kInvalidPageId;
        catalog::Oid index_oid = 0;
        bool created = false;  // queued at a rollback, not at a commit
    };
    std::vector<OwedIndex> owed_indexes_;

    // The live assertions and their reservation bookkeeping (workplan
    // AST06/AST07): CREATE ASSERTION's build moves its LiveAssertion in
    // here, DROP evicts it, the three write paths check and reserve through
    // it, and the commit/abort hooks below settle what a transaction
    // reserved. **The instance's since AT-S5d** when `set_assertions` is
    // handed one, this dispatcher's own otherwise (assertion.md §6.1). The
    // entry *pages* are durable; this registry is the memory-resident half a
    // restart loses until recovery replays it (AST05's fold) - and SHOW
    // ASSERTIONS derives `enforcing` from its presence, so the loss reports
    // itself instead of hiding.
    std::unique_ptr<exec::AssertionEnforcer> owned_enforcer_ =
        std::make_unique<exec::AssertionEnforcer>();
    exec::AssertionEnforcer* enforcer_ = owned_enforcer_.get();

    // The commit a write path staged and did not wait for, read out at the
    // end of DispatchAndStage(). One statement runs at a time on a core, so
    // this cannot hold two.
    wal::Lsn pending_commit_lsn_ = wal::kNoLsn;

    // Per-statement members, on `pending_commit_lsn_`'s terms (one statement
    // at a time on a core), zeroed at the top of `DispatchAndStage` and read
    // out at its end. `blocking_writer_` is the undecided transaction that
    // refused this statement, `blocked_tuple_key_` the row it holds, and
    // `statement_trail_mark_` the transaction's trail length when this
    // statement's write scope opened - which says whether the statement
    // wrote anything before it was refused, so whether a re-run is a repeat
    // or a second application.
    std::uint64_t blocking_writer_ = 0;
    std::uint64_t blocked_tuple_key_ = 0;

    // On `blocking_writer_`'s terms, the relation lock wait (AO-S6e-b): set
    // where the borrow is refused, read back out by `DispatchAndStage` into
    std::optional<DispatchOutcome::LockWait> lock_wait_ = std::nullopt;
    // **The wake a refused unit ask registered** (AX-S2b), on the same
    std::optional<DispatchOutcome::LockWait> blocking_wake_ = std::nullopt;
    std::size_t statement_trail_mark_ = 0;
    // The running statement's `fetch_id`, set by the parse that has one and
    // copied onto its outcome by `DispatchAndStage` (BO-S5).
    std::uint64_t statement_fetch_id_ = 0;

    // **The fault net every wait in this file is bounded by**, and the
    // one thing `lock_wait_fault_net_ms` sets; defaults to the constant.
    // **0 is a value, not an absence**: the other policy, a statement
    // refused at once instead of waiting.
    sched::MonoTimeNs lock_wait_fault_net_ns_ = txn::kLockWaitFaultNetNs;
    // See `set_locks`. Null means no detector, so AO-S3's guard.
    txn::LockTable* locks_ = nullptr;

    // The transaction manager, or null in a socket-free test: every write
    // then stamps kBootstrapXid and every read sees everything.
    txn::TransactionManager* txn_ = nullptr;

    // ---- Core affinity (crosscore.md CC3/§6) ---------------------------
    //
    // Which core this dispatcher runs on; 0 is the system core and the only
    // value a single-core build has.
    std::uint32_t core_id_ = 0;

    // the borrow cap's refusal counter; the accessor above states its contract.
    std::uint64_t borrow_cap_stops_ = 0;

    // This core's sequence for `ReadHolderId` (`read_borrow.hpp`, the one
    // home of the layout). One per statement that declares - every read, every
    // write - since AT-S1. It wraps, and a wrap could only collide with a
    // borrow still held four billion statements later on the same core.
    std::uint32_t read_borrow_seq_ = 0;
    std::uint64_t NextReadHolder() noexcept;
    std::uint64_t read_borrows_ = 0;

    // **The refusal the borrow cap raised on this statement, and the wire
    // detail that goes with it.** Carried as a member for
    // `blocking_writer_`'s reason - the refusal is raised deep inside a row
    // callback and the outcome is built at the top, with no return path
    // between them wide enough to hold it.
    //
    // **The `Status` and not only the detail**: `StatusFromErrorReply` would
    // fold the cap's line into `InvalidArgument` (`kErrorSpellings` has no
    // `ResourceExhausted`), and `protocol.md` §11's details are **per
    // category**, so `kLockCap` there would mean nothing.
    //
    // Cleared at the top of `DispatchAndStage` as well as at the bottom,
    // exactly as `blocking_writer_` is and for the same reason: a public
    // seam can set it and never reach the copy-out. `ExecuteInsert` is that
    // seam - `KwpLoadServer` drives it on the same dispatcher a session
    // uses - so without the top clear a load chunk's cap refusal would
    // label the next unrelated statement's failure.
    Status last_refusal_ = Status::OK();
    std::uint16_t last_refusal_detail_ = wire::kNoDetail;

    // The read-path index switch (`indexes`, default on). Read-path only:
    // maintenance is not switchable, because an index that stops being
    // maintained is wrong rather than slow.
    bool indexes_enabled_ = true;

    // BI3's per-statement row cap, from the `max_insert_rows` config key.
    // A refusal, never a truncation.
    std::uint64_t max_insert_rows_ = parser::kDefaultMaxInsertRows;

    // PHY01's collector. The per-statement counters that feed its S2 are
    // `SelectScratch::stats`, pooled for the aggregator's reason: `For()`
    // sizes a vector, and reuse makes the ordinary SELECT allocate nothing
    // for its counting.
    stats::OptimizerSignals* optimizer_signals_ = nullptr;
    // §II.6's switch, off by default and experimental. **The instance's
    // since AT-S8** (`OptimizerSurface::cabin_optimizer_on`): a `SET` on any
    // core flips the one flag the controller's cadence reads. A dispatcher
    // handed none points at its own.
    std::atomic<bool> own_cabin_optimizer_on_{false};
    std::atomic<bool>* cabin_optimizer_on_ = &own_cabin_optimizer_on_;
    // Held across the controller view's read (AT-S8): core 0's cadence
    // mutates the controller under it. Null where one thread owns both.
    Latch* cabin_optimizer_view_latch_ = nullptr;
    const MountRecovery* recovery_ = nullptr;  // RC09, set_recovery()
    BackupControl* backup_ = nullptr;          // BR-S2, set_backup()
    const WalArchiver* archiver_ = nullptr;    // BR-S4, set_archiver()

    // PHY06's view sources: the controller's managed table and decision
    // log, the executor's applied-action counters. Read-only - the view
    // renders, it never drives - and null wherever the controller was
    // never constructed (`cabins = off`, or a test that wired neither).
    const stats::CabinOptimizer* cabin_controller_ = nullptr;
    const exec::CabinOptimizerExecutor* cabin_executor_ = nullptr;

    // This core's reactor, set_scheduler_view(); null off a reactor.
    const sched::Scheduler* scheduler_view_ = nullptr;

    // Statement shipping and cross-owner 2PC retired (AT-S6); their history
    // is at cf889b63.

    // The local commit and rollback - the whole of both.
    DispatchOutcome CommitLocal(Session& session);
    DispatchOutcome RollbackLocal(Session& session);



    // **The one refusal left before a write touches a page** (AT-S9): a
    // relation under an assertion the instance cannot
    // enforce refuses its writes on every core. Every write verb asks it
    // once per statement.
    Status CheckWriteAdmission(const catalog::TableAccess& access);

    txn::IsolationLevel default_isolation_ = txn::IsolationLevel::kReadCommitted;

    Session autocommit_session_;

    // Implicit-transaction ids for the inserts `LogInsert` logs with no
    // TransactionManager (`own_txn`). Process-local, restarting from 1 every
    // boot, so two boots' ids can collide in one stream - a known gap of the
    // manager-less path, which no assembled instance takes.
    std::uint64_t next_txn_id_ = 1;
};

}  // namespace kds::server
