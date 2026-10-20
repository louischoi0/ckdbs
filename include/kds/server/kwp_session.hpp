#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/sched/clock.hpp"
#include "kds/server/auth.hpp"
#include "kds/server/cancel_registry.hpp"
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
// §10; P08, P10, P11 of `docs/inflight/in-progress/protocol-wp.md` at 1769487).
//
// One of these per connection. It owns the protocol's state - the phase,
// the named statements, the portals, the skip-to-`C_SYNC` posture - and it
// owns a `server::Session`, which is the *engine's* per-connection state.
// It owns no socket: frames arrive decoded and replies leave as bytes in a
// caller's buffer, so every rule below is testable without one.
//
// ---- It lives in `server/`, not `wire/` ----------------------------------
//
// KW-D1 binds it to the real `CommandDispatcher`, and `kds::wire` sits
// *below* `kds::server`. What stays in `wire` is what has no engine
// dependency: the frame codec, the row codec, the handshake, the error
// registry.
//
// ---- The dispatch is the caller's, and that is not a stub ---------------
//
// `OnFrame` never runs a statement. It answers with a `FrameAction` that
// may say "run this text", and the caller runs it and hands the outcome
// back through `OnStatementComplete`. That is exactly `TcpServer`'s shape
// and it exists for `TcpServer`'s reason: a statement may **park** - a
// lock wait, a group commit's durability wait - and a function that returns a finished reply cannot wait. The endpoint
// submits `DispatchAsync`; a test calls `Dispatch` and feeds the result
// back. Both run the real engine, which is what KW-D1 requires; what
// differs is who waits.
//
// ---- What a portal is in v1, and what it is not -------------------------
//
// §7 describes a suspended portal as "a suspended foreground task holding
// pins". **It is not one here, and the spec is amended rather than the
// build overstated.** This engine has no suspension point at a row
// boundary: a walk holds a page pin across every row of a page, and a
// `SELECT`'s walk yields only between slices (BA-S15), never mid-page. So
// `C_EXECUTE {max_rows}` bounds
// **delivery**, not execution: the statement runs whole into the portal's
// buffered batches, and `max_rows` decides how many rows leave now.
//
// So a suspended portal holds **memory**, not pins, released when it
// closes, and the portal-idle timeout (§10, KW-D3) bounds memory - which is
// why its refusal is `kResourceExhausted` and not a protocol error.
//
// ---- Bound parameters are substituted, and why ---------------------------
//
// `C_BIND`'s parameters become **SQL literals in the statement text**,
// which the dispatcher then runs; binding into a parsed statement would
// need `?` in value positions and a parameter vector in the compiler
// (`parser-v2.md`). The fingerprint already assumes it: `?` and a literal
// emit the **same** shape marker, so the `?` form and the substituted form
// share a pattern while `arg_hash` differs per parameter set, as §5 wants
// (`KwpSessionTest` asserts the equality).
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
// **An idle portal waits on a client**, so it takes the 60 s this tree
// gave human-or-network-scale waits rather than a third number. Defensible,
// not measured (KW-D3).
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
    // two bytes: one fact, one reading.
    struct Batch {
        std::uint16_t rows = 0;
        std::vector<std::byte> payload;
    };
    std::vector<Batch>& batches() noexcept { return batches_; }

    void Reset();

    // ---- Streaming (CE, `instructions/v3.2.0/workorder-ce-streaming-reply.md`)
    //
    // What the connection gives the sink so a running walk's rows can leave
    // before it ends. Owned by the connection, which outlives the statement,
    // and installed per statement where it is submitted (CE-A3): `ship`
    // sends the sealed batches and drops them from the sink (CE-A4), and
    // `resume_when` holds while the connection can take more (CE-A5).
    struct Stream {
        std::function<void()> ship;
        std::function<bool()> resume_when;
    };
    void set_stream(Stream* stream) noexcept { stream_ = stream; }
    // Takes the sealed batches, which the caller sends; the open one stays.
    std::vector<Batch> TakeSealed();

    void Rewind() override;
    bool Streamed() const noexcept override { return streamed_; }
    void AtSliceBoundary() override;
    const std::function<bool()>* ResumeWhen() const noexcept override;

