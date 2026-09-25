#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "kds/base/latch.hpp"
#include "kds/base/log.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/aggregate.hpp"
#include "kds/exec/sort.hpp"
#include "kds/exec/assertion_check.hpp"
#include "kds/exec/budget.hpp"
#include "kds/exec/fk_check.hpp"
#include "kds/exec/plan_printer.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/cabin_ddl.hpp"
#include "kds/exec/cabin_optimizer_exec.hpp"
#include "kds/exec/index_maintain.hpp"
#include "kds/parser/ast.hpp"
#include "kds/stats/access_stats.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/stats/trail_recorder.hpp"
#include "kds/stats/trail_store.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/coro.hpp"


#include "kds/stats/trace.hpp"
#include "kds/server/result_sink.hpp"
#include "kds/server/session.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wire/error_registry.hpp"

// Command dispatch: turns one client-supplied line of text into an action
// against the running database and a text response. Deliberately pure
// engine logic - no sockets, no syscalls, no clock reads (rules.md #4:
// engine logic goes through injectable interfaces; there is nothing to
// inject here because Dispatch() needs none of those, which is exactly
// why this is split out from the platform-layer listener that calls it
// (TcpServer, tcp_server.hpp) - Dispatch() can be unit-tested directly,
// with no socket or thread involved.
//
// The SQL statements (CREATE TABLE, INSERT, SELECT, UPDATE) are parsed by
// src/parser and executed here. Column types are resolved through
// Catalog::ResolveTypeByName() against sys.types, which stands in for the
// type registry that does not exist yet; src/exec/row_codec.hpp names
// exactly what that covers - no NULLs, no float or decimal columns,
// fixed-width ints and varchar only.
//
// CREATE TABLE also keeps a bare-name form with no parens, which asks for
// a zero-column table and therefore always errors now that every relation
// needs a Keystone pk column. The two forms are disambiguated by whether a
// '(' follows the table name.
//
// Protocol: one command per line, case-insensitive keyword, arguments
// space-separated. A response is always exactly one line back (never
// containing embedded newlines) - the platform-layer listener appends the
// line terminator itself.
//
// ---- WAL: INSERT is logged, nothing else is -----------------------------
//
// Given a WalManager, INSERT appends the records that describe it and does
// not answer the client until they are durable to the configured class
// (wal.md sections 1 and 5.2). Every other mutating path - CREATE TABLE,
// UPDATE, and the catalog rows underneath both - still writes pages
// outside the log, so a crash still loses them. INSERT went first because
// it is the path with a benchmark pointed at it; the others follow the
// same shape.
//
// One INSERT is one implicit transaction, and it emits:
//
//     TXN_BEGIN
//     [FULL_PAGE_IMAGE  of the old tail]  only when the chain grew
//     [PAGE_INIT        of the new tail]  only when the chain grew
//     HEAP_INSERT       the tuple, its slot, its writer
//     TXN_COMMIT        + the durability class's wait
//
// The FULL_PAGE_IMAGE is there because chain growth mutates two pages: the
// new page's `next_page_id` link lives in the *old* tail's header, and a
// new page redo cannot reach is a new page redo cannot use. No record type
// describes a link edit on its own (record.hpp's enum is frozen and
// append-only, so inventing one is a format-version event), and an FPI is
// the existing record that makes a page whole. It costs one page of log
// per page of heap - roughly +50% log volume on small rows - and it is
// paid once per 8 KB of tuples, never per tuple. A HEAP_CHAIN_LINK record
// type would remove it; that is a format decision, not this file's.
//
// ---- Clustered type: one dispatcher, two storages ----------------------
//
// A relation is either a chain of heap pages (`ClusteredType::kHeap`,
// heap_chain.hpp) or a clustered B+ tree (`kBtree`, btree.hpp), and every
// statement handler below branches on `TableAccess::clustered_type` in
// exactly one place - `InsertIntoRelation`, `VisitRelation`, `LocateByPk`.
// Everything else in this file is storage-agnostic, which is possible
// because a btree **leaf is a heap page**: the row codec, `PageView`
// reads/overwrites, `HEAP_INSERT` and the `SHOW PAGE` dump all work on
// either without knowing which they hold.
//
// The observable differences are narrow and worth stating:
//
//   - `SELECT`/`UPDATE ... WHERE id = <n>` on a btree relation descends
//     the tree, which is **authoritative** - a miss means the row does not
//     exist, and no scan follows. The same statement on a heap relation
//     scans the chain, because a heap relation has no pk index at all.
//   - `INSERT` may split a leaf and grow the tree a level, in which case
//     the relation's `desc_page_id` is repointed at the new root before
//     the client is answered.
//   - A full scan of either is a left-to-right walk of the same
//     `next_page_id` links, so `SELECT *` returns rows in the same order.
//
// ---- Ordering: the records are appended after the page is mutated -------
//
// ChainInsert() writes the tuple into the page frame, and only then are
// the records appended and page_lsn stamped. That is safe here, and the
// reason is narrow enough to be worth stating: the server is a single
// cooperative thread (sched.md), the checkpoint and drain tasks are other
// tasks on it, and nothing suspends between the mutation and the stamp -
// so no flush can observe the page in between. What protects the interval
// is the store's WAL gate (device_page_store.hpp): once page_lsn is
// stamped, no write-back can outrun the log. A path that ever suspends
// mid-statement must generate the record while holding the page latch
// instead, which is what wal.md section 8-1 actually asks for.

namespace kds::sched {
// Only the pointer is held here; `set_scheduler_view` below says what for.
class Scheduler;
}  // namespace kds::sched

namespace kds::server {

// What the mount's recovery did (`server/mount_recovery.hpp`), reported by
// SHOW META. Forward-declared rather than included: only the pointer is held
// here, and the definition drags in the WAL and catalog headers that every
// consumer of this file would then pay for.
struct MountRecovery;

// The `physical_optimizer` config key's two legal states
// (docs/spec/physical-optimizer.md R3). There is deliberately no `kOn`:
// the config layer refuses `on` at startup naming §6's gates, so a mode a
// mover would need cannot exist before the mover does.
enum class PhysicalOptimizerMode : std::uint8_t {
    kOff = 0,
    kShadow = 1,
};


struct DispatchOutcome {
    std::string response;
    bool should_stop = false;

    // **How many rows the statement affected**, for a caller that needs the
    // number rather than the sentence. `S_COMPLETE{tag, rows_affected}`
    // (docs/spec/protocol.md §7) is the one such caller today.
    //
    // Carried rather than read back out of `response`, which was the first
    // shape and is the drift this codebase refuses everywhere else: the
    // three DML replies do not share a spelling - `UPDATED 7` puts the
    // count second, `INSERTED oid=.. id=..` implies one, and a bulk insert
    // writes `rows=` - so a parser here would be a *second* reading of a
    // string the renderer owns, and would answer 0 for the commonest write
    // in the engine. Set where the count is known; 0 everywhere else, which
    // is the truthful answer for a read, a DDL and a session statement.
    std::uint64_t rows_affected = 0;

    // **Why the statement failed, as a `Status`.** OK on every success and
    // on the handful of refusals built from a bare message.
    //
    // Carried for `rows_affected`'s reason, one step further: a typed
    // client switches on the error's *category* (protocol.md §11, and the
    // `retryable` bit it makes a compatibility surface), and the only route
    // to one used to be `StatusFromErrorReply` parsing the rendered line -
    // whose bare arm folds `NotFound`, `Unsupported`, `OutOfRange`,
    // `Corruption`, `OutOfSpace`, `IoError`, `ResourceExhausted` and
    // `CardinalityViolation` into `InvalidArgument`. Every one of those
    // reached a KWP client as INVALID_ARGUMENT, which defeats the whole
    // point of a category taxonomy at the one seam that feeds it.
    //
    // `StatusFromErrorReply` stays, and is still right where it is used:
    // the cross-core path genuinely has only a rendered line to recover a
    // code from, and the four spellings it recovers exactly are the ones
    // that matter there.
    Status status = Status::OK();

    // **The wire detail this failure carries**, or `kNoDetail`. A `Status`
    // has no room for one - `protocol.md` §11's details live on the wire
    // error, which the session builds - so a refusal that knows *which*
    // limit it hit has to carry the answer up here beside the status. Only
    // the borrow cap sets it today: `wire::ResourceDetail::kLockCap` was
    // numbered at AO-S1 and had no setter for as long as the cap was
    // swallowed, because a refusal nobody ever saw needed no detail. The
    // client's fix for it differs from every other `ResourceExhausted` -
    // not a smaller statement or a slower client, but a shorter
    // transaction - which is the whole reason it has a number of its own.
    std::uint16_t resource_detail = wire::kNoDetail;

    // A write this core is holding back because something it needs is held
    // by a transaction that **has not decided yet** (AO-S3; R6-5 and D5 are
    // where the narrower first version came from).
    //
    // Two things set it, and both mean the same: `CheckWriteConflictBlocking`
    // when the row's last writer is in flight, and the foreign-key forward
    // resolution when the *parent* row's writer is. `NoteBlockingWriter` is
    // the single gate on both, and it is where the conditions live - among
    // them the two that keep this stage deadlock-free without a detector.
    //
    // Set only where the refusal is one a re-run could get past: the
    // statement wrote nothing before it hit the conflict, so running it
    // again once the doubt clears is exactly what the client would do.
    // `DispatchAsync` parks on the doubt and re-runs; a statement that had
    // already written rows is not re-runnable - re-applying `SET v = v + 1`
    // to the rows it did write would be a second increment - and is
    // answered with the conflict it produced, unblocked.
    //
    // **The synchronous `Dispatch()` never waits on one**: a path with no
    // reactor cannot park, and the honest
    // answer there is the retryable conflict itself. So the block is a
    // property of served connections, and a fixture sees the pre-R6-5
    // behaviour.
    struct WriteBlock {
        std::uint64_t trx_id = 0;  // the undecided writer being waited for
        std::uint64_t pk = 0;      // the row it holds
    };
    std::optional<WriteBlock> write_block = std::nullopt;

    // **AO-S6e-b: the relation unit this statement asked for and did not
    // get**, or nullopt. Asked by a DDL's relation `X` (`DROP TABLE`,
    // `CREATE`/`DROP INDEX`, a `CREATE ASSERTION`'s build) against a
    // writer's `IX` or a reader's `IS`, and since AT-S5e by a writer's first
    // `IX` against a DDL's `X`. The wait is on the table's own slot and not
    // on the holder's decide, because the holder may be on another core and
    // `TransactionManager::IsInFlight` is per-core.
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
    };
    std::optional<LockWait> lock_wait = std::nullopt;

    // **AO-S3b: the walk stopped inside the statement and the scope is
    // still open.** `write_block` says which row and which holder; this
    // says the statement is *resumable* rather than re-runnable, which is
    // the distinction the comment above draws and could not act on: a
    // statement that had written rows used to be answered with its
    // conflict, because re-running it would write those rows twice and
    // there are no savepoints to undo them with. Resuming writes them
    // once. `Session::parked_write` carries what the resume needs; this
    // flag is only what tells the handler's tail not to end the scope.
    bool parked_mid_walk = false;
    WalkCursor walk_cursor = {};

