#include "kds/server/kwp_session.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#include "kds/catalog/well_known.hpp"
#include "kds/exec/type_literals.hpp"
#include "kds/parser/fingerprint.hpp"

// The KWP/1 session (protocol-wp.md P08, P10, P11). Every rule is argued on
// the header; this file is the frames.

namespace kds::server {

namespace {

using wire::ClientFrameType;
using wire::ErrorCategory;
using wire::ProtocolDetail;
using wire::ResourceDetail;
using wire::ServerFrameType;
using wire::Severity;

// `C_DESCRIBE` / `C_CLOSE` kinds. Two values, frozen with the frames.
constexpr std::uint8_t kKindStatement = 1;
constexpr std::uint8_t kKindPortal = 2;

// `S_READY.txn_state` (§3). The engine's own three states, in the same
// order `Session::State` numbers them - one numbering, not two.
constexpr std::uint8_t TxnStateOf(const Session& session) noexcept {
    return static_cast<std::uint8_t>(session.state());
}

// `S_TXN_OK.flags` bit 0 (§9): "for D3 the reply carries flags.RELAXED=1 so
// audit logs can distinguish ack semantics".
constexpr std::uint8_t kTxnOkRelaxed = 0x1;

wire::WireError Protocol(ProtocolDetail detail, std::string message) {
    return wire::ProtocolError(detail, std::move(message), Severity::kError);
}

// ---- A bound parameter as a SQL literal ---------------------------------
//
// The header argues why substitution rather than value binding. This is the
// whole of it: one arm per storable type, each producing the literal text
// this engine's own lexer reads back as the same value.
//
// **Every arm renders through the type's own formatter** where one exists
// (`exec::FormatDate`, `FormatTimestamp`, `FormatDecimal`), which are the
// inverses of the `Parse*Literal` functions the encoder validates with. A
// second rendering here would be the drift `type_literals.hpp`'s "one
// parser per type, two callers, zero drift" header refuses.
StatusOr<std::string> LiteralOf(const wire::BoundParam& param) {
    if (!param.bytes.has_value()) return std::string("NULL");
    const std::span<const std::byte> bytes = *param.bytes;

    auto load_at = [&](std::size_t at, std::size_t width) -> std::uint64_t {
        std::uint64_t u = 0;
        for (std::size_t i = 0; i < width && at + i < bytes.size(); ++i) {
            u |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[at + i]))
                 << (8 * i);
        }
        return u;
    };
    auto load = [&](std::size_t width) { return load_at(0, width); };
    auto signed_of = [&](std::size_t width) -> std::int64_t {
        const std::uint64_t u = load(width);
        const unsigned shift = static_cast<unsigned>(64 - 8 * width);
        // Sign-extend by shifting the value up to the top and back down as
        // a signed quantity - the same widening the row codec's decode
        // does, spelled once.
        return static_cast<std::int64_t>(u << shift) >> shift;
    };

    const std::int16_t want = wire::WireTypeLen(param.type_oid);
    if (want >= 0 && bytes.size() != static_cast<std::size_t>(want)) {
        return Status::InvalidArgument(
            "C_BIND: a parameter of type " + std::to_string(param.type_oid) + " carries " +
            std::to_string(bytes.size()) + " bytes where " + std::to_string(want) +
            " are its width; a disagreeing length is never interpreted");
    }

    switch (param.type_oid) {
        case catalog::kTypeValInt8:
        case catalog::kTypeValInt16:
        case catalog::kTypeValInt32:
        case catalog::kTypeValInt64:
            return std::to_string(signed_of(static_cast<std::size_t>(want)));
        case catalog::kTypeValUint64:
            // Unsigned, because the upper half of the range is exactly what
            // a `uint64` column exists to hold and a signed rendering would
            // spell it as a negative number.
            return std::to_string(load(8));
        case catalog::kTypeValBool:
            // `0`/`1`: this grammar has no boolean literal, which the row
            // encoder says in as many words.
            return load(1) != 0 ? std::string("1") : std::string("0");
        case catalog::kTypeValDate:
            return "'" + exec::FormatDate(static_cast<std::int32_t>(signed_of(4))) + "'";
        case catalog::kTypeValTimestamp:
            return "'" + exec::FormatTimestamp(signed_of(8)) + "'";
        case catalog::kTypeValDecimal:
            return exec::FormatDecimal(signed_of(8), catalog::DecimalScaleOf(param.type_mod));
        case catalog::kTypeValDecimalWide: {
            // 16 LE bytes, **low half first** (§6). The high half carries
            // the sign, so it is the one read as signed.
            const Int128 value =
                Int128FromHalves(static_cast<std::int64_t>(load_at(8, 8)),
                                 static_cast<std::int64_t>(load_at(0, 8)));
            return exec::FormatDecimalWide(value, catalog::DecimalScaleOf(param.type_mod));
        }
        case catalog::kTypeValVarchar:
        case catalog::kTypeValChar: {
            std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (text.find('\'') != std::string::npos) {
                return Status::InvalidArgument(
                    "C_BIND: a text parameter containing a single quote cannot be bound; this "
                    "engine's SQL text has no escape for a quote inside a string literal, so "
                    "the value has no spelling any statement can carry - through this protocol "
                    "or through the newline one (docs/inflight/known-gaps.md)");
            }
            return "'" + text + "'";
        }
        case catalog::kTypeValFloat:
            return Status::Unsupported(
                "C_BIND: FLOAT64 is on the wire and unstorable; no column can hold one, and a "
                "parameter of that type has nothing to be bound into (docs/spec/types.md)");
        default:
            return Status::InvalidArgument("C_BIND: unknown parameter type " +
                                          std::to_string(param.type_oid));
    }
}

