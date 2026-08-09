#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/aggregate.hpp"
#include "kds/exec/bound_cabin.hpp"
#include "kds/exec/budget.hpp"
#include "kds/exec/plan_printer.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/pattern_ddl.hpp"
#include "kds/exec/cabin_ddl.hpp"
#include "kds/exec/index_maintain.hpp"
#include "kds/parser/ast.hpp"
#include "kds/stats/access_stats.hpp"
#include "kds/stats/cabin_store.hpp"
#include "kds/stats/trail_recorder.hpp"
#include "kds/stats/trail_store.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/coro.hpp"
#include "kds/server/core_affinity.hpp"
#include "kds/server/session.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/manager.hpp"

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

namespace kds::server {

struct DispatchOutcome {
    std::string response;
    bool should_stop = false;

    // The commit this statement staged, when the client may not be told
    // about it until the log is durable (`durability = group`, docs/wal.md
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
};

// The one spelling of an error reply on the newline protocol (docs/txn.md
// §5, docs/protocol.md §11): `ERR <TOKEN> retryable=<b> <message>` for the
// codes a client library switches on - TXN_CONFLICT, FK_VIOLATION,
// ASSERTION_VIOLATION - and `ERR <message>` for everything else. Every
// dispatcher path reports through it, which is what keeps the shape from
// drifting between them; declared here so the spellings, a compatibility
// surface, can be pinned by a test that owns no socket and no dispatcher.
std::string ErrorReply(const Status& status);

// Where a tuple lives, as a point lookup reports it. Local to the
// dispatcher because it is the shape of an answer to "skip the scan and
// look here", not a storage-layer concept.
struct TupleLocation {
    PageId page_id = kInvalidPageId;
    std::uint16_t slot = 0;
    // The page's bytes, when whoever produced this location already had
    // them - a btree descent does. Empty means "fetch page_id yourself";
    // a reader must handle both. Dynamic extent because it has to have an
    // unset state (btree.hpp Location::leaf).
    std::span<std::byte> page{};
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
                       std::uint32_t core_id = 0, bool indexes = true) noexcept
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
          default_isolation_(isolation),
          autocommit_session_(isolation),
          core_id_(core_id),
          indexes_enabled_(indexes) {}

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
    //                            section per sys.patterns row, carrying
    //                            `origin=user|auto` and `pinned=yes|no`
    //                            plus `name=` and `params=` for the ones
    //                            that were declared (auto patterns have no
    //                            sys.pattern_defs row and stay bare hex).
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
    //                            input (docs/heap-and-tuple.md §7), keyed
    //                            by *columns* and never by values - so
    //                            `WHERE flag = 1` and `WHERE flag = 2` are
    //                            one shape, which is what keeps the list
    //                            bounded by the schema rather than by the
    //                            data.
    //   CREATE PATTERN <name> ($p <type> [, ...]) [WITH (<k> = <v>, ...)]
    //       OF <select>
    //                         -> "CREATED PATTERN name=<s>
    //                            pattern_id=0x<hex> dir_depth=<n>
    //                            params=<n>", or "ADOPTED PATTERN ..." when
    //                            an auto-registered row for the same shape
    //                            already existed and was upgraded in place
    //                            (keeping its recorded trails). Checks that
    //                            pass with something to say append
    //                            "\n"-escaped "WARN ..." sections - an
    //                            implicit conversion, or a body whose trail
    //                            can never replay. See
    //                            src/exec/pattern_ddl.hpp for the full
    //                            validation chain and where the error /
    //                            warning line falls.
    //   DROP PATTERN <name>   -> "DROPPED PATTERN name=<s>
    //                            pattern_id=0x<hex>". Removes the
    //                            declaration, not the shape: the waystones
    //                            are left for retention, and auto
    //                            registration may re-learn the shape later
    //                            as a nameless row.
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
    //   CREATE TABLE <name> (<col> <type> [, ...]) [HEAP | BTREE]
    //                         -> same CREATED/EXISTS response as above,
    //                            but with real columns: parsed via
    //                            src/parser, types resolved through
    //                            Catalog::ResolveTypeByName(). The trailing
    //                            keyword picks the storage: HEAP (default)
    //                            is a chain of heap pages, BTREE is a
    //                            clustered B+ tree on the Keystone pk. See
    //                            src/exec/row_codec.hpp for the supported
    //                            column type set.
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
    DispatchOutcome Dispatch(std::string_view line, Session* session = nullptr);

