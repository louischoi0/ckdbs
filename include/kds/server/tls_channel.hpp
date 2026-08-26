#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "kds/base/status.hpp"
#include "kds/server/wire_channel.hpp"

// The TLS wire channel (docs/spec/protocol.md §1): OpenSSL driven entirely
// through memory BIOs, so the channel is the pure byte transformer
// wire_channel.hpp requires - it owns no fd, performs no I/O, and both
// ends of a handshake can be pumped against each other in a unit test
// with no socket anywhere. Direct TLS, decided 2026-08-13: the first
// byte on a TLS-enabled port is a ClientHello, and there is no
// STARTTLS-style upgrade from plaintext.
//
// Protocol floor: TLS 1.3, compiled in as the *minimum*. KDS ships its
// own clients (protocol.md D1), so there is no legacy population to
// accommodate and no downgrade surface worth carrying.
//
// Lock/atomic protocol: none. A context is immutable after creation and
// safe to share across channels on one core; a channel belongs to one
// connection on one reactor thread, like the connection buffers it
// feeds (docs/rules/rules.md §3).
//
// OpenSSL stays behind this header: no <openssl/*> include appears
// outside the KDS_WITH_TLS translation units (CMakeLists.txt), which is
// what keeps a no-OpenSSL build possible.

namespace kds::server {

class TlsChannel;

// One process-lifetime TLS configuration: the certificate, the key, and
// the verification policy, from which per-connection channels are cut.
//
// PEM *contents*, never paths: reading files - and refusing to start on
// a bad one - is the config layer's job (expeditor.cpp), and keeping the
// filesystem out of this class is what lets tests build contexts from
// generated strings.
class TlsContext {
public:
    // A server context: `cert_pem` is presented to every client,
    // `key_pem` must be its private key (checked at creation, refused
    // with InvalidArgument naming the OpenSSL reason otherwise).
    static StatusOr<TlsContext> NewServer(std::string_view cert_pem, std::string_view key_pem);

    // A client context trusting exactly the certificate(s) in
    // `trust_pem` - for the unit tests and, later, the conformance
    // harness. Chain verification only; no hostname check, because the
    // peers it exists to reach are memory BIOs and loopback.
    static StatusOr<TlsContext> NewClient(std::string_view trust_pem);

    TlsContext(TlsContext&&) noexcept;
    TlsContext& operator=(TlsContext&&) noexcept;
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;
    ~TlsContext();

    // A fresh channel in this context's role, or nullptr if OpenSSL
    // cannot allocate one. The channel holds a reference to the context's
    // OpenSSL state (SSL_new refcounts the SSL_CTX), so it stays valid if
    // the TlsContext is destroyed first - which is what lets a factory
    // hand back a channel and drop the context it was cut from.
    std::unique_ptr<TlsChannel> NewChannel() const;

private:
    struct Impl;
    explicit TlsContext(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// One connection's TLS state machine. All three WireChannel methods
// follow its append-only buffer convention.
//
// The handshake is driven by OnWireData; a channel that must speak first
// (the client role) is kicked by calling OnWireData with an empty
// wire_in, which emits the ClientHello. Send() before the handshake
// completes buffers the plaintext and releases it the moment the
// handshake finishes - the caller never tracks handshake state.
class TlsChannel final : public WireChannel {
public:
    ~TlsChannel() override;

    Status OnWireData(std::string_view wire_in, std::string& plain,
                      std::string& wire_out) override;
    Status Send(std::string_view plain, std::string& wire_out) override;
    void Close(std::string& wire_out) override;

    // Observability for tests: whether the handshake has completed, and
    // whether the peer has sent its orderly close_notify.
    bool handshake_done() const noexcept;
    bool peer_closed() const noexcept;

private:
    friend class TlsContext;
    struct Impl;
    explicit TlsChannel(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace kds::server