    // The commit this statement staged, when the client may not be told
    // about it until the log is durable (`durability = group`, docs/spec/wal.md
    // D2). `kNoLsn` means there is nothing to wait for - every relaxed
    // statement, every read, and every strict commit, which synced on its
    // own stack before returning.
    //
    // **The wait is deliberately not taken where the commit happens.** A
    // statement that syncs inline is a statement that holds the core while
    // the device works, which serializes every other connection behind it -
    // measured as a batch size of exactly 1 and TPS that does not move with
    // the connection count (bench/results-latency-matrix.md). Returning the
    // LSN instead lets the *caller* decide how to wait: `Dispatch()` waits
    // inline, because its callers have no scheduler to park on;
    // `DispatchAsync()` parks, which is what lets the next connection's
    // statement run and stage its own commit into the same sync.
    wal::Lsn pending_lsn = wal::kNoLsn;

    // **The COMMIT record's own LSN, whatever the class and whatever the
    // ack point** (XF4). Distinct from `pending_lsn`, which is *"the wait
    // this statement still owes"* and is deliberately empty where the
    // caller was answered at the append. It was carried for a cross-owner
    // participant under `CommitAck::kAtAppend` and for
    // `ShippedStatementExecutor::FinishDecision`, which timed the leg
    // between its ack and its own durability; both went with 2PC at AT-S6.
    // `kNoLsn` on every statement that is not a commit.
    wal::Lsn commit_lsn = wal::kNoLsn;
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
// the *code* back out of it: the KWP session and the KWP load server, four
// call sites. It was written for one caller that no longer exists - a
// statement executed on its owner core answered in a rendered line, and
// the `retryable` bit a client's retry loop read had to be the bit the
// owner meant - which went with statement shipping at AT-S6.
//
// A dispatcher's outcome carries no `Status`: every handler renders one at
// its own return, and threading a code back out would touch every write
// path in this file. Recovering it from the rendered line instead is exact
// where it matters - the four spellings a client switches on are recovered
// as themselves - and lossy only where nothing reads it: every other code
// renders as a bare `ERR <message>`, so all of them come back as one, and
// the re-render is byte-identical. `ErrorReply(StatusFromErrorReply(line))
// == line` for every line `ErrorReply` produces, which is the property
// worth having and the one its test asserts.
//
// A line that is not an error reply is a bare `kOk` - **the line itself is
// not carried**, because on the success arm the caller already holds it and
// copying it into a status message would only duplicate the answer. This
// refuses to invent a failure for a success; it does not claim to
// reconstruct the success.
//
// Which makes the classification purely prefix-shaped: a *success* line
// beginning `ERR ` would be read as a refusal. What keeps that unreachable
// is not this function but the replies themselves - a DML answer opens with
// a fixed keyword (`INSERTED`/`UPDATED`/`DELETED`/`OK`), and a SELECT's
// header line is comma-joined identifiers, so no shippable success has a
// space at byte 3. Stated because it is a property of the *callers*, and a
// reply shape added later that can lead with free text breaks it silently.
Status StatusFromErrorReply(std::string_view reply);

// **The deadlock victim's refusal, one sentence for both sites** (AO-R7,
// AO-S4a's dispatcher loop and AO-S4b's owner-side arm). `TxnConflict`,
// because that is the one retryable code and a deadlock genuinely is
// retryable - the survivor will have released by the time this one comes
// back - and the message names deadlock, because an operator meeting a
// conflict needs to know whether to look for contention or for a
// lock-order bug. What is aborted is the statement; the transaction is
// poisoned and still holds its rows, and the sentence says so rather than
// "the other proceeds" without saying when. `waited_for` names what the
// victim waited on: a row and its holder, or a core's transaction.
Status DeadlockVictim(const std::string& waited_for);

// Where a tuple lives, as a point lookup reports it. Local to the
// dispatcher because it is the shape of an answer to "skip the scan and
// look here", not a storage-layer concept.
struct TupleLocation {
    PageId page_id = kInvalidPageId;
    std::uint16_t slot = 0;
    // No bytes field, for btree.hpp Location's reason: a span here outlived
    // the pin that made it valid, and every reader already re-fetches by
    // page_id. Deleted with its one producer 2026-08-13.
};

// **The statement limits a config file sets, as one value** (AT-S8). Every
// core accepts sessions, so every core's dispatcher must run under the same
// ones; while they travelled as five separate arguments and setters, a
// peer's dispatcher was handed none of them and ran under the defaults. One
// struct, built once from the config and applied by one call on every core,
// is what keeps the next limit from missing the peers the same way. Each
// default is the dispatcher's own.
struct StatementLimits {
    bool indexes = true;
    std::uint64_t max_insert_rows = parser::kDefaultMaxInsertRows;
    exec::AggregateLimits aggregate;
    std::size_t sort_max_rows = exec::kDefaultSortMaxRows;
    std::size_t join_build_max_rows = exec::kDefaultJoinBuildMaxRows;
};

// **The physical optimizer's surface, as one value** (AT-S8, on the
// operator's word). Every core accepts sessions, and a peer's dispatcher was
// wired with none of this: `SET CABIN_OPTIMIZER` there answered OK and moved
// a flag nothing read, `SHOW CABIN_OPTIMIZER` answered "absent", and its
// reads fed no signal. The pieces are the instance's - one relayout mode,
// one signal collector (latched above one core), one switch, one controller
// and executor - and every dispatcher is handed the same value.
// `view_latch` is what core 0's cadence holds across a tick; a view read
// from another core takes it too. A null switch leaves the dispatcher's own; every other member is taken as given.
struct OptimizerSurface {
    PhysicalOptimizerMode relayout_mode = PhysicalOptimizerMode::kShadow;
    sched::MonoTimeNs decay_half_life_ns = 600'000'000'000ULL;
    stats::OptimizerSignals* signals = nullptr;
    std::atomic<bool>* cabin_optimizer_on = nullptr;
    const stats::CabinOptimizer* controller = nullptr;
    const exec::CabinOptimizerExecutor* executor = nullptr;
    Latch* view_latch = nullptr;
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
    //                            store back to stable storage. Until the
    //                            WAL lands, this and STOP are the only
    //                            things that make a mutation survive the
    //                            process dying.
    //   SHOW META             -> superblock stats, one line
    //   SHOW TABLES           -> space-separated table names
    //   SHOW PATTERNS         -> "patterns=<n>", then one "\n"-escaped
    //                            section per sys.patterns row, identified
    //                            by its hex pattern_id and carrying
    //                            `origin=` and `pinned=`, both of which
    //                            read `auto` / `no` on every row since
    //                            declared patterns were withdrawn.
    //                            An inspection surface: it lists rows from
    //                            older fingerprint revisions too, marked
    //                            `stale=v<n>`, because those are the dead
    //                            weight a version bump leaves behind and
    //                            seeing them is the point.
    //   SHOW ACCESS           -> "access_shapes=<n>", then one "\n"-escaped
    //                            section per recorded access shape:
    //                            "kind=<Lookup|Probe|Range|FilterScan|Scan>
    //                             rel=<s> columns=[<s>,...] uses=<n>
    //                             last_seen=<n>". The physical optimizer's
    //                            input (docs/spec/heap-and-tuple.md §7), keyed
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
    //                             notnull=<yes|no> pk=<yes|no>
    //                             autoincrement=<yes|no>". Replaces the
    //                            former FIND TABLE, which reported the
    //                            same header and no schema. DESC is
    //                            accepted as a synonym.
    //   CREATE TABLE <name>   -> the bare, pre-parser form: a zero-column
    //                            table. Now always "ERR ...", because
    //                            every relation's first column is its
    //                            mandatory Keystone primary key
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
    //                            word: HEAP (default) is a chain of heap
    //                            pages, BTREE is a clustered B+ tree on the
    //                            Keystone pk. EXPLICIT is vacuous (§4.1) -
    //                            every relation takes a caller-named pk or
    //                            issues one when INSERT omits it, so the
    //                            word states the default; ASSIGNED is
    //                            refused. Either order, each at most once.
    //                            See src/exec/row_codec.hpp for the
    //                            supported column type set.
    //   INSERT INTO <name> VALUES (<val> [, ...])
    //                         -> "INSERTED oid=<table_oid> id=<n> slot=<n>"
    //                            or "ERR ...". Values are positional, one
    //                            per schema column in `pos` order, *after*
    //                            the primary key - see ast.hpp: no
    //                            explicit column list in this grammar. The
    //                            pk is not supplied: it is the Keystone id,
    //                            issued by Catalog::AllocateRowId() and
    //                            reported as `id=`. Supplying a full-width
    //                            value list is an error naming the pk
    //                            column (CLAUDE.md invariant 10).
    //   SELECT * FROM <name> [WHERE <cond> [AND <cond>]*]
    //                         -> a full ordered scan of the relation,
    //                            WHERE-filtered; a bare `WHERE <pk> = <n>`
    //                            instead takes the point path (a tree
    //                            descent, on a btree relation).
    //                            One wire line: "col1,col2,..." then one
    //                            "\n"-escaped (see SHOW PAGE above) section
    //                            per matching row, comma-joined values.
    //                            No rows matching -> just the header line.
    //   UPDATE <name> SET <col> = <val> [, ...] [WHERE <cond> [AND <cond>]*]
    //                         -> "UPDATED <n>" (n = row count touched) or
    //                            "ERR ...". In-place HOT-style overwrite
    //                            (PageView::OverwriteTuple) - fails with
    //                            an ERR (no fallback) if a changed value
    //                            no longer fits the tuple's original slot
    //                            capacity, e.g. growing a varchar.
    //
    // `session` carries the connection's transaction state (session.hpp).
    // **Null means autocommit through a private session**, which is what
    // every caller that predates transactions gets - and what keeps their
    // behaviour identical, because an autocommit statement outside an
    // explicit transaction is exactly what the engine did before.
    // A cache-dropping boundary, as `DispatchAndStage` below states.
    DispatchOutcome Dispatch(std::string_view line, Session* session = nullptr);

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
    // This is the seam `docs/inflight/in-progress/workplan-crosscore.md` P4 needs: a statement
    // that reaches another core has to send and then *wait*, and a function
    // returning a finished reply cannot. Making it a coroutine is what lets
    // the executor grow a suspension point later without the server around
    // it changing again.
    //
    // The synchronous `Dispatch()` above stays, and is not deprecated. Every
    // caller that has no reactor - the socket-free tests, a peer's
    // `CoreRuntime`, the rollback on connection close - would otherwise have
    // to acquire one to run a statement, which is a lot of machinery to
    // demand of a caller that never suspends. It is implemented in terms of
    // nothing; the coroutine wraps *it*, so there is one dispatch path and
    // no chance of the two drifting.
    //
    // **`line` is not copied.** A coroutine's parameters live in its frame,
    // but a `string_view` parameter copies the view and not the bytes - and
    // the parser's tokens are themselves views into this buffer
    // (parser-v2.md's zero-copy tokens). The caller must keep the statement
    // text alive until the coroutine finishes, which is why `TcpServer`
    // copies each line out of its inbox before dispatching one.
    // **When a D2 commit inside this dispatch owes its acknowledgement**
    // (`docs/spec/cross-owner-txn.md` §2, ratified 2026-08-31 by
    // `instructions/v2.7.1/ratification-xd1.md`, enacted by
    // `instructions/v2.7.1/workorder-xd.md`).
    //
    // `kWhenDurable` is every client-facing path and the default: a client
    // told `COMMIT` under `group` has been told the record is on the
    // platter, and the wait is what makes that true.
    //
    // `kAtAppend` **has no caller since AT-S6.** Its one caller was a
    // cross-owner **participant** applying a decide it was told, whose
    // acknowledgement went to a coordinator that had already made the
    // decision durable in its own stream; the participant went with 2PC.
    //
    // **D1 and D3 are unreachable by this**, by construction rather than by
    // a second branch: `kStrict` synced inside `WalManager::Commit` before
    // it returned and `kRelaxed` waits for nothing, so neither ever stages
    // a `pending_commit_lsn_` for this to suppress. The one site that reads
    // it says so.
    enum class CommitAck {
        kWhenDurable,
        kAtAppend,
    };

