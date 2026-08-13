#include "kds/server/scram.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>

#include "kds/server/auth.hpp"

// The SCRAM contract suite. The anchor is RFC 7677 §3's example
// SCRAM-SHA-256 exchange, reproduced byte for byte with injected nonces
// - if these four strings match, the crypto, the attribute grammar and
// the AuthMessage assembly are all right at once. Everything after that
// is failure-path: SCRAM's value is which handshakes it refuses.

namespace kds::server::scram {
namespace {

// The module's own decoder, not a test re-implementation: two copies of
// the padding-trim semantics would be two chances to get them wrong.
std::string B64DecodeForTest(std::string_view b64) {
    auto out = B64Decode(b64);
    EXPECT_TRUE(out.ok()) << out.status().message();
    return out.ok() ? out.value() : std::string();
}

// RFC 7677 §3's fixed points.
constexpr char kUser[] = "user";
constexpr char kPassword[] = "pencil";
constexpr char kClientNonce[] = "rOprNGfwEbeRWgbNEkqO";
constexpr char kServerNonce[] = "%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0";
constexpr char kSaltB64[] = "W22ZaJ0SNY7soEsUEjb6gQ==";

constexpr char kClientFirst[] = "n,,n=user,r=rOprNGfwEbeRWgbNEkqO";
constexpr char kServerFirst[] =
    "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";
constexpr char kClientFinal[] =
    "c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,"
    "p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=";
constexpr char kServerFinal[] = "v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=";

Verifier PencilVerifier() {
    auto v = DeriveVerifier(kPassword, B64DecodeForTest(kSaltB64), 4096);
    EXPECT_TRUE(v.ok()) << v.status().message();
    return v.value();
}

Server::VerifierLookup LookupPencil() {
    return [](std::string_view user) -> StatusOr<Verifier> {
        if (user != kUser) return Status::NotFound("no such user");
        return PencilVerifier();
    };
}

TEST(ScramTest, Rfc7677GoldenExchange) {
    Server server(LookupPencil(), [] { return std::string(kServerNonce); });
    Client client(kUser, kPassword, [] { return std::string(kClientNonce); });

    EXPECT_EQ(client.First(), kClientFirst);

    auto server_first = server.OnClientFirst(kClientFirst);
    ASSERT_TRUE(server_first.ok()) << server_first.status().message();
    EXPECT_EQ(server_first.value(), kServerFirst);

    auto client_final = client.OnServerFirst(server_first.value());
    ASSERT_TRUE(client_final.ok()) << client_final.status().message();
    EXPECT_EQ(client_final.value(), kClientFinal);

    auto server_final = server.OnClientFinal(client_final.value());
    ASSERT_TRUE(server_final.ok()) << server_final.status().message();
    EXPECT_EQ(server_final.value(), kServerFinal);

    EXPECT_TRUE(client.OnServerFinal(server_final.value()).ok());
    EXPECT_EQ(server.username(), kUser);
}

TEST(ScramTest, RandomNoncesRoundTrip) {
    Server server(LookupPencil());
    Client client(kUser, kPassword);
    auto server_first = server.OnClientFirst(client.First());
    ASSERT_TRUE(server_first.ok());
    auto client_final = client.OnServerFirst(server_first.value());
    ASSERT_TRUE(client_final.ok());
    auto server_final = server.OnClientFinal(client_final.value());
    ASSERT_TRUE(server_final.ok()) << server_final.status().message();
    EXPECT_TRUE(client.OnServerFinal(server_final.value()).ok());
}

TEST(ScramTest, WrongPasswordRefused) {
    Server server(LookupPencil());
    Client client(kUser, "not pencil");
    auto server_first = server.OnClientFirst(client.First());
    ASSERT_TRUE(server_first.ok());
    auto client_final = client.OnServerFirst(server_first.value());
    ASSERT_TRUE(client_final.ok());
    auto server_final = server.OnClientFinal(client_final.value());
    EXPECT_FALSE(server_final.ok());
    EXPECT_EQ(server_final.status().code(), StatusCode::kInvalidArgument);
}

TEST(ScramTest, UnknownUserRefusedAtTheProofNotTheHello) {
    // Enumeration resistance: the unknown user gets a well-formed
    // server-first (mock salt) and fails only where a wrong password
    // fails, with the same message.
    Server server([](std::string_view) -> StatusOr<Verifier> {
        return Status::NotFound("no such user");
    });
    Client client("nobody", kPassword);
    auto server_first = server.OnClientFirst(client.First());
    ASSERT_TRUE(server_first.ok()) << "unknown user must not be refused at the hello";
    EXPECT_EQ(server_first.value().substr(0, 2), "r=");
    auto client_final = client.OnServerFirst(server_first.value());
    ASSERT_TRUE(client_final.ok());
    auto server_final = server.OnClientFinal(client_final.value());
    EXPECT_FALSE(server_final.ok());
    EXPECT_EQ(server_final.status().message(), "SCRAM: authentication failed")
        << "the unknown-user refusal must be spelled exactly like the wrong-password one";
}

TEST(ScramTest, TamperedProofRefused) {
    Server server(LookupPencil(), [] { return std::string(kServerNonce); });
    Client client(kUser, kPassword, [] { return std::string(kClientNonce); });
    (void)client.First();
    auto server_first = server.OnClientFirst(kClientFirst);
    ASSERT_TRUE(server_first.ok());
    std::string tampered(kClientFinal);
    tampered[tampered.size() - 5] = tampered[tampered.size() - 5] == 'A' ? 'B' : 'A';
    EXPECT_FALSE(server.OnClientFinal(tampered).ok());
}

TEST(ScramTest, NonceSubstitutionRefused) {
    Server server(LookupPencil(), [] { return std::string(kServerNonce); });
    auto server_first = server.OnClientFirst(kClientFirst);
    ASSERT_TRUE(server_first.ok());
    // A replayed client-final whose nonce is not this exchange's.
    std::string forged(kClientFinal);
    std::size_t r = forged.find("r=");
    forged[r + 2] = 'X';
    auto out = server.OnClientFinal(forged);
    EXPECT_FALSE(out.ok());
    EXPECT_NE(out.status().message().find("nonce"), std::string::npos);
}

TEST(ScramTest, ChannelBindingDowngradeRefused) {
    // c= must re-state the gs2 header the hello used; "biws" is b64 of
    // "n,,". A client-final claiming "y,," ("eSws") after an "n,," hello
    // is the tamper the attribute exists to catch.
    Server server(LookupPencil(), [] { return std::string(kServerNonce); });
    ASSERT_TRUE(server.OnClientFirst(kClientFirst).ok());
    std::string forged(kClientFinal);
    forged.replace(forged.find("c=biws"), 6, "c=eSws");
    auto out = server.OnClientFinal(forged);
    EXPECT_FALSE(out.ok());
    EXPECT_NE(out.status().message().find("channel-binding"), std::string::npos);
}

TEST(ScramTest, ScramPlusHelloRefusedAsUnsupported) {
    Server server(LookupPencil());
    auto out = server.OnClientFirst("p=tls-unique,,n=user,r=abc");
    EXPECT_FALSE(out.ok());
    EXPECT_EQ(out.status().code(), StatusCode::kUnsupported);
}

TEST(ScramTest, AuthzidRefusedAsUnsupported) {
    Server server(LookupPencil());
    auto out = server.OnClientFirst("n,a=admin,n=user,r=abc");
    EXPECT_FALSE(out.ok());
    EXPECT_EQ(out.status().code(), StatusCode::kUnsupported);
}

TEST(ScramTest, StepsOutOfOrderRefused) {
    Server server(LookupPencil());
    EXPECT_FALSE(server.OnClientFinal(kClientFinal).ok());

    Server server2(LookupPencil());
    ASSERT_TRUE(server2.OnClientFirst(kClientFirst).ok());
    EXPECT_FALSE(server2.OnClientFirst(kClientFirst).ok()) << "a second hello must not reset";
}

TEST(ScramTest, ClientRejectsForgedServerSignature) {
    // The mutual half: a server that answers with the wrong v= does not
    // hold the verifier, and the client must say so.
    Server server(LookupPencil(), [] { return std::string(kServerNonce); });
    Client client(kUser, kPassword, [] { return std::string(kClientNonce); });
    (void)client.First();
    auto server_first = server.OnClientFirst(kClientFirst);
    ASSERT_TRUE(server_first.ok());
    ASSERT_TRUE(client.OnServerFirst(server_first.value()).ok());
    EXPECT_FALSE(client.OnServerFinal("v=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=").ok());
    EXPECT_FALSE(client.OnServerFinal("e=other-error").ok());
}

TEST(ScramTest, UsernameEscapingRoundTrips) {
    std::string seen;
    Server server([&seen](std::string_view user) -> StatusOr<Verifier> {
        seen = std::string(user);
        return Status::NotFound("irrelevant");
    });
    Client client("od,d=user", "pw");
    std::string first = client.First();
    EXPECT_NE(first.find("n=od=2Cd=3Duser,"), std::string::npos);
    ASSERT_TRUE(server.OnClientFirst(first).ok());
    EXPECT_EQ(seen, "od,d=user");
}

TEST(ScramTest, MockSaltIsNotDistinguishableByItsAlphabet) {
    // The enumeration oracle this must not be: a mock salt built from a
    // nonce is base64 text, so all 16 bytes land in [A-Za-z0-9+/], which a
    // real random salt manages with probability 2^-32. One round trip per
    // name would then answer "does this user exist". Eight draws make an
    // accidental failure here 2^-256, so a failure means the mock leaks.
    for (int attempt = 0; attempt < 8; ++attempt) {
        Server server([](std::string_view) -> StatusOr<Verifier> {
            return Status::NotFound("no such user");
        });
        Client client("nobody", "pw");
        auto first = server.OnClientFirst(client.First());
        ASSERT_TRUE(first.ok());
        const std::string& msg = first.value();
        std::size_t begin = msg.find(",s=") + 3;
        std::size_t end = msg.find(",i=");
        ASSERT_NE(end, std::string::npos);
        std::string salt = B64DecodeForTest(msg.substr(begin, end - begin));
        ASSERT_EQ(salt.size(), 16u) << "a mock salt must be a real salt's width";
        bool all_base64_alphabet = true;
        for (unsigned char c : salt) {
            bool in_alphabet = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                               (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
            if (!in_alphabet) all_base64_alphabet = false;
        }
        if (!all_base64_alphabet) return;
    }
    ADD_FAILURE() << "every mock salt was drawn from the base64 alphabet";
}

TEST(ScramTest, ClientRefusesAServerFinalBeforeTheServerFirst) {
    // Out of order, server_key_ and auth_message_ are both empty, and
    // HMAC-SHA256(key="", data="") - the literal below - is a constant
    // anyone can compute. Verifying against it would call a stranger
    // authentic.
    Client client(kUser, kPassword);
    (void)client.First();
    EXPECT_FALSE(client.OnServerFinal("v=thNnmggU2ex3L5XXeMNfxf8Wl8STcVZTxscSFEKSxa0=").ok());
}

TEST(ScramTest, MandatoryExtensionRefusedInEitherClientMessage) {
    // RFC 5802 §5.1: 'm' is reserved, and its presence in *any* message
    // must fail authentication at the end that parses it.
    Server hello(LookupPencil(), [] { return std::string(kServerNonce); });
    EXPECT_EQ(hello.OnClientFirst("n,,m=future,n=user,r=abc").status().code(),
              StatusCode::kUnsupported);

    Server server(LookupPencil(), [] { return std::string(kServerNonce); });
    ASSERT_TRUE(server.OnClientFirst(kClientFirst).ok());
    std::string with_ext(kClientFinal);
    with_ext.insert(with_ext.rfind(",p="), ",m=future");
    EXPECT_EQ(server.OnClientFinal(with_ext).status().code(), StatusCode::kUnsupported);
}

TEST(ScramVerifierTest, RandomSaltIsRawBytesOfTheDeclaredWidth) {
    // Provisioning must salt from here, not from RandomNonce(): a nonce is
    // base64 text, and a users file salted with one is a file whose every
    // real salt is 24 bytes wide in a 64-character alphabet - which tells
    // an unknown user's mock salt apart on sight.
    bool saw_non_base64_byte = false;
    for (int attempt = 0; attempt < 8 && !saw_non_base64_byte; ++attempt) {
        std::string salt = RandomSalt();
        ASSERT_EQ(salt.size(), kSaltBytes);
        for (unsigned char c : salt) {
            bool in_alphabet = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                               (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
            if (!in_alphabet) saw_non_base64_byte = true;
        }
    }
    EXPECT_TRUE(saw_non_base64_byte) << "RandomSalt is producing base64 text, not raw bytes";
    EXPECT_FALSE(DeriveVerifier(kPassword, "", 4096).ok()) << "an empty salt must be refused";
}

TEST(ScramVerifierTest, SerializeParseRoundTrip) {
    Verifier v = PencilVerifier();
    std::string text = v.Serialize();
    EXPECT_EQ(text.rfind("SCRAM-SHA-256$4096:", 0), 0u) << text;
    auto parsed = Verifier::Parse(text);
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    EXPECT_EQ(parsed.value().iterations, v.iterations);
    EXPECT_EQ(parsed.value().salt, v.salt);
    EXPECT_EQ(parsed.value().stored_key, v.stored_key);
    EXPECT_EQ(parsed.value().server_key, v.server_key);
}

TEST(ScramVerifierTest, GarbageRefused) {
    EXPECT_FALSE(Verifier::Parse("not a verifier").ok());
    EXPECT_FALSE(Verifier::Parse("SCRAM-SHA-256$0:AAAA$AAAA:AAAA").ok());
    EXPECT_FALSE(Verifier::Parse("SCRAM-SHA-256$4096:!!$AAAA:AAAA").ok());
    // Well-formed base64, wrong key width.
    EXPECT_FALSE(Verifier::Parse("SCRAM-SHA-256$4096:QUJD$QUJD:QUJD").ok());
}

// ---- The users file ---------------------------------------------------

TEST(FileCredentialStoreTest, ParsesUsersRolesAndComments) {
    Verifier v = PencilVerifier();
    std::string text = "# staff\nalice admin " + v.Serialize() + "\n\nbob readonly " +
                       v.Serialize() + "  # trailing comment\n";
    auto store = FileCredentialStore::Parse(text, "users.probe");
    ASSERT_TRUE(store.ok()) << store.status().message();
    EXPECT_EQ(store.value().size(), 2u);
    EXPECT_TRUE(store.value().Has("alice"));
    EXPECT_EQ(store.value().Lookup("alice").value().role, Role::kAdmin);
    EXPECT_EQ(store.value().Lookup("bob").value().role, Role::kReadOnly);
    EXPECT_EQ(store.value().Lookup("mallory").status().code(), StatusCode::kNotFound);
}

TEST(FileCredentialStoreTest, DuplicateUserRefusedNamingTheLine) {
    Verifier v = PencilVerifier();
    std::string text = "alice admin " + v.Serialize() + "\nalice admin " + v.Serialize() + "\n";
    auto store = FileCredentialStore::Parse(text, "users.probe");
    ASSERT_FALSE(store.ok());
    EXPECT_NE(store.status().message().find("users.probe:2"), std::string::npos)
        << store.status().message();
}

TEST(FileCredentialStoreTest, MalformedVerifierRefusedNamingTheLine) {
    auto store = FileCredentialStore::Parse("alice admin not-a-verifier\n", "users.probe");
    ASSERT_FALSE(store.ok());
    EXPECT_NE(store.status().message().find("users.probe:1"), std::string::npos);
}

TEST(FileCredentialStoreTest, PreAuthzTwoColumnFormatRefusedByName) {
    // The 2026-08-13-morning format, refused with editing instructions
    // rather than defaulted - a silently guessed role is a permission
    // nobody granted.
    Verifier v = PencilVerifier();
    auto store = FileCredentialStore::Parse("alice " + v.Serialize() + "\n", "users.probe");
    ASSERT_FALSE(store.ok());
    EXPECT_NE(store.status().message().find("role"), std::string::npos)
        << store.status().message();
}

TEST(FileCredentialStoreTest, UnknownRoleRefusedNamingTheLine) {
    Verifier v = PencilVerifier();
    auto store = FileCredentialStore::Parse("alice root " + v.Serialize() + "\n", "users.probe");
    ASSERT_FALSE(store.ok());
    EXPECT_NE(store.status().message().find("users.probe:1"), std::string::npos);
    EXPECT_NE(store.status().message().find("readonly, readwrite or admin"), std::string::npos)
        << store.status().message();
}

// ---- The gate over the store ------------------------------------------

class GateTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto store = FileCredentialStore::Parse(
            "user readwrite " + PencilVerifier().Serialize() + "\n", "users.probe");
        ASSERT_TRUE(store.ok());
        store_.emplace(std::move(store.value()));
    }
    std::optional<FileCredentialStore> store_;
};

TEST_F(GateTest, FullExchangeAuthenticates) {
    ScramAuthGate gate(&*store_);
    Client client(kUser, kPassword);

    auto r1 = gate.OnLine("AUTH SCRAM-SHA-256 " + client.First());
    ASSERT_FALSE(r1.close);
    ASSERT_EQ(r1.reply.rfind("AUTH+ ", 0), 0u);
    EXPECT_FALSE(r1.authenticated);

    auto client_final = client.OnServerFirst(r1.reply.substr(6));
    ASSERT_TRUE(client_final.ok());
    auto r2 = gate.OnLine("AUTH " + client_final.value());
    EXPECT_TRUE(r2.authenticated);
    EXPECT_FALSE(r2.close);
    ASSERT_EQ(r2.reply.rfind("AUTH+ ", 0), 0u);
    EXPECT_TRUE(client.OnServerFinal(r2.reply.substr(6)).ok());
    // The identity hand-over: who got in and the store's role for them -
    // the non-default rank proves it was read, not assumed.
    EXPECT_EQ(r2.username, kUser);
    EXPECT_EQ(r2.role, Role::kReadWrite);
}

TEST_F(GateTest, StatementBeforeAuthIsRefusedAndClosed) {
    ScramAuthGate gate(&*store_);
    auto r = gate.OnLine("SELECT * FROM accounts");
    EXPECT_TRUE(r.close);
    EXPECT_FALSE(r.authenticated);
    EXPECT_EQ(r.reply.rfind("ERR ", 0), 0u);
}

TEST_F(GateTest, StopBeforeAuthIsRefused) {
    ScramAuthGate gate(&*store_);
    EXPECT_TRUE(gate.OnLine("STOP").close);
}

TEST_F(GateTest, WrongPasswordClosesWithTheOneRefusal) {
    ScramAuthGate gate(&*store_);
    Client client(kUser, "wrong");
    auto r1 = gate.OnLine("AUTH SCRAM-SHA-256 " + client.First());
    ASSERT_FALSE(r1.close);
    auto client_final = client.OnServerFirst(r1.reply.substr(6));
    ASSERT_TRUE(client_final.ok());
    auto r2 = gate.OnLine("AUTH " + client_final.value());
    EXPECT_TRUE(r2.close);
    EXPECT_FALSE(r2.authenticated);
    EXPECT_EQ(r2.reply.rfind("ERR ", 0), 0u);
}

}  // namespace
}  // namespace kds::server::scram
