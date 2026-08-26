#include "kds/server/scram.hpp"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/rand.h>

#include <array>
#include <memory>
#include <utility>
#include <vector>

// The second translation unit allowed <openssl/*> (tls_channel.cpp is
// the first); compiled only under KDS_WITH_TLS, whose OpenSSL dependency
// this rides. Same platform-boundary allowance for the library RNG.

namespace kds::server::scram {
namespace {

constexpr std::size_t kHashBytes = 32;  // SHA-256

// ---- Crypto primitives, EVP throughout (the 3.0 API - the one-shot
// HMAC() is deprecated and would warn under -Wall) ----------------------

// Checked, and checked for a reason: an unchecked failure here returns an
// empty digest, an empty digest compares equal to an empty StoredKey, and
// a failed hash would then authenticate every password. Fail closed.
StatusOr<std::string> Sha256(std::string_view data) {
    std::array<unsigned char, kHashBytes> out{};
    unsigned int len = 0;
    if (EVP_Digest(data.data(), data.size(), out.data(), &len, EVP_sha256(), nullptr) != 1 ||
        len != out.size()) {
        return Status::IoError("SCRAM: SHA-256 failed");
    }
    return std::string(reinterpret_cast<char*>(out.data()), len);
}

StatusOr<std::string> HmacSha256(std::string_view key, std::string_view data) {
    struct CtxFree {
        void operator()(EVP_MAC_CTX* p) const noexcept { EVP_MAC_CTX_free(p); }
    };
    // Fetched once per process: an EVP_MAC is immutable after fetch and
    // safe to share; the per-call state lives in the context below. Five
    // HMACs per handshake would otherwise pay the provider lookup each.
    static EVP_MAC* const kMac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
    if (kMac == nullptr) return Status::IoError("SCRAM: HMAC unavailable");
    std::unique_ptr<EVP_MAC_CTX, CtxFree> ctx(EVP_MAC_CTX_new(kMac));
    if (ctx == nullptr) return Status::IoError("SCRAM: HMAC context allocation failed");

    char digest_name[] = "SHA256";
    std::array<OSSL_PARAM, 2> params = {
        OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest_name, 0),
        OSSL_PARAM_construct_end(),
    };
    std::array<unsigned char, kHashBytes> out{};
    std::size_t out_len = 0;
    if (EVP_MAC_init(ctx.get(), reinterpret_cast<const unsigned char*>(key.data()), key.size(),
                     params.data()) != 1 ||
        EVP_MAC_update(ctx.get(), reinterpret_cast<const unsigned char*>(data.data()),
                       data.size()) != 1 ||
        EVP_MAC_final(ctx.get(), out.data(), &out_len, out.size()) != 1) {
        return Status::IoError("SCRAM: HMAC computation failed");
    }
    return std::string(reinterpret_cast<char*>(out.data()), out_len);
}

std::string XorBytes(std::string_view a, std::string_view b) {
    std::string out(a);
    for (std::size_t i = 0; i < out.size() && i < b.size(); ++i) {
        out[i] = static_cast<char>(out[i] ^ b[i]);
    }
    return out;
}

bool ConstantTimeEqual(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

// ---- RFC 5802 attribute plumbing --------------------------------------

// "k=value" -> value, refusing the wrong key. SCRAM fixes attribute
// order, so positional parsing with named checks is the honest grammar.
StatusOr<std::string_view> TakeAttr(std::string_view field, char key) {
    if (field.size() < 2 || field[0] != key || field[1] != '=') {
        return Status::InvalidArgument(std::string("SCRAM: expected '") + key +
                                       "=' attribute");
    }
    return field.substr(2);
}

std::vector<std::string_view> SplitCommas(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (true) {
        std::size_t comma = s.find(',', start);
        if (comma == std::string_view::npos) {
            out.push_back(s.substr(start));
            return out;
        }
        out.push_back(s.substr(start, comma - start));
        start = comma + 1;
    }
}

// RFC 5802 saslname: "=2C" encodes ',' and "=3D" encodes '='; a bare
// '=' anywhere else is malformed.
StatusOr<std::string> UnescapeUsername(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (name[i] != '=') {
            out.push_back(name[i]);
            continue;
        }
        if (name.substr(i, 3) == "=2C") {
            out.push_back(',');
        } else if (name.substr(i, 3) == "=3D") {
            out.push_back('=');
        } else {
            return Status::InvalidArgument("SCRAM: malformed username escape");
        }
        i += 2;
    }
    return out;
}

std::string EscapeUsername(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (c == ',') {
            out += "=2C";
        } else if (c == '=') {
            out += "=3D";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

StatusOr<std::uint32_t> ParseIterations(std::string_view text) {
    if (text.empty() || text.size() > 9) {
        return Status::InvalidArgument("SCRAM: iteration count out of range");
    }
    std::uint32_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return Status::InvalidArgument("SCRAM: iteration count is not a number");
        }
        value = value * 10 + static_cast<std::uint32_t>(c - '0');
    }
    if (value == 0) return Status::InvalidArgument("SCRAM: iteration count is zero");
    return value;
}

// PBKDF2, then the two RFC 5802 keys, raw. Shared by the verifier
// derivation (which hashes ClientKey away into StoredKey) and the
// client's proof (which needs it un-hashed) - one definition of the
// derivation, two consumers.
struct DerivedKeys {
    std::string client_key;
    std::string server_key;
};

StatusOr<DerivedKeys> DeriveKeys(std::string_view password, std::string_view salt,
                                 std::uint32_t iterations) {
    if (iterations == 0) return Status::InvalidArgument("SCRAM: iteration count is zero");
    if (salt.empty()) return Status::InvalidArgument("SCRAM: empty salt");
    std::array<unsigned char, kHashBytes> salted{};
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                          reinterpret_cast<const unsigned char*>(salt.data()),
                          static_cast<int>(salt.size()), static_cast<int>(iterations),
                          EVP_sha256(), static_cast<int>(salted.size()), salted.data()) != 1) {
        return Status::IoError("SCRAM: PBKDF2 failed");
    }
    std::string salted_password(reinterpret_cast<char*>(salted.data()), salted.size());
    auto client_key = HmacSha256(salted_password, "Client Key");
    if (!client_key.ok()) return client_key.status();
    auto server_key = HmacSha256(salted_password, "Server Key");
    if (!server_key.ok()) return server_key.status();
    return DerivedKeys{std::move(client_key.value()), std::move(server_key.value())};
}

}  // namespace

