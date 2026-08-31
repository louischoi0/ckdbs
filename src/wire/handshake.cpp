#include "kds/wire/handshake.hpp"

#include <algorithm>
#include <string>

// The negotiation itself (docs/spec/protocol.md §3, protocol-wp.md P07).
// Every rule and every refusal is argued on the header; this file is the
// arithmetic.

namespace kds::wire {

namespace {

HandshakeOutcome Refuse(ProtocolDetail detail, std::string message) {
    HandshakeOutcome out;
    out.accepted = false;
    // Fatal without exception: §11 lists handshake failures beside framing
    // corruption as the two cases that close a connection, and there is no
    // handshake refusal a client can recover from in place - it has
    // nothing yet to sync back to.
    out.error = ProtocolError(detail, std::move(message), Severity::kFatal);
    return out;
}

}  // namespace

HandshakeOutcome Negotiate(const ClientHello& hello, const HandshakeConfig& config,
                           bool auth_required, std::uint64_t session_id,
                           std::uint64_t cancel_key) {
    // The magic is checked by `DecodeClientHello`, which refuses a payload
    // that does not carry it - so a hello that reaches here has already
    // proved the peer speaks KWP. Nothing re-checks it; a second check
    // would be a second answer to one question.

    // ---- Version (§3) ---------------------------------------------------
    //
    // A client whose own range is inverted is refused before the
    // intersection is computed, because `[5, 2]` intersects everything
    // under the arithmetic below and means nothing.
    if (hello.min_version > hello.max_version) {
        return Refuse(ProtocolDetail::kUnsupportedVersion,
                      "C_HELLO carries min_version " + std::to_string(hello.min_version) +
                          " above max_version " + std::to_string(hello.max_version));
    }
    const std::uint16_t chosen = std::min(hello.max_version, config.max_version);
    const std::uint16_t floor = std::max(hello.min_version, config.min_version);
    if (chosen < floor) {
        return Refuse(ProtocolDetail::kUnsupportedVersion,
                      "no common KWP version: client speaks [" +
                          std::to_string(hello.min_version) + ", " +
                          std::to_string(hello.max_version) + "], this server speaks [" +
                          std::to_string(config.min_version) + ", " +
                          std::to_string(config.max_version) + "]");
    }

    // ---- The TLS demand (§1) --------------------------------------------
    const bool wants_tls =
        (hello.capabilities & static_cast<std::uint64_t>(Capability::kTlsRequired)) != 0;
    if (wants_tls && !config.tls_active) {
        return Refuse(ProtocolDetail::kUnexpectedFrame,
                      "C_HELLO set TLS_REQUIRED on a connection that is not inside TLS; this "
                      "port serves plaintext, and a port that speaks TLS speaks it from its "
                      "first byte");
    }

    // ---- Authentication (§3, §14) ---------------------------------------
    if (hello.auth_method != kAuthNone && hello.auth_method != kAuthScramSha256) {
        return Refuse(ProtocolDetail::kUnexpectedFrame,
                      "C_HELLO named auth_method " + std::to_string(hello.auth_method) +
                          "; this server offers 0 (none) and 1 (SCRAM-SHA-256), and MTLS is "
                          "reserved rather than implemented");
    }
    if (auth_required && hello.auth_method == kAuthNone) {
        return Refuse(ProtocolDetail::kUnexpectedFrame,
                      "this server requires authentication: send C_HELLO with auth_method 1 "
                      "(SCRAM-SHA-256)");
    }
    if (!auth_required && hello.auth_method == kAuthScramSha256) {
        // Refused rather than downgraded. A client that asked to
        // authenticate and was quietly admitted without doing so has been
        // told it is on a secured connection when it is not - the same
        // failure `kTlsRequired` above refuses, one layer up.
        return Refuse(ProtocolDetail::kUnexpectedFrame,
                      "C_HELLO asked for SCRAM-SHA-256 but this server has no credential store "
                      "and admits every connection; it will not pretend to authenticate one");
    }

    HandshakeOutcome out;
    out.accepted = true;
    out.auth_owed = hello.auth_method == kAuthScramSha256;
    out.hello.version = chosen;
    // The intersection, which is also how an unknown bit is ignored.
    out.hello.capabilities = hello.capabilities & config.capabilities;
    out.hello.session_id = session_id;
    out.hello.cancel_key = cancel_key;
    out.hello.server_info = config.server_info;
    return out;
}

}  // namespace kds::wire