// **One scan over the statement text, and only one.**
//
// A placeholder is a `?` that is neither inside a string literal nor inside
// a comment. Both exclusions are the lexer's own rules read back
// (`src/parser/lexer.cpp`): a literal runs from `'` to the next `'` with no
// escaping, and `--` runs to the end of the line. Missing the comment rule
// was a real refusal - `WHERE v = ? -- don't` has an apostrophe in a
// comment, which left the scan believing the statement ended inside a
// string literal and rejected a statement the parser accepts.
//
// `on_placeholder` is called for each one, in order, and answers the text
// to splice in; `out`, when given, collects the rewritten statement. Two
// callers - `C_PARSE` counts, `C_BIND` substitutes - because counting is
// substituting with nothing to splice, and writing the scan twice is how
// one of the two comes to know a rule the other does not.
template <typename OnPlaceholder>
Status ScanPlaceholders(std::string_view sql, std::string* out,
                        OnPlaceholder&& on_placeholder) {
    bool in_string = false;
    for (std::size_t i = 0; i < sql.size(); ++i) {
        const char c = sql[i];
        if (!in_string && c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            const std::size_t eol = sql.find('\n', i);
            const std::size_t end = eol == std::string_view::npos ? sql.size() : eol;
            if (out != nullptr) out->append(sql.substr(i, end - i));
            i = end - 1;  // the loop's ++i lands on the newline, or ends
            continue;
        }
        if (c == '\'') in_string = !in_string;
        if (c != '?' || in_string) {
            if (out != nullptr) out->push_back(c);
            continue;
        }
        if (Status s = on_placeholder(out); !s.ok()) return s;
    }
    if (in_string) {
        return Status::InvalidArgument("C_BIND: the statement ends inside a string literal");
    }
    return Status::OK();
}

StatusOr<std::string> Substitute(std::string_view sql,
                                 const std::vector<wire::BoundParam>& params) {
    std::string out;
    out.reserve(sql.size() + params.size() * 8);
    std::size_t next = 0;
    Status scanned = ScanPlaceholders(sql, &out, [&](std::string* dst) -> Status {
        if (next >= params.size()) {
            return Status::InvalidArgument(
                "C_BIND: the statement has more '?' placeholders than the " +
                std::to_string(params.size()) + " parameters bound");
        }
        auto literal = LiteralOf(params[next++]);
        if (!literal.ok()) return literal.status();
        *dst += literal.value();
        return Status::OK();
    });
    if (!scanned.ok()) return scanned;
    if (next != params.size()) {
        return Status::InvalidArgument("C_BIND: " + std::to_string(params.size()) +
                                       " parameters were bound to a statement with " +
                                       std::to_string(next) + " '?' placeholders");
    }
    return out;
}

// The `?` count, by the same scan. Reported at `C_PARSE` so a client knows
// how many parameters `C_BIND` owes.
std::uint32_t PlaceholderCount(std::string_view sql) {
    std::uint32_t n = 0;
    (void)ScanPlaceholders(sql, nullptr, [&](std::string*) {
        ++n;
        return Status::OK();
    });
    return n;
}

}  // namespace

// ---- WireResultSink ------------------------------------------------------

Status WireResultSink::Describe(std::vector<wire::FieldDescription> fields) {
    fields_ = std::move(fields);
    described_ = true;
    return Status::OK();
}

Status WireResultSink::EncodeProjectedRow(std::span<const exec::ColumnRef> projection,
                                          std::span<const std::uint32_t> types,
                                          const exec::ChainFrame& frame, std::string& out) {
    out.clear();
    std::vector<std::byte> bytes;
    bytes.reserve(projection.size() * 12);
    for (std::size_t i = 0; i < projection.size(); ++i) {
        const std::uint32_t type_val = i < types.size() ? types[i] : 0;
        const std::uint32_t type_mod = i < fields_.size() ? fields_[i].type_mod : 0;
        if (Status s = wire::EncodeValue(type_val, type_mod, frame.Get(projection[i]), bytes);
            !s.ok()) {
            return s;
        }
    }
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return Status::OK();
}

Status WireResultSink::EncodeValueRow(std::span<const std::uint32_t> types,
                                      std::span<const parser::AstValue> values,
                                      std::string& out) {
    out.clear();
    std::vector<std::byte> bytes;
    bytes.reserve(values.size() * 12);
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::uint32_t type_val = i < types.size() ? types[i] : 0;
        const std::uint32_t type_mod = i < fields_.size() ? fields_[i].type_mod : 0;
        if (Status s = wire::EncodeValue(type_val, type_mod, values[i], bytes); !s.ok()) return s;
    }
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return Status::OK();
}