// ---- Base64 (exported - scram.hpp states the leniency caveat) ---------

std::string B64Encode(std::string_view raw) {
    std::string out((raw.size() + 2) / 3 * 4, '\0');
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                            reinterpret_cast<const unsigned char*>(raw.data()),
                            static_cast<int>(raw.size()));
    out.resize(static_cast<std::size_t>(n));
    return out;
}

StatusOr<std::string> B64Decode(std::string_view b64) {
    if (b64.empty() || b64.size() % 4 != 0) {
        return Status::InvalidArgument("SCRAM: base64 length not a multiple of 4");
    }
    std::string out(b64.size() / 4 * 3, '\0');
    int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                            reinterpret_cast<const unsigned char*>(b64.data()),
                            static_cast<int>(b64.size()));
    if (n < 0) return Status::InvalidArgument("SCRAM: invalid base64");
    // EVP_DecodeBlock counts the '=' padding as data; trim it back off.
    std::size_t pad = 0;
    if (b64.size() >= 1 && b64[b64.size() - 1] == '=') ++pad;
    if (b64.size() >= 2 && b64[b64.size() - 2] == '=') ++pad;
    out.resize(static_cast<std::size_t>(n) - pad);
    return out;
}

// ---- Verifier ---------------------------------------------------------

std::string Verifier::Serialize() const {
    return "SCRAM-SHA-256$" + std::to_string(iterations) + ":" + B64Encode(salt) + "$" +
           B64Encode(stored_key) + ":" + B64Encode(server_key);
}

