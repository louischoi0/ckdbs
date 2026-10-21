// BS-S5 target 2: KWP/1 (instructions/v3.0.0/workorder-bs-test-hardening.md).
//
// The bytes a client sends - frame decode, the handshake and, behind it,
// SCRAM or the statement frames - into one `KwpSession` over a fresh
// instance, driven as the endpoint drives it (kwp_session_test.cpp's
// `Feed`). The first byte chooses the path: odd puts a SCRAM gate in front,
// the unauthenticated input F11 names; even reaches PARSE/BIND/EXECUTE.
// The next byte, if any, sets the chunk size the decoder is fed in. The
// obligation: a reply, a refusal or a close for every input, never a crash.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "fuzz_instance.hpp"
#include "kds/server/auth.hpp"
#include "kds/server/kwp_session.hpp"
#include "kds/server/scram.hpp"
#include "kds/wire/handshake.hpp"
#include "kds/wire/kwp.hpp"

namespace {

const kds::server::FileCredentialStore& Credentials() {
    static const kds::server::FileCredentialStore store = [] {
        auto verifier = kds::server::scram::DeriveVerifier("pencil", "fuzz-salt-16byte", 4096);
        if (!verifier.ok()) __builtin_trap();
        auto parsed = kds::server::FileCredentialStore::Parse(
            "user readwrite " + verifier.value().Serialize() + "\n", "fuzz");
        if (!parsed.ok()) __builtin_trap();
        return std::move(parsed.value());
    }();
    return store;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2) return 0;
    const bool scram = (data[0] & 1) != 0;
    const std::size_t chunk = 1 + data[1] % 64;
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data + 2), size - 2);

    kds::fuzz::Instance instance;
    kds::wire::HandshakeConfig config;
    config.capabilities = kds::wire::kServerCapabilities;
    config.server_info = "kds-fuzz";
    kds::server::Session engine_session(kds::txn::IsolationLevel::kReadCommitted);
    kds::sched::ManualClock clock(1000);
    kds::server::KwpSession session(engine_session, config, kds::wal::DurabilityClass::kRelaxed,
                                    &clock);
    session.set_identity(0x1111, 0x2222);
    if (scram) session.set_auth_gate(std::make_unique<kds::server::ScramAuthGate>(&Credentials()));

    kds::wire::FrameDecoder decoder;
    for (std::size_t at = 0; at < bytes.size(); at += chunk) {
        if (!decoder.Feed(bytes.subspan(at, std::min(chunk, bytes.size() - at))).ok()) return 0;
        while (auto frame = decoder.PopFrame()) {
            std::vector<std::byte> out;
            const kds::server::FrameAction action = session.OnFrame(*frame, out);
            if (action.dispatch) {
                engine_session.set_result_sink(action.sink);
                const kds::server::DispatchOutcome outcome =
                    instance.dispatcher().Dispatch(action.sql, &engine_session);
                engine_session.set_result_sink(nullptr);
                session.OnStatementComplete(outcome, out);
            }
            if (action.close) return 0;
        }
    }
    return 0;
}