    // The statement path without the durability wait: it runs the statement
    // and reports any commit it staged through `DispatchOutcome::pending_lsn`.
    // Both entry points above go through it; they differ only in how they
    // wait.
    DispatchOutcome DispatchAndStage(std::string_view line, Session* session);

    // The suspendable form. Writes its reply through `out` - which the
    // **caller** owns and must keep alive across suspension - and finishes
    // with a Status describing the dispatch itself, not the statement (a
    // failed statement is an "ERR ..." in `out`, exactly as it is for the
    // synchronous form).
    //
    // ---- Why both forms exist -------------------------------------------
    //
    // This is the seam `docs/workplan-crosscore.md` P4 needs: a statement
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
    sched::Coro DispatchAsync(std::string_view line, Session* session, DispatchOutcome* out);

    // The level a fresh session starts at (`isolation`). TcpServer stamps
    // it on each connection's session at accept.
    txn::IsolationLevel default_isolation() const noexcept { return default_isolation_; }

private:
    // ---- Transaction control (docs/txn.md sections 1, 6) ----------------
    DispatchOutcome HandleBegin(std::string_view args, Session& session);
    DispatchOutcome HandleCommit(Session& session);
    DispatchOutcome HandleRollback(Session& session);
    DispatchOutcome HandleSetIsolation(std::string_view args, Session& session);

    // The read view a statement reads through. In autocommit it is minted
    // fresh here and belongs to no transaction; inside an explicit one it
    // is the transaction's, re-minted per statement under READ COMMITTED
    // and held since BEGIN under REPEATABLE READ.
    //
    // Returns a snapshot that sees everything when no TransactionManager
    // was given - the pre-MVCC engine, exactly.
    StatusOr<txn::Snapshot> SnapshotFor(Session& session);

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
    Status EndWrite(Session& session, WriteScope& scope, const Status& result);

    // The trx_id a write stamps: the scope's transaction, or
    // kBootstrapXid when there is no manager.
    static std::uint64_t WriterId(const WriteScope& scope);

    DispatchOutcome HandleShowMeta();
    DispatchOutcome HandleListTables();
    DispatchOutcome HandleDescribe(std::string_view args);
    DispatchOutcome HandleShowPage(std::string_view args);
    DispatchOutcome HandleShowPatterns();
    DispatchOutcome HandleShowAccess();

    // `SHOW BUDGET` - every relation's Keystone id consumption
    // (`docs/keystoneid-invariant.md` K-M4). Listed for *every* relation
    // including the catalog's own, because two of those - sys.patterns and
    // sys.pattern_defs - genuinely issue ids, and a listing that hid them
    // would hide the only relations whose consumption an operator does not
    // control.
    DispatchOutcome HandleShowBudget();

    DispatchOutcome HandleCreateTable(std::string_view args);
    DispatchOutcome HandleCreateTableSql(std::string_view line);

    // `CREATE PATTERN` / `DROP PATTERN`. Both take the whole statement
    // line, not a suffix: a declaration's stored canon is its own text
    // verbatim, so the parser has to see exactly what the client sent.
    DispatchOutcome HandleCreatePattern(std::string_view line);
    DispatchOutcome HandleDropPattern(std::string_view line);

    // `CREATE CABIN` / `DROP CABIN` (docs/feat-cabin.md §10). One handler
    // for both: they share a parse and a reply shape, and differ only in
    // which catalog call they reach. Takes the whole statement line, like
    // the pattern handlers, because the parser is what resolves the two
    // identifiers.
    DispatchOutcome HandleCabin(std::string_view line);

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

    // `CREATE INDEX` / `DROP INDEX` (docs/feat-index.md §10). One handler
    // for both, for HandleCabin's reason: they share a parse and a reply
    // shape and differ only in which catalog call they reach.
    // Emits one INDEX_INSERT per index mutation, or a full page image per
    // page a split restructured. Called **before** the HEAP_INSERT or
    // HEAP_OVERWRITE the entries point at (docs/feat-index.md §12.1): a
    // dangling entry is dropped by verification, a row with no entry is
    // lost.
    Status LogIndexWrites(const std::vector<exec::IndexWrite>& writes, std::uint64_t txn_id);

    DispatchOutcome HandleIndex(std::string_view line);
    DispatchOutcome HandleShowIndexes();