private:
    void Seal();
    // A capped portal is fed one batch per client frame and never streams
    // (CE-Q6 (a)).
    bool Streams() const noexcept { return stream_ != nullptr && batch_row_cap_ == 0; }

    bool described_ = false;
    std::vector<wire::FieldDescription> fields_;
    std::vector<Batch> batches_;
    std::vector<std::byte> open_;   // the batch being filled, rows only
    std::uint16_t open_rows_ = 0;
    std::uint64_t rows_ = 0;
    std::uint32_t batch_row_cap_ = 0;
    Stream* stream_ = nullptr;
    bool streamed_ = false;
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

    // ---- Streaming (CE) -------------------------------------------------
    //
    // `ArmStream` gives the running portal's sink the connection's stream,
    // or takes it back with null; a transaction frame has no portal and arms
    // nothing. `StreamRunning` appends to `out` what the running portal can
    // send before its statement ends: `S_ROW_DESC` once, then every sealed
    // batch, which leaves the sink.
    void ArmStream(WireResultSink::Stream* stream);
    void StreamRunning(std::vector<std::byte>& out);

    // Expires portals idle past `kPortalIdleTimeoutNs`. A no-op with no
    // clock. Called by the endpoint on its timer tick; nothing is emitted -
    // a timed-out portal is discovered by the client when it names it.
    void ExpireIdlePortals();

    // §10's cancel flag, the engine session's (`Session::cancel_flag`). Set
    // from another connection; observed by a running `SELECT` at its next
    // slice boundary (BA-S15) and otherwise here at the next frame - there
    // is no preemption, so cancellation is best-effort-fast and
    // guaranteed-eventually exactly as §10 says.
    void RequestCancel() noexcept { session_.RequestCancel(); }

    // Where a `C_CANCEL` this connection receives is matched (§10), or null
    // where the server offers no `CANCEL` - and then the frame is refused
    // as any unexpected first frame is.
    void set_cancel_registry(CancelRegistry* registry) noexcept { cancels_ = registry; }

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
        // `C_CONTINUE` must be refused rather than answered, or it would
        // re-send the batch and a second `S_COMPLETE`.
        bool complete_sent = false;
        // Its `S_ROW_DESC` has gone out ahead of the statement's end, with
        // the first streamed batch (CE); the completion does not send it
        // again.
        bool desc_sent = false;
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
    // `S_ROW_DESC`, unless the portal has sent it already (CE).
    void SendRowDescOnce(Portal& portal, std::vector<std::byte>& out);
    // `S_COMPLETE`, and the portal is spent.
    void Complete(Portal& portal, std::vector<std::byte>& out);
    // The completion of a diagnostic statement: a reply the engine has
    // never typed, delivered as a one-column TEXT result set.
    void DeliverTextLines(std::string_view reply, std::vector<std::byte>& out);

    Portal* FindPortal(const std::string& name);
    // The refusal for a frame naming a portal this session does not hold:
    // `kPortalIdleTimeout` for one the sweep released, `kUnknownPortal`
    // otherwise. `frame` names the frame in the message.
    FrameAction RefuseMissingPortal(std::string_view frame, const std::string& name,
                                    std::vector<std::byte>& out);
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
    CancelRegistry* cancels_ = nullptr;
    std::uint64_t session_id_ = 0;
    std::uint64_t cancel_key_ = 0;

    // Ordered, so a `SHOW`-style enumeration and the golden sessions see a
    // stable order; the counts are small and bounded by the caps above.
    std::map<std::string, Statement> statements_;
    std::map<std::string, Portal> portals_;

    // Portals the idle sweep released, oldest first, so a frame naming one
    // is answered `kPortalIdleTimeout` (§7, §11) rather than
    // `kUnknownPortal`: the client did hold it. An entry leaves when its
    // name is bound again, when it is closed, or when its statement is
    // closed - after which "bind it again" is advice the client cannot
    // follow - and past `kMaxSessionPortals` the oldest is forgotten, so a
    // client that never touches its expired names cannot grow this.
    struct ExpiredPortal {
        std::string name;
        std::string statement;
    };
    std::deque<ExpiredPortal> expired_portals_;

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