Status WireResultSink::Emit(std::string_view row) {
    // **Sealed before the row that would cross the target, not after.** A
    // batch that grew past `kRowBatchTargetBytes` and then sealed would
    // exceed the target by one row's width every time, which for a wide row
    // is not a rounding error. One row wider than the whole target still
    // ships whole, because the alternative is a row nothing can send.
    if (open_rows_ > 0 &&
        (open_.size() + row.size() > target_bytes_ ||
         open_rows_ == std::numeric_limits<std::uint16_t>::max() ||
         (batch_row_cap_ != 0 && open_rows_ >= batch_row_cap_))) {
        Seal();
    }
    const auto* p = reinterpret_cast<const std::byte*>(row.data());
    open_.insert(open_.end(), p, p + row.size());
    ++open_rows_;
    ++rows_;
    return Status::OK();
}

void WireResultSink::Seal() {
    Batch batch;
    batch.rows = open_rows_;
    batch.payload.reserve(open_.size() + 2);
    batch.payload.push_back(static_cast<std::byte>(open_rows_ & 0xFF));
    batch.payload.push_back(static_cast<std::byte>((open_rows_ >> 8) & 0xFF));
    batch.payload.insert(batch.payload.end(), open_.begin(), open_.end());
    batches_.push_back(std::move(batch));
    open_.clear();
    open_rows_ = 0;
}

void WireResultSink::Finish() {
    if (open_rows_ > 0) Seal();
}

void WireResultSink::Reset() {
    described_ = false;
    fields_.clear();
    batches_.clear();
    open_.clear();
    open_rows_ = 0;
    rows_ = 0;
    batch_row_cap_ = 0;
}

// ---- KwpSession ----------------------------------------------------------

KwpSession::KwpSession(Session& session, wire::HandshakeConfig handshake,
                       wal::DurabilityClass server_durability,
                       const sched::Clock* clock) noexcept
    : handshake_(std::move(handshake)),
      server_durability_(server_durability),
      clock_(clock),
      session_(session) {}

sched::MonoTimeNs KwpSession::Now() const noexcept {
    return clock_ != nullptr ? clock_->Now() : 0;
}

void KwpSession::Send(std::vector<std::byte>& out, ServerFrameType type, std::uint8_t flags,
                      std::span<const std::byte> payload) {
    const auto frame = wire::EncodeFrame(static_cast<std::uint8_t>(type), flags, payload);
    out.insert(out.end(), frame.begin(), frame.end());
}

void KwpSession::SendError(std::vector<std::byte>& out, const wire::WireError& error) {
    Send(out, ServerFrameType::kError, 0, wire::EncodeError(error));
}

void KwpSession::SendReady(std::vector<std::byte>& out) {
    wire::PayloadWriter w;
    w.U8(TxnStateOf(session_));
    Send(out, ServerFrameType::kReady, 0, w.Take());
}

FrameAction KwpSession::Refuse(std::vector<std::byte>& out, const wire::WireError& error) {
    SendError(out, error);
    // §5's whole pipelining contract: after an error the server discards
    // frames until the next `C_SYNC`. Armed here so every refusal in this
    // file gets it without remembering to.
    skipping_to_sync_ = true;
    FrameAction action;
    action.close = error.severity == Severity::kFatal;
    return action;
}

FrameAction KwpSession::OnFrame(const wire::DecodedFrame& frame, std::vector<std::byte>& out) {
    const auto type = static_cast<ClientFrameType>(frame.type);

    // `C_TERMINATE` is answered in every phase and in every posture,
    // which is what makes it the one frame a client can always send -
    // **including with a cancel pending**, since refusing the frame that
    // closes the connection would be refusing the thing a cancelled client
    // most likely wants next.
    if (type == ClientFrameType::kTerminate) {
        FrameAction action;
        action.close = true;
        return action;
    }

    // **Cancellation is observed here** (§10). The reactor has no
    // preemption, so a session learns it was cancelled at its next frame,
    // and the frame that learns it is refused rather than run. Consumed, so
    // one cancel cancels one statement.
    if (cancel_requested_ && phase_ == Phase::kReady) {
        cancel_requested_ = false;
        wire::WireError e =
            wire::ErrorFromStatus(Status::InvalidArgument("cancelled by request"));
        e.code = wire::MakeErrorCode(ErrorCategory::kCancelled, wire::kNoDetail);
        session_.Poison();
        return Refuse(out, e);
    }

    if (phase_ == Phase::kAwaitHello) {
        if (type != ClientFrameType::kHello) {
            return Refuse(out, wire::ProtocolError(
                                   ProtocolDetail::kUnexpectedFrame,
                                   "the first frame on a connection must be C_HELLO",
                                   Severity::kFatal));
        }
        return OnHello(frame.payload, out);
    }
    if (phase_ == Phase::kAwaitAuth) {
        if (type != ClientFrameType::kAuth) {
            return Refuse(out, wire::ProtocolError(
                                   ProtocolDetail::kUnexpectedFrame,
                                   "this connection owes an authentication exchange; send C_AUTH",
                                   Severity::kFatal));
        }
        return OnAuth(frame.payload, out);
    }

    // ---- §5's skip-to-sync ----------------------------------------------
    if (skipping_to_sync_) {
        if (type != ClientFrameType::kSync) return FrameAction{};  // discarded, silently
        skipping_to_sync_ = false;
        SendReady(out);
        return FrameAction{};
    }

    switch (type) {
        case ClientFrameType::kSync:
            SendReady(out);
            return FrameAction{};
        case ClientFrameType::kPing:
            Send(out, ServerFrameType::kPong, 0, {});
            return FrameAction{};
        case ClientFrameType::kParse: return OnParse(frame.payload, out);
        case ClientFrameType::kBind: return OnBind(frame.payload, out);
        case ClientFrameType::kDescribe: return OnDescribe(frame.payload, out);
        case ClientFrameType::kExecute: return OnExecute(frame.payload, out);
        case ClientFrameType::kContinue: return OnContinue(frame.payload, out);
        case ClientFrameType::kClose: return OnClose(frame.payload, out);
        case ClientFrameType::kTxnBegin:
        case ClientFrameType::kTxnCommit:
        case ClientFrameType::kTxnAbort: return OnTxn(type, frame.payload, out);
        case ClientFrameType::kHello:
            return Refuse(out, Protocol(ProtocolDetail::kUnexpectedFrame,
                                        "C_HELLO after the handshake"));
        default:
            // Not fatal: a client built against a later version sending a
            // frame this build has never heard of is what capability bits
            // exist to survive, and skip-to-sync is how it recovers.
            return Refuse(out, Protocol(ProtocolDetail::kUnknownFrameType,
                                        "unknown client frame type " +
                                            std::to_string(frame.type)));
    }
}