    // `CREATE ASSERTION` / `DROP ASSERTION` (docs/feat-assertion.md §3,
    // workplan AST03). One handler for both, for HandleCabin's reason.
    //
    // **This is the catalog half only.** It validates the declaration
    // against the catalog - the relation exists, every GROUP BY column
    // exists, the SUM column exists and is int64, the name is free (§3.1) -
    // and records it. It builds no Bound Cabin (AST04), enforces nothing
    // (AST07) and scans nothing (AST06), so an assertion recorded today
    // constrains no write. Saying so in the reply is deliberate: a
    // constraint that silently does not run is not a degraded mode.
    DispatchOutcome HandleAssertion(std::string_view line);

    // `SHOW ASSERTIONS` - every declared assertion, with the relation it is
    // on and its declaration verbatim.
    //
    // The `SHOW` surface rather than `SELECT * FROM sys.assertions`, for the
    // reason `sys.pattern_defs` has no view either: a catalog *view* is read
    // through `catalog::Catalog` alone, and a row-codec relation's rows need
    // a `PageStore` to resolve their var-heap spills. Both row-codec catalog
    // relations are therefore surfaced by `SHOW`, which has one.
    DispatchOutcome HandleShowAssertions();

    // ---- Foreign-key checks (docs/impl-foreign-keys.md §§2-4) -----------
    //
    // The write paths' three entry points. They live here rather than in
    // `exec/` because they are what turns a verdict into a *reply* - which
    // needs relation names, the access-statistics switch, and the retryable
    // spelling - while the verdicts themselves are `exec::fk_check`'s, so
    // there is exactly one implementation of each check.

    // A read view of **now**, for a constraint check. See §4: not the
    // statement's snapshot, because a check reads latest state.
    StatusOr<txn::ReadView> CheckView(const WriteScope& scope);

    // The forward check for one foreign key and one written value (§2).
    // OK when the value is not an id at all - the row codec has the better
    // error for that.
    Status CheckForeignKeyOnWrite(const catalog::TableAccess& child,
                                  const catalog::ForeignKeyRef& fk, const parser::AstValue& value,
                                  const txn::ReadView& check_view);

    // The reverse check for every foreign key pointing at `parent` (§3),
    // run per row about to be delete-marked.
    Status CheckNoChildrenBeforeDelete(const catalog::TableAccess& parent, std::uint64_t parent_pk,
                                       const txn::ReadView& check_view);

    // One access shape, recorded by hand because a check is not a step
    // (FK-M4). Never fails a write.
    void RecordFkAccess(exec::AccessKind kind, catalog::Oid rel_oid, std::uint64_t column_mask);

    // A relation's name for a human-readable reply, or `oid=<n>` when it
    // cannot be resolved. Inspection surfaces only: catalog rows store oids
    // so they stay fixed width, and printing one is where the name is
    // needed. Never called from an execute path - resolving a name during
    // execution is what parser-v2.md I11 forbids.
    std::string RelationNameOf(catalog::Oid oid);

    // Every declared foreign key (docs/impl-foreign-keys.md §1). One line
    // per sys.fkeys row: which relation references which, through which
    // column. Prints `action=RESTRICT` unconditionally, because v1 has one
    // action (F2) - a stored action field would have exactly one value.
    DispatchOutcome HandleShowFkeys();
    DispatchOutcome HandleInsert(std::string_view line, Session& session);

    // The statement itself, inside a write scope the wrapper opened and
    // will close. Split so that every early return below is an ordinary
    // return rather than one that has to remember to end a transaction.
    DispatchOutcome InsertInner(std::string_view line, WriteScope& scope);
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
    // statement's `pattern_id` - the number a `CREATE PATTERN` declaration
    // printed, which is how an operator checks that traffic actually
    // matches what they declared.
    // `trail` and `replay` are the same two halves an ordinary execution
    // gets. ANALYZE takes them because its contract is that the run it
    // describes is the run that actually happened: a diagnostic that
    // skipped replay would report descents no real execution performs.
    //
    // It takes no statement text: the `pattern_id` it prints comes from
    // `instance`, which the caller got from the parse. It used to re-lex
    // `sql` to recompute a number it had already been handed.
    DispatchOutcome RunAnalyze(const exec::StepChain& chain, exec::TrailCollector* trail,
                               const exec::TrailReplay* replay,
                               const std::optional<stats::InstanceKey>& instance,
                               const txn::Snapshot& snapshot);

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

private:
    // The aggregated SELECT path (docs/feat-aggregate.md AG1): the same
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
    DispatchOutcome RunAggregated(const exec::StepChain& chain, std::ostringstream& os,
                                  exec::TrailCollector* trail, const exec::TrailReplay* replay,
                                  const std::optional<stats::InstanceKey>& instance,
                                  const txn::Snapshot& snapshot);

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

