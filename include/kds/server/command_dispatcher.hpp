#pragma once

#include <string>
#include <string_view>

#include "kds/catalog/catalog.hpp"
#include "kds/server/superblock.hpp"

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
// today: there is no SQL parser yet (src/parser is still an empty
// placeholder) and no query executor (the exec_* resumable state machines
// the legacy engine had). Real INSERT/SELECT support arrives once those
// exist; this only exposes what Catalog/SuperBlock can already do.
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
    CommandDispatcher(SuperBlock& superblock, catalog::Catalog& catalog) noexcept
        : superblock_(superblock), catalog_(catalog) {}

    // Parses and executes one line. Never fails outward: a malformed or
    // unrecognized line produces an "ERR ..." response rather than any
    // kind of error return - a bad line from one client must never be
    // able to bring the dispatcher (or the server driving it) down.
    //
    // Recognized commands (case-insensitive):
    //   PING                  -> "PONG"
    //   STOP                  -> "OK bye" and should_stop = true
    //   SHOW META             -> superblock stats, one line
    //   LIST TABLES           -> space-separated table names
    //   FIND TABLE <name>     -> "oid=<n>" or "ERR ..."
    DispatchOutcome Dispatch(std::string_view line);

private:
    DispatchOutcome HandleShowMeta();
    DispatchOutcome HandleListTables();
    DispatchOutcome HandleFindTable(std::string_view args);

    SuperBlock& superblock_;
    catalog::Catalog& catalog_;
};

}  // namespace kds::server
