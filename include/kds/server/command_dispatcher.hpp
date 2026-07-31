#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/parser/ast.hpp"
#include "kds/sched/clock.hpp"
#include "kds/server/superblock.hpp"
#include "kds/stats/waystone.hpp"
#include "kds/stats/waystone_hooks.hpp"
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
// Command set was originally a small, fixed vocabulary because nothing
// called into src/parser: there was no query executor, and column types
// in its grammar couldn't be resolved to on-disk type_val/len without a
// type registry. That gap is now closed for a narrow scope - see
// src/exec/row_codec.hpp's file comment for exactly what's supported
// (no NULLs, no float/decimal columns, fixed-width ints + varchar only) -
// by using Catalog::ResolveTypeByName() (sys.types, populated by
// Bootstrap()) as the type registry stand-in. UPDATE is still not wired
// in. CREATE TABLE keeps its original bare-name form (no parens - always
// a zero-column ClusteredType::kHeap table) alongside the new SQL form,
// disambiguated by whether a '(' follows the table name, so existing
// callers of the bare form are unaffected.
//
// Protocol: one command per line, case-insensitive keyword, arguments
// space-separated. A response is always exactly one line back (never
// containing embedded newlines) - the platform-layer listener appends the
// line terminator itself.
//
// ---- WAL (2026-07-30): INSERT is logged, nothing else is ----------------
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
// ---- Clustered type (2026-07-31): one dispatcher, two storages ----------
//
// A relation is either a chain of heap pages (`ClusteredType::kHeap`,
// heap_chain.hpp) or a clustered B+ tree (`kBtree`, btree.hpp), and every
// statement handler below branches on `TableAccess::clustered_type` in
// exactly one place - `InsertIntoRelation`, `VisitRelation`, `LocateByPk`.
// Everything else in this file is storage-agnostic, which is possible
// because a btree **leaf is a heap page**: the row codec, `PageView`
// reads/overwrites, `HEAP_INSERT`, Waystone's `(page_id, slot)` and the
// `SHOW PAGE` dump all work on either without knowing which they hold.
//
// The observable differences are narrow and worth stating:
//
//   - `SELECT`/`UPDATE ... WHERE id = <n>` on a btree relation descends
//     the tree, which is **authoritative** - a miss means the row does not
//     exist, and no scan follows. The same statement on a heap relation
//     may probe Waystone, which is advisory and always falls back.
//   - `INSERT` may split a leaf and grow the tree a level, in which case
//     the relation's `desc_page_id` is repointed at the new root before
//     the client is answered.
//   - A full scan of either is a left-to-right walk of the same
//     `next_page_id` links, so `SELECT *` returns rows in the same order.
//
// ---- Waystone (2026-07-30) ----------------------------------------------
//
// Off for every relation until `WAYSTONE ENABLE <table>` turns it on
// (docs/waystone-concpets.md section 7). While it is on:
//
//   INSERT  additionally writes one 32-byte entry through the relation's
//           page directory - O(1), a handful of page touches, no fsync.
//   SELECT  with a `WHERE id = <const>` predicate probes for the tuple's
//           location instead of scanning the chain.
//
// The probe is advisory and the fallback is unconditional: a miss, a
// stale entry, an epoch mismatch, or a tuple at the named slot carrying
// the wrong Keystone id all fall through to the same full `ChainVisit`
// the query would have done anyway (invariant 8 as amended, spec 3.1).
// The one property that must survive every change here is that turning
// Waystone off, or deleting its pages, changes no result.
//
// `WAYSTONE ENABLE` is a dispatcher command rather than `CREATE TABLE`
// syntax because the parser is being replaced wholesale (docs/parser.md
// PR01+) and adding a keyword to the legacy grammar would be work thrown
// away. It is a development surface on a protocol already documented as
// one; KWP/1 decides the real spelling.
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
};

// Where a tuple lives, as a Waystone probe reports it. Local to the
// dispatcher because it is the shape of an answer to "skip the scan and
// look here", not a storage-layer concept.
struct TupleLocation {
    PageId page_id = kInvalidPageId;
    std::uint16_t slot = 0;
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
    // logging them - the pre-2026-07-30 behaviour, which the socket-free
    // unit tests and the catalog-level tests still run on.
    CommandDispatcher(SuperBlock& superblock, catalog::Catalog& catalog,
                       storage::PageStore& page_store, Logger* log = nullptr,
                       const sched::Clock* clock = nullptr, wal::WalManager* wal = nullptr,
                       wal::DurabilityClass durability = wal::DurabilityClass::kGroup) noexcept
        : superblock_(superblock),
          catalog_(catalog),
          page_store_(page_store),
          log_(log),
          clock_(clock),
          wal_(wal),
          durability_(durability) {}

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
    //                            descent, or a Waystone probe).
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
    DispatchOutcome Dispatch(std::string_view line);

private:
    DispatchOutcome HandleShowMeta();
    DispatchOutcome HandleListTables();
    DispatchOutcome HandleDescribe(std::string_view args);
    DispatchOutcome HandleShowPage(std::string_view args);
    DispatchOutcome HandleCreateTable(std::string_view args);
    DispatchOutcome HandleCreateTableSql(std::string_view line);
    DispatchOutcome HandleInsert(std::string_view line);
    DispatchOutcome HandleSelect(std::string_view line);
    DispatchOutcome HandleUpdate(std::string_view line);
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
    Status VisitRelation(const catalog::TableAccess& access,
                         const std::function<Status(PageId, heap::PageView&, std::uint16_t)>& fn);

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
    Status LogInsert(const storage::InsertPlacement& placed, PageType leaf_type,
                     std::span<const std::byte> tuple, std::uint64_t trx_id);