    // ---- The Cabin write hook (docs/feat-cabin.md §5) --------------------
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

    // `DELETE FROM <t> [WHERE ...]` (docs/txn.md sections 4.3, 6).
    //
    // A **delete-mark**, never a physical removal: the slot keeps its bytes
    // and gains kSlotFlagDeleted, and the deleter's id goes in the tuple's
    // writer field. That pair is the whole of DELETE in the no-xmax model,
    // and it is why an older snapshot still reads the row - it steps back
    // over the kDeleteMark undo record and finds the tuple's own payload
    // unchanged.
    //
    // **The Cabin write hook is deliberately not called here.** By
    // feat-cabin.md section 5 removal is forbidden: an older snapshot may
    // still be entitled to match the row through the undo chain, so
    // dropping its entry would break the superset invariant. The surplus is
    // subtracted at read time, which now includes the visibility predicate.
    DispatchOutcome HandleDelete(std::string_view line, Session& session);
    DispatchOutcome DeleteInner(std::string_view line, WriteScope& scope,
                                const txn::Snapshot& snapshot);
    DispatchOutcome UpdateInner(std::string_view line, WriteScope& scope,
                                const txn::Snapshot& snapshot);
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

    Status VisitRelation(
        const catalog::TableAccess& access, storage::PageAccess page_access,
        const std::function<StatusOr<storage::VisitControl>(PageId, heap::PageView&,
                                                            std::uint16_t)>& fn);

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
    // docs/rule-fixed-length-tuple.md section 5 requires: a replay must
    // never reach a tuple whose pointer resolves to nothing. A crash
    // between the two leaves an unreferenced value for purge, which is the
    // harmless direction.
    // `own_txn` false means a TransactionManager owns the transaction and
    // has already logged TXN_BEGIN; this emits only the page records and
    // leaves TXN_COMMIT and its durability wait to EndWrite().
    Status LogInsert(const storage::InsertPlacement& placed, PageType leaf_type,
                     std::span<const std::byte> tuple, std::uint64_t trx_id,
                     const std::vector<exec::AppendedSpill>& spills = {},
                     const std::vector<exec::IndexWrite>& index_writes = {},
                     bool own_txn = true);

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

    // The bytes of the page a located tuple sits on, for a reader. Reuses
    // the span the locator carried out when it has one, and fetches
    // read-only when it does not.
    //
    // Read paths only. A writer must go through page_store_.Get() even
    // when TupleLocation::page is populated: the span is the same frame
    // either way, but only Get() marks it dirty, and a write to a frame
    // nothing will write back is a write that never happened.
    StatusOr<std::span<std::byte, kPageSize>> PageForRead(const TupleLocation& at);

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
    Logger* log_;
    const sched::Clock* clock_;
    wal::WalManager* wal_;
    wal::DurabilityClass durability_;

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

    // The live Bound Cabin directories, keyed by assertion id (workplan
    // AST06): CREATE ASSERTION's build moves its directory in here, DROP
    // erases it, and AST07's admission check is the reader this exists for.
    // Core-local like everything on this dispatcher (feat-assertion.md
    // §6.1). The entry *pages* are durable; this map is the memory-resident
    // half a restart loses until recovery replays it (AST05's fold) - which
    // is one of the two reasons nothing reports `enforcing=1` yet.
    std::unordered_map<std::uint64_t, exec::BoundCabin> bound_cabins_;

    // The commit a write path staged and did not wait for, read out at the
    // end of DispatchAndStage(). One statement runs at a time on a core, so
    // this cannot hold two.
    wal::Lsn pending_commit_lsn_ = wal::kNoLsn;

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

    // The read-path index switch (`indexes`, default on). Read-path only:
    // maintenance is not switchable, because an index that stops being
    // maintained is wrong rather than slow.
    bool indexes_enabled_ = true;
    CrossCoreWriteCounters cross_core_writes_;

    // Refuses a write to a relation this core may not write, and binds the
    // transaction's home core on the first one that is allowed. See
    // core_affinity.hpp - the restriction is decided (CC3), not a stand-in
    // for the pipeline.
    Status CheckWriteAffinity(const catalog::TableAccess& access, std::string_view relation,
                              Session& session);

    // Refuses a read whose chain touches a relation owned by another core.
    // Temporary in a way the write check is not: this is what the step
    // pipeline will replace, and it exists so the refusal names the reason
    // instead of surfacing as a page-store fault.
    Status CheckReadAffinity(const exec::StepChain& chain);

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