StatusOr<Verifier> Verifier::Parse(std::string_view text) {
    constexpr std::string_view kPrefix = "SCRAM-SHA-256$";
    if (text.substr(0, kPrefix.size()) != kPrefix) {
        return Status::InvalidArgument("SCRAM: verifier does not begin 'SCRAM-SHA-256$'");
    }
    std::string_view rest = text.substr(kPrefix.size());
    std::size_t colon1 = rest.find(':');
    std::size_t dollar = rest.find('$');
    if (colon1 == std::string_view::npos || dollar == std::string_view::npos || colon1 > dollar) {
        return Status::InvalidArgument("SCRAM: verifier shape is <iter>:<salt>$<stored>:<server>");
    }
    std::size_t colon2 = rest.find(':', dollar);
    if (colon2 == std::string_view::npos) {
        return Status::InvalidArgument("SCRAM: verifier is missing its server key");
    }

    Verifier v;
    auto iters = ParseIterations(rest.substr(0, colon1));
    if (!iters.ok()) return iters.status();
    v.iterations = iters.value();

    auto salt = B64Decode(rest.substr(colon1 + 1, dollar - colon1 - 1));
    if (!salt.ok()) return salt.status();
    v.salt = std::move(salt.value());

    auto stored = B64Decode(rest.substr(dollar + 1, colon2 - dollar - 1));
    if (!stored.ok()) return stored.status();
    v.stored_key = std::move(stored.value());

    auto server = B64Decode(rest.substr(colon2 + 1));
    if (!server.ok()) return server.status();
    v.server_key = std::move(server.value());

    if (v.stored_key.size() != kHashBytes || v.server_key.size() != kHashBytes) {
        return Status::InvalidArgument("SCRAM: verifier keys are not SHA-256 sized");
    }
    return v;
}

StatusOr<Verifier> DeriveVerifier(std::string_view password, std::string_view salt,
                                  std::uint32_t iterations) {
    auto keys = DeriveKeys(password, salt, iterations);
    if (!keys.ok()) return keys.status();
    auto stored_key = Sha256(keys.value().client_key);
    if (!stored_key.ok()) return stored_key.status();

    Verifier v;
    v.iterations = iterations;
    v.salt = std::string(salt);
    v.stored_key = std::move(stored_key.value());
    v.server_key = std::move(keys.value().server_key);
    return v;
}

std::string RandomNonce() {
    std::array<unsigned char, 18> raw{};  // 24 base64 chars, comma-free
    // An unchecked RAND_bytes leaves `raw` zeroed, which is a *predictable*
    // nonce - the one thing a nonce may not be, since SCRAM's replay
    // resistance is exactly the uniqueness of the two nonces. The empty
    // string is the fail-closed answer: every caller refuses it (Server
    // below, and the server refuses an empty client nonce).
    if (RAND_bytes(raw.data(), static_cast<int>(raw.size())) != 1) return {};
    return B64Encode(std::string_view(reinterpret_cast<char*>(raw.data()), raw.size()));
}

std::string RandomSalt() {
    std::array<unsigned char, kSaltBytes> raw{};
    // Empty on failure, like RandomNonce: DeriveVerifier refuses an empty
    // salt, so a broken RNG cannot provision a saltless verifier.
    if (RAND_bytes(raw.data(), static_cast<int>(raw.size())) != 1) return {};
    return std::string(reinterpret_cast<char*>(raw.data()), raw.size());
}

// ---- Server -----------------------------------------------------------

Server::Server(VerifierLookup lookup, NonceGenerator nonce)
    : lookup_(std::move(lookup)), nonce_(std::move(nonce)) {}