FrameAction KwpSession::OnHello(std::span<const std::byte> payload, std::vector<std::byte>& out) {
    auto hello = wire::DecodeClientHello(payload);
    if (!hello.ok()) {
        return Refuse(out, wire::ProtocolError(ProtocolDetail::kBadMagic,
                                               hello.status().message(), Severity::kFatal));
    }
    auto outcome = wire::Negotiate(hello.value(), handshake_, auth_gate_ != nullptr,
                                   session_id_, cancel_key_);
    if (!outcome.accepted) return Refuse(out, outcome.error);

    if (outcome.auth_owed) {
        // **No `S_HELLO` yet.** The session id and the cancel key are what
        // §10's out-of-band cancel is built on, and an unauthenticated
        // connection must not learn either.
        phase_ = Phase::kAwaitAuth;
        return FrameAction{};
    }
    phase_ = Phase::kReady;
    Send(out, ServerFrameType::kHello, 0, wire::EncodeServerHello(outcome.hello));
    SendReady(out);
    return FrameAction{};
}

FrameAction KwpSession::OnAuth(std::span<const std::byte> payload, std::vector<std::byte>& out) {
    wire::PayloadReader r(payload);
    auto body = r.Text();
    if (!body.has_value() || !body->has_value()) {
        return Refuse(out, wire::ProtocolError(ProtocolDetail::kMalformedPayload,
                                               "C_AUTH: truncated payload", Severity::kFatal));
    }
    // The gate's own line protocol, byte for byte: `auth.hpp` says "KWP
    // will carry these same SCRAM message bodies in handshake frames; only
    // this line framing is protocol-specific", and this is that sentence
    // built rather than restated. One gate serves both surfaces.
    AuthGate::Result result = auth_gate_->OnLine(**body);
    wire::PayloadWriter w;
    w.Text(result.reply);
    Send(out, ServerFrameType::kAuth, 0, w.Take());

    if (result.close) {
        FrameAction action;
        action.close = true;
        return action;
    }
    if (!result.authenticated) return FrameAction{};

    session_.set_role(result.role);
    auth_gate_.reset();
    phase_ = Phase::kReady;

    wire::ServerHello hello;
    hello.version = handshake_.max_version;
    hello.capabilities = handshake_.capabilities;
    hello.session_id = session_id_;
    hello.cancel_key = cancel_key_;
    hello.server_info = handshake_.server_info;
    Send(out, ServerFrameType::kHello, 0, wire::EncodeServerHello(hello));
    SendReady(out);
    return FrameAction{};
}

FrameAction KwpSession::OnParse(std::span<const std::byte> payload, std::vector<std::byte>& out) {
    wire::PayloadReader r(payload);
    auto name = r.Str();
    auto sql = r.Text();
    if (!name.has_value() || !sql.has_value() || !sql->has_value()) {
        return Refuse(out, Protocol(ProtocolDetail::kMalformedPayload,
                                    "C_PARSE: truncated payload"));
    }
    if (statements_.size() >= kMaxSessionStatements &&
        statements_.find(*name) == statements_.end()) {
        wire::WireError e = wire::ErrorFromStatus(
            Status::ResourceExhausted("this session already holds " +
                                      std::to_string(kMaxSessionStatements) +
                                      " named statements; close one before parsing another"),
            static_cast<std::uint16_t>(ResourceDetail::kStatementLimit));
        return Refuse(out, e);
    }

    Statement stmt;
    stmt.sql = std::move(**sql);
    stmt.param_count = PlaceholderCount(stmt.sql);
    // **The Waystone tie-in** (§5): the fingerprint is computed at PARSE,
    // and `pattern_id` goes back informational. `FingerprintOf` answers
    // nullopt for every statement that has no shape worth remembering - DDL,
    // session statements, anything that will not lex - and 0 is what a
    // client sees then, which is the same "no pattern" a `sys.patterns`
    // row can never carry (its version pin reserves 0).
    if (auto fp = parser::FingerprintOf(stmt.sql); fp.has_value()) {
        stmt.pattern_id = fp->pattern_id;
    }
    // The unnamed statement is overwritten by the next PARSE (§5); a named
    // one survives until `C_CLOSE` or disconnect. One assignment does both.
    const std::uint64_t pattern_id = stmt.pattern_id;
    statements_[*name] = std::move(stmt);

    wire::PayloadWriter w;
    w.U64(pattern_id);
    Send(out, ServerFrameType::kParseOk, 0, w.Take());
    return FrameAction{};
}

