#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "kds/wire/handshake.hpp"
#include "kds/wire/kwp.hpp"
#include "kds/wire/kwp_types.hpp"

// The handshake (docs/spec/protocol.md §3, protocol-wp.md P07). Socket-free
// by construction: `Negotiate` reads two structs and writes one, and the
// session id and cancel key are arguments, so every case below is exact.

namespace kds::wire {
namespace {

HandshakeConfig ServerAt(std::uint16_t lo, std::uint16_t hi) {
    HandshakeConfig c;
    c.min_version = lo;
    c.max_version = hi;
    c.capabilities = kServerCapabilities;
    c.server_info = "kds-test";
    return c;
}

ClientHello ClientAt(std::uint16_t lo, std::uint16_t hi) {
    ClientHello h;
    h.min_version = lo;
    h.max_version = hi;
    return h;
}

// ---- Version intersection ------------------------------------------------

TEST(KwpHandshakeTest, TheVersionMatrixPicksTheHighestBothSpeak) {
    struct Case {
        std::uint16_t cl, ch, sl, sh;  // client [lo, hi], server [lo, hi]
        bool accepted;
        std::uint16_t chosen;
    };
    constexpr Case kCases[] = {
        {1, 1, 1, 1, true, 1},    // the only pair this build can actually run
        {1, 3, 1, 2, true, 2},    // client newer than server: the server's ceiling
        {1, 2, 1, 3, true, 2},    // server newer than client: the client's ceiling
        {2, 3, 1, 1, false, 0},   // client too new, no overlap
        {1, 1, 2, 3, false, 0},   // client too old, no overlap
        {2, 2, 2, 2, true, 2},    // one version each, and it is the same one
    };
    for (const Case& c : kCases) {
        const auto out = Negotiate(ClientAt(c.cl, c.ch), ServerAt(c.sl, c.sh), /*auth_required=*/false, 7, 9);
        EXPECT_EQ(out.accepted, c.accepted)
            << "client [" << c.cl << "," << c.ch << "] server [" << c.sl << "," << c.sh << "]";
        if (c.accepted) {
            EXPECT_EQ(out.hello.version, c.chosen);
        } else {
            EXPECT_EQ(out.error.category(), ErrorCategory::kProtocol);
            EXPECT_EQ(out.error.detail_code(),
                      static_cast<std::uint16_t>(ProtocolDetail::kUnsupportedVersion));
            EXPECT_EQ(out.error.severity, Severity::kFatal);
            EXPECT_NE(out.error.message.find("no common KWP version"), std::string::npos);
        }
    }
}

TEST(KwpHandshakeTest, AnInvertedClientRangeIsRefusedRatherThanIntersected) {
    // [5, 2] would intersect anything under a naive min/max, and means
    // nothing. Caught before the arithmetic, so the message can say what
    // is wrong with the *client* rather than reporting a version mismatch
    // that does not exist.
    const auto out = Negotiate(ClientAt(5, 2), ServerAt(1, 9), /*auth_required=*/false, 1, 2);
    ASSERT_FALSE(out.accepted);
    EXPECT_NE(out.error.message.find("above max_version"), std::string::npos);
}

// ---- Capabilities --------------------------------------------------------

TEST(KwpHandshakeTest, CapabilitiesIntersectAndUnknownBitsAreIgnoredNotRejected) {
    ClientHello h = ClientAt(1, 1);
    // Streaming (offered by both), bulk load (offered by both), an unknown
    // bit 63 from some later version, and cancel - which the *server* does
    // not offer, because `C_CANCEL` is not answered (handshake.hpp).
    h.capabilities = static_cast<std::uint64_t>(Capability::kStreaming) | kCapBulkLoad |
                     static_cast<std::uint64_t>(Capability::kCancel) |
                     (std::uint64_t{1} << 63);

    const auto out = Negotiate(h, ServerAt(1, 1), /*auth_required=*/false, 42, 43);
    ASSERT_TRUE(out.accepted) << "an unknown capability bit must not refuse a connection";
    EXPECT_EQ(out.hello.capabilities,
              static_cast<std::uint64_t>(Capability::kStreaming) | kCapBulkLoad)
        << "the answer is the intersection: the unknown bit is dropped, and so is a bit the "
           "client asked for that this server does not implement";
}

TEST(KwpHandshakeTest, TheServerNeverOffersCompression) {
    // KW-D5 deferred it; the bit stays reserved in the enum. A server that
    // offered it would be promising a shape nobody has specified.
    EXPECT_EQ(kServerCapabilities & static_cast<std::uint64_t>(Capability::kCompression), 0u);
    ClientHello h = ClientAt(1, 1);
    // Everything except the TLS *demand*, which is not a capability the
    // client is asking the server to provide - it is a condition on the
    // transport, and setting it here would refuse this connection for a
    // reason that has nothing to do with compression.
    h.capabilities = ~std::uint64_t{0} & ~static_cast<std::uint64_t>(Capability::kTlsRequired);
    const auto out = Negotiate(h, ServerAt(1, 1), /*auth_required=*/false, 1, 1);
    ASSERT_TRUE(out.accepted);
    EXPECT_EQ(out.hello.capabilities & static_cast<std::uint64_t>(Capability::kCompression), 0u);
}

TEST(KwpHandshakeTest, TlsRequiredIsADemandAndIsRefusedOnAPlaintextPort) {
    ClientHello h = ClientAt(1, 1);
    h.capabilities = static_cast<std::uint64_t>(Capability::kTlsRequired);

    HandshakeConfig plaintext = ServerAt(1, 1);
    plaintext.tls_active = false;
    const auto refused = Negotiate(h, plaintext, /*auth_required=*/false, 1, 1);
    ASSERT_FALSE(refused.accepted);
    EXPECT_EQ(refused.error.severity, Severity::kFatal)
        << "the client asked not to be served in the clear; answering anything else on this "
           "connection would serve it in the clear";

    HandshakeConfig secured = ServerAt(1, 1);
    secured.tls_active = true;
    EXPECT_TRUE(Negotiate(h, secured, /*auth_required=*/false, 1, 1).accepted);
}

// ---- Authentication ------------------------------------------------------

TEST(KwpHandshakeTest, AuthIsNegotiatedAndNeverDowngradedInEitherDirection) {
    HandshakeConfig open = ServerAt(1, 1);
    HandshakeConfig guarded = ServerAt(1, 1);

    ClientHello none = ClientAt(1, 1);
    ClientHello scram = ClientAt(1, 1);
    scram.auth_method = kAuthScramSha256;

    const auto plain = Negotiate(none, open, /*auth_required=*/false, 1, 1);
    EXPECT_TRUE(plain.accepted);
    EXPECT_FALSE(plain.auth_owed);

    const auto owed = Negotiate(scram, guarded, /*auth_required=*/true, 1, 1);
    EXPECT_TRUE(owed.accepted);
    EXPECT_TRUE(owed.auth_owed) << "accepted means the negotiation succeeded, not that the "
                                   "connection is authenticated";

    const auto too_little = Negotiate(none, guarded, /*auth_required=*/true, 1, 1);
    ASSERT_FALSE(too_little.accepted);
    EXPECT_NE(too_little.error.message.find("requires authentication"), std::string::npos);

    const auto too_much = Negotiate(scram, open, /*auth_required=*/false, 1, 1);
    ASSERT_FALSE(too_much.accepted)
        << "a client that asked to authenticate must not be quietly admitted without doing so";
    EXPECT_NE(too_much.error.message.find("will not pretend"), std::string::npos);
}

TEST(KwpHandshakeTest, AReservedAuthMethodIsRefusedByName) {
    ClientHello h = ClientAt(1, 1);
    h.auth_method = kAuthMtls;
    const auto out = Negotiate(h, ServerAt(1, 1), /*auth_required=*/false, 1, 1);
    ASSERT_FALSE(out.accepted);
    EXPECT_NE(out.error.message.find("MTLS is reserved"), std::string::npos);
}

// ---- The answer ----------------------------------------------------------

TEST(KwpHandshakeTest, TheServerHelloCarriesTheIdentifiersItWasGiven) {
    const auto out = Negotiate(ClientAt(1, 1), ServerAt(1, 1), /*auth_required=*/false,
                               0xABCDEF0123456789ull, 0x0123456789ABCDEFull);
    ASSERT_TRUE(out.accepted);
    EXPECT_EQ(out.hello.session_id, 0xABCDEF0123456789ull);
    EXPECT_EQ(out.hello.cancel_key, 0x0123456789ABCDEFull);
    EXPECT_EQ(out.hello.server_info, "kds-test");
}

TEST(KwpHandshakeTest, BothHelloPayloadsRoundTrip) {
    ClientHello c = ClientAt(1, 4);
    c.capabilities = kCapBulkLoad;
    c.auth_method = kAuthScramSha256;
    c.client_name = "ckdbs-cli/2";
    auto c_back = DecodeClientHello(EncodeClientHello(c));
    ASSERT_TRUE(c_back.ok()) << c_back.status().message();
    EXPECT_EQ(c_back.value().min_version, 1);
    EXPECT_EQ(c_back.value().max_version, 4);
    EXPECT_EQ(c_back.value().capabilities, kCapBulkLoad);
    EXPECT_EQ(c_back.value().auth_method, kAuthScramSha256);
    EXPECT_EQ(c_back.value().client_name, "ckdbs-cli/2");

    ServerHello s;
    s.version = 1;
    s.capabilities = kServerCapabilities;
    s.session_id = 12345;
    s.cancel_key = 67890;
    s.server_info = "kds/2.2.1";
    auto s_back = DecodeServerHello(EncodeServerHello(s));
    ASSERT_TRUE(s_back.ok()) << s_back.status().message();
    EXPECT_EQ(s_back.value().version, 1);
    EXPECT_EQ(s_back.value().capabilities, kServerCapabilities);
    EXPECT_EQ(s_back.value().session_id, 12345u);
    EXPECT_EQ(s_back.value().cancel_key, 67890u);
    EXPECT_EQ(s_back.value().server_info, "kds/2.2.1");
}

TEST(KwpHandshakeTest, ABadMagicIsRefusedByTheDecoderBeforeNegotiationSeesIt) {
    auto bytes = EncodeClientHello(ClientAt(1, 1));
    bytes[0] = static_cast<std::byte>(static_cast<unsigned char>(bytes[0]) ^ 0xFF);
    auto out = DecodeClientHello(bytes);
    ASSERT_FALSE(out.ok());
    EXPECT_NE(out.status().message().find("magic"), std::string::npos);
}

TEST(KwpHandshakeTest, ATruncatedHelloIsRefusedAtEveryLength) {
    ClientHello c = ClientAt(1, 1);
    c.client_name = "x";
    const auto bytes = EncodeClientHello(c);
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        EXPECT_FALSE(DecodeClientHello(std::span<const std::byte>(bytes.data(), n)).ok())
            << "a C_HELLO one byte short decoded as a whole hello at n=" << n;
    }
}

}  // namespace
}  // namespace kds::wire