StatusOr<std::string> Server::OnClientFirst(std::string_view client_first) {
    if (expect_ != Expect::kClientFirst) {
        expect_ = Expect::kFailed;
        return Status::InvalidArgument("SCRAM: client-first out of order");
    }
    expect_ = Expect::kFailed;  // stays failed unless every check below passes

    // gs2 header: "n,," (no channel binding) or "y,," (client would have
    // bound but believes this server cannot). "p=..." is SCRAM-PLUS,
    // unbuilt; an authzid between the commas is authorization, an Open
    // Decision - both are Unsupported, not InvalidArgument: understood
    // and declined.
    std::string_view rest;
    if (client_first.substr(0, 3) == "n,," || client_first.substr(0, 3) == "y,,") {
        gs2_header_ = std::string(client_first.substr(0, 3));
        rest = client_first.substr(3);
    } else if (client_first.substr(0, 2) == "p=") {
        return Status::Unsupported("SCRAM: channel binding is not offered (no SCRAM-PLUS)");
    } else if (client_first.substr(0, 2) == "n," || client_first.substr(0, 2) == "y,") {
        return Status::Unsupported("SCRAM: authorization identities are not accepted");
    } else {
        return Status::InvalidArgument("SCRAM: malformed gs2 header");
    }

    client_first_bare_ = std::string(rest);
    std::vector<std::string_view> fields = SplitCommas(rest);
    if (fields.size() < 2) {
        return Status::InvalidArgument("SCRAM: client-first-bare needs n= and r=");
    }
    if (fields[0].substr(0, 2) == "m=") {
        // A mandatory extension this server does not know is a refusal
        // by the RFC's own rule.
        return Status::Unsupported("SCRAM: mandatory extension not understood");
    }

    auto name = TakeAttr(fields[0], 'n');
    if (!name.ok()) return name.status();
    auto unescaped = UnescapeUsername(name.value());
    if (!unescaped.ok()) return unescaped.status();
    username_ = std::move(unescaped.value());

    auto client_nonce = TakeAttr(fields[1], 'r');
    if (!client_nonce.ok()) return client_nonce.status();
    if (client_nonce.value().empty()) {
        return Status::InvalidArgument("SCRAM: empty client nonce");
    }

    // Both draws happen before the lookup and happen whether or not the
    // user exists: an unknown user must not cost an RNG draw that a known
    // user does not also pay. They are two independent draws on purpose -
    // deriving the mock salt from the server nonce would publish it, since
    // the nonce goes out in the clear one attribute later.
    const std::string mock_seed = nonce_();
    const std::string server_nonce = nonce_();
    if (mock_seed.empty() || server_nonce.empty()) {
        return Status::IoError("SCRAM: the nonce source produced nothing");
    }

    StatusOr<Verifier> found = lookup_(username_);
    if (found.ok()) {
        user_known_ = true;
        verifier_ = std::move(found.value());
    } else if (found.status().code() == StatusCode::kNotFound) {
        // The mock exchange: a fake salt so an unknown user is refused
        // at the proof, exactly where a wrong password is - the reply
        // shapes match and the user list cannot be probed by shape.
        // (The fake salt varies across *attempts*; a cross-connection
        // salt-consistency probe is future hardening, noted in
        // docs/spec/protocol.md §14's SCRAM entry.)
        //
        // Hashed, not used raw: a nonce is base64 text, so a raw nonce
        // would make every mock salt 16 bytes drawn from the base64
        // alphabet - which a real 16-byte random salt is with probability
        // 2^-32. That is a one-round-trip user enumeration oracle, i.e.
        // exactly what the mock exists to deny. A digest is uniform.
        auto salt = Sha256(mock_seed);
        if (!salt.ok()) return salt.status();
        user_known_ = false;
        verifier_.iterations = kDefaultIterations;
        verifier_.salt = salt.value().substr(0, kSaltBytes);
        verifier_.stored_key.assign(kHashBytes, '\0');
        verifier_.server_key.assign(kHashBytes, '\0');
    } else {
        return found.status();  // operational failure, reported as itself
    }

    combined_nonce_ = std::string(client_nonce.value()) + server_nonce;
    server_first_ = "r=" + combined_nonce_ + ",s=" + B64Encode(verifier_.salt) +
                    ",i=" + std::to_string(verifier_.iterations);
    expect_ = Expect::kClientFinal;
    return server_first_;
}

