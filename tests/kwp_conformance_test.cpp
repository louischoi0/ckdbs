#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/kwp_session.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/wire/handshake.hpp"
#include "kds/wire/kwp.hpp"
#include "kds/wire/kwp_types.hpp"

// **The conformance suite's first half** (protocol-wp.md P16; KW-D6 moved it
// ahead of the cut). Byte-exact golden sessions against the endpoint
// **in-process**, before any port moves.
//
// ---- Why this exists before P13, and not after --------------------------
//
// KW-D6 directs a single cut-over of the text protocol, and CLA's raised
// cost was that a cut-over before P16 makes the first real user of the new
// protocol the test suite meant to validate it. The adaptation the operator
// accepted is this file: the golden sessions need `KwpSession` and the
// codecs, and *not* a socket - P16's socket-level half is what needs P13
// and P15. So the wire is pinned before anything starts speaking it.
//
// ---- What is pinned, and how a drift shows up ---------------------------
//
// Two hex blobs per session: the bytes a client sends, and the bytes the
// server answers. The client half is built here by name - readable, and a
// change to a frame builder moves it - and the server half is whatever this
// build produces. Both are compared against `testdata/kwp_golden.txt`.
//
// **A failure here is never "the golden is stale".** It is a wire change,
// and the question is whether it was intended: a deployed client compiled
// against the old bytes will not read the new ones. Update the file
// deliberately, in the change that moves the wire, and say so.
//
// Determinism is the fixture's job: fixed session id and cancel key, a
// fixed server_info string, a fixed relation with fixed rows, and no
// statement whose answer carries a clock or an address.

namespace kds::server {
namespace {

using wire::ClientFrameType;

std::string ToHex(const std::vector<std::byte>& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::byte b : bytes) {
        const auto v = std::to_integer<std::uint8_t>(b);
        out.push_back(kDigits[v >> 4]);
        out.push_back(kDigits[v & 0x0F]);
    }
    return out;
}

// The golden file: `== <session>` then `client <hex>` and `server <hex>`.
std::map<std::string, std::pair<std::string, std::string>> LoadGolden(const std::string& path) {
    std::map<std::string, std::pair<std::string, std::string>> out;
    std::ifstream in(path);
    std::string line;
    std::string current;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("== ", 0) == 0) {
            current = line.substr(3);
            continue;
        }
        std::istringstream ls(line);
        std::string key, hex;
        ls >> key >> hex;
        if (key == "client") out[current].first = hex;
        if (key == "server") out[current].second = hex;
    }
    return out;
}

class KwpConformanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, /*now_unix_seconds=*/4000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/false, /*cabins=*/nullptr, &*mgr_);

        Session setup;
        ASSERT_EQ(dispatcher_->Dispatch("CREATE TABLE t (id int64, v int32)", &setup)
                      .response.substr(0, 7),
                  "CREATED");
        ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO t VALUES (10)", &setup)
                      .response.substr(0, 8),
                  "INSERTED");
        ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO t VALUES (20)", &setup)
                      .response.substr(0, 8),
                  "INSERTED");

        wire::HandshakeConfig config;
        config.capabilities = wire::kServerCapabilities;
        config.server_info = "kds-conformance";
        engine_session_.emplace(txn::IsolationLevel::kReadCommitted);
        session_.emplace(*engine_session_, config, wal::DurabilityClass::kRelaxed);
        // Fixed, because a golden transcript cannot contain a random
        // number - and because `wire::Negotiate` takes them as arguments
        // precisely so a test can pin them (rules.md §4).
        session_->set_identity(0xAABBCCDD00112233ull, 0x0011223344556677ull);
    }

    // Appends one client frame to the transcript and runs it.
    void Frame(ClientFrameType type, const std::vector<std::byte>& payload) {
        const auto encoded =
            wire::EncodeFrame(static_cast<std::uint8_t>(type), 0, payload);
        client_.insert(client_.end(), encoded.begin(), encoded.end());

        FrameAction action = session_->OnFrame(
            wire::DecodedFrame{static_cast<std::uint8_t>(type), 0, payload}, server_);
        if (action.dispatch) {
            session_->session().set_result_sink(action.sink);
            DispatchOutcome outcome = dispatcher_->Dispatch(action.sql, &session_->session());
            session_->session().set_result_sink(nullptr);
            session_->OnStatementComplete(outcome, server_);
        }
    }

    static std::vector<std::byte> Str(std::string_view a) {
        wire::PayloadWriter w;
        w.Str(a);
        return w.Take();
    }
    static std::vector<std::byte> Parse(std::string_view name, std::string_view sql) {
        wire::PayloadWriter w;
        w.Str(name);
        w.Text(sql);
        return w.Take();
    }
    static std::vector<std::byte> Bind(std::string_view portal, std::string_view stmt) {
        wire::PayloadWriter w;
        w.Str(portal);
        w.Str(stmt);
        std::vector<std::byte> out = w.Take();
        (void)wire::EncodeBindParams({}, out);
        return out;
    }
    static std::vector<std::byte> Execute(std::string_view portal, std::uint32_t max_rows) {
        wire::PayloadWriter w;
        w.Str(portal);
        w.U32(max_rows);
        return w.Take();
    }
    static std::vector<std::byte> Hello() {
        wire::ClientHello h;
        h.client_name = "conformance";
        return wire::EncodeClientHello(h);
    }
    static std::vector<std::byte> Byte(std::uint8_t v) {
        wire::PayloadWriter w;
        w.U8(v);
        return w.Take();
    }

    void Check(const std::string& name) {
        static const auto golden = LoadGolden(KDS_KWP_GOLDEN);
        auto it = golden.find(name);
        ASSERT_NE(it, golden.end())
            << "no golden session named '" << name << "' in " << KDS_KWP_GOLDEN
            << "\nclient " << ToHex(client_) << "\nserver " << ToHex(server_);
        EXPECT_EQ(ToHex(client_), it->second.first)
            << "the *client* bytes moved for session '" << name
            << "': a frame builder changed shape, so every client compiled against the old "
               "one now sends something this server reads differently";
        EXPECT_EQ(ToHex(server_), it->second.second)
            << "the *server* bytes moved for session '" << name
            << "': this is a wire change, not a stale golden - update it in the change that "
               "moves the wire, deliberately";
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
    // Declared before the protocol session, which borrows it.
    std::optional<Session> engine_session_;
    std::optional<KwpSession> session_;
    std::vector<std::byte> client_;
    std::vector<std::byte> server_;
};

