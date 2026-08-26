#include "kds/server/tls_channel.hpp"

#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <memory>
#include <string>
#include <utility>

// The TLS channel's contract suite (docs/spec/protocol.md §1): both ends of a
// real TLS 1.3 handshake pumped against each other entirely through the
// WireChannel interface - no socket, no fd, every byte movement scripted
// by the test. The certificate is generated here and thrown away, so the
// suite depends on no checked-in key material.

namespace kds::server {
namespace {

struct TestCert {
    std::string cert_pem;
    std::string key_pem;
};

std::string BioToString(BIO* bio) {
    char* data = nullptr;
    long n = BIO_get_mem_data(bio, &data);  // NOLINT(google-runtime-int) - OpenSSL's type
    return std::string(data, static_cast<std::size_t>(n));
}

// A throwaway self-signed certificate: EC P-256, CN=localhost, valid for
// one hour of test run.
TestCert MakeSelfSigned() {
    EVP_PKEY* key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
    EXPECT_NE(key, nullptr);

    X509* cert = X509_new();
    EXPECT_NE(cert, nullptr);
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
    X509_set_pubkey(cert, key);

    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    X509_set_issuer_name(cert, name);
    EXPECT_GT(X509_sign(cert, key, EVP_sha256()), 0);

    TestCert out;
    BIO* cert_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(cert_bio, cert);
    out.cert_pem = BioToString(cert_bio);
    BIO_free(cert_bio);

    BIO* key_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(key_bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    out.key_pem = BioToString(key_bio);
    BIO_free(key_bio);

    X509_free(cert);
    EVP_PKEY_free(key);
    return out;
}

struct Pair {
    std::unique_ptr<TlsChannel> client;
    std::unique_ptr<TlsChannel> server;
    std::string client_plain;
    std::string server_plain;
};

Pair MakePair(const TestCert& cert, const std::string& trust_pem) {
    auto server_ctx = TlsContext::NewServer(cert.cert_pem, cert.key_pem);
    EXPECT_TRUE(server_ctx.ok()) << server_ctx.status().message();
    auto client_ctx = TlsContext::NewClient(trust_pem);
    EXPECT_TRUE(client_ctx.ok()) << client_ctx.status().message();

    Pair pair;
    pair.server = server_ctx.value().NewChannel();
    pair.client = client_ctx.value().NewChannel();
    EXPECT_NE(pair.server, nullptr);
    EXPECT_NE(pair.client, nullptr);
    return pair;
}

// Ferries wire bytes both ways until neither side has anything left to
// say, `chunk` bytes at a time (0 = all at once). Returns the first
// non-OK status either side produced.
Status Ferry(Pair& pair, std::size_t chunk = 0) {
    std::string c2s, s2c;
    if (Status s = pair.client->OnWireData("", pair.client_plain, c2s); !s.ok()) return s;
    for (int rounds = 0; (!c2s.empty() || !s2c.empty()) && rounds < 4096; ++rounds) {
        std::string next_c2s, next_s2c;
        const auto feed = [chunk](TlsChannel& ch, const std::string& in, std::string& plain,
                                  std::string& out) {
            if (chunk == 0) return ch.OnWireData(in, plain, out);
            for (std::size_t i = 0; i < in.size(); i += chunk) {
                Status s = ch.OnWireData(std::string_view(in).substr(i, chunk), plain, out);
                if (!s.ok()) return s;
            }
            return Status::OK();
        };
        if (!c2s.empty()) {
            if (Status s = feed(*pair.server, c2s, pair.server_plain, next_s2c); !s.ok()) return s;
        }
        if (!s2c.empty()) {
            if (Status s = feed(*pair.client, s2c, pair.client_plain, next_c2s); !s.ok()) return s;
        }
        c2s = std::move(next_c2s);
        s2c = std::move(next_s2c);
    }
    if (!c2s.empty() || !s2c.empty()) {
        return Status::IoError("Ferry hit its round cap with bytes still in flight");
    }
    return Status::OK();
}

TEST(TlsChannelTest, HandshakeCompletes) {
    TestCert cert = MakeSelfSigned();
    Pair pair = MakePair(cert, cert.cert_pem);
    Status s = Ferry(pair);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_TRUE(pair.client->handshake_done());
    EXPECT_TRUE(pair.server->handshake_done());
    EXPECT_TRUE(pair.client_plain.empty());
    EXPECT_TRUE(pair.server_plain.empty());
}

TEST(TlsChannelTest, CommandAndReplyRoundTrip) {
    TestCert cert = MakeSelfSigned();
    Pair pair = MakePair(cert, cert.cert_pem);
    ASSERT_TRUE(Ferry(pair).ok());

    std::string c2s, s_out;
    ASSERT_TRUE(pair.client->Send("SELECT 1\n", c2s).ok());
    ASSERT_TRUE(pair.server->OnWireData(c2s, pair.server_plain, s_out).ok());
    EXPECT_EQ(pair.server_plain, "SELECT 1\n");

    std::string s2c, c_out;
    ASSERT_TRUE(pair.server->Send("id\n1\nOK\n", s2c).ok());
    ASSERT_TRUE(pair.client->OnWireData(s2c, pair.client_plain, c_out).ok());
    EXPECT_EQ(pair.client_plain, "id\n1\nOK\n");
}

// Send() before the handshake buffers and the bytes arrive once it
// lands - the tcp_server never has to know what state the channel is in.
TEST(TlsChannelTest, SendBeforeHandshakeIsBuffered) {
    TestCert cert = MakeSelfSigned();
    Pair pair = MakePair(cert, cert.cert_pem);

    std::string hello;
    ASSERT_TRUE(pair.client->Send("EARLY\n", hello).ok());
    EXPECT_FALSE(hello.empty());  // the nudged ClientHello, not the data
    EXPECT_FALSE(pair.client->handshake_done());

    // Deliver the flights by hand - fresh strings each leg, because the
    // append-only convention means an output string still holds its past.
    std::string flight1, flight2, flight3;
    ASSERT_TRUE(pair.server->OnWireData(hello, pair.server_plain, flight1).ok());
    ASSERT_TRUE(pair.client->OnWireData(flight1, pair.client_plain, flight2).ok());
    ASSERT_TRUE(pair.server->OnWireData(flight2, pair.server_plain, flight3).ok());
    EXPECT_TRUE(pair.server->handshake_done());
    EXPECT_EQ(pair.server_plain, "EARLY\n");
}

// TLS records survive arbitrary TCP segmentation: the same handshake and
// round trip with every wire delivery cut to one byte.
TEST(TlsChannelTest, OneByteFeeds) {
    TestCert cert = MakeSelfSigned();
    Pair pair = MakePair(cert, cert.cert_pem);
    Status s = Ferry(pair, 1);
    ASSERT_TRUE(s.ok()) << s.message();
    ASSERT_TRUE(pair.client->handshake_done());
    ASSERT_TRUE(pair.server->handshake_done());

    std::string c2s, s_out;
    ASSERT_TRUE(pair.client->Send("PING\n", c2s).ok());
    for (char byte : c2s) {
        ASSERT_TRUE(
            pair.server->OnWireData(std::string_view(&byte, 1), pair.server_plain, s_out).ok());
    }
    EXPECT_EQ(pair.server_plain, "PING\n");
}

// A value crossing many 16 KiB TLS records comes through intact.
TEST(TlsChannelTest, LargeTransfer) {
    TestCert cert = MakeSelfSigned();
    Pair pair = MakePair(cert, cert.cert_pem);
    ASSERT_TRUE(Ferry(pair).ok());

    std::string big(1 << 20, 'x');
    big.back() = '\n';
    std::string c2s, s_out;
    ASSERT_TRUE(pair.client->Send(big, c2s).ok());
    ASSERT_TRUE(pair.server->OnWireData(c2s, pair.server_plain, s_out).ok());
    EXPECT_EQ(pair.server_plain, big);
}

// A plaintext client on a TLS port is a fatal transform error the server
// reports and closes on - never a crash, never a silent skip (the same
// posture docs/spec/protocol.md §2 takes for malformed frames).
//
// **What is pinned is the channel's contract, not OpenSSL's byte count**
// (rewritten 2026-08-26). This test used to assert `out.empty()`, on the
// stated ground that "OpenSSL queues no alert for a first record that was
// never TLS". That was a claim about a library version, not about this
// engine: OpenSSL 3.5.5 *does* queue one, so the assertion failed on a
// channel that was behaving correctly, and it would have failed again on
// the next version that changed its mind. Whether an alert accompanies the
// close is the library's choice; what the channel owes is below.
TEST(TlsChannelTest, PlaintextGarbageIsFatal) {
    TestCert cert = MakeSelfSigned();
    Pair pair = MakePair(cert, cert.cert_pem);
    std::string out;
    Status s = pair.server->OnWireData("SELECT 1\nSELECT 2\n", pair.server_plain, out);
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(pair.server_plain.empty());

    // **Nothing the peer sent comes back.** The security-bearing half, and
    // the one that holds on every version: a channel that echoed an
    // attacker's bytes onto the wire would be a reflector, and "the alert
    // buffer happened to be empty" never proved it was not one.
    EXPECT_EQ(out.find("SELECT"), std::string::npos)
        << "the fatal path put the peer's own plaintext on the wire";

    // **And whatever is there is a TLS alert**, never arbitrary bytes:
    // content type 21 in the record header (RFC 8446 §5.1), which is what
    // makes "the channel hands over what OpenSSL produced" a checkable
    // claim rather than a description. Empty stays legal - that is what the
    // older OpenSSL did - so this is "if anything, then an alert", and the
    // record's *length* is deliberately not asserted: how many records a
    // version queues is the same kind of fact that broke this test before.
    if (!out.empty()) {
        ASSERT_GE(out.size(), 5u) << "wire_out holds " << out.size()
                                  << " bytes, too few to be a TLS record";
        EXPECT_EQ(static_cast<unsigned>(static_cast<unsigned char>(out[0])), 21u)
            << "wire_out does not begin with an alert record";
    }
}

// A client that does not trust the server's certificate refuses the
// handshake; nothing the server sent ever surfaces as plaintext, and the
// fatal alert *is* drained into wire_out for the caller to flush.
//
// **This one may assert the alert exists where its sibling may not**, and
// the difference is whose rule it is: a peer that rejects a certificate
// owes the other side a fatal alert by RFC 8446 §6.2, so an empty
// `wire_out` here would be the channel swallowing it. Whether a *non-TLS*
// first record produces one is unspecified and version-dependent, which
// is why `PlaintextGarbageIsFatal` pins the shape of what comes back and
// not its presence.
TEST(TlsChannelTest, UntrustedServerCertRefused) {
    TestCert server_cert = MakeSelfSigned();
    TestCert other_cert = MakeSelfSigned();
    Pair pair = MakePair(server_cert, other_cert.cert_pem);

    std::string hello, flight1, alert;
    ASSERT_TRUE(pair.client->OnWireData("", pair.client_plain, hello).ok());
    ASSERT_TRUE(pair.server->OnWireData(hello, pair.server_plain, flight1).ok());
    Status s = pair.client->OnWireData(flight1, pair.client_plain, alert);
    EXPECT_FALSE(s.ok());
    EXPECT_FALSE(alert.empty());
    EXPECT_TRUE(pair.client_plain.empty());
    EXPECT_TRUE(pair.server_plain.empty());
}

// Application bytes and the peer's close_notify in one TCP segment: the
// data is surfaced *before* the shutdown is recorded. The last statement
// of a session must not be lost to the goodbye that follows it in the
// same read.
TEST(TlsChannelTest, DataAheadOfCloseNotifyIsDelivered) {
    TestCert cert = MakeSelfSigned();
    Pair pair = MakePair(cert, cert.cert_pem);
    ASSERT_TRUE(Ferry(pair).ok());

    std::string c2s;
    ASSERT_TRUE(pair.client->Send("LAST\n", c2s).ok());
    pair.client->Close(c2s);

    std::string s_out;
    Status s = pair.server->OnWireData(c2s, pair.server_plain, s_out);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(pair.server_plain, "LAST\n");
    EXPECT_TRUE(pair.server->peer_closed());
}

// The orderly goodbye: Close() emits close_notify, and the peer records
// it without treating it as an error.
TEST(TlsChannelTest, CloseNotifyIsOrderly) {
    TestCert cert = MakeSelfSigned();
    Pair pair = MakePair(cert, cert.cert_pem);
    ASSERT_TRUE(Ferry(pair).ok());

    std::string bye, s_out;
    pair.client->Close(bye);
    ASSERT_FALSE(bye.empty());
    Status s = pair.server->OnWireData(bye, pair.server_plain, s_out);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_TRUE(pair.server->peer_closed());
    EXPECT_TRUE(pair.server_plain.empty());
}

TEST(TlsContextTest, GarbagePemRefused) {
    auto ctx = TlsContext::NewServer("not a pem", "also not a pem");
    ASSERT_FALSE(ctx.ok());
    EXPECT_EQ(ctx.status().code(), StatusCode::kInvalidArgument);
}

TEST(TlsContextTest, MismatchedKeyRefused) {
    TestCert a = MakeSelfSigned();
    TestCert b = MakeSelfSigned();
    auto ctx = TlsContext::NewServer(a.cert_pem, b.key_pem);
    ASSERT_FALSE(ctx.ok());
    EXPECT_EQ(ctx.status().code(), StatusCode::kInvalidArgument);
}

TEST(TlsContextTest, EmptyTrustRefused) {
    auto ctx = TlsContext::NewClient("");
    ASSERT_FALSE(ctx.ok());
    EXPECT_EQ(ctx.status().code(), StatusCode::kInvalidArgument);
}

// A bundle that parses partly is refused whole, on both sides: loading
// the readable prefix of a chain or a trust store turns one legible
// startup refusal into a per-connection handshake failure much later.
TEST(TlsContextTest, PartlyReadableBundleRefused) {
    TestCert cert = MakeSelfSigned();
    const std::string half =
        cert.cert_pem + "-----BEGIN CERTIFICATE-----\nNOTBASE64!!!\n-----END CERTIFICATE-----\n";

    auto server = TlsContext::NewServer(half, cert.key_pem);
    ASSERT_FALSE(server.ok());
    EXPECT_EQ(server.status().code(), StatusCode::kInvalidArgument);

    auto client = TlsContext::NewClient(half);
    ASSERT_FALSE(client.ok()) << "a trust store must not silently load its readable prefix";
    EXPECT_EQ(client.status().code(), StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace kds::server