StatusOr<std::string> Server::OnClientFinal(std::string_view client_final) {
    if (expect_ != Expect::kClientFinal) {
        expect_ = Expect::kFailed;
        return Status::InvalidArgument("SCRAM: client-final out of order");
    }
    expect_ = Expect::kFailed;

    // "c=<b64 gs2>,r=<nonce>[,extensions],p=<proof>" - the proof is
    // always last, and everything before it is the signed text.
    std::size_t proof_at = client_final.rfind(",p=");
    if (proof_at == std::string_view::npos) {
        return Status::InvalidArgument("SCRAM: client-final has no proof");
    }
    std::string_view without_proof = client_final.substr(0, proof_at);
    std::string_view proof_b64 = client_final.substr(proof_at + 3);

    std::vector<std::string_view> fields = SplitCommas(without_proof);
    if (fields.size() < 2) {
        return Status::InvalidArgument("SCRAM: client-final-without-proof needs c= and r=");
    }
    auto channel = TakeAttr(fields[0], 'c');
    if (!channel.ok()) return channel.status();
    if (channel.value() != B64Encode(gs2_header_)) {
        // A changed gs2 header here is the downgrade-tamper signal the
        // attribute exists to catch.
        return Status::InvalidArgument("SCRAM: channel-binding attribute disagrees with hello");
    }
    auto nonce = TakeAttr(fields[1], 'r');
    if (!nonce.ok()) return nonce.status();
    if (nonce.value() != combined_nonce_) {
        return Status::InvalidArgument("SCRAM: nonce mismatch");
    }
    // Anything past r= is an extension, and unrecognized extensions are
    // ignored (RFC 5802 §7) - except 'm', which §5.1 reserves and whose
    // mere presence MUST fail authentication at whichever end parses it.
    for (std::size_t i = 2; i < fields.size(); ++i) {
        if (fields[i].substr(0, 2) == "m=") {
            return Status::Unsupported("SCRAM: mandatory extension not understood");
        }
    }

    auto proof = B64Decode(proof_b64);
    if (!proof.ok()) return proof.status();
    if (proof.value().size() != kHashBytes) {
        return Status::InvalidArgument("SCRAM: proof is not SHA-256 sized");
    }

    const std::string auth_message =
        client_first_bare_ + "," + server_first_ + "," + std::string(without_proof);

    auto client_signature = HmacSha256(verifier_.stored_key, auth_message);
    if (!client_signature.ok()) return client_signature.status();
    std::string client_key = XorBytes(proof.value(), client_signature.value());
    auto client_key_hash = Sha256(client_key);
    if (!client_key_hash.ok()) return client_key_hash.status();

    // One failure, one message: an unknown user and a wrong password are
    // indistinguishable from here on purpose. The comparison is computed
    // *before* the user_known_ test rather than after it - `||` would skip
    // the digest for an unknown user, which is the cost difference the
    // mock exchange exists to erase.
    const bool proof_ok = ConstantTimeEqual(client_key_hash.value(), verifier_.stored_key);
    if (!user_known_ || !proof_ok) {
        return Status::InvalidArgument("SCRAM: authentication failed");
    }

    auto server_signature = HmacSha256(verifier_.server_key, auth_message);
    if (!server_signature.ok()) return server_signature.status();
    expect_ = Expect::kDone;
    return "v=" + B64Encode(server_signature.value());
}

// ---- Client -----------------------------------------------------------

Client::Client(std::string username, std::string password, NonceGenerator nonce)
    : username_(std::move(username)), password_(std::move(password)), nonce_(std::move(nonce)) {}

