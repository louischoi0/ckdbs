#include "kds/server/tls_channel.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <utility>
#include <vector>

// One of the KDS_WITH_TLS translation units - the only places
// <openssl/*> appears (this file, scram.cpp, their tests, and main.cpp's
// provisioning mode). Compiled only under that option.
//
// Like tcp_server.cpp with its syscalls, this file is a platform
// boundary: the handshake's randomness comes from OpenSSL's own RNG, not
// the injected source rules.md §4 requires of engine logic. That is
// acceptable here for the reason it is for the socket layer - the
// channel below the seam is not engine logic, and everything above it
// (the byte streams a test scripts) stays deterministic.

namespace kds::server {
namespace {

struct SslCtxFree {
    void operator()(SSL_CTX* p) const noexcept { SSL_CTX_free(p); }
};
struct SslFree {
    void operator()(SSL* p) const noexcept { SSL_free(p); }
};
struct BioFree {
    void operator()(BIO* p) const noexcept { BIO_free(p); }
};
struct X509Free {
    void operator()(X509* p) const noexcept { X509_free(p); }
};
struct PkeyFree {
    void operator()(EVP_PKEY* p) const noexcept { EVP_PKEY_free(p); }
};

using UniqueSslCtx = std::unique_ptr<SSL_CTX, SslCtxFree>;
using UniqueSsl = std::unique_ptr<SSL, SslFree>;
using UniqueBio = std::unique_ptr<BIO, BioFree>;
using UniqueX509 = std::unique_ptr<X509, X509Free>;
using UniquePkey = std::unique_ptr<EVP_PKEY, PkeyFree>;

// Drains OpenSSL's thread-local error queue into one line. Called only
// on failure paths, and every SSL_* call it reports on is opened by its
// own ERR_clear_error(), so what it says belongs to that call and not to
// a predecessor. That per-call clear is also what SSL_get_error()
// requires to classify WANT_READ correctly: it inspects the error queue
// *before* the retry flags, so one stale entry would turn a benign retry
// into a fatal connection error.
std::string ErrString() {
    std::string out;
    unsigned long code;  // NOLINT(google-runtime-int) - OpenSSL's type
    char buf[256];
    while ((code = ERR_get_error()) != 0) {
        ERR_error_string_n(code, buf, sizeof(buf));
        if (!out.empty()) out += "; ";
        out += buf;
    }
    return out.empty() ? "no OpenSSL error detail" : out;
}

// A read-only memory BIO over a PEM string.
UniqueBio MemBio(std::string_view pem) {
    return UniqueBio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
}

// Every certificate in a PEM bundle, in order - the leaf-then-intermediates
// order a CA-issued bundle already comes in, and the same order a trust
// bundle lists its roots.
//
// A bundle whose *first* entry fails to parse yields an empty vector, not
// an error: what an absent certificate means differs by caller. Any later
// failure is refused rather than half-loaded - the reader stops at the
// first unreadable entry, and a chain or trust store silently missing its
// tail fails much later and much less legibly than a startup refusal.
StatusOr<std::vector<UniqueX509>> ReadPemCerts(std::string_view pem) {
    std::vector<UniqueX509> certs;
    UniqueBio bio = MemBio(pem);
    for (;;) {
        UniqueX509 cert(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
        if (cert != nullptr) {
            certs.push_back(std::move(cert));
            continue;
        }
        // The reader signals "no more entries" with PEM_R_NO_START_LINE;
        // anything else is a bundle that is only partly readable.
        unsigned long code = ERR_peek_last_error();  // NOLINT(google-runtime-int)
        if (code != 0 && ERR_GET_REASON(code) != PEM_R_NO_START_LINE) {
            return Status::InvalidArgument("TLS: PEM bundle is only partly readable: " +
                                           ErrString());
        }
        ERR_clear_error();
        return certs;
    }
}

// A fresh context in either role, with the one setting both share: TLS
// 1.3 as the floor, not a preference (tls_channel.hpp). KDS ships its
// own clients, so there is no downgrade population to serve.
StatusOr<UniqueSslCtx> NewCtx(const SSL_METHOD* method) {
    ERR_clear_error();
    UniqueSslCtx ctx(SSL_CTX_new(method));
    if (ctx == nullptr) {
        return Status::IoError("TLS: SSL_CTX_new failed: " + ErrString());
    }
    if (SSL_CTX_set_min_proto_version(ctx.get(), TLS1_3_VERSION) != 1) {
        return Status::IoError("TLS: cannot require TLS 1.3: " + ErrString());
    }
    return ctx;
}

}  // namespace

// ---- TlsContext ------------------------------------------------------------

struct TlsContext::Impl {
    UniqueSslCtx ctx;
    bool is_server = false;
};

TlsContext::TlsContext(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
TlsContext::TlsContext(TlsContext&&) noexcept = default;
TlsContext& TlsContext::operator=(TlsContext&&) noexcept = default;
TlsContext::~TlsContext() = default;

StatusOr<TlsContext> TlsContext::NewServer(std::string_view cert_pem, std::string_view key_pem) {
    StatusOr<UniqueSslCtx> made = NewCtx(TLS_server_method());
    if (!made.ok()) return made.status();
    UniqueSslCtx ctx = std::move(made.value());

    // The leaf first, then any intermediates that follow it in the same
    // PEM. add1_ (never add0_) because the vector still owns each X509.
    StatusOr<std::vector<UniqueX509>> certs = ReadPemCerts(cert_pem);
    if (!certs.ok()) return certs.status();
    if (certs.value().empty()) {
        return Status::InvalidArgument("TLS: certificate PEM holds no certificate");
    }
    if (SSL_CTX_use_certificate(ctx.get(), certs.value().front().get()) != 1) {
        return Status::InvalidArgument("TLS: certificate rejected: " + ErrString());
    }
    for (std::size_t i = 1; i < certs.value().size(); ++i) {
        if (SSL_CTX_add1_chain_cert(ctx.get(), certs.value()[i].get()) != 1) {
            return Status::InvalidArgument("TLS: chain certificate rejected: " + ErrString());
        }
    }

    UniqueBio key_bio = MemBio(key_pem);
    UniquePkey key(PEM_read_bio_PrivateKey(key_bio.get(), nullptr, nullptr, nullptr));
    if (key == nullptr) {
        return Status::InvalidArgument("TLS: private key PEM unreadable: " + ErrString());
    }
    if (SSL_CTX_use_PrivateKey(ctx.get(), key.get()) != 1 ||
        SSL_CTX_check_private_key(ctx.get()) != 1) {
        return Status::InvalidArgument(
            "TLS: private key does not match the certificate: " + ErrString());
    }

    auto impl = std::make_unique<Impl>();
    impl->ctx = std::move(ctx);
    impl->is_server = true;
    return TlsContext(std::move(impl));
}

StatusOr<TlsContext> TlsContext::NewClient(std::string_view trust_pem) {
    StatusOr<UniqueSslCtx> made = NewCtx(TLS_client_method());
    if (!made.ok()) return made.status();
    UniqueSslCtx ctx = std::move(made.value());

    // Same reader, same refusal of a partly-readable bundle: a trust store
    // missing the root the peer chains to would fail as a handshake error
    // per connection rather than as one refusal at startup.
    StatusOr<std::vector<UniqueX509>> certs = ReadPemCerts(trust_pem);
    if (!certs.ok()) return certs.status();
    if (certs.value().empty()) {
        return Status::InvalidArgument("TLS: trust PEM holds no certificate");
    }
    X509_STORE* store = SSL_CTX_get_cert_store(ctx.get());
    for (const UniqueX509& cert : certs.value()) {
        if (X509_STORE_add_cert(store, cert.get()) != 1) {
            return Status::InvalidArgument("TLS: trust certificate rejected: " + ErrString());
        }
    }
    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);

    auto impl = std::make_unique<Impl>();
    impl->ctx = std::move(ctx);
    impl->is_server = false;
    return TlsContext(std::move(impl));
}

// ---- TlsChannel ------------------------------------------------------------

struct TlsChannel::Impl {
    UniqueSsl ssl;
    bool peer_closed = false;
    // Every plaintext byte Send() accepts goes through here - there is
    // exactly one write path, so a reply can neither jump the queue
    // around an unflushed tail nor land in a buffer nobody reads.
    // Non-empty after a flush only while the handshake is pending or a
    // write returned WANT_READ.
    std::string pending_plain;

