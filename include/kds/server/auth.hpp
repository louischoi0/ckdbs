#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "kds/base/status.hpp"
#include "kds/server/scram.hpp"

// The connection-level authentication gate (docs/protocol.md D8's auth
// stage, on the text protocol until KWP P07 exists). TcpServer routes
// every line of an unauthenticated connection here instead of to the
// dispatcher, so a statement cannot run - and STOP cannot stop the
// server - before the client proves who it is.
//
// The exchange, one SCRAM message per line, raw (SCRAM messages are
// printable and comma-separated; they cannot contain a newline):
//
//   C: AUTH SCRAM-SHA-256 <client-first-message>
//   S: AUTH+ <server-first-message>
//   C: AUTH <client-final-message>
//   S: AUTH+ <server-final-message>        and the connection is open
//
// Any other line before authentication, and any failed step, answers
// "ERR ..." and severs the connection. KWP will carry these same SCRAM
// message bodies in handshake frames; only this line framing is
// protocol-specific.
//
// No OpenSSL appears here: the SCRAM types come from scram.hpp, and the
// implementations live behind KDS_WITH_TLS in src/server/auth.cpp.

namespace kds::server {

// One connection's gate. Owned by the connection, like its WireChannel.
class AuthGate {
public:
    struct Result {
        std::string reply;           // one line, newline appended by the caller
        bool authenticated = false;  // the exchange just succeeded
        bool close = false;          // sever after sending `reply`
    };

    virtual ~AuthGate() = default;
    virtual Result OnLine(std::string_view line) = 0;
};

// Where verifiers come from - an interface so the file-backed v1 store
// and a future catalog-backed one (the authorization Open Decision's
// territory) are interchangeable under the gate.
class CredentialStore {
public:
    virtual ~CredentialStore() = default;
    // NotFound for an unknown user; the SCRAM server mocks the rest of
    // the exchange so the caller's reply shape does not reveal which.
    virtual StatusOr<scram::Verifier> Lookup(std::string_view username) const = 0;
};

// The v1 store: a flat file, one "<username> <verifier>" per line, '#'
// comments, loaded whole at startup. A malformed line or a duplicate
// user refuses the whole file naming the line - the config_file.cpp
// posture, because a users file that is quietly half-applied is an
// authentication hole, not a convenience.
class FileCredentialStore final : public CredentialStore {
public:
    static StatusOr<FileCredentialStore> Load(const std::string& path);
    static StatusOr<FileCredentialStore> Parse(std::string_view text, const std::string& origin);

    StatusOr<scram::Verifier> Lookup(std::string_view username) const override;
    bool Has(std::string_view username) const {
        return users_.find(std::string(username)) != users_.end();
    }
    std::size_t size() const noexcept { return users_.size(); }

private:
    std::unordered_map<std::string, scram::Verifier> users_;
};

// The SCRAM-SHA-256 gate over a store. One per connection; the store
// outlives every gate (the Expeditor owns it for the server's life).
class ScramAuthGate final : public AuthGate {
public:
    explicit ScramAuthGate(const CredentialStore* store);
    Result OnLine(std::string_view line) override;

private:
    scram::Server server_;
    bool got_first_ = false;
};

}  // namespace kds::server