FrameAction KwpSession::OnBind(std::span<const std::byte> payload, std::vector<std::byte>& out) {
    wire::PayloadReader r(payload);
    auto portal_name = r.Str();
    auto stmt_name = r.Str();
    if (!portal_name.has_value() || !stmt_name.has_value()) {
        return Refuse(out, Protocol(ProtocolDetail::kMalformedPayload,
                                    "C_BIND: truncated payload"));
    }
    Statement* stmt = FindStatement(*stmt_name);
    if (stmt == nullptr) {
        return Refuse(out, Protocol(ProtocolDetail::kUnknownStatement,
                                    "C_BIND names statement '" + *stmt_name +
                                        "', which this session does not hold"));
    }
    auto params = wire::DecodeBindParams(r.Rest());
    if (!params.ok()) {
        return Refuse(out, Protocol(ProtocolDetail::kMalformedPayload,
                                    params.status().message()));
    }
    auto bound = Substitute(stmt->sql, params.value());
    if (!bound.ok()) return Refuse(out, wire::ErrorFromStatus(bound.status()));

    if (portals_.size() >= kMaxSessionPortals && portals_.find(*portal_name) == portals_.end()) {
        wire::WireError e = wire::ErrorFromStatus(
            Status::ResourceExhausted("this session already holds " +
                                      std::to_string(kMaxSessionPortals) +
                                      " portals; close one before binding another"),
            static_cast<std::uint16_t>(ResourceDetail::kPortalLimit));
        return Refuse(out, e);
    }

    Portal portal;
    portal.statement = *stmt_name;
    portal.sql = std::move(bound.value());
    portal.idle_since = Now();
    portals_[*portal_name] = std::move(portal);

    Send(out, ServerFrameType::kBindOk, 0, {});
    return FrameAction{};
}

FrameAction KwpSession::OnDescribe(std::span<const std::byte> payload,
                                   std::vector<std::byte>& out) {
    wire::PayloadReader r(payload);
    auto kind = r.U8();
    auto name = r.Str();
    if (!kind.has_value() || !name.has_value()) {
        return Refuse(out, Protocol(ProtocolDetail::kMalformedPayload,
                                    "C_DESCRIBE: truncated payload"));
    }
    if (*kind != kKindStatement && *kind != kKindPortal) {
        return Refuse(out, Protocol(ProtocolDetail::kMalformedPayload,
                                    "C_DESCRIBE: kind must be 1 (statement) or 2 (portal)"));
    }
    if (*kind == kKindStatement) {
        // **Refused, with the reason** (§6, amended). A result's shape here
        // is resolved by compiling the statement against a catalog and a
        // snapshot - which is what execution does - so the shape of an
        // unexecuted statement is not knowable without running it, and
        // running it to answer a describe would make a describe a side
        // effect. A client that needs the shape executes and reads
        // `S_ROW_DESC`.
        return Refuse(out, wire::ErrorFromStatus(Status::Unsupported(
                               "C_DESCRIBE of a statement is not answerable: this engine "
                               "resolves a result's shape by compiling the statement, which "
                               "happens at execution - describe the portal after C_EXECUTE")));
    }
    Portal* portal = FindPortal(*name);
    if (portal == nullptr) {
        return Refuse(out, Protocol(ProtocolDetail::kUnknownPortal,
                                    "C_DESCRIBE names portal '" + *name +
                                        "', which this session does not hold"));
    }
    if (!portal->executed || !portal->sink.described()) {
        return Refuse(out, wire::ErrorFromStatus(Status::Unsupported(
                               "portal '" + *name +
                               "' has no row description: it has not been executed, or its "
                               "statement produced a completion rather than a result set")));
    }
    std::vector<std::byte> desc;
    wire::EncodeRowDescription(portal->sink.fields(), desc);
    Send(out, ServerFrameType::kRowDesc, 0, desc);
    return FrameAction{};
}

FrameAction KwpSession::OnExecute(std::span<const std::byte> payload,
                                  std::vector<std::byte>& out) {
    wire::PayloadReader r(payload);
    auto name = r.Str();
    auto max_rows = r.U32();
    if (!name.has_value() || !max_rows.has_value()) {
        return Refuse(out, Protocol(ProtocolDetail::kMalformedPayload,
                                    "C_EXECUTE: truncated payload"));
    }
    Portal* portal = FindPortal(*name);
    if (portal == nullptr) {
        return Refuse(out, Protocol(ProtocolDetail::kUnknownPortal,
                                    "C_EXECUTE names portal '" + *name +
                                        "', which this session does not hold"));
    }
    if (portal->executed) {
        // Re-executing a portal would re-run its statement, which for a
        // write is a second write. `C_CONTINUE` is how more rows are asked
        // for; a fresh run needs a fresh `C_BIND`.
        return Refuse(out, Protocol(ProtocolDetail::kUnexpectedFrame,
                                    "portal '" + *name +
                                        "' has already been executed; use C_CONTINUE for more "
                                        "rows, or C_BIND again to run it afresh"));
    }
    portal->executed = true;
    portal->idle_since = Now();
    // The quota goes to the sink *before* the statement runs, so the
    // batches are sealed on the boundary the client asked for rather than
    // metered afterwards on one the sink chose.
    portal->batch_row_cap = *max_rows;
    portal->sink.set_batch_row_cap(*max_rows);
    running_portal_ = *name;
    // Cleared here and not only where it is set: a transaction dispatch the
    // caller declined to run (no dispatcher, no reactor) leaves the flag
    // standing, and the next portal's completion would then take the
    // transaction arm and answer `S_TXN_OK` where the client is waiting for
    // rows. The flag belongs to the dispatch in flight, so every dispatch
    // states it.
    running_txn_ = false;

    FrameAction action;
    action.dispatch = true;
    action.sql = portal->sql;
    action.sink = &portal->sink;
    return action;
}