    // A cache-dropping boundary, as `DispatchAndStage` states.
    sched::Coro DispatchAsync(std::string_view line, Session* session, DispatchOutcome* out,
                              CommitAck commit_ack = CommitAck::kWhenDurable);

    // The level a fresh session starts at (`isolation`). TcpServer stamps
    // it on each connection's session at accept.
    txn::IsolationLevel default_isolation() const noexcept { return default_isolation_; }

private:
    // ---- Transaction control (docs/spec/txn.md sections 1, 6) ----------------
    DispatchOutcome HandleBegin(std::string_view args, Session& session);
    DispatchOutcome HandleCommit(Session& session);
    DispatchOutcome HandleRollback(Session& session);
    DispatchOutcome HandleSetIsolation(std::string_view args, Session& session);
    // `SET DURABILITY {STRICT|GROUP|RELAXED}` (protocol-wp.md P03,
    // docs/spec/protocol.md §9). The session rung of the same chain
    // `HandleSetIsolation` sets, and deliberately the same shape: a
    // session statement the dispatcher routes, not a `parser::Statement`
    // arm - see the note at its definition.
    DispatchOutcome HandleSetDurability(std::string_view args, Session& session);

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
    // manager returns an empty scope, and the write path then stamps
    // kBootstrapXid exactly as it always did.
    StatusOr<WriteScope> BeginWrite(Session& session);

    // Ends what BeginWrite began. `result` is the statement's outcome: OK
    // commits an owned scope, anything else aborts it. Inside an explicit
    // transaction a failure **poisons the session** rather than unwinding -
    // rows already written stay, and the client must ROLLBACK (section 6).
    //
    // **No `statement_ends` since AT-S5f.** It existed for one reader - the
    // pending-delete registration a parked DELETE had to keep across its
    // fan-out - and both the registration and the park went with the probe
    // protocol. The scope's own unwind was always identical on both arms.
    Status EndWrite(Session& session, WriteScope& scope, const Status& result);

    // **The one way an owned scope ends without committing** (AO-S6d,
    // AO-0 item 15). Three arms of `EndWrite` reach it: the statement
    // failed, its assertion entries could not be cleared, or its commit
    // record could not be written - and until this existed the last two
    // returned without unwinding anything at all.
    //
    // What that cost is not a tidiness: `TransactionManager::Commit` fails
    // only *before* `PublishCommit`, so the transaction is still active,
    // and `Release` refuses to free an active one. The object stayed in
    // `live_` for the life of the process, `MintView` counted it in flight
    // for every later view, and - since AO-S6c-a gave every writer a
    // borrow - its tenancies were never released either, so every later
    // writer of a row it touched parked until the fault net and was told a
    // defect report. `Abort` is exactly the operation defined for a
    // still-active transaction, and it releases both.
    //
    // `reported` is what the client is owed: the commit's own failure
    // where there was one, so the answer names what actually went wrong
    // rather than the compensation that followed it. `Status::OK()` means
    // the caller has no answer of its own and wants the unwind's.
    Status AbortOwnedScope(WriteScope& scope, const Status& reported);

    // The trx_id a write stamps: the scope's transaction, or
    // kBootstrapXid when there is no manager.
    static std::uint64_t WriterId(const WriteScope& scope);