    DispatchOutcome HandleWaystone(std::string_view args);

    // Records a freshly inserted tuple's location, growing the relation's
    // directory first if the new id has outrun it.
    //
    // Returns the (possibly re-acquired) TableAccess, because growth is a
    // catalog write and a catalog write invalidates the cache: the caller's
    // `const TableAccess*` is dangling afterwards. Returning the new one
    // makes that impossible to forget, which a `Status` return would not.
    //
    // A failure here is logged and swallowed by the caller, never surfaced:
    // Waystone is advisory, and an INSERT that succeeded must not be
    // reported as failed because a hint structure could not be updated.
    StatusOr<const catalog::TableAccess*> RecordWaystoneInsert(
        const catalog::TableAccess& access, std::uint64_t id,
        const storage::InsertPlacement& placed);

    // What a `WHERE id = <const>` statement should do instead of scanning.
    // The three cases are distinct because the *authority* of the answer
    // differs, and that difference is invariant 8's whole content:
    //
    //   kScan    no shortcut - or one that declined. Scan; the scan is the
    //            authoritative path and produces the same answer.
    //   kAt      look at this (page, slot). For a btree this is the
    //            authoritative descent; for a heap relation it is a
    //            Waystone probe that has already validated epoch and
    //            Keystone id.
    //   kAbsent  **no such row**, on authority. Only a btree descent can
    //            say this - a Waystone miss never can (spec 3.1 rule 1), so
    //            a heap relation never produces it.
    struct PkLookup {
        enum class Kind { kScan, kAt, kAbsent };
        Kind kind = Kind::kScan;
        TupleLocation at;
    };
    PkLookup LocateByPk(const catalog::TableAccess& access, std::uint64_t pk);

    // The location a `WHERE id = <const>` select should look at first, or
    // nullopt to scan. Every reason to decline - relation not enabled,
    // predicate not a bare pk equality, entry missing or stale, wrong
    // Keystone id at the target - collapses to nullopt, because the caller
    // must do the same thing in all of them.
    std::optional<TupleLocation> ProbeForPk(const catalog::TableAccess& access,
                                                  std::uint64_t pk);

    // The pk value a WHERE clause is a *bare* equality against, or nullopt
    // if it is anything else - no WHERE, more than one condition, a non-pk
    // column, a non-equality operator, a non-integer or negative literal,
    // or a relation with no live Waystone.
    //
    // Shared by SELECT and UPDATE so the two cannot disagree about which
    // predicates are probeable. Duplicating this check is how one path ends
    // up probing a query the other correctly scans.
    std::optional<std::uint64_t> PkEqualityTarget(
        const catalog::TableAccess& access,
        const std::vector<parser::Condition>& where) const;

    // Reads `pk` out of the Keystone word of the tuple at (page, slot).
    // Passed to stats::ProbeAndVerify as its id reader.
    static std::optional<std::uint64_t> KeystoneIdAtSlot(storage::PageStore& store,
                                                         PageId page_id, std::uint16_t slot,
                                                         void* ctx);

    // The relation's Waystone as the hooks and probe want it.
    static stats::WaystoneRef WaystoneRefOf(const catalog::TableAccess& access) noexcept {
        return stats::WaystoneRef{access.waystone_dir_root, access.waystone_dir_depth};
    }

    // Diagnostics. Levels are chosen so the default (info) is quiet under
    // load: DDL and SYNC are Info because they are rare and consequential,
    // a completed query is Debug, and the per-tuple heap events are Trace.
    // Enabling trace on a busy server costs a write() per tuple - it is a
    // development tool, not an operating mode.
    // Dispatch() wraps this to time it and log the outcome once, in one
    // place, rather than at every return of every handler.
    DispatchOutcome DispatchInner(std::string_view line);

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

    // Every page at epoch 0 until a physical optimizer exists to bump one
    // (waystone-workplan.md T19). Correct today because nothing moves a
    // tuple; owned here rather than injected because there is exactly one
    // implementation and swapping it is T19's job, not a caller's.
    stats::StaticEpochProvider epochs_;

    // Probe accounting, so "is Waystone doing anything" is answerable
    // without a profiler. Reported by WAYSTONE STATUS.
    std::uint64_t waystone_probe_hits_ = 0;
    std::uint64_t waystone_probe_misses_ = 0;

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