void KwpSession::OnStatementComplete(const DispatchOutcome& outcome,
                                     std::vector<std::byte>& out) {
    // ---- Transaction control (§9) ---------------------------------------
    if (running_txn_) {
        running_txn_ = false;
        if (Status failed = !outcome.status.ok() ? outcome.status
                                                 : StatusFromErrorReply(outcome.response);
            !failed.ok()) {
            (void)Refuse(out, wire::ErrorFromStatus(failed));
            return;
        }
        // **`S_TXN_OK` is sent only after the WAL ack point of the chosen
        // class** (§9), and it already has been: the dispatcher waits
        // inline for D1 and D2 before it returns, so by the time an
        // outcome reaches here the acknowledgement it represents is the
        // one the class promised.
        //
        // The RELAXED flag is set on the class the commit *used*, read
        // through the same precedence chain the commit read it through -
        // not on the byte `C_TXN_BEGIN` carried, which may have said
        // "session default".
        const bool relaxed =
            running_txn_begin_ ? session_.EffectiveDurability(server_durability_) ==
                                     wal::DurabilityClass::kRelaxed
                               : running_relaxed_;
        Send(out, ServerFrameType::kTxnOk, relaxed ? kTxnOkRelaxed : 0, {});
        return;
    }

    Portal* portal = FindPortal(running_portal_);
    if (portal == nullptr) {
        // The portal was closed while its statement ran - a client may
        // pipeline `C_CLOSE` behind `C_EXECUTE`. The statement still ran
        // and its writes stand; there is simply nobody to deliver rows to.
        running_portal_.clear();
        return;
    }
    portal->sink.Finish();

    // **The status the dispatcher carried**, falling back to recovering one
    // from the rendered line.
    //
    // The carried one is the whole taxonomy: `NotFound`, `Unsupported`,
    // `Corruption` and the rest reach the client as themselves. The
    // fallback is `StatusFromErrorReply`, which recovers exactly the four
    // spellings `ErrorReply` gives a token to and folds every other into
    // `InvalidArgument` - right for the cross-core path it was written for,
    // where a rendered line is genuinely all there is, and a floor rather
    // than the answer here.
    if (Status failed = !outcome.status.ok() ? outcome.status
                                             : StatusFromErrorReply(outcome.response);
        !failed.ok()) {
        // **XG-R8: the portal goes with the statement that failed.**
        //
        // It used to have its sink reset and its entry kept, and the client
        // was expected to close it - which §5's skip-to-sync makes it
        // unable to do: after this `Refuse` arms the skip, every frame up
        // to the next `C_SYNC` is discarded, **including a `C_CLOSE` the
        // client pipelined behind this statement**. So the close was
        // dropped on exactly the statements that most needed it, the portal
        // leaked, and at `kMaxSessionPortals` every further `C_BIND` was
        // refused until the 60 s idle sweep freed one - a retrying client
        // running at one statement per portal lifetime. Clients worked
        // around it by sending `C_CLOSE` as its own frame after `S_READY`,
        // at a round trip on every *successful* statement.
        //
        // Erased here rather than admitting `C_CLOSE` into the skip, which
        // was the other option: this is where the portal is already in
        // hand, it needs no new rule about what "skipping" means, and it
        // makes the client's close a no-op instead of a requirement.
        // `protocol.md` §7 states the lifecycle a client sees for it - a
        // portal ceases to exist when its statement fails.
        //
        // No `sink.Reset()`: the whole portal goes, and `portal` dangles
        // from here on - nothing below reads it.
        portals_.erase(running_portal_);
        running_portal_.clear();
        (void)Refuse(out, wire::ErrorFromStatus(failed));
        return;
    }

    if (portal->sink.described()) {
        std::vector<std::byte> desc;
        wire::EncodeRowDescription(portal->sink.fields(), desc);
        Send(out, ServerFrameType::kRowDesc, 0, desc);
        portal->complete_tag = "SELECT";
        portal->rows_affected = portal->sink.row_count();
    } else {
        // No result set: the reply *is* the completion tag. A multi-line
        // diagnostic answer (`SHOW META`, `DESCRIBE`, `ANALYZE`) is carried
        // as a one-column TEXT result set instead - see `Deliver`.
        // The count the dispatcher already holds, not a number parsed back
        // out of the sentence it rendered - the three DML replies do not
        // share a spelling, so a parser here would answer 0 for the
        // commonest write in the engine (`DispatchOutcome::rows_affected`).
        portal->complete_tag = outcome.response;
        portal->rows_affected = outcome.rows_affected;
    }
    Deliver(running_portal_, *portal, out);
    running_portal_.clear();
}

