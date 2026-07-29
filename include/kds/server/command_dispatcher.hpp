#pragma once

#include <string>
#include <string_view>

#include "kds/base/log.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/sched/clock.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/page_store.hpp"

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

namespace kds::server {

struct DispatchOutcome {
    std::string response;
    bool should_stop = false;
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
    CommandDispatcher(SuperBlock& superblock, catalog::Catalog& catalog,
                       storage::PageStore& page_store, Logger* log = nullptr,
                       const sched::Clock* clock = nullptr) noexcept
        : superblock_(superblock),
          catalog_(catalog),
          page_store_(page_store),
          log_(log),
          clock_(clock) {}

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
    //                         -> heap page header + slot directory dump.
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
    //                             columns=<n>", then one "\n"-escaped
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
    //                            Catalog::ResolveTypeByName(). BTREE is
    //                            parsed but rejected (not implemented -
    //                            see catalog.cpp). See
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
    //                         -> a full heap-page scan (root page only -
    //                            no multi-page chains yet), WHERE-filtered.
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
};

}  // namespace kds::server
