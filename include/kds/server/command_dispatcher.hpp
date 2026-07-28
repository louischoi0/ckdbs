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
// Command set is intentionally small and matches what's actually built
// today: src/parser can parse full CREATE TABLE/INSERT/SELECT/UPDATE
// statements, but nothing here calls into it yet - there is no query
// executor (the exec_* resumable state machines the legacy engine had),
// and column types in that grammar can't be resolved to on-disk type_val/
// len without the not-yet-ported type registry (see parser/ast.hpp's file
// comment). CREATE TABLE below is therefore a minimal wire-level command,
// not the SQL statement: it takes a bare name, no column list, and always
// creates a zero-column ClusteredType::kHeap table. Real INSERT/SELECT and
// SQL-grammar CREATE TABLE support arrive once the executor and type
// registry exist; until then this only exposes what Catalog/SuperBlock
// can already do without them.
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
    //   FIND TABLE <name>     -> "oid=<n>" or "ERR ..."
    //   CREATE TABLE <name>   -> "CREATED oid=<n>" if newly created,
    //                            "EXISTS oid=<n>" if a table with this
    //                            name already exists (idempotent - does
    //                            NOT error or create a duplicate). Always
    //                            a zero-column ClusteredType::kHeap table
    //                            under the public namespace - see the file
    //                            comment above for why.
    DispatchOutcome Dispatch(std::string_view line);

private:
    DispatchOutcome HandleShowMeta();
    DispatchOutcome HandleListTables();
    DispatchOutcome HandleFindTable(std::string_view args);
    DispatchOutcome HandleShowPage(std::string_view args);
    DispatchOutcome HandleCreateTable(std::string_view args);

    SuperBlock& superblock_;
    catalog::Catalog& catalog_;
    storage::PageStore& page_store_;
};

}  // namespace kds::server