FrameAction KwpSession::OnContinue(std::span<const std::byte> payload,
                                   std::vector<std::byte>& out) {
    wire::PayloadReader r(payload);
    auto name = r.Str();
    auto max_rows = r.U32();
    if (!name.has_value() || !max_rows.has_value()) {
        return Refuse(out, Protocol(ProtocolDetail::kMalformedPayload,
                                    "C_CONTINUE: truncated payload"));
    }
    Portal* portal = FindPortal(*name);
    if (portal == nullptr) {
        return Refuse(out, Protocol(ProtocolDetail::kUnknownPortal,
                                    "C_CONTINUE names portal '" + *name +
                                        "', which this session does not hold"));
    }
    if (!portal->executed) {
        return Refuse(out, Protocol(ProtocolDetail::kUnexpectedFrame,
                                    "portal '" + *name +
                                        "' has not been executed; C_CONTINUE resumes a "
                                        "suspended portal"));
    }
    if (portal->complete_sent) {
        // A portal that has answered `S_COMPLETE` is spent. Without this a
        // second `C_CONTINUE` re-sent a diagnostic answer's whole batch -
        // duplicate rows - and a duplicate completion carrying the full
        // count again; and after a *failed* statement it answered an empty
        // tag rather than refusing.
        return Refuse(out, Protocol(ProtocolDetail::kUnexpectedFrame,
                                    "portal '" + *name +
                                        "' has already completed; bind again to run its "
                                        "statement afresh"));
    }
    // **`max_rows` is not re-read here.** The quota was applied when the
    // batches were sealed, so a `C_CONTINUE` naming a different one cannot
    // change a boundary that already exists - and honouring it would be a
    // second quota model. The field stays on the wire because §7 puts it
    // there; what it does is resume.
    (void)max_rows;
    Deliver(*name, *portal, out);
    return FrameAction{};
}

// A reply the engine has never typed - `SHOW META`, `DESCRIBE`, `ANALYZE`,
// `SHOW PAGE` - as a one-column TEXT result set, one row per line.
//
// §10 calls these "ordinary statements returning ordinary result sets", and
// one column of text is the ordinary result set such an answer can honestly
// be: the alternative is either a bespoke typed shape per diagnostic - a
// second model of every one of them - or a completion tag with newlines
// inside it, which is not a tag. The line separator is the newline
// protocol's own two-character `\n` escape, which is what the dispatcher
// wrote.
void KwpSession::DeliverTextLines(std::string_view reply, std::vector<std::byte>& out) {
    std::vector<wire::FieldDescription> fields(1);
    fields[0].name = "line";
    fields[0].type_oid = catalog::kTypeValVarchar;
    fields[0].type_len = -1;
    fields[0].flags = wire::kFieldFlagDiagnosticLine;
    std::vector<std::byte> desc;
    wire::EncodeRowDescription(fields, desc);
    Send(out, ServerFrameType::kRowDesc, 0, desc);

    wire::RowBatchWriter batch;
    catalog::Schema schema;
    catalog::SysColumnRow col{};
    col.type_val = catalog::kTypeValVarchar;
    schema.columns.push_back(col);
    std::uint64_t lines = 0;
    std::string_view rest = reply;
    while (true) {
        const std::size_t at = rest.find("\\n");
        parser::AstValue value;
        value.type = parser::ValueType::kStr;
        value.str_val = std::string(rest.substr(0, at));
        const std::array<parser::AstValue, 1> row{value};
        if (Status s = batch.AppendRow(schema, row); !s.ok()) {
            // Reported, never truncated. A short answer counted as complete
            // is a diagnostic that lies about what the server said.
            (void)Refuse(out, wire::ErrorFromStatus(s));
            return;
        }
        ++lines;
        if (at == std::string_view::npos) break;
        rest = rest.substr(at + 2);
    }
    if (batch.row_count() > 0) Send(out, ServerFrameType::kRowBatch, 0, batch.Finish());
    wire::PayloadWriter w;
    w.Text("SELECT");
    w.U64(lines);
    Send(out, ServerFrameType::kComplete, 0, w.Take());
}

void KwpSession::Complete(Portal& portal, std::vector<std::byte>& out) {
    wire::PayloadWriter w;
    w.Text(portal.complete_tag);
    w.U64(portal.rows_affected);
    Send(out, ServerFrameType::kComplete, 0, w.Take());
    portal.complete_sent = true;
}

void KwpSession::Deliver(const std::string& name, Portal& portal, std::vector<std::byte>& out) {
    portal.idle_since = Now();

    // A completion with no result set: one `S_COMPLETE`, or - for a reply
    // that carries lines rather than a tag - a text result set. Delivered
    // whole either way: both are bounded by the reply the dispatcher
    // already built, and neither has batches to meter.
    if (!portal.sink.described()) {
        if (portal.complete_tag.find("\\n") != std::string::npos) {
            DeliverTextLines(portal.complete_tag, out);
            portal.complete_sent = true;
            return;
        }
        Complete(portal, out);
        return;
    }

    // **No quota arithmetic here.** The `max_rows` a `C_EXECUTE` asked for
    // was given to the sink before the statement ran, so the batches are
    // already sealed on the boundary the client named: one batch per
    // `C_EXECUTE`/`C_CONTINUE` is exactly the quota. Metering at this end
    // could only ever stop on a boundary the sink had already chosen, which
    // is how `max_rows = 1` used to deliver a whole result set.
    auto& batches = portal.sink.batches();
    if (portal.next_batch < batches.size()) {
        Send(out, ServerFrameType::kRowBatch, 0, batches[portal.next_batch].payload);
        ++portal.next_batch;
    }
    // An uncapped portal seals at the size target alone, so its remaining
    // batches all go now; a capped one stops after the batch above.
    if (portal.batch_row_cap == 0) {
        while (portal.next_batch < batches.size()) {
            Send(out, ServerFrameType::kRowBatch, 0, batches[portal.next_batch].payload);
            ++portal.next_batch;
        }
    }

    if (portal.next_batch < batches.size()) {
        wire::PayloadWriter w;
        w.Str(name);
        Send(out, ServerFrameType::kPortalSuspended, 0, w.Take());
        return;
    }
    Complete(portal, out);
}