    bool init_done() const noexcept { return SSL_is_init_finished(ssl.get()) == 1; }
    // The SSL owns both BIOs (SSL_set_bio); asking it beats caching a
    // copy that could disagree.
    BIO* rbio() const noexcept { return SSL_get_rbio(ssl.get()); }
    BIO* wbio() const noexcept { return SSL_get_wbio(ssl.get()); }

    void DrainWbio(std::string& out) {
        char buf[4096];
        while (BIO_ctrl_pending(wbio()) > 0) {
            int n = BIO_read(wbio(), buf, static_cast<int>(sizeof(buf)));
            if (n <= 0) break;
            out.append(buf, static_cast<std::size_t>(n));
        }
    }

    // The state machine in three halves, so each caller takes only what
    // it may: Send never calls ReadPlain, which is what makes "a Send
    // routed application bytes into a discarded buffer" impossible by
    // construction rather than by an invariant three files away. Every
    // fatal path drains the wbio first - a verify failure queues an
    // alert the peer should hear, and so, on OpenSSL 3.5.5, does a first
    // record that was never TLS.
    //
    // **Whether an alert exists at all is the library's choice, not this
    // channel's** (corrected 2026-08-26; this comment used to say a peer
    // whose bytes were never TLS "gets nothing", which was true of an
    // older OpenSSL and is not true here). The contract the tests pin is
    // the one this code can keep: whatever the wbio holds is handed to the
    // caller verbatim, and nothing the peer sent is ever among it.

