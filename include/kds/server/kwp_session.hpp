#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/sched/clock.hpp"
#include "kds/server/auth.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/result_sink.hpp"
#include "kds/server/session.hpp"
#include "kds/wire/error_registry.hpp"
#include "kds/wire/handshake.hpp"
#include "kds/wire/kwp.hpp"
#include "kds/wire/kwp_types.hpp"
#include "kds/wire/row_codec.hpp"
#include "kds/wal/manager.hpp"

// **The KWP/1 session state machine** (`docs/spec/protocol.md` §5, §7, §9,
// §10; `docs/inflight/in-progress/protocol-wp.md` P08, P10, P11).
//
// One of these per connection. It owns the protocol's state - the phase,
// the named statements, the portals, the skip-to-`C_SYNC` posture - and it
// owns a `server::Session`, which is the *engine's* per-connection state.
// It owns no socket: frames arrive decoded and replies leave as bytes in a
// caller's buffer, so every rule below is testable without one.
//
// ---- It lives in `server/`, not `wire/` ----------------------------------
//
// The workplan puts this at `src/wire/session.cpp`. It cannot be there:
// KW-D1 binds it to the real `CommandDispatcher`, and `kds::wire` sits
// *below* `kds::server` - the row codec is in `wire` precisely so the
// server and the cross-core path can both use it (`wire/row_codec.hpp`'s
// "one encoder, two consumers"). A dispatcher-dependent state machine in
// `wire` would invert that arrow. What stays in `wire` is what has no
// engine dependency: the frame codec, the handshake, the error registry.
//
// ---- The dispatch is the caller's, and that is not a stub ---------------
//
// `OnFrame` never runs a statement. It answers with a `FrameAction` that
// may say "run this text", and the caller runs it and hands the outcome
// back through `OnStatementComplete`. That is exactly `TcpServer`'s shape
// and it exists for `TcpServer`'s reason: a statement may **park** - a
// cross-core read, a shipped statement, a group commit's durability wait -
// and a function that returns a finished reply cannot wait. The endpoint
// submits `DispatchAsync`; a test calls `Dispatch` and feeds the result
// back. Both run the real engine, which is what KW-D1 requires; what
// differs is who waits.
//
// ---- What a portal is in v1, and what it is not -------------------------
//
// §7 describes a suspended portal as "a suspended foreground task holding
// pins". **It is not one here, and the spec is amended rather than the
// build overstated.** This engine has no suspension point at a row
// boundary: a walk holds a page pin across every row of a page, and the one
// place a statement parks is a page boundary under the cross-core gate
// (`exec::ExecuteAsync`'s resume gate). So `C_EXECUTE {max_rows}` bounds
// **delivery**, not execution: the statement runs whole into the portal's
// buffered batches, and `max_rows` decides how many rows leave now.
//
// Three consequences, all stated rather than discovered:
//
//   - A suspended portal holds **memory**, not pins. That is not a
//     regression: the newline protocol already materialised every result
//     set into one reply string, so the bytes were always spent - a portal
//     spends them in a better shape and releases them when it closes.
//   - The portal-idle timeout (§10, KW-D3) therefore bounds memory rather
//     than pins, which is why its refusal is `kResourceExhausted` and not
//     a protocol error.
//   - A **true** cursor is reachable without redesigning this: the
//     cross-core `RemoteStepServer` already streams a walk under credit and
//     parks it at the page boundary, which is exactly the mechanism a
//     row-bounded EXECUTE needs. It is a later task, not a different
//     design.
//
// ---- Bound parameters are substituted, and why ---------------------------
//
// `C_BIND`'s parameters become **SQL literals in the statement text**,
// which the dispatcher then runs. The alternative - binding values into a
// parsed statement - needs the parser to accept `?` in value positions and
// the compiler to carry a parameter vector, which is `parser-v2.md`'s work
// and not this milestone's.
//
// Substitution is not a workaround here; it is what this engine's own
// fingerprint design already assumes. `?` and a literal emit the **same**
// shape marker (waystone-workplan.md P01: "a bind parameter's type is
// unknown at parse, so distinguishing int-shaped from string-shaped holes
// would break convergence"), so the `pattern_id` of the `?` form and of the
// substituted form are equal by construction, and the `arg_hash` differs
// per parameter set exactly as §5 wants. `KwpSessionTest` asserts that
// equality rather than assuming it.
//
// **One value cannot be bound, and it is the grammar's limit rather than
// the protocol's**: a `TEXT`/`CHAR`/`VARCHAR` parameter containing an ASCII
// single quote. This engine's lexer has no escape for a quote inside a
// string literal ("No escaping, deliberately", `src/parser/lexer.cpp`), so
// the value has no spelling any SQL text this server accepts can carry -
// through KWP or through the newline protocol. It is refused at `C_BIND`
// with that reason, and it is registered in `known-gaps.md` against
// `parser-v2.md`, which owns the string-literal grammar.

