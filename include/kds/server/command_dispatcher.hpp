#pragma once

#include <string>
#include <string_view>

#include "kds/catalog/catalog.hpp"
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
    CommandDispatcher(SuperBlock& superblock, catalog::Catalog& catalog,
                       storage::PageStore& page_store) noexcept
        : superblock_(superblock), catalog_(catalog), page_store_(page_store) {}

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
    //   FIND TABLE <name>     -> "oid=<n> root_page_id=<n> clustered_type=<HEAP|BTREE>"
    //                            or "ERR ..."
    //   CREATE TABLE <name>   -> "CREATED oid=<n>" if newly created,
    //                            "EXISTS oid=<n>" if a table with this
    //                            name already exists (idempotent - does
    //                            NOT error or create a duplicate). Always
    //                            a zero-column ClusteredType::kHeap table
    //                            under the public namespace. Legacy bare
    //                            form - no column list.
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
    //                         -> "INSERTED oid=<table_oid> slot=<n>" or
    //                            "ERR ...". Values are positional, one per
    //                            schema column in `pos` order - see
    //                            ast.hpp: no explicit column list in this
    //                            grammar.
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
    DispatchOutcome HandleFindTable(std::string_view args);
    DispatchOutcome HandleShowPage(std::string_view args);
    DispatchOutcome HandleCreateTable(std::string_view args);
    DispatchOutcome HandleCreateTableSql(std::string_view line);
    DispatchOutcome HandleInsert(std::string_view line);
    DispatchOutcome HandleSelect(std::string_view line);
    DispatchOutcome HandleUpdate(std::string_view line);
    DispatchOutcome HandleSync();

    SuperBlock& superblock_;
    catalog::Catalog& catalog_;
    storage::PageStore& page_store_;
};

}  // namespace kds::server