    Status Handshake(std::string& wire_out) {
        if (init_done()) return Status::OK();
        ERR_clear_error();
        int r = SSL_do_handshake(ssl.get());
        if (r == 1) return Status::OK();
        int err = SSL_get_error(ssl.get(), r);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return Status::OK();
        DrainWbio(wire_out);
        return Status::IoError("TLS handshake failed: " + ErrString());
    }

    // Encrypts as much of pending_plain as the engine will take. A
    // WANT_READ keeps the tail pending - TLS 1.3 may ask a writer to
    // consume a post-handshake message first - and the next OnWireData
    // releases it. SSL_write_ex consumes nothing when it fails, so the
    // tail is exact, in order, and never re-encrypted.
    Status FlushPending(std::string& wire_out) {
        if (!init_done() || pending_plain.empty()) return Status::OK();
        std::size_t off = 0;
        Status result = Status::OK();
        while (off < pending_plain.size()) {
            std::size_t written = 0;
            ERR_clear_error();
            int r = SSL_write_ex(ssl.get(), pending_plain.data() + off,
                                 pending_plain.size() - off, &written);
            if (r != 1) {
                int err = SSL_get_error(ssl.get(), r);
                if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                    DrainWbio(wire_out);
                    result = Status::IoError("TLS write failed: " + ErrString());
                }
                break;
            }
            off += written;
        }
        pending_plain.erase(0, off);
        return result;
    }