namespace kds::server {

// KW-D3: 60 seconds on the injected clock.
//
// The derivation is the ratification's: this tree already spends 60 s on
// the two waits whose event is human-or-network-scale
// (`kIndexBuildReplyDeadlineNs`, `kAssertionBuildReplyDeadlineNs`) against
// 10 s for the machine-scale ones (`kShippedStatementDeadlineNs`,
// `kTxnPhaseDeadlineNs`). **An idle portal waits on a client**, so it takes
// the first number, and reusing one rather than inventing a third is the
// point.
//
// Defensible, not measured: no workload has ever held a portal open in this
// engine, because portals did not exist. KW-D3 says so and says P16's
// conformance run is the first place a real number could come from.
inline constexpr sched::MonoTimeNs kPortalIdleTimeoutNs = 60ull * 1000 * 1000 * 1000;

// §10's "server may cap statement/portal counts (`ERROR(LIMIT)` beyond)".
//
// 64 each. A prepared-statement workload holds a handful - one per distinct
// shape the client issues - and a client that holds sixty-four open has
// either a leak or a generated-SQL pattern that prepared statements do not
// help. The cap is per **session**, so it bounds one connection's state and
// not the server's; what bounds the server is the connection count.
inline constexpr std::size_t kMaxSessionStatements = 64;
inline constexpr std::size_t kMaxSessionPortals = 64;

// The result sink that produces `S_ROW_DESC` and `S_ROW_BATCH` payloads.
//
// It buffers whole frames rather than rows: the batch is sealed at
// `wire::kRowBatchTargetBytes` or at the u16 row-count ceiling, whichever
// comes first, and a sealed batch is a payload waiting for a `C_EXECUTE` or
// `C_CONTINUE` to carry it.
class WireResultSink final : public ResultSink {
public:
    Status Describe(std::vector<wire::FieldDescription> fields) override;
    Status EncodeProjectedRow(std::span<const exec::ColumnRef> projection,
                              std::span<const std::uint32_t> types,
                              const exec::ChainFrame& frame, std::string& out) override;
    Status EncodeValueRow(std::span<const std::uint32_t> types,
                          std::span<const parser::AstValue> values, std::string& out) override;
    Status Emit(std::string_view row) override;

    bool described() const noexcept { return described_; }
    const std::vector<wire::FieldDescription>& fields() const noexcept { return fields_; }
    std::uint64_t row_count() const noexcept { return rows_; }

    // **A `C_EXECUTE {max_rows}` quota, applied where the batch is sealed.**
    //
    // It has to be here, because a quota enforced at *delivery* can only
    // stop on a batch boundary - and with a target-sized batch that means
    // `max_rows = 1` ships the whole result. Sealing on the quota instead
    // makes the boundary the client asked for the boundary the batches
    // have, and leaves `Deliver` with nothing to compute.
    //
    // 0 is no cap, which is `max_rows`'s own meaning (§7).
    void set_batch_row_cap(std::uint32_t rows) noexcept { batch_row_cap_ = rows; }

    // Seals whatever is still open. Call once, after the statement.
    void Finish();

    // One sealed batch: the payload, and the row count already inside it.
    //
    // The count is carried rather than re-read out of the payload's first
    // two bytes, which is what `Deliver` used to do - one fact with two
    // readings, and the re-read was an unchecked index into a buffer whose
    // size nothing had asserted.
    struct Batch {
        std::uint16_t rows = 0;
        std::vector<std::byte> payload;
    };
    std::vector<Batch>& batches() noexcept { return batches_; }

    void Reset();

private:
    void Seal();