std::string Client::First() {
    client_nonce_ = nonce_();
    client_first_bare_ = "n=" + EscapeUsername(username_) + ",r=" + client_nonce_;
    sent_first_ = true;
    return "n,," + client_first_bare_;
}

StatusOr<std::string> Client::OnServerFirst(std::string_view server_first) {
    if (!sent_first_) return Status::InvalidArgument("SCRAM: server-first before client-first");

    std::vector<std::string_view> fields = SplitCommas(server_first);
    if (fields.size() < 3) {
        return Status::InvalidArgument("SCRAM: server-first needs r=, s= and i=");
    }
    auto combined = TakeAttr(fields[0], 'r');
    if (!combined.ok()) return combined.status();
    if (combined.value().substr(0, client_nonce_.size()) != client_nonce_ ||
        combined.value().size() <= client_nonce_.size()) {
        return Status::InvalidArgument("SCRAM: server nonce does not extend the client's");
    }
    auto salt_b64 = TakeAttr(fields[1], 's');
    if (!salt_b64.ok()) return salt_b64.status();
    auto salt = B64Decode(salt_b64.value());
    if (!salt.ok()) return salt.status();
    auto iter_text = TakeAttr(fields[2], 'i');
    if (!iter_text.ok()) return iter_text.status();
    auto iterations = ParseIterations(iter_text.value());
    if (!iterations.ok()) return iterations.status();
    // RFC 7677 §4's client-side duty: i= is CPU the *server* makes this
    // end burn, so an absurd demand is a DoS and a tiny one is a cheap
    // offline attack on the proof it would capture. Floor and ceiling
    // are scram.hpp's, shared with provisioning.
    if (iterations.value() < kDefaultIterations || iterations.value() > kMaxIterations) {
        return Status::InvalidArgument(
            "SCRAM: server demanded an iteration count outside " +
            std::to_string(kDefaultIterations) + ".." + std::to_string(kMaxIterations));
    }

    auto keys = DeriveKeys(password_, salt.value(), iterations.value());
    if (!keys.ok()) return keys.status();
    server_key_ = std::move(keys.value().server_key);

    const std::string without_proof = "c=biws,r=" + std::string(combined.value());
    auth_message_ =
        client_first_bare_ + "," + std::string(server_first) + "," + without_proof;

    auto stored_key = Sha256(keys.value().client_key);
    if (!stored_key.ok()) return stored_key.status();
    auto client_signature = HmacSha256(stored_key.value(), auth_message_);
    if (!client_signature.ok()) return client_signature.status();
    std::string proof = XorBytes(keys.value().client_key, client_signature.value());
    return without_proof + ",p=" + B64Encode(proof);
}

Status Client::OnServerFinal(std::string_view server_final) {
    // Out of order, this would verify v= against HMAC(key="", data="") -
    // a value anyone can compute - and call a stranger authentic. The
    // mutual half only means anything once OnServerFirst has set the key
    // and the AuthMessage it is checked against.
    if (auth_message_.empty()) {
        return Status::InvalidArgument("SCRAM: server-final before server-first");
    }
    if (server_final.substr(0, 2) == "e=") {
        return Status::InvalidArgument("SCRAM: server refused: " +
                                       std::string(server_final.substr(2)));
    }
    auto sig_b64 = TakeAttr(SplitCommas(server_final)[0], 'v');
    if (!sig_b64.ok()) return sig_b64.status();
    auto sig = B64Decode(sig_b64.value());
    if (!sig.ok()) return sig.status();
    auto expected = HmacSha256(server_key_, auth_message_);
    if (!expected.ok()) return expected.status();
    if (!ConstantTimeEqual(sig.value(), expected.value())) {
        return Status::InvalidArgument(
            "SCRAM: server signature is wrong - the peer does not hold this user's verifier");
    }
    return Status::OK();
}

}  // namespace kds::server::scram