FrameAction KwpSession::OnClose(std::span<const std::byte> payload, std::vector<std::byte>& out) {
    wire::PayloadReader r(payload);
    auto kind = r.U8();
    auto name = r.Str();
    if (!kind.has_value() || !name.has_value()) {
        return Refuse(out, Protocol(ProtocolDetail::kMalformedPayload,
                                    "C_CLOSE: truncated payload"));
    }
    if (*kind == kKindPortal) {
        portals_.erase(*name);
    } else if (*kind == kKindStatement) {
        statements_.erase(*name);
        // Its portals go with it: a portal is a binding *of* a statement,
        // and one left behind would name text nothing holds.
        for (auto it = portals_.begin(); it != portals_.end();) {
            it = it->second.statement == *name ? portals_.erase(it) : std::next(it);
        }
    } else {
        return Refuse(out, Protocol(ProtocolDetail::kMalformedPayload,
                                    "C_CLOSE: kind must be 1 (statement) or 2 (portal)"));
    }
    // No reply frame: §4 has none for CLOSE, and `C_SYNC` is the barrier a
    // pipelining client waits on.
    return FrameAction{};
}

FrameAction KwpSession::OnTxn(ClientFrameType type, std::span<const std::byte> payload,
                              std::vector<std::byte>& out) {
    std::string sql;
    if (type == ClientFrameType::kTxnBegin) {
        wire::PayloadReader r(payload);
        auto durability = r.U8();
        if (!durability.has_value()) {
            return Refuse(out, Protocol(ProtocolDetail::kMalformedPayload,
                                        "C_TXN_BEGIN: truncated payload"));
        }
        sql = "BEGIN";
        switch (static_cast<wire::DurabilityLevel>(*durability)) {
            case wire::DurabilityLevel::kSessionDefault: break;
            case wire::DurabilityLevel::kStrict: sql += " DURABILITY strict"; break;
            case wire::DurabilityLevel::kGroup: sql += " DURABILITY group"; break;
            case wire::DurabilityLevel::kRelaxed: sql += " DURABILITY relaxed"; break;
            default:
                return Refuse(out, Protocol(ProtocolDetail::kMalformedPayload,
                                            "C_TXN_BEGIN: durability " +
                                                std::to_string(*durability) +
                                                " is not 0 (session default), 1 (strict), "
                                                "2 (group) or 3 (relaxed)"));
        }
    } else {
        sql = type == ClientFrameType::kTxnCommit ? "COMMIT" : "ROLLBACK";
    }

    // Transaction control has no portal and no rows: it dispatches through
    // the same path with no sink, and `OnTxnComplete` is `Deliver`'s
    // completion arm specialised to `S_TXN_OK`.
    running_portal_.clear();
    running_txn_ = true;
    // **`flags.RELAXED` names the class of the transaction the frame is
    // about, and the two verbs read it at opposite moments.**
    //
    // A `COMMIT`/`ROLLBACK` *ends* the transaction, and `Session::Finish()`
    // clears the transaction rung with it - so read after the dispatch,
    // `EffectiveDurability` would answer about the *next* statement. Read
    // here, while the transaction it describes is still open.
    //
    // A `BEGIN` is the mirror: the rung it sets does not exist yet, so its
    // answer is taken after the dispatch instead (`OnStatementComplete`).
    // One rule - the class of *this* transaction - read where the
    // transaction exists.
    running_txn_begin_ = type == ClientFrameType::kTxnBegin;
    running_relaxed_ =
        session_.EffectiveDurability(server_durability_) == wal::DurabilityClass::kRelaxed;
    FrameAction action;
    action.dispatch = true;
    action.sql = std::move(sql);
    action.sink = nullptr;
    return action;
}

void KwpSession::ExpireIdlePortals() {
    if (clock_ == nullptr) return;
    const sched::MonoTimeNs now = clock_->Now();
    for (auto it = portals_.begin(); it != portals_.end();) {
        const bool stale = now - it->second.idle_since >= kPortalIdleTimeoutNs;
        it = stale ? portals_.erase(it) : std::next(it);
    }
}

KwpSession::Portal* KwpSession::FindPortal(const std::string& name) {
    auto it = portals_.find(name);
    return it == portals_.end() ? nullptr : &it->second;
}

KwpSession::Statement* KwpSession::FindStatement(const std::string& name) {
    auto it = statements_.find(name);
    return it == statements_.end() ? nullptr : &it->second;
}

}  // namespace kds::server