    bool described_ = false;
    std::vector<wire::FieldDescription> fields_;
    std::vector<Batch> batches_;
    std::vector<std::byte> open_;   // the batch being filled, rows only
    std::uint16_t open_rows_ = 0;
    std::uint64_t rows_ = 0;
    std::uint32_t batch_row_cap_ = 0;
};

// What `OnFrame` asks of its caller.
struct FrameAction {
    // Sever the connection once `out` has been flushed. Set for a fatal
    // protocol error, a handshake refusal and `C_TERMINATE`.
    bool close = false;

    // Run `sql` under this session's `server::Session`, with `sink`
    // installed on the dispatcher, then call `OnStatementComplete`. Nothing
    // else in the session advances until that happens.
    bool dispatch = false;
    std::string sql;
    ResultSink* sink = nullptr;
};

class KwpSession {
public:
    // `clock` may be null, which disables the portal-idle timeout - the
    // socket-free tests that do not care about it, and the same posture
    // `CommandDispatcher` takes towards its own clock.
    // **`session` is borrowed, not owned.** The engine session belongs to
    // the *connection* - it holds the open transaction, and `TcpServer`
    // rolls that back when a client dies mid-statement - so a protocol
    // object that owned a second one would leave the connection's teardown
    // unwinding the wrong transaction. It must outlive this.
    //
    // `server_durability` is the bottom rung of §9's chain - what a commit
    // is acked under when neither the session nor the transaction overrode
    // it. Held here because `S_TXN_OK` must set `flags.RELAXED` on the
    // class the commit actually used, and a session that overrode nothing
    // has no other way to name it.
    KwpSession(Session& session, wire::HandshakeConfig handshake,
               wal::DurabilityClass server_durability = wal::DurabilityClass::kGroup,
               const sched::Clock* clock = nullptr) noexcept;

    // The engine-side session this connection owns.
    Session& session() noexcept { return session_; }
    const Session& session() const noexcept { return session_; }

    // Set once the handshake has chosen them. Zero until then, which is
    // what "an unauthenticated connection learns no cancel key" means
    // concretely.
    std::uint64_t session_id() const noexcept { return session_id_; }
    std::uint64_t cancel_key() const noexcept { return cancel_key_; }

    // The identifiers this session will answer with. Supplied rather than
    // generated, for `wire::Negotiate`'s reason (rules.md §4).
    void set_identity(std::uint64_t session_id, std::uint64_t cancel_key) noexcept {
        session_id_ = session_id;
        cancel_key_ = cancel_key;
    }

    // The gate an authenticated connection must pass. Owned by the session
    // for the connection's life, or null when the server admits everyone.
    void set_auth_gate(std::unique_ptr<AuthGate> gate) noexcept { auth_gate_ = std::move(gate); }

    bool handshake_done() const noexcept { return phase_ == Phase::kReady; }

    // Handles one decoded frame, appending reply frames to `out`.
    FrameAction OnFrame(const wire::DecodedFrame& frame, std::vector<std::byte>& out);

    // The other half of a `FrameAction{dispatch}`.
    void OnStatementComplete(const DispatchOutcome& outcome, std::vector<std::byte>& out);

    // Expires portals idle past `kPortalIdleTimeoutNs`. A no-op with no
    // clock. Called by the endpoint on its timer tick; nothing is emitted -
    // a timed-out portal is discovered by the client when it names it.
    void ExpireIdlePortals();

    // §10's cancel flag. Set from another connection; observed here at the
    // next frame, which is this engine's only cooperative point - there is
    // no preemption, so cancellation is best-effort-fast and
    // guaranteed-eventually exactly as §10 says.
    void RequestCancel() noexcept { cancel_requested_ = true; }

    std::size_t statement_count() const noexcept { return statements_.size(); }
    std::size_t portal_count() const noexcept { return portals_.size(); }

private:
    enum class Phase : std::uint8_t { kAwaitHello, kAwaitAuth, kReady };

    struct Statement {
        std::string sql;
        std::uint64_t pattern_id = 0;
        std::uint32_t param_count = 0;
    };

