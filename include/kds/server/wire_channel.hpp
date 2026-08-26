#pragma once

#include <string>
#include <string_view>

#include "kds/base/status.hpp"

// The byte-transform seam between a socket and the protocol above it
// (docs/spec/protocol.md §1: TLS sits below whichever protocol the port
// speaks). TcpServer reads and writes *wire* bytes; a connection with a
// WireChannel installed passes them through it, and one without a channel
// is exactly the plaintext path that existed before this seam did.
//
// The channel is a pure transformer - it owns no fd, calls no syscall,
// and never blocks - which is what keeps the TLS state machine runnable
// under deterministic simulation with scripted byte streams (rules.md #4:
// the seam is injectable, the syscalls stay in tcp_server.cpp).
//
// Buffer convention: every method *appends* to its output strings and
// never clears them, so a caller can accumulate into a connection's
// outbox across calls.

namespace kds::server {

class WireChannel {
public:
    virtual ~WireChannel() = default;

    // Wire bytes that arrived from the peer. Appends any application
    // bytes that became readable to `plain`, and any wire bytes the
    // channel itself must send (handshake replies, alerts) to `wire_out`.
    //
    // A non-OK status is fatal to the connection: the transform cannot
    // continue (e.g. a TLS record that fails authentication), and the
    // caller must close. Bytes already appended to `wire_out` before the
    // failure - typically the fatal alert - should still be flushed
    // best-effort.
    virtual Status OnWireData(std::string_view wire_in, std::string& plain,
                              std::string& wire_out) = 0;

    // Application bytes to send to the peer. Appends their wire form to
    // `wire_out`. May buffer internally until the handshake completes;
    // buffered bytes are released by a later OnWireData/Send call.
    virtual Status Send(std::string_view plain, std::string& wire_out) = 0;

    // Appends the orderly-shutdown bytes (TLS close_notify) to `wire_out`,
    // best effort - the caller may not manage to flush them, exactly like
    // the STOP goodbye in tcp_server.cpp.
    virtual void Close(std::string& wire_out) = 0;
};

}  // namespace kds::server