// The handshake alone: the shortest session there is, and the one every
// other transcript starts with.
TEST_F(KwpConformanceTest, Handshake) {
    Frame(ClientFrameType::kHello, Hello());
    Frame(ClientFrameType::kTerminate, {});
    Check("handshake");
}

// PARSE / BIND / EXECUTE over the fixed relation: a description, one batch
// of two rows, a completion.
TEST_F(KwpConformanceTest, ExtendedSelect) {
    Frame(ClientFrameType::kHello, Hello());
    Frame(ClientFrameType::kParse, Parse("s", "SELECT id, v FROM t"));
    Frame(ClientFrameType::kBind, Bind("p", "s"));
    Frame(ClientFrameType::kExecute, Execute("p", 0));
    Frame(ClientFrameType::kSync, {});
    Check("extended-select");
}

// A write: no description, one completion carrying the tag and the count.
//
// An `UPDATE` rather than an `INSERT`, deliberately: `INSERTED`'s reply
// names the page and slot the row landed in, so an unrelated storage change
// would fail a *protocol* conformance test for a reason that has nothing to
// do with the wire. A golden should fail when the wire moves and at no
// other time.
TEST_F(KwpConformanceTest, Write) {
    Frame(ClientFrameType::kHello, Hello());
    Frame(ClientFrameType::kParse, Parse("s", "UPDATE t SET v = 99 WHERE id = 1"));
    Frame(ClientFrameType::kBind, Bind("p", "s"));
    Frame(ClientFrameType::kExecute, Execute("p", 0));
    Check("write");
}

// §15-3's exactness, byte for byte: an error, three frames swallowed, then
// `C_SYNC` and `S_READY`.
TEST_F(KwpConformanceTest, SkipToSync) {
    Frame(ClientFrameType::kHello, Hello());
    Frame(ClientFrameType::kParse, Parse("s", "SELECT id FROM nope"));
    Frame(ClientFrameType::kBind, Bind("p", "s"));
    Frame(ClientFrameType::kExecute, Execute("p", 0));
    Frame(ClientFrameType::kParse, Parse("x", "SELECT id FROM t"));
    Frame(ClientFrameType::kPing, {});
    Frame(ClientFrameType::kSync, {});
    Check("skip-to-sync");
}

// Transaction control, with the RELAXED flag the fixture's server class
// makes true.
TEST_F(KwpConformanceTest, Transaction) {
    Frame(ClientFrameType::kHello, Hello());
    Frame(ClientFrameType::kTxnBegin,
          Byte(static_cast<std::uint8_t>(wire::DurabilityLevel::kStrict)));
    Frame(ClientFrameType::kParse, Parse("s", "UPDATE t SET v = 7 WHERE id = 2"));
    Frame(ClientFrameType::kBind, Bind("p", "s"));
    Frame(ClientFrameType::kExecute, Execute("p", 0));
    Frame(ClientFrameType::kTxnCommit, {});
    Frame(ClientFrameType::kSync, {});
    Check("transaction");
}

}  // namespace
}  // namespace kds::server