    struct Portal {
        std::string statement;  // the named statement it was bound from
        std::string sql;        // that statement's text with parameters substituted
        bool executed = false;
        // Its `S_COMPLETE` has gone out, so the portal is spent: a further
        // `C_CONTINUE` must be refused rather than answered. Without it a
        // second `C_CONTINUE` re-sent a diagnostic answer's whole batch -
        // duplicate rows - and a second `S_COMPLETE` carrying the full
        // `rows_affected` again.
        bool complete_sent = false;
        // What its `C_EXECUTE` asked for, remembered so a `C_CONTINUE`
        // knows whether this portal meters (a cap) or drains (0). Handed to
        // the sink before the statement ran; kept here because `Deliver`
        // has to know which of the two it is doing.
        std::uint32_t batch_row_cap = 0;
        std::size_t next_batch = 0;
        std::string complete_tag;
        std::uint64_t rows_affected = 0;
        WireResultSink sink;
        sched::MonoTimeNs idle_since = 0;
    };

    // ---- Frame handlers, one per client frame ---------------------------
    FrameAction OnHello(std::span<const std::byte> payload, std::vector<std::byte>& out);
    FrameAction OnAuth(std::span<const std::byte> payload, std::vector<std::byte>& out);
    FrameAction OnParse(std::span<const std::byte> payload, std::vector<std::byte>& out);
    FrameAction OnBind(std::span<const std::byte> payload, std::vector<std::byte>& out);
    FrameAction OnDescribe(std::span<const std::byte> payload, std::vector<std::byte>& out);
    FrameAction OnExecute(std::span<const std::byte> payload, std::vector<std::byte>& out);
    FrameAction OnContinue(std::span<const std::byte> payload, std::vector<std::byte>& out);
    FrameAction OnClose(std::span<const std::byte> payload, std::vector<std::byte>& out);
    FrameAction OnTxn(wire::ClientFrameType type, std::span<const std::byte> payload,
                      std::vector<std::byte>& out);

    // ---- Emission -------------------------------------------------------
    void Send(std::vector<std::byte>& out, wire::ServerFrameType type, std::uint8_t flags,
              std::span<const std::byte> payload);
    void SendError(std::vector<std::byte>& out, const wire::WireError& error);
    void SendReady(std::vector<std::byte>& out);
    // Refuses the current frame and arms the skip-to-`C_SYNC` posture (§5).
    FrameAction Refuse(std::vector<std::byte>& out, const wire::WireError& error);
    // Streams `portal`'s next batch - or all of them, where nothing
    // metered it - then `S_PORTAL_SUSPENDED` (naming `name`) or
    // `S_COMPLETE`.
    void Deliver(const std::string& name, Portal& portal, std::vector<std::byte>& out);
    // `S_COMPLETE`, and the portal is spent.
    void Complete(Portal& portal, std::vector<std::byte>& out);
    // The completion of a diagnostic statement: a reply the engine has
    // never typed, delivered as a one-column TEXT result set.
    void DeliverTextLines(std::string_view reply, std::vector<std::byte>& out);

    Portal* FindPortal(const std::string& name);
    Statement* FindStatement(const std::string& name);

    sched::MonoTimeNs Now() const noexcept;

    wire::HandshakeConfig handshake_;
    wal::DurabilityClass server_durability_;
    const sched::Clock* clock_;
    Session& session_;
    std::unique_ptr<AuthGate> auth_gate_;

    Phase phase_ = Phase::kAwaitHello;
    // §5's pipelining contract: after an `S_ERROR`, every frame is
    // discarded until the next `C_SYNC`. One bool, because that is the
    // whole rule.
    bool skipping_to_sync_ = false;
    bool cancel_requested_ = false;
    std::uint64_t session_id_ = 0;
    std::uint64_t cancel_key_ = 0;

    // Ordered, so a `SHOW`-style enumeration and the golden sessions see a
    // stable order; the counts are small and bounded by the caps above.
    std::map<std::string, Statement> statements_;
    std::map<std::string, Portal> portals_;

    // The portal a `FrameAction{dispatch}` is running for. Empty when the
    // dispatch is transaction control, which has no portal and answers
    // `S_TXN_OK`.
    std::string running_portal_;
    bool running_txn_ = false;
    // Whether the transaction-control dispatch in flight runs under D3,
    // and whether that dispatch is the `BEGIN` - which decides at which
    // side of the dispatch the class is readable. See `OnTxn`.
    bool running_relaxed_ = false;
    bool running_txn_begin_ = false;
};

}  // namespace kds::server
