#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "kds/base/status.hpp"

// SCRAM-SHA-256 (RFC 5802 mechanics, RFC 7677 hash) - the reserved auth
// method of docs/spec/protocol.md D8, built as a pure message transformer so
// the same state machines serve the newline text protocol today and the
// KWP handshake frames when P07 lands. Neither class does I/O; a caller
// feeds messages and forwards what comes back.
//
// Why SCRAM and not password-over-TLS: the server stores a salted
// verifier and never sees the password; the wire carries proofs, not
// secrets; and authentication is mutual - the client verifies the
// server's signature too (Client::OnServerFinal).
//
// Randomness: nonces default to OpenSSL's RNG (RandomNonce), the same
// platform-boundary allowance tls_channel.cpp documents. Both state
// machines take an injectable generator instead, which is what lets the
// tests pin RFC 7677's example exchange byte for byte.
//
// Deliberately absent from v1, stated rather than discovered:
// channel binding (`p=` gs2 headers are refused - SCRAM-SHA-256-PLUS is
// additive later), SASLprep username normalization (provisioning
// restricts usernames to printable ASCII without ',' instead), and any
// authorization identity (`a=` is refused; authorization is an Open
// Decision in docs/spec/protocol.md §14).
//
// Lock/atomic protocol: none. One Server or Client per connection
// attempt, on that connection's core.

namespace kds::server::scram {

// RFC 7677 recommends 4096 as the floor and PostgreSQL ships it as the
// default; the cost is paid by the *client* per connection (PBKDF2 runs
// client-side), so raising it hardens a stolen verifier file at the
// price of connection latency. Per-verifier, chosen at provisioning -
// the server just replays what the verifier records.
inline constexpr std::uint32_t kDefaultIterations = 4096;
inline constexpr std::size_t kSaltBytes = 16;

// The ceiling both ends enforce: provisioning refuses to write a larger
// count, and the client refuses a server that demands one - a hostile
// server's i= is CPU it makes *this* end burn (RFC 7677 §4). The floor
// on both sides is kDefaultIterations, the RFC's own minimum.
inline constexpr std::uint32_t kMaxIterations = 10'000'000;

// Base64 (RFC 4648) - the alphabet SCRAM's grammar transports salts,
// keys and proofs in, exported because the verifier format and the tests
// speak it too. NOT canonical-strict: over-padding and non-canonical
// trailing bits are accepted (every decoded value here is length-checked
// or MAC-compared), so do not reuse it where canonical form matters.
std::string B64Encode(std::string_view raw);
StatusOr<std::string> B64Decode(std::string_view b64);

// What the server stores per user - never the password. StoredKey can
// verify a proof but cannot produce one, which is what makes the file
// worth less to a thief than a password file.
struct Verifier {
    std::uint32_t iterations = kDefaultIterations;
    std::string salt;        // raw bytes
    std::string stored_key;  // H(ClientKey), 32 raw bytes
    std::string server_key;  // HMAC(SaltedPassword, "Server Key"), 32 raw bytes

    // "SCRAM-SHA-256$<iter>:<salt b64>$<stored b64>:<server b64>" - the
    // shape RFC 5803 gives LDAP, readable and greppable in a users file.
    std::string Serialize() const;
    static StatusOr<Verifier> Parse(std::string_view text);
};

// The provisioning-side derivation (RFC 5802 §3): PBKDF2, then the two
// keys. Also what a test or client uses to compute proofs. An empty salt
// is refused - PBKDF2 without one is a rainbow table waiting to happen.
StatusOr<Verifier> DeriveVerifier(std::string_view password, std::string_view salt,
                                  std::uint32_t iterations);

// A fresh salt for provisioning: kSaltBytes of raw randomness, and the
// only correct argument for DeriveVerifier's `salt`.
//
// It exists so that no caller reaches for RandomNonce() instead. A nonce
// is base64 *text*, so salting with one puts every real salt in a
// 64-character alphabet at 24 bytes wide - and the unknown-user mock,
// which salts with kSaltBytes of uniform bytes, would then be
// identifiable on sight from the s= attribute alone. The two producers
// have to agree, so there is one of them.
std::string RandomSalt();

// A fresh printable nonce (no commas, per the grammar) from OpenSSL's
// RNG. The injectable default.
//
// The contract every call site relies on: each call returns an
// independent value - Server draws twice per hello and one of the two
// must not be derivable from the other - and returns the empty string
// only to mean "the randomness source failed", which refuses the
// exchange rather than proceeding on a predictable nonce.
using NonceGenerator = std::function<std::string()>;
std::string RandomNonce();

// The server half: client-first -> server-first, client-final ->
// server-final. An unknown user is answered with a *mock* server-first
// (a fake salt, uniform bytes hashed from a fresh nonce) and refused only
// at the proof, so the failure is indistinguishable from a wrong password
// and the user list cannot be probed.
//
// What that mock does *not* yet defend, stated rather than implied: the
// fake salt is redrawn per attempt, so two hellos for the same name
// answer with two different salts where a real user's salt would repeat.
// Closing that needs a salt keyed deterministically by username under a
// server-lifetime secret - future hardening, docs/spec/protocol.md §14.
class Server {
public:
    // NotFound from the lookup means "no such user"; any other non-OK
    // status is an operational failure and aborts the exchange as itself.
    using VerifierLookup = std::function<StatusOr<Verifier>(std::string_view username)>;

    explicit Server(VerifierLookup lookup, NonceGenerator nonce = RandomNonce);

    StatusOr<std::string> OnClientFirst(std::string_view client_first);
    StatusOr<std::string> OnClientFinal(std::string_view client_final);

    // Who authenticated: the empty string until OnClientFinal has
    // succeeded, so a failed attempt's *claimed* name can never be read
    // as an identity. (Logging failed attempts is future observability
    // work and will want its own accessor, named for what it returns.)
    const std::string& username() const noexcept {
        static const std::string kNobody;
        return expect_ == Expect::kDone ? username_ : kNobody;
    }

private:
    enum class Expect : std::uint8_t { kClientFirst, kClientFinal, kDone, kFailed };

    VerifierLookup lookup_;
    NonceGenerator nonce_;
    Expect expect_ = Expect::kClientFirst;
    bool user_known_ = false;
    std::string username_;
    std::string gs2_header_;
    std::string client_first_bare_;
    std::string server_first_;
    std::string combined_nonce_;
    Verifier verifier_;
};

// The client half - what the tests drive both ends with, and what the
// CLI and the future KWP conformance harness will reuse.
class Client {
public:
    Client(std::string username, std::string password, NonceGenerator nonce = RandomNonce);

    std::string First();
    StatusOr<std::string> OnServerFirst(std::string_view server_first);
    // Verifies the server's signature - the mutual half. A server that
    // cannot produce it does not hold the verifier it claimed to.
    Status OnServerFinal(std::string_view server_final);

private:
    std::string username_;
    std::string password_;
    NonceGenerator nonce_;
    std::string client_nonce_;
    std::string client_first_bare_;
    std::string auth_message_;
    std::string server_key_;
    bool sent_first_ = false;
};

}  // namespace kds::server::scram