    Status ReadPlain(std::string& plain, std::string& wire_out) {
        if (!init_done()) return Status::OK();
        char buf[4096];
        for (;;) {
            std::size_t got = 0;
            ERR_clear_error();
            int r = SSL_read_ex(ssl.get(), buf, sizeof(buf), &got);
            if (r == 1) {
                plain.append(buf, got);
                continue;
            }
            int err = SSL_get_error(ssl.get(), r);
            if (err == SSL_ERROR_ZERO_RETURN) {
                // The peer's orderly close_notify. Not an error: the
                // TCP FIN behind it is what tcp_server acts on.
                peer_closed = true;
                return Status::OK();
            }
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return Status::OK();
            DrainWbio(wire_out);
            return Status::IoError("TLS read failed: " + ErrString());
        }
    }
};

TlsChannel::TlsChannel(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
TlsChannel::~TlsChannel() = default;

std::unique_ptr<TlsChannel> TlsContext::NewChannel() const {
    UniqueSsl ssl(SSL_new(impl_->ctx.get()));
    if (ssl == nullptr) return nullptr;
    UniqueBio rbio(BIO_new(BIO_s_mem()));
    UniqueBio wbio(BIO_new(BIO_s_mem()));
    if (rbio == nullptr || wbio == nullptr) return nullptr;
    // An empty memory BIO must read as "no data yet" (WANT_READ), not as
    // end-of-stream - the socket decides when the stream ends. The wbio
    // needs no such setting: nothing reads it while empty (DrainWbio
    // guards on BIO_ctrl_pending).
    BIO_set_mem_eof_return(rbio.get(), -1);
    SSL_set_bio(ssl.get(), rbio.release(), wbio.release());  // ssl now owns both
    if (impl_->is_server) {
        SSL_set_accept_state(ssl.get());
    } else {
        SSL_set_connect_state(ssl.get());
    }

    auto impl = std::make_unique<TlsChannel::Impl>();
    impl->ssl = std::move(ssl);
    return std::unique_ptr<TlsChannel>(new TlsChannel(std::move(impl)));
}

Status TlsChannel::OnWireData(std::string_view wire_in, std::string& plain,
                              std::string& wire_out) {
    ERR_clear_error();
    std::size_t off = 0;
    while (off < wire_in.size()) {
        int chunk = static_cast<int>(std::min<std::size_t>(wire_in.size() - off, INT_MAX));
        int n = BIO_write(impl_->rbio(), wire_in.data() + off, chunk);
        if (n <= 0) {
            return Status::IoError("TLS: rbio refused bytes: " + ErrString());
        }
        off += static_cast<std::size_t>(n);
    }
    if (Status s = impl_->Handshake(wire_out); !s.ok()) return s;
    if (Status s = impl_->FlushPending(wire_out); !s.ok()) return s;
    if (Status s = impl_->ReadPlain(plain, wire_out); !s.ok()) return s;
    impl_->DrainWbio(wire_out);
    return Status::OK();
}

Status TlsChannel::Send(std::string_view plain, std::string& wire_out) {
    // One write path: through pending_plain, always. Before the
    // handshake this is pure buffering - and the Handshake() nudge is
    // how a client channel's first Send emits the ClientHello with no
    // wire bytes to react to. After it, FlushPending drains the queue in
    // arrival order, so a WANT_READ tail can never be overtaken by a
    // later reply.
    impl_->pending_plain.append(plain);
    if (Status s = impl_->Handshake(wire_out); !s.ok()) return s;
    if (Status s = impl_->FlushPending(wire_out); !s.ok()) return s;
    impl_->DrainWbio(wire_out);
    return Status::OK();
}

void TlsChannel::Close(std::string& wire_out) {
    ERR_clear_error();
    (void)SSL_shutdown(impl_->ssl.get());  // queues close_notify; best effort
    impl_->DrainWbio(wire_out);
}

bool TlsChannel::handshake_done() const noexcept { return impl_->init_done(); }
bool TlsChannel::peer_closed() const noexcept { return impl_->peer_closed; }

}  // namespace kds::server