    // First-updater-wins, plus the one thing R6-5 adds to it: **who** the
    // conflicting writer is. `TransactionManager::CheckWriteConflict` is
    // unchanged and still decides the verdict; this notes, when the verdict
    // is a conflict against a transaction this core prepared and is in
    // doubt about, that the refusal is one a bounded wait could get past
    // (D5's ratified "block, with a bounded ceiling ending in a named
    // refusal"). One function for the two call sites, so the two write
    // paths cannot come to disagree about which conflicts are waitable.
    // Records `trx` as the holder this statement is waiting for, when
    // waiting is both **safe** and **capable of a different answer**. Both
    // tests are the point, and each closes a hole the S3 cutover would
    // otherwise open.
    //
    // **Safe: the waiter must hold nothing.** A cycle needs an edge out of
    // a holder, and a transaction that has written nothing is one no other
    // transaction can be waiting for. Restricting the wait to those makes
    // the wait-for graph edges-from-waiters-only, which has no cycle by
    // construction - so this stage cannot deadlock, and it does not need
    // AO-S4a's detector to be safe. A transaction that has already written
    // keeps the old retryable refusal until that detector exists, which is
    // the honest half-step: axis 1 reaches autocommit and every
    // transaction's first write now, and the rest at AO-S4a.
    //
    // **Capable: the re-run must be able to answer differently.** Under
    // `kRepeatableRead` the view is minted at `BEGIN` and `StartStatement`
    // does not re-mint it, so a holder that **commits** after that view
    // stays invisible and `CheckWriteConflict` refuses again on the same
    // ground - a stall ending in the refusal it was already owed.
    //
    // **Conservative, not exact, and the difference is worth knowing.** An
    // *aborted* holder is a different case: the compensation restores the
    // row's prior writer id, so the same view would admit the write. The
    // level is excluded whole rather than split, because a wait that helps
    // only when the holder rolls back is a narrower promise than the one
    // this stage makes for every other level, and offering it silently
    // would be the convenient answer rather than the true one.
    //
    // **That exclusion is now asked per wait rather than per level**
    // (AO-S6d, AO-0 item 17). Everything above is an argument about a
    // holder that wrote *this row*, and since AO-S6c-b that is no longer
    // the only kind of blocker: one may hold a coarser unit over the key
    // and have written no version of it - a range fence, or the relation
    // unit a `WHERE`-less write declares - and its commit changes what a
    // repeatable-read view admits only for rows it wrote. There is a
    // second kind too: a statement whose verdict does not go through the
    // view at all, which is every `INSERT`, since a caller-named key's
    // uniqueness is proved by a **physical** descent (`btree.cpp`'s leaf
    // scan, `heap_chain.cpp`'s tail scan) and an issued one by the mark.
    // `RepeatableReadWait` is that question, answered at each recording
    // site because only the site knows.
    //
    // `waiter` is the transaction the statement runs in, or null in
    // autocommit before one is opened - which is the safest case of all,
    // since a transaction that does not exist holds nothing and re-mints
    // its view by construction.
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
    void NoteBlockingWriter(const txn::Transaction* waiter, std::uint64_t trx, std::uint64_t pk,
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

    // **The write-block wait, and the two places a statement can meet one**
    // (AO-S6d, AO-0 item 16). Parks until `out->write_block`'s holder
    // decides and then re-runs `line` whole, ending at a grant, at the
    // deadlock verdict, or at the fault net - and on every one of those
    // `out->write_block` is empty on return, which is what makes a
    // caller's test of it *after* the call mean "the re-run opened a new
    // wait" rather than "the old one is still up".
    //
    // It became a function because the foreign-key probe arm called it too
    // (the arm went at AT-S5f; `AwaitStatementWaits` is its caller now).
    // `DispatchAsync` ran this wait before that arm and never after it, so
    // a statement that parked on a foreign parent and met a held row on
    // its resume was refused where the same statement dispatched directly
    // would have waited. `foreign-keys.md` §2a carries the rest of the
    // argument, that section being the one that owns the resume.
    //
    // **What it was not**: unsound. The resume ran without `may_park_`, so
    // no blocker was recorded there and `EndWrite` poisoned an explicit
    // transaction normally. The refusal was unhelpful, not a client told
    // `ERR` over a transaction that then commits - that is what *this*
    // change would produce if the allowance around the resume landed
    // without the wait beside it, which is the mutation its cell names.
    //
    // `statement_deadline_ns` is in-out and zero means "not taken yet":
    // the fault net bounds the *statement*, so a second entry inherits the
    // first one's deadline instead of starting a fresh one.
    //
    sched::Coro AwaitWriteBlock(std::string_view line, Session* session, DispatchOutcome* out,
                                sched::MonoTimeNs* statement_deadline_ns);

    // **AO-S6e-b: the statement waits for the unit it was refused, then
    // runs again whole.** The retired index-build window's shape, with the
    // table's slot in place of the window's predicate - so it costs one atomic load per
    // reactor iteration and never a partition latch on the poll path - and
    // with the same statement deadline as the write-block wait, because
    // both are the lock family's fault net and a second bound would be a
    // second name for one quantity.
    //
    // The registration is dropped before the re-run, whichever way the wait
    // ended: the re-run asks under a new transaction id, so the record left
    // by this one is addressable only by its slot.
    //
    // On return `out->lock_wait` is empty unless the re-run asked again,
    // which the loop that calls this is what handles.
    sched::Coro AwaitRelationLock(std::string_view line, Session* session, DispatchOutcome* out,
                                  sched::MonoTimeNs* statement_deadline_ns);

    // Both waits, looped until neither is set, through one function so the
    // two cannot diverge (the AT-S5e review's C3): each wait's re-run is a
    // whole fresh statement and can meet what the other waits for.
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
    // granted, or none was asked for because a coarse unit already covers
    // the row, or no table exists to hold one. Before this the wait's holder was
    // `cur`, which is why a statement could only ever wait for the writer
    // of the row it was looking at - a transaction refused by a range
    // fence had no holder to name and simply failed.
    //
    // *Out*, the holder this write is blocked by: the table's where the
    // table named one, `cur` where it did not. The two sources are a
    // **union and not a cutover** - the table names holders the header
    // never could (a fence over a key nobody has written), and the header
    // names writers the table does not (a dispatcher with no lock table, and
    // every row on a dispatcher with no table at all), so reading either
    // alone drops a wait AO-S3 already made. Zero out means there is
    // nothing to wait for and the refusal is the plain conflict it always
    // was, which is what the two write sites test it for.
    Status CheckWriteConflictBlocking(const WriteScope& scope, std::uint64_t cur,
                                      std::uint64_t pk, std::uint64_t& blocker);

    // The refusal a borrow the table declined produces, in
    // `CheckWriteConflict`'s own shape - `txn/manager.cpp` calls that
    // message part of the wire contract rather than a diagnostic, so the
    // three sites that can now raise it must not each invent one. It names
    // the **holder** rather than the row's writer, because that is who the
    // client is waiting for and, for a range fence, the two are not the
    // same transaction.
    static Status HeldByHolder(std::uint64_t pk, std::uint64_t holder);

    // **The borrow, and the refusal a unit somebody else holds produces.**
    // `nullopt` means the unit is this statement's to write. A `Status`
    // means it is not - and the holder has already been recorded where
    // `DispatchAsync` can park on it, so the caller has only to render it.
    //
    // **A refusal naming nobody is not one of these.** Since AO-S6c-c the
    // cap comes back as a `Status` rather than as "not granted", so the two
    // causes no longer share a bool - but the `blocker` test stays, because
    // a grant and a conflict are still the only two things the bool can
    // mean and reading it without asking who refused is how the cap looked
    // like transaction 0 holding a row for as long as it did.
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
    std::optional<Status> BorrowOrWait(const WriteScope& scope, const txn::LockKey& unit,
                                       RepeatableReadWait rerun);

    // Is `cond` a non-negative integer literal compared against `access`'s
    // primary key, and if so which id? The shared half of the test
    // `PkEqualityTarget` and `DeclaredWriteBorrow` both need.
    //
    // **`kind` before `op` and before `val`, and that is the whole reason
    // this is one function.** `ast.hpp` records that both default to a
    // value reading as a legal equality, and names the two consumers of a
    // raw `Condition` that were burned by it - the catalog view's bound
    // reader and `PkEqualityTarget`, where a `kBetween`'s low bound made
    // an UPDATE write one row and answer `UPDATED 1`. A third reader
    // arrived with AO-S6b; a third copy of the test would be a third
    // chance to get it wrong. Answers `nullopt` for every kind that is not
    // a `kCompareValue` against a literal, which is what leaves `kBetween`
    // to the caller that knows it carries two bounds.
    std::optional<std::uint64_t> PkLiteral(const catalog::TableAccess& access,
                                           const parser::Condition& cond) const;

    // **The unit a write declares before it walks** (AO-0 item 14, marked
    // by the operator on 2026-09-08), or `nullopt` for the per-row borrow
    // that was the only shape before it. A write whose predicate covers a
    // key window borrows that window **once** instead of accumulating one
    // entry per row, which is what keeps a bulk write inside
    // `max_locks_per_txn` when AO-S6c makes the cap refuse rather than
    // truncate.
    //
    // **A declaration, never an escalation.** The unit is read off the
    // predicate at dispatch, before a row is touched, so it does not
    // depend on how many rows turn out to match. That distinction is the
    // whole reason this is admissible: the mark of 2026-09-08 §1 item 4
    // forbids *widening* a transaction's accumulated tuple borrows into a
    // coarse one, because that makes one transaction's refusal into a wait
    // other transactions pay for with no record of why. Choosing coarsely
    // up front is what AR2 §3 already does for DDL, for a mover's changed
    // key interval, and for an FK reverse check with no covering
    // structure - "a refusal-class fact rather than a performance one".
    //
    // **What it costs, and it runs in both directions** - the first half
    // is user-visible, the second was a review finding against the first
    // draft of this stage. A coarse borrow blocks concurrent writers that
    // the per-row borrow admitted: a `DELETE FROM t` with no `WHERE` holds
    // the relation against every other writer of it for the length of the
    // transaction. And two coarse borrows must still meet each other -
    // `WHERE id > 1` and `WHERE id BETWEEN 2 AND 3` overlap at row 2 - or
    // the declaration would be *weaker* in detection than the per-row
    // borrow it replaces, which is a wrong answer rather than a cost.
    // `LockTable::ConflictingOverlap` is what makes them meet.
    //
    // **What it does not reach**: a predicate that names no pk window -
    // `WHERE name = 'x'` - is left per-row, because it names no interval
    // to borrow. Such a write still accumulates an entry per row and is
    // still the one shape the cap can refuse once S6c propagates it.
    std::optional<txn::LockKey> DeclaredWriteBorrow(
        const catalog::TableAccess& access, const std::vector<parser::Condition>& where) const;

    // **The borrow a write takes, before the header it judges is
    // interpreted** (AO-S6a, on the operator's choice of 2026-09-08
    // between the two orders `lock_table.hpp` left open) - one chain for
    // every unit, the declared one before the walk and a tuple per row
    // where nothing was declared. The intention above it first - relation
    // `IX`, then the unit - because a unit borrowed without it is
    // invisible to a relation-level ask. **Ordered and conditional**: a
    // relation `IX` that is not granted takes nothing under it, so the
    // pair is a chain rather than two independent asks that happen to run
    // in sequence. A relation unit is the exception with no intention
    // above it, being outermost itself.
    //
    // **Non-queueing, and that is still the whole of the claim.** It takes
    // the unit when it is free and reports `false` when it is not; the
    // undecided-writer wait stays where AO-S3 put it until AO-S6c moves it
    // here. So the only status this returns other than OK is the cap's,
    // which is a refusal rather than a conflict and cannot be waited out
    // (AO-R10, AR2-R4).
    //
    // **The `bool` is what a refused declaration needs**, and leaving it
    // out was a defect in this stage's first draft: a caller that declared
    // a coarse unit and never looked at the answer recorded *nothing* for
    // the whole statement when the unit was refused - no relation `IX` and
    // no rows - which is strictly less than the per-row path it replaced.
    // The caller waited on it instead, until AO-S6c-c made a refused declaration a wait of its own.
    //
    // A dispatcher with no lock table borrows nothing and answers granted
    // - every fixture that builds none. The null-transaction arm is
    // reached by no caller today: both write sites test `scope.txn`
    // themselves before calling, so the bootstrap-xid path never arrives
    // here at all. Kept as the guard for the callers AO-S6c adds.
    // **The DDL's relation `X`** (AO-S6e-b; AR2 §3's DDL row). Empty when
    // it was taken - and it is taken for the DDL transaction, so its decide
    // is what releases it. A conflict answers the refusal and, where there
    // is a reactor to park on, leaves `lock_wait_` set so `DispatchAsync`
    // waits for the holder to release and runs the statement again.
    //
    // One caller, `DROP TABLE`: AR2 §3's DDL row names `CREATE`, `ALTER`
    // and `CREATE INDEX` too, and none of them takes this - the drop is the
    // only one whose census fate AO-S6e owed.
    std::optional<Status> BorrowRelationForDdl(txn::Transaction* holder, catalog::Oid oid,
                                               bool poisons = true);

    StatusOr<bool> BorrowChain(const WriteScope& scope, const txn::LockKey& unit,
                               std::uint64_t* blocker = nullptr);

    // **One wait per statement, and the registration of the one it drops
    // goes with it.** A statement can reach two asks that each want to
    // park: since AT-S5f the forward foreign-key check takes the parent
    // row's at the dispatch fork, and the relation's `IX` is asked after
    // it. `DispatchOutcome` carries one, so the second install replaces the
    // first - and a wake registration is the caller's to remove
    // (`lock_table.hpp`), with nothing else that will: an entry is erased
    // only when it has neither holder nor waiter, so one left behind keeps
    // its entry, its waiter and its per-release kick for the life of the
    // instance.
    void TakeLockWait(DispatchOutcome::LockWait wait);

    DispatchOutcome HandleShowMeta();
    DispatchOutcome HandleListTables(Session& session);

    // `SHOW NAMESPACES` (`docs/spec/namespace.md` NS9). `sys.objects`
    // filtered by type, the way `SHOW TABLES` filters by `kTypeTable`.
    DispatchOutcome HandleShowNamespaces(Session& session);

    // `{CREATE | DROP} NAMESPACE <name>` (AF-T3). One handler for both, as
    // the parser has one production - see `NamespaceStmt` (ast.hpp).
    DispatchOutcome HandleNamespace(std::string_view line, Session& session);
    DispatchOutcome HandleDescribe(std::string_view args, Session& session);
    DispatchOutcome HandleShowPage(std::string_view args);
    DispatchOutcome HandleShowPatterns();
    DispatchOutcome HandleShowAccess();

    // `SHOW BUDGET` - every relation's Keystone id consumption
    // (`docs/rules/keystoneid-invariant.md` K-M4). Listed for *every* relation
    // including the catalog's own, because some of those - sys.patterns,
    // sys.cabins, sys.assertions - genuinely issue ids, and a listing that
    // hid them would hide the only relations whose consumption an operator
    // does not control.
    DispatchOutcome HandleShowBudget();

    // Both take the session so a `CREATE TABLE` inside an explicit
    // transaction can stamp its catalog rows with that transaction's id
    // and register them for rollback (workplan-ddl-transactional.md
    // DT3b). In autocommit they behave exactly as they always did.
    DispatchOutcome HandleCreateTable(std::string_view args, Session& session);
    DispatchOutcome HandleCreateTableSql(std::string_view line, Session& session);

    // The other half of the duplicate-name refusal, for **both** CREATE
    // TABLE forms: the reply to send when `name` is claimed by a drop that
    // has not committed, or nullopt when it is genuinely free.
    //
    // The unfiltered duplicate check answers "is a live relation using this
    // name" and is deliberately unfiltered so a second create is refused
    // (ddl-transactional.md §6). It cannot see the case this covers -
    // `DROP TABLE` retypes the `sys.objects` row in place, so the name
    // reads as free to everyone while the drop is still undoable, and a
    // create that took it would leave two live rows claiming one name once
    // the drop rolled back.
    std::optional<DispatchOutcome> RefuseIfNameHeldByPendingDrop(std::string_view name,
                                                                 Session& session);

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
    // hook is uninstalled by FinishDdlStatement, every exit.
    DdlScope DdlScopeFor(WriteScope& scope);
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
    void EndDdlScopeById(std::uint64_t txn_id);

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
    // stake.
    std::optional<txn::ReadView> ViewFor(Session& session);

    // **The statement boundary, taken exactly once per statement.**
    // Under READ COMMITTED a transaction re-mints its view at each
    // statement (`txn.md` §1), and before DT3c only the routes that
    // reached `SnapshotFor`/`BeginWrite` ever took it — so `DESCRIBE`,
    // `SHOW TABLES`, `SHOW INDEXES`, `ALTER`, `DROP TABLE` and the FK
    // parent lookup resolved under whatever view the transaction last
    // happened to hold, and could miss a relation committed since. That
    // is a READ COMMITTED violation and it breaks DT3c's own property
    // that every route agrees.
    //
    // Latched rather than called per site, because the alternative -
    // each caller taking its own boundary - moves the view *within* one
    // statement as soon as a handler resolves twice (the FK loop did),
    // and then two resolutions in one statement disagree. One latch,
    // reset per statement, is the single answer to "when does the view
    // move".
    Status EnsureStatementBoundary(Session& session);
    bool statement_boundary_taken_ = false;

public:
    // **AN-S3: a participant adopts the coordinator's snapshot** (AN-R5).
    // The transaction open on `session` takes `snapshot_lsn` as its view in
    // place of the one its own `BEGIN` minted, and this core's slot is
    // lowered to cover it before the transaction reads anything
    // (`TransactionManager::AdoptSnapshot`). **No caller since AT-S6**: the
    // shipped-statement executor that opened such a context with `BEGIN`
    // and called this went with the ship.
    Status AdoptSnapshot(Session& session, std::uint64_t snapshot_lsn);

private:
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

    // Transactions holding catalog rows nobody has committed yet. Empty
    // is the normal state and the one `ViewFor` optimises for. Entries
    // are removed when the transaction resolves, by `EndDdlScope`.
    std::vector<std::uint64_t> ddl_txns_;
    void EndDdlScope(const Session& session);
    // Delete-marked catalog rows retired since mount by the horizon-gated
    // purge EndDdlScope runs (ddl-transactional.md §5d). SHOW META
    // prints it beside `catalog_marks_finalized`, whose count is the
    // previous mount's leftovers - this one is this mount's own.
    std::uint64_t catalog_marks_purged_ = 0;
    // Records that this transaction now holds uncommitted catalog rows,
    // which is what turns on `ViewFor`'s filtering.
    void MarkHoldsDdl(txn::Transaction& txn);

    // `CREATE CABIN` / `DROP CABIN` (docs/spec/cabin.md §10). One handler
    // for both: they share a parse and a reply shape, and differ only in
    // which catalog call they reach. Takes the whole statement line rather
    // than a suffix, because the parser is what resolves the two
    // identifiers.
    DispatchOutcome HandleCabin(std::string_view line);

    // `ALTER TABLE ... RENAME TO | RENAME COLUMN` (docs/spec/alter.md,
    // workplan ALT03). One handler for both forms, for HandleCabin's
    // reason; the AL4 assertion RESTRICT and the AL7 system-relation
    // refusal live here, before the catalog write.
    DispatchOutcome HandleAlter(std::string_view line, Session& session);

    // `DROP TABLE <name>` (docs/spec/drop-table.md, workplan DT03). The
    // DT3 RESTRICT gate lives here - a referencing foreign key and an
    // assertion each refuse naming the blocker - before the catalog's
    // tombstone-and-retire; the in-memory Cabin sets are forgotten after.
    DispatchOutcome HandleDropTable(std::string_view line, Session& session);

    // `SHOW CABINS` - every declared Cabin, with what it has observed.
    //
    // The line joins two sources on purpose. The catalog says which
    // relation and column, who declared it, and whether it is serving - the
    // *declaration*, which is DDL and survives a restart. The core-local
    // store says how many values are observed, how many entries they hold,
    // and how the probes have gone - runtime state, which by §9 does not
    // survive a restart at all. Reporting them together is what makes "this
    // Cabin exists but has never been probed" visible.
    DispatchOutcome HandleShowCabins();

    // `CREATE INDEX` / `DROP INDEX` (docs/spec/index.md §10). One handler
    // for both, for HandleCabin's reason: they share a parse and a reply
    // shape and differ only in which catalog call they reach.
    // Emits one INDEX_INSERT per index mutation, or a full page image per
    // page a split restructured. Called **before** the HEAP_INSERT or
    // HEAP_OVERWRITE the entries point at (docs/spec/index.md §12.1): a
    // dangling entry is dropped by verification, a row with no entry is
    // lost.
    Status LogIndexWrites(const std::vector<exec::IndexWrite>& writes, std::uint64_t txn_id);

    // Both statements take the relation `X` before their first catalog write
    // and build where the session is (AT-S5e); the owner-built arm of
    // PW1c-6b-3 went with the per-core structures it served.
    DispatchOutcome HandleIndex(std::string_view line, Session& session);
    DispatchOutcome HandleShowIndexes(Session& session);

    // `CREATE ASSERTION` / `DROP ASSERTION` (docs/spec/assertion.md §3,
    // workplan AST03). One handler for both, for HandleCabin's reason.
    //
    // Validates the declaration against the catalog (§3.1), builds the
    // Bound Cabin (AST06), publishes the row and adopts the live directory
    // into the instance's registry, which is what makes the reply's
    // `enforcing=1` true rather than a claim about a row.
    //
    // **Built where the session is, whoever "owns" the relation** (AT-S5d).
    // The build was the owner's from PW1c-6c until then, shipped to it and
    // adopted into its registry, because only the owner wrote the relation
    // and only its registry was asked; the registry is one now and every
    // core writes. Non-transactional DDL (`ddl-transactional.md` §5): the
    // session is read only to tell its own transaction's `IX` from another
    // writer's when the build's relation `X` is refused (AT-S5e).
    DispatchOutcome HandleAssertion(std::string_view line, Session& session);

    // `SHOW ASSERTIONS` - every declared assertion, with the relation it is
    // on and its declaration verbatim.
    //
    // The `SHOW` surface rather than `SELECT * FROM sys.assertions`: a
    // catalog *view* is read through `catalog::Catalog` alone, and a
    // row-codec relation's rows need a `PageStore` to resolve their
    // var-heap spills. So the one row-codec catalog relation is surfaced by
    // `SHOW`, which has one. `sys.pattern_defs` had no view for the same
    // reason until it was withdrawn on 2026-08-31.
    DispatchOutcome HandleShowAssertions();
    DispatchOutcome HandleShowRelayout(std::string_view rest);
    DispatchOutcome HandleSetCabinOptimizer(std::string_view rest);

    // `SHOW CABIN_OPTIMIZER` - PO9's view (workplan PHY06): the switch and
    // budget line, the executor's applied-action counters, and one line
    // per managed candidate with its state, last B/C scores, S3 quality
    // rates and last logged action. Everything it prints already exists on
    // an inspection surface (`ManagedEntries`, `DecisionLog`, `counters`,
    // `QualityOf`) - this handler renders and never computes.
    DispatchOutcome HandleShowCabinOptimizer();

    // H6 step 3 (`observability.md` §10): `TRACE ON|OFF` is the manual
    // sampler, `SHOW TRACES` the ring, `SHOW TRACE <id>` one span tree with
    // self-time separated from child-time - which is the number that finds
    // the culprit, where total only says which subtree to open next.
    DispatchOutcome HandleTrace(std::string_view rest);
    DispatchOutcome HandleShowTraces();
    DispatchOutcome HandleShowTrace(std::string_view rest);

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

    // The forward check for one foreign key and one written value (§2),
    // **answered from what the extraction pass already resolved** (§2a,
    // AH-T1). OK when the value is not an id at all - the row codec has the
    // better error for that.
    //
    // `check_view` is still taken, and is used by exactly one arm: a
    // **self-referencing** foreign key, which `ResolveForeignKeyParents`
    // deliberately does not hoist. See its comment for why that arm is not
    // a hole in AH-R1.
    Status CheckForeignKeyOnWrite(const catalog::TableAccess& child,
                                  const catalog::ForeignKeyRef& fk, const parser::AstValue& value,
                                  const txn::ReadView& check_view,
                                  const exec::FkParentVerdicts& held);

    // The extraction pass (§2a, AH-R1): resolves every parent pk one row's
    // body names, into `into`, deduplicated by (parent relation, pk) so a
    // statement naming one parent from a thousand rows descends once.
    //
    // Called **before any row work** - at the dispatch fork for a statement
    // whose rows are all known there, and once per row otherwise. Nothing it
    // does may depend on a row having been written, which is what makes it
    // legal to run early and what the self-referencing carve-out protects.
    // `waiter` is the transaction this statement runs in (null in
    // autocommit before one is opened); it is what decides whether a busy
    // parent becomes a wait - see `WaitForParentRowWriter`.
    Status ResolveForeignKeyParents(const catalog::TableAccess& child,
                                     const std::vector<parser::AstValue>& body,
                                     const txn::ReadView& check_view,
                                     exec::FkParentVerdicts& into,
                                     txn::Transaction* waiter = nullptr);

    // **The forward check's wait** (AT-S5f): the parent row named by `pk`
    // is held by `holder`, which has not decided, so the statement waits
    // for it rather than being refused. Records the wait; it is taken in
    // `AwaitStatementWaits`, outside every page span, exactly as a write
    // block's is.
    //
    // Two shapes for one wait, and which one is used is a property of the
    // dispatcher rather than of the parent: with a lock table the parent
    // row's own borrow is asked for, so a holder on any core is waited
    // for; without one there is no peer to reach and `NoteBlockingWriter`'s
    // per-core predicate is the honest wait. The body states both.
    void WaitForParentRowWriter(txn::Transaction* waiter, catalog::Oid parent_rel,
                                std::uint64_t pk, std::uint64_t holder);

    // The body `ResolveForeignKeyParents` and the FK checks index into: the
    // columns after the pk, which is the shape every downstream consumer
    // takes. Mirrors `InsertOneRow`'s arity split without repeating its
    // refusals - a row of neither legal length yields an empty body here and
    // is refused there, in the order it always was.
    static std::vector<parser::AstValue> InsertBodyOf(
        const catalog::TableAccess& ta, const std::vector<parser::AstValue>& values);

    // The reverse check for every foreign key pointing at `parent` (§3),
    // run per row about to be delete-marked.
    // Every child is walked here since AT-S5f, whoever owns it: the answer
    // is this core's for the whole relation, so there is nothing resolved
    // elsewhere to read.
    Status CheckNoChildrenBeforeDelete(const catalog::TableAccess& parent, std::uint64_t parent_pk,
                                       const txn::ReadView& check_view);

    // One access shape, recorded by hand because a check is not a step
    // (FK-M4). Never fails a write.
    void RecordFkAccess(exec::AccessKind kind, catalog::Oid rel_oid, std::uint64_t column_mask);

    // The namespace a `CREATE TABLE ns.t` names, or `kNamespacePublic` for
    // an unqualified name (AF-T3). The one qualifier in the grammar that
    // decides rather than asserts: it decides the namespace the relation is
    // created in (and until AT-S9 its owner core). An unknown namespace is refused with its byte and is
    // **not** created - see `ast.hpp`'s namespace-qualifier rule.
    StatusOr<catalog::Oid> ResolveCreateNamespace(std::string_view qualifier,
                                                  std::uint32_t byte_offset,
                                                  const txn::ReadView* view);

    // A relation's name for a human-readable reply, or `oid=<n>` when it
    // cannot be resolved. Inspection surfaces only: catalog rows store oids
    // so they stay fixed width, and printing one is where the name is
    // needed. Never called from an execute path - resolving a name during
    // execution is what parser-v2.md I11 forbids.
    std::string RelationNameOf(catalog::Oid oid);

    // Every declared foreign key (docs/spec/foreign-keys.md §1). One line
    // per sys.fkeys row: which relation references which, through which
    // column. Prints `action=RESTRICT` unconditionally, because v1 has one
    // action (F2) - a stored action field would have exactly one value.
    DispatchOutcome HandleShowFkeys();
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
    std::optional<std::string> InsertOneRow(catalog::Oid oid, const catalog::TableAccess*& ta,
                                            const std::vector<parser::AstValue>& values,
                                            WriteScope& scope,
                                            const exec::FkParentVerdicts& fk_held,
                                            InsertRowResult& out);

    // T3, the sorted heap fill (docs/inflight/in-progress/workplan-t3.md). The gate is T3-2's,
    // conservative and only able to widen: heap-clustered, nothing that
    // maintains per-row (no index, no Cabin, no assertion), no spillable
    // schema. FK stays allowed - its checks run per row before anything
    // burns. Outside the gate the row loop runs, with byte-identical
    // replies and relation state - the equivalence test is the contract.
    bool SortedFillEligible(const catalog::TableAccess& ta, catalog::Oid oid) const;
    DispatchOutcome SortedFillInner(const parser::InsertStmt& stmt,
                                    catalog::Oid oid, const catalog::TableAccess& ta,
                                    WriteScope& scope);

    // The already-parsed half of InsertInner: everything after the parse -
    // cap, manager guard, resolution, affinity, the T3 gate, the row loop.
    // Split out for the KWP load session (docs/inflight/in-progress/workplan-kwp-load.md KW5),
    // whose rows arrive binary and never had text - BI2's "same write
    // path" made literal, since this IS the path a T1 statement takes.
    // **No `line` since AT-S5f.** The text was carried for the two things
    // that needed to re-run the statement elsewhere - the write ship, gone
    // at AT-S5, and the foreign-key probe's resume - so an INSERT now
    // reaches this path the way a KWP load chunk always did, with no text
    // and nowhere to send it.
    DispatchOutcome InsertParsed(const parser::InsertStmt& stmt, WriteScope& scope);

public:
    // KW5's public seam: run one parsed INSERT under `session` exactly as
    // HandleInsert runs a textual one - same write scope, same verdict
    // rule, same atomicity. The load session synthesizes an InsertStmt per
    // chunk and calls this.
    DispatchOutcome ExecuteInsert(const parser::InsertStmt& stmt, Session& session);

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
    DispatchOutcome HandleSelect(std::string_view line, Session& session,
                                 bool analyze = false);

    // The ANALYZE reply: run the chain for its counters, print the plan
    // beside them. Split out so HandleSelect's row-formatting path and
    // this one visibly share everything above the sink.
    //
    // `sql` is the stripped statement, taken so the reply can report the
    // statement's `pattern_id` - the same number `SHOW PATTERNS` lists a
    // row under, which is how an operator checks which observed pattern a
    // statement actually matched.
    // `trail` and `replay` are the same two halves an ordinary execution
    // gets. ANALYZE takes them because its contract is that the run it
    // describes is the run that actually happened: a diagnostic that
    // skipped replay would report descents no real execution performs.
    //
    // It takes no statement text: the `pattern_id` it prints comes from
    // `instance`, which the caller got from the parse. It used to re-lex
    // `sql` to recompute a number it had already been handed.
    //
    // `borrow` is the statement's read borrow, taken at the bind by
    // `HandleSelect` (AT-R1) rather than constructed here: a borrow made at
    // execution would be made after the compile it exists to protect.
    DispatchOutcome RunAnalyze(const exec::StepChain& chain, exec::TrailCollector* trail,
                               const exec::TrailReplay* replay,
                               const std::optional<stats::InstanceKey>& instance,
                               const txn::Snapshot& snapshot, exec::PositionSink& borrow);

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

    // All five at once, as an instance hands them to each core (AT-S8,
    // `StatementLimits`).
    void set_statement_limits(const StatementLimits& limits) noexcept {
        indexes_enabled_ = limits.indexes;
        max_insert_rows_ = limits.max_insert_rows;
        set_aggregate_limits(limits.aggregate);
        set_sort_max_rows(limits.sort_max_rows);
        set_join_build_max_rows(limits.join_build_max_rows);
    }


    // H6: the core-local trace ring this dispatcher records into when a
    // session has asked for it (`TRACE ON`). A dispatcher never told
    // collects nothing, which is every configuration that does not want the
    // instrument. `sink` must outlive this.
    void SetTraceSink(stats::TraceSink* sink) noexcept { traces_ = sink; }

    // **`SetAccessBatch` and `SetAccessStatsApplied` stood here and are
    // gone** (AT-S7). A peer had no way to write `sys.access_stats`, so it
    // folded its shapes into an `AccessBatch` and core 0 applied the fold;
    // both halves and the `SHOW META` block that reported them retire with
    // the ring kind, because every core writes the relation itself now
    // (`crosscore.md` CC13).

    // **R6-5's in-doubt ceiling stood here and is gone** (AT-S6):
    // `in_doubt_ceiling_ms` bounded a writer's wait on a cross-owner
    // transaction in doubt, and with 2PC retired nothing is in doubt. A
    // writer's wait is the lock family's, under `lock_wait_fault_net_ms`,
    // and the old key is refused at startup naming it.
    // **The instance lock table** (AO-R2), borrowed and null until an
    // expeditor hands one over. Its only use here is AO-S4a's wait-for
    // graph: with a table this core detects deadlock and may therefore let
    // a transaction that already holds rows wait; without one it keeps
    // AO-S3's guard, where only a transaction holding nothing waits and no
    // cycle can form. Both states are correct; the second is narrower.
    void set_locks(txn::LockTable* locks) noexcept { locks_ = locks; }
    // Transactions that reached `max_locks_per_txn` and kept writing with
    // an incomplete borrow ledger. **Expected zero**, and a cell that sees
    // it nonzero has found a statement whose borrows S6b would have to
    // refuse rather than truncate.
    // The cap's refusals since this dispatcher was built. A **refusal**
    // now, not a truncation: AO-S6c-c stopped swallowing it, so this counts
    // statements the cap ended rather than ledgers it silently shortened.
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

    // **The lock family's fault net**, from `lock_wait_fault_net_ms`. It
    // was the in-doubt ceiling until AT-S6 retired the thing that could be
    // in doubt; the quantity is the same one, re-scoped (AT-0 item 7).
    sched::MonoTimeNs LockWaitFaultNetNs() const noexcept { return lock_wait_fault_net_ns_; }
    void set_lock_wait_fault_net_ns(sched::MonoTimeNs ns) noexcept {
        lock_wait_fault_net_ns_ = ns;
    }


    // The physical optimizer's shadow surface (docs/spec/physical-optimizer.md
    // R3/R10, workplan PX06). A setter for `set_aggregate_limits`'s reason,
    // with the same default posture: a dispatcher never told behaves as the
    // documented configuration - shadow on, the spec's `[PROPOSED]` 600 s
    // half-life. `on` never reaches here: the config layer refuses it at
    // startup naming §6's gates.
    void set_relayout(PhysicalOptimizerMode mode, sched::MonoTimeNs half_life_ns) noexcept {
        relayout_mode_ = mode;
        decay_half_life_ns_ = half_life_ns;
    }

    // The cabin optimizer's signal collector (workplan PHY01), a setter
    // for the two above's reason. Null - every existing construction site,
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
        set_relayout(surface.relayout_mode, surface.decay_half_life_ns);
        set_optimizer_signals(surface.signals);
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

    // This core's reactor, for `SHOW META`'s group-accounting block
    // (`docs/spec/sched.md` §4's last bullet, owed since `bench/v2.1.0` §11-5).
    // The scheduler outlives this dispatcher on every core: core 0's is a
    // local in `Expeditor::Serve`, a peer's is `CoreRuntime::scheduler_`.
    // Null wherever no reactor runs the dispatcher - every socket-free test
    // - and the block is then omitted rather than printed as zeroes, the
    // rule the recovery block already follows.
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
    // A sibling of RunAnalyze rather than a branch inside the row loop, for
    // the reason ANALYZE is one: the two differ in what consumes the rows
    // and in nothing else, and a per-row `if` would put that difference
    // where it is paid for on every row of every statement.
    // `os` is the caller's buffer, already holding the column-heading line -
    // taken by reference rather than as a copied header, because building a
    // second `std::ostringstream` costs a stringbuf and a locale and was
    // measured as most of the fold's per-statement overhead (AP03).
    // `sink` is where the rows go; `text_sink` is the same object when
    // nothing else was installed, and is what the reply is taken from.
    // Two references to one thing on the newline path, because a sink that
    // is somebody else's has no reply to give back.
    // `borrow` as on `RunAnalyze` above (AT-R1).
    DispatchOutcome RunAggregated(ResultSink& sink, TextResultSink& text_sink,
                                  const exec::StepChain& chain, exec::TrailCollector* trail,
                                  const exec::TrailReplay* replay,
                                  const std::optional<stats::InstanceKey>& instance,
                                  const txn::Snapshot& snapshot, exec::PositionSink& borrow);

    // **The success-path recording point.** Three collectors observe the
    // same moment - a completed execution - and they are called from one
    // place so a fourth cannot be added to two of the three sites. Every
    // caller reaches here only after the execution succeeded; there is
    // deliberately no failure-path form (see RecordTrail).
    void RecordExecution(const std::optional<stats::InstanceKey>& instance,
                         exec::TrailCollector* trail, const exec::StepChain& chain,
                         const exec::ExecStats& stats);

    // Hands a successful execution's trail to the recorder. Shared by the
    // row-returning path and ANALYZE so the two cannot come to disagree
    // about when a trail is written.
    void RecordTrail(const std::optional<stats::InstanceKey>& instance,
                     exec::TrailCollector* trail, const exec::StepChain& chain);

    // Counts one execution of every step's access shape. Shared by the
    // row-returning path and ANALYZE, for the reason RecordTrail is: two
    // call sites that could disagree about when a statistic is written
    // would make the statistic mean two things.
    void RecordAccessShapes(const exec::StepChain& chain);

    // The cabin optimizer's S1/S2 (physical-optimizer.md §II.2,
    // workplan PHY01): one decayed touch per successful fingerprinted
    // SELECT, carrying the statement's page count. Beside RecordTrail and
    // RecordAccessShapes because it is the same moment - a completed
    // execution - observed by a third collector.
    void RecordOptimizerSignals(const std::optional<stats::InstanceKey>& instance,
                                const exec::StepChain& chain, const exec::ExecStats& stats);

    // ---- The Cabin write hook (docs/spec/cabin.md §5) --------------------
    //
    // **This is what "observed ⇒ complete" costs**, and the whole reason a
    // Cabin can be authoritative where a Waystone trail cannot: absence has
    // a witness, and this is the witness. One directory probe per cabined
    // column per write - core-local, in-memory, O(1), and skipped entirely
    // by the `cabin_mask == 0` test for a relation with no Cabin.
    //
    // Every mandatory action is an **append**. Nothing is ever removed here:
    // an older snapshot may still be entitled to match a row through the
    // undo chain, so eager removal is *incorrect* and not merely
    // unnecessary. The surplus is subtracted at read time by verification.
    //
    // `values[i]` is the value of column `first_col_pos + i`, which lets
    // INSERT pass the VALUES list (whose first entry is column 1, since the
    // pk is engine-issued) and UPDATE pass the whole decoded row. `pk`,
    // `page_id` and `slot` are the tuple's identity and its location - both
    // already in hand at both call sites, which is why C6's hints cost
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
    void NoteCabinWrite(const catalog::TableAccess& access,
                        std::span<const parser::AstValue> values, std::uint16_t first_col_pos,
                        std::uint64_t pk, PageId page_id, std::uint16_t slot,
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
    // `resume_from` is where a parked walk of this statement stopped
    // (AO-S3b); an inactive cursor is a first run and starts at the head,
    // which is what every caller but `DispatchAsync`'s resume passes. Not
    // defaulted, for `VisitRelation`'s `span` reason alone: a caller that
    // starts from the head says so. (An earlier note here claimed `= {}`
    // would not compile. That was true while `WalkCursor` was nested in
    // this class and stopped being true when it moved to namespace scope
    // in `session.hpp`; it is deleted rather than corrected, because the
    // rule above never needed it.)
    //
    // The snapshot is the *same* one the first run minted - the resumed
    // statement must not re-mint it, or the rows it already wrote and the
    // rows it has yet to reach would be read under two views.
    DispatchOutcome DeleteInner(std::string_view line, WriteScope& scope,
                                const txn::Snapshot& snapshot, WalkCursor resume_from);
    DispatchOutcome UpdateInner(std::string_view line, WriteScope& scope,
                                const txn::Snapshot& snapshot, WalkCursor resume_from);
    DispatchOutcome HandleSync();

    // Runs the insert against whichever storage the relation uses, and
    // reports the result in the vocabulary both share
    // (storage/insert_placement.hpp).
    StatusOr<storage::InsertPlacement> InsertIntoRelation(const catalog::TableAccess& access,
                                                          std::uint64_t id,
                                                          std::span<const std::byte> payload,
                                                          std::uint64_t trx_id);


    // A full ordered scan of the relation, whichever storage it uses. Both
    // walk sibling/next links left to right, so the row order is identical.
    //
    // `page_access` must be kWrite whenever `fn` modifies a tuple - UPDATE
    // and DELETE scan through here - and kRead otherwise, which is what
    // keeps a SELECT from dirtying every page it reads (page_store.hpp).
    //
    // `fn` returns storage::VisitControl: kStop ends the scan successfully,
    // which is what `LIMIT` and an `Exists` step will need and what no
    // caller here does yet.
    // `SELECT ... FROM sys.<view>`. Answered without the compiler: a
    // catalog view is materialized from the catalog's typed readers, not
    // walked out of pages, so it is not a relation a step can read
    // (exec/catalog_view.hpp).
    DispatchOutcome HandleCatalogView(const parser::SelectStmt& stmt);

    // `span` is the pk window the statement can possibly touch (R4/IS4),
    // and it narrows *which ranges are walked* - never which rows match,
    // which stays `fn`'s. `PkSpan::Whole()` is the whole relation, which is
    // what a predicate naming no pk means; a `WHERE pk = k` write passes
    // `PkSpan::Equality`, and on a spread relation that is the difference
    // between walking one range and meeting the ownership refusal on
    // somebody else's. Not defaulted: both callers have an answer, and a
    // default here would let a third one walk every range by omission.
    // `cursor` is AO-S3b's resume position: null for a walk that cannot
    // park (every read walk), otherwise in **and** out - read to start
    // where a parked walk stopped, written when this walk stops. Not
    // defaulted, for `span`'s reason: a caller that cannot park says so by
    // passing null rather than by omission.
    Status VisitRelation(
        const catalog::TableAccess& access, storage::PageAccess page_access,
        const std::function<StatusOr<storage::VisitControl>(PageId, heap::PageView&,
                                                            std::uint16_t)>& fn,
        catalog::PkSpan span, WalkCursor* cursor);

    // The page loop `VisitRelation`'s heap arm owns since AO-S3b, hoisted
    // out of `heap::ChainVisit` so the gap between two pages - no pin, no
    // span - is a place the statement above it may park (AO-R2).
    // `heads` is one chain per range in `lo` order (RD6).
    Status WalkHeapChains(
        std::span<const PageId> heads, storage::PageAccess page_access,
        const std::function<StatusOr<storage::VisitControl>(PageId, heap::PageView&,
                                                            std::uint16_t)>& fn,
        WalkCursor* cursor);

    // The same for a clustered btree, and **not the same shape**: a resume
    // descends by key rather than returning to a remembered leaf, which is
    // what makes it safe against a split that happened while the statement
    // was parked (`WalkCursor`, `session.hpp`).
    Status WalkBtreeLeaves(
        PageId root, storage::PageAccess page_access,
        const std::function<StatusOr<storage::VisitControl>(PageId, heap::PageView&,
                                                            std::uint16_t)>& fn,
        WalkCursor* cursor);

    // Appends the record set above for one placed tuple, stamps page_lsn
    // on every page it touched, and applies the durability class. A no-op
    // returning OK when no WalManager was supplied.
    //
    // A failure here is reported to the client and the tuple stays in the
    // page frame: the mutation happened, and the record describing it did
    // not. That is a lost write on a crash, not a wrong answer now, and
    // the alternative - unwinding a heap insert with no transaction
    // manager to unwind it - would be the worse lie. The WAL gate still
    // holds, because an unstamped page carries page_lsn 0 and a page whose
    // records failed to append is indistinguishable from one nothing
    // logged; closing that needs the abort path a transaction layer owns.
    // `leaf_type` is the page type a PAGE_INIT record names for a new tuple
    // page: kHeap for a chain, kBtreeLeaf for a tree.
    // `spills` are the var-heap values this tuple's cells point at. They
    // are logged *first*, before the HEAP_INSERT, which is the ordering
    // docs/rules/rule-fixed-length-tuple.md section 5 requires: a replay must
    // never reach a tuple whose pointer resolves to nothing. A crash
    // between the two leaves an unreferenced value for purge, which is the
    // harmless direction.
    // `own_txn` false means a TransactionManager owns the transaction and
    // has already logged TXN_BEGIN; this emits only the page records and
    // leaves TXN_COMMIT and its durability wait to EndWrite().
    // `owner_oid` (page.md §2a): the target relation's oid, carried by any
    // PAGE_INIT this insert emits so redo re-stamps what the live path
    // stamped.
    // Gives every spilled value a rollback: one undo record per spill, in
    // the writing transaction's chain, plus the trail entry a live Abort
    // reads. Both compensate by releasing the slot.
    //
    // **Called before whatever logs the VARHEAP_APPENDs**, because an
    // UNDO_WRITE must precede the record it can undo - RV3's rule for
    // catalog writes, for the same reason: redo alone must never resurrect
    // an append the undo phase has no record to release.
    //
    // A scope with no transaction records nothing, which is the pre-existing
    // unowned path (a dispatcher built without a manager, and the
    // `kNoTxnId` writes `LogChainInsert` makes for the assertion catalog).
    // Those spills still leak on rollback, and that is stated in
    // `workplan-varchar-char.md` rather than silently true.
    Status NoteSpills(const WriteScope& scope, std::uint32_t rel_oid, std::uint64_t pk,
                      const std::vector<exec::AppendedSpill>& spills);

    Status LogInsert(const storage::InsertPlacement& placed, PageType leaf_type,
                     std::span<const std::byte> tuple, std::uint64_t trx_id,
                     std::uint64_t owner_oid,
                     const std::vector<exec::AppendedSpill>& spills = {},
                     const std::vector<exec::IndexWrite>& index_writes = {},
                     bool own_txn = true);

    // One page's full image, logged and the page stamped behind it.
    //
    // **Four call sites wrote these ten lines identically** - an index split, a
    // var-heap link edit, a heap structural change, and bulk insert's per-page
    // images - and a fifth lives in `assertion_build.cpp`. An image is the
    // instrument for "no record type describes this change", so the pattern
    // recurs by design; what does not need to recur is the stamp, which is the
    // step a copy can silently omit (`redo.cpp` gates every record on
    // `page_lsn`, so a missing stamp is a record that replays when it should
    // not).
    //
    // No-op with no WAL attached, like every other logging helper here.
    Status LogFullPageImage(PageId page_id, std::uint64_t txn_id);

    // Every record a set of var-heap appends owes, in replay order: the
    // PAGE_INIT for a page the append created, the full page image for the
    // tail whose link now reaches it, then the VARHEAP_APPEND for the value.
    //
    // Shared by INSERT and UPDATE deliberately. The first two records were
    // missing entirely and the third was missing on the UPDATE path
    // (`docs/inflight/known-gaps.md`'s var-heap entry), and two copies of this
    // sequence is two chances to lose one of them again.
    //
    // The caller owes the *ordering*: these records precede the HEAP_INSERT or
    // HEAP_OVERWRITE whose cell points at the value, so a replay never reaches
    // a pointer that resolves to nothing (spec §5).

    // What a `WHERE id = <const>` statement should do instead of scanning.
    // The three cases are distinct because the *authority* of the answer
    // differs:
    //
    //   kScan    no shortcut. Scan; the scan is the authoritative path and
    //            produces the same answer. Every heap relation lands here,
    //            having no pk index to descend.
    //   kAt      look at this (page, slot) - a btree descent, which is
    //            authoritative.
    //   kAbsent  **no such row**, on authority. Only a btree descent can
    //            say this, so a heap relation never produces it.
    struct PkLookup {
        enum class Kind { kScan, kAt, kAbsent };
        Kind kind = Kind::kScan;
        TupleLocation at;
    };
    PkLookup LocateByPk(const catalog::TableAccess& access, std::uint64_t pk);

    // The row-relocation callback a rollback needs when a leaf division has
    // moved rows this transaction wrote (txn/manager.hpp's RowLocator).
    // Built per abort, never stored on the manager - see the definition.
    txn::TransactionManager::RowLocator RowLocatorForRollback();

    // The bytes of the page a located tuple sits on, for a reader. Reuses
    // the span the locator carried out when it has one, and fetches
    // read-only when it does not.
    //
    // Read paths only. A writer must go through page_store_.Get() even
    // when TupleLocation::page is populated: the span is the same frame
    // either way, but only Get() marks it dirty, and a write to a frame
    // nothing will write back is a write that never happened.

    // The pk value a WHERE clause is a *bare* equality against, or nullopt
    // if it is anything else - no WHERE, more than one condition, a non-pk
    // column, a non-equality operator, or a non-integer or negative
    // literal.
    //
    // Shared by SELECT and UPDATE so the two cannot disagree about which
    // predicates take the point path. Duplicating this check is how one
    // path ends up descending for a query the other correctly scans.
    std::optional<std::uint64_t> PkEqualityTarget(
        const catalog::TableAccess& access,
        const std::vector<parser::Condition>& where) const;

    // The pk window a predicate-shaped write walks (R4/IS4): one id when
    // the WHERE is a bare pk equality inside the 40-bit space, so a split
    // relation's walk visits the one range that can hold it, and the whole
    // relation otherwise. A literal above the space names no row and walks
    // whole, answering `0 rows` rather than a resolve error.
    catalog::PkSpan WriteWalkSpan(const catalog::TableAccess& access,
                                  const std::vector<parser::Condition>& where) const {
        const std::optional<std::uint64_t> pk = PkEqualityTarget(access, where);
        return pk.has_value() && *pk <= kMaxKeystoneId ? catalog::PkSpan::Equality(*pk)
                                                       : catalog::PkSpan::Whole();
    }

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

    // **The allowance, taken and given back structurally** (AO-S6d).
    // `CommitAckScope`'s shape, and the argument for it is the one the
    // `DispatchAsync` site already makes about a hand-placed pair: it is
    // correct today and silently wrong the day a `co_await` or an early
    // `co_return` appears between the two lines. There are three of them
    // now - the statement's own dispatch, the write-block re-run and the
    // probe arm's resume - and the third sits inside a loop next to a
    // suspension point, which is exactly the shape the argument names.
    // Restores rather than clears, so the three cannot come to disagree
    // about what "off" was.
    class MayParkScope {
    public:
        MayParkScope(CommandDispatcher& owner, bool allowed) noexcept
            : owner_(owner), saved_(owner.may_park_) {
            owner_.may_park_ = allowed;
        }
        ~MayParkScope() { owner_.may_park_ = saved_; }
        MayParkScope(const MayParkScope&) = delete;
        MayParkScope& operator=(const MayParkScope&) = delete;

    private:
        CommandDispatcher& owner_;
        bool saved_;
    };
    // Where the statement now in flight owes a D2 commit's acknowledgement
    // (see `CommitAck`). A member for `may_park_`'s reason; unlike it, the
    // stamp is scoped, because leaking this one drops a durability wait
    // rather than granting a parking allowance.
    CommitAck commit_ack_ = CommitAck::kWhenDurable;

    // The stamp, and the whole of its lifetime. Restores rather than
    // assigning the default, so a nested dispatch would compose - there is
    // none today, and a guard that assumed so would be the kind of thing
    // that stops being true quietly.
    class CommitAckScope {
    public:
        CommitAckScope(CommandDispatcher& owner, CommitAck ack) noexcept
            : owner_(owner), saved_(owner.commit_ack_) {
            owner_.commit_ack_ = ack;
        }
        ~CommitAckScope() { owner_.commit_ack_ = saved_; }
        CommitAckScope(const CommitAckScope&) = delete;
        CommitAckScope& operator=(const CommitAckScope&) = delete;

    private:
        CommandDispatcher& owner_;
        CommitAck saved_;
    };
    // Minted `Session::ship_id()` until AT-S6, which retired the id with
    // the ship; nothing reads it.
    std::uint64_t next_ship_session_id_ = 1;
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
    // A plain member for `statement_boundary_taken_`'s reason, stated in
    // the same place: one statement runs at a time on a core (sched.md
    // §3), so per-statement state is a member and not a parameter. The
    // alternative was threading a `Session&` into `LogInsert` and
    // `AwaitDdlDurability`, neither of which has one or wants one - and a
    // second copy of the precedence chain at each site, which is the drift
    // `Session::EffectiveDurability` exists to prevent.
    //
    // Initialised to the server's class so a caller that reaches a commit
    // path without a dispatch - there is none today, and the field must
    // still be defined if one appears - behaves exactly as before.
    wal::DurabilityClass effective_durability_ = durability_;

    // The per-statement work ceiling, from `max_rows_touched`. Held by
    // value and handed to each execution, which takes its own copy - so
    // one statement's spend never carries into the next.
    exec::Budget budget_;

    // AG11's caps, handed to every fold this dispatcher runs. Held by value
    // for the reason `budget_` is: a limit is a property of the server's
    // configuration, and reading it per statement from somewhere else would
    // let one statement's fold see a different ceiling from the next.
    // AG07 makes the two numbers config keys; until then they are spec §6's
    // `[PROPOSED]` defaults.
    exec::AggregateLimits aggregate_limits_;

    // The fold, **reused rather than constructed per statement** (workplan
    // AP03). Same reason `trail_scratch_` and `replay_scratch_` beside it
    // are hoisted, and the same shape of measurement: building one per
    // statement cost about 4 microseconds of server CPU on a pk lookup,
    // roughly 6.5% of what that statement spends there, nearly all of it
    // allocation for buffers the previous statement had already sized.
    //
    // `Reset` points it at each statement's spec and labels, which live on
    // that statement's chain - so between statements it holds pointers that
    // are not valid, and nothing may read it there. That is the same
    // contract `trail_scratch_` has with `Clear()`.
    exec::Aggregator aggregator_;

    // The output sort (OB4), hoisted for `aggregator_`'s reason and holding
    // the same contract: `Reset` points it at one statement's keys, and
    // between statements it holds a buffer nothing may read. Statements
    // that wrote no `ORDER BY`, and those whose order the compiler elided,
    // leave it inactive and untouched.
    exec::OutputSort sorter_;

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
    // first. Defaults off here so a dispatcher built without one - which
    // is every pre-existing test - behaves exactly as it always did.
    bool replay_enabled_ = false;

    // Reused across statements so recording costs no allocation on the read
    // path - a collector reserves a whole trail's worth of room, and doing
    // that per SELECT is an 8 KB malloc per query. Cleared at the start of
    // each execution, never read between them.
    exec::TrailCollector trail_scratch_{stats::kMaxTrailEntries};

    // The replay index, reused across statements for the same reason.
    exec::TrailReplay replay_scratch_;

    // Whether a successful SELECT records its access shapes
    // (`access_statistics`). Defaults **on**: unlike Waystone this collects
    // input for a decision nobody has made yet, and a physical optimizer
    // that arrives to an empty history is a physical optimizer that has to
    // wait for one.
    bool access_stats_enabled_;
    stats::AccessStatsCounters access_counters_;

    // The core-local Cabin store, or null when cabins are switched off
    // (`cabins = off`). Null is the default here so a dispatcher built
    // without one - which is every pre-existing test - behaves exactly as it
    // always did, and so that "identical replies with cabins on and off" is
    // a property of the structure rather than of the test data.
    stats::CabinStore* cabins_ = nullptr;

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

    // R6-5's three, on `pending_commit_lsn_`'s terms and for its reason:
    // one statement runs at a time on a core, so a member is exact and
    // threading three values through every `*Inner()` and `EndWrite` would
    // be signatures on a dozen functions for a case that fires on a
    // failure. All three are zeroed at the top of `DispatchAndStage` and
    // read out at its end.
    //
    // `blocking_writer_` is the undecided transaction that refused this
    // statement, `blocked_pk_` the row it holds, and
    // `statement_trail_mark_` the transaction's trail length when this
    // statement's write scope opened - which is what says whether the
    // statement wrote anything before it was refused, and therefore whether
    // re-running it is a repeat or a second application.
    std::uint64_t blocking_writer_ = 0;
    std::uint64_t blocked_pk_ = 0;

    // On `blocking_writer_`'s terms, AO-S6e-b's lock wait: set where the borrow is
    // refused, read back out by `DispatchAndStage` into
    // `DispatchOutcome::lock_wait`. It carries a `shared_ptr`, so the slot
    // outlives the DDL transaction whose ask registered it - which it must,
    // because that transaction is unwound before the park it is for.
    std::optional<DispatchOutcome::LockWait> lock_wait_ = std::nullopt;
    std::size_t statement_trail_mark_ = 0;

    // **The fault net every wait in this file is bounded by**, and the
    // one thing `lock_wait_fault_net_ms` sets. Defaulted to the constant
    // rather than to 0, which is what makes a dispatcher nobody
    // configured - every fixture - behave exactly as it did when the four
    // wait sites read the constant directly. **0 is a value, not an
    // absence**: it is the other policy, a statement refused at once
    // instead of waiting, and the key's documentation promises it.
    sched::MonoTimeNs lock_wait_fault_net_ns_ = txn::kLockWaitFaultNetNs;
    // See `set_locks`. Null means no detector, which means AO-S3's guard.
    txn::LockTable* locks_ = nullptr;

    // The transaction manager, or null when this dispatcher predates
    // transactions - which every socket-free test does, and which is why
    // null must behave exactly as the engine did before MVCC: every write
    // stamped kBootstrapXid, every read seeing everything.
    txn::TransactionManager* txn_ = nullptr;

    // The session a caller who passed none gets. One per dispatcher rather
    // than one per statement so `SET ISOLATION LEVEL` still means something
    // to a single-connection tool, and so an autocommit write does not
    // allocate a session per statement.
    // The level a new session starts at, from the `isolation` config key.
    // Held so TcpServer can stamp it on each connection's session rather
    // than every connection defaulting to the compiled-in level.
    // ---- Core affinity (crosscore.md CC3/§6) ---------------------------
    //
    // Which core this dispatcher runs on, and the refused-write counters
    // §6 asks for. 0 is the system core and the only value a single-core
    // build ever has, so every pre-multicore construction site is unchanged.
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
    // **The `Status` and not only the detail**, which is what the first
    // draft carried and was a defect: `InsertOneRow` answers a rendered
    // string with no status, so the outcome reached `KwpSession` status-less
    // and `StatusFromErrorReply` folded the cap's line into
    // `InvalidArgument` - `kErrorSpellings` has no `ResourceExhausted`
    // entry. The detail rode out beside it, and `protocol.md` §11's details
    // are **per category**, so a client was handed `kLockCap` in the
    // invalid-argument namespace, where it means nothing. That is worse
    // than carrying no detail at all, on the path most likely to reach the
    // cap.
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

    // The physical optimizer's mode and R1 half-life (workplan PX06).
    // Shadow costs nothing at rest - the planner is pull-only, computed
    // when `SHOW RELAYOUT` asks - so shadow is the default here as it is
    // in the config.
    PhysicalOptimizerMode relayout_mode_ = PhysicalOptimizerMode::kShadow;
    sched::MonoTimeNs decay_half_life_ns_ = 600'000'000'000ULL;

    // PHY01's collector, and the per-statement counters that feed its S2.
    // The ExecStats is hoisted for the aggregator's reason: `For()` sizes a
    // vector, and a member reused across statements makes the ordinary
    // SELECT allocate nothing for its counting.
    stats::OptimizerSignals* optimizer_signals_ = nullptr;
    exec::ExecStats exec_stats_;
    // §II.6's switch, off by default and experimental. **The instance's
    // since AT-S8** (`OptimizerSurface::cabin_optimizer_on`): a `SET` on any
    // core flips the one flag the controller's cadence reads, where it used
    // to flip a per-dispatcher bool that only core 0's was ever read from.
    // A dispatcher handed none points at its own.
    std::atomic<bool> own_cabin_optimizer_on_{false};
    std::atomic<bool>* cabin_optimizer_on_ = &own_cabin_optimizer_on_;
    // Held across the controller view's read (AT-S8): core 0's cadence
    // mutates the controller under it. Null where one thread owns both.
    Latch* cabin_optimizer_view_latch_ = nullptr;
    const MountRecovery* recovery_ = nullptr;  // RC09, set_recovery()

    // PHY06's view sources: the controller's managed table and decision
    // log, the executor's applied-action counters. Read-only - the view
    // renders, it never drives - and null wherever the controller was
    // never constructed (`cabins = off`, or a test that wired neither).
    const stats::CabinOptimizer* cabin_controller_ = nullptr;
    const exec::CabinOptimizerExecutor* cabin_executor_ = nullptr;
    // ---- H6: per-request tracing (`observability.md` §10 steps 1-3) -----
    //
    // `traces_` is the core-local ring; `tracing_` is the session's
    // `TRACE ON`; `trace_` is the context of the statement in flight, null
    // on every untraced one - which is what every `SpanScope` in this file
    // branches on and the whole of the disabled path.
    //
    // Owned here rather than passed, for `pending_commit_lsn_`'s reason
    // stated at `DispatchAndStage`: one statement runs at a time on a core,
    // so there is no second value to confuse it with, and the alternative
    // is a parameter on a dozen signatures. Off by default, so a dispatcher
    // that is never told behaves exactly as it did.
    stats::TraceSink* traces_ = nullptr;
    stats::TraceContext* trace_ = nullptr;
    bool tracing_ = false;

    // This core's reactor, set_scheduler_view(); null off a reactor.
    const sched::Scheduler* scheduler_view_ = nullptr;

    // **Nothing ships since AT-S6.** `MayShip`, `MayEnrolShip`,
    // `ShipStatement`, `SoleForeignOwner`, `ForwardAnswerEdge`,
    // `FinishShippedStatement`, `PrepareAcrossOwners` and
    // `DescribePrepareFailure` stood here: the fork that decided a
    // statement belonged on another core, the send, the park, the answer
    // edge's forwarding, and D4's two phases over the participants the
    // send enrolled. A read runs where the session is now, as a write has
    // since AT-S5, so there is no statement to send and no participant to
    // prepare (`docs/spec/cross-owner-txn.md`).

    // The local commit and rollback - the whole of both since AT-S6,
    // where they used to be one arm of a fork.
    DispatchOutcome CommitLocal(Session& session, wal::Lsn* commit_lsn = nullptr);
    DispatchOutcome RollbackLocal(Session& session);



    // **The one refusal left before a write touches a page** (AT-S9, which
    // renamed it from `CheckWriteAffinity` when the last question about a
    // core left it): a relation under an assertion the instance cannot
    // enforce refuses its writes on every core. Every write verb asks it
    // once per statement.
    Status CheckWriteAdmission(const catalog::TableAccess& access);

    txn::IsolationLevel default_isolation_ = txn::IsolationLevel::kReadCommitted;

    Session autocommit_session_;

    // Implicit-transaction ids for the statements this dispatcher logs.
    // Process-local and restarting from 1 every boot, which is wrong the
    // moment recovery reads two boots' worth of one stream back - ids from
    // different runs would collide. Allocating them durably is the
    // transaction manager's job (wal.md section 12 has no owner yet), so
    // this is deliberately the cheapest thing that produces a distinct id
    // per statement within a run, and it is a known gap, not an oversight.
    std::uint64_t next_txn_id_ = 1;
};

}  // namespace kds::server
