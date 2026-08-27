#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// RocksDB-Status-style explicit error type. rules.md #1: `throw` is
// forbidden everywhere in the engine; every fallible function returns one
// of these instead, and constructors must not fail (fallible construction
// goes through a static factory returning StatusOr<T>).

namespace kds {

enum class StatusCode {
    kOk = 0,
    kInvalidArgument,
    kOutOfSpace,
    kNotFound,
    kAlreadyExists,
    kOutOfRange,
    kCorruption,
    kIoError,
    // A write lost a first-updater-wins race (docs/spec/txn.md §5): the row was
    // written by a transaction still in flight, or one that committed after
    // the writer's read view. Appending is free - nothing persists a
    // StatusCode, and no on-disk format encodes one.
    //
    // The only **retryable** code in this enum, and the distinction matters
    // outward: it maps to wire::ErrorCategory::kTxnConflict with
    // retryable = 1, which docs/spec/protocol.md §11 calls part of the
    // compatibility surface because client libraries build retry loops on
    // that bit. Every other code here means "this will fail the same way
    // again".
    kTxnConflict,
    // A form the language reserves but cannot execute (docs/spec/parser-v2.md J2,
    // I18): table-position nesting, outer joins, over-depth sub-chains,
    // non-pk ORDER BY. Distinct from kInvalidArgument on purpose - the
    // statement is well-formed and the position it carries points at what
    // the engine will not do, not at a typo. A client that sees it should
    // rewrite the statement, never retry it.
    kUnsupported,
    // A scalar subquery returned more than one row (docs/spec/parser-v2.md §2).
    // Parse time cannot prove cardinality in general, so this is per
    // execution; zero rows is NULL and not an error. Picking a first row
    // instead would make the answer depend on physical order.
    kCardinalityViolation,
    // A statement spent its per-statement work budget (exec/budget.hpp).
    // Distinct from kOutOfSpace, which is about storage: nothing is full
    // here, the statement was simply going to read more than it is allowed
    // to. Not retryable - re-running it does the same work and stops at
    // the same place; the fix is a different statement.
    //
    // It exists because nothing suspends mid-statement on a cooperative
    // core, so an unbounded statement holds that core against every other
    // client on it. Failing after a bounded amount of work is the kinder
    // answer than a connection that never replies.
    kResourceExhausted,
    // A write would leave a foreign key unsatisfied (docs/spec/foreign-keys.md
    // F2): a child row referencing a parent that is not there, or a parent
    // delete with a child still referencing it. RESTRICT, the only action v1
    // has.
    //
    // **Not retryable, and its sibling case deliberately is.** A check that
    // meets a *committed* absence will meet it again, so this code says
    // "wrong statement". A check that meets an **in-flight** writer instead
    // answers kTxnConflict, because that one may succeed on a retry - which
    // is the whole of F3's fail-fast rule, and why it needs no code of its
    // own: the retryable case already had one, and clients already retry on
    // it. Splitting the two verdicts across an existing retryable code and a
    // new non-retryable one keeps the wire's `retryable` bit - a
    // compatibility surface (docs/spec/protocol.md §11) - one code wide.
    kFkViolation,
    // A write would take a declared assertion's group aggregate past its
    // bound (docs/spec/assertion.md §4.4, AS9): the admission check on the
    // relation's home core refused the statement before anything was
    // mutated.
    //
    // **Not retryable, and the near-miss is deliberate.** The aggregate a
    // check reads counts committed *and* reserved rows (§4.1), so there is
    // one race where a retry could succeed: a refusal caused by a
    // reservation whose transaction later aborts. §4.3 accepts that as a
    // bounded false rejection precisely because it can only happen where at
    // most one contender could have won anyway - so granting the code the
    // retryable bit would buy that sliver at the price of every client
    // spinning on a group that is genuinely full, which is the common case
    // a bound exists for. A violation against committed state answers the
    // same way forever, kFkViolation's argument exactly.
    kAssertionViolation,
    // **A statement whose outcome nobody can state.** A shipped statement
    // was sent to its relation's owner and no reply arrived before the
    // deadline (server/statement_ship_service.hpp, the work order's D4):
    // it may have committed, it may never have run, and this core cannot
    // tell which.
    //
    // **Its whole reason for existing is that it is not retryable.** Every
    // other refusal in this enum means "nothing happened" - a retry is at
    // worst wasted work. This one does not, and the engine issues primary
    // keys, so a client that retried on it would insert the row twice with
    // two different ids and no way to notice. Folding it into
    // kTxnConflict, which is where a timeout would naturally land, is
    // precisely the mistake: that code carries the wire's `retryable` bit
    // and clients build retry loops on it (docs/spec/protocol.md §11).
    //
    // It is also not kIoError, though that is where an unrecognised code
    // decodes to. IoError says the engine failed to do something; this
    // says the engine may well have *done* it. A client seeing this must
    // read its own data back before deciding anything - which is advice
    // no other code in this enum needs to carry.
    kUnknownOutcome,
};

// True for a Status a caller may sensibly re-issue the same statement for.
// Spelled out here rather than at each call site so the wire layer's
// `retryable` bit and the engine's notion of retryable cannot drift.
//
// **One code, by decision** (docs/spec/protocol.md section 11: the bit is a
// compatibility surface). Everything the engine means by "wait and retry"
// spells itself kTxnConflict - a lost write race, a write to another core's
// relation, a peer's rights still in flight, an index build's window - and
// since 2026-08-25 a peer's **spent lease** too (row-id, transaction-id,
// extent: catalog/row_id_lease.hpp, txn/trx_id_lease.hpp,
// storage/extent_lease.cpp). Those three were kResourceExhausted, whose
// message promised a retry the wire never carried, so a client retrying on
// the bit lost rows (PW6, docs/inflight/known-gaps.md). kResourceExhausted stays for
// what a retry cannot fix: a cap, a budget, a ring's backpressure, and a
// refill core 0 has denied.
constexpr bool IsRetryable(StatusCode code) noexcept { return code == StatusCode::kTxnConflict; }

class [[nodiscard]] Status {
public:
    Status() noexcept : code_(StatusCode::kOk) {}

    static Status OK() { return Status(); }
    static Status InvalidArgument(std::string msg) {
        return Status(StatusCode::kInvalidArgument, std::move(msg));
    }
    static Status OutOfSpace(std::string msg) {
        return Status(StatusCode::kOutOfSpace, std::move(msg));
    }
    static Status NotFound(std::string msg) {
        return Status(StatusCode::kNotFound, std::move(msg));
    }
    static Status AlreadyExists(std::string msg) {
        return Status(StatusCode::kAlreadyExists, std::move(msg));
    }
    static Status OutOfRange(std::string msg) {
        return Status(StatusCode::kOutOfRange, std::move(msg));
    }
    static Status Corruption(std::string msg) {
        return Status(StatusCode::kCorruption, std::move(msg));
    }
    static Status IoError(std::string msg) { return Status(StatusCode::kIoError, std::move(msg)); }
    static Status TxnConflict(std::string msg) {
        return Status(StatusCode::kTxnConflict, std::move(msg));
    }
    static Status Unsupported(std::string msg) {
        return Status(StatusCode::kUnsupported, std::move(msg));
    }
    static Status CardinalityViolation(std::string msg) {
        return Status(StatusCode::kCardinalityViolation, std::move(msg));
    }
    static Status ResourceExhausted(std::string msg) {
        return Status(StatusCode::kResourceExhausted, std::move(msg));
    }
    static Status FkViolation(std::string msg) {
        return Status(StatusCode::kFkViolation, std::move(msg));
    }
    static Status AssertionViolation(std::string msg) {
        return Status(StatusCode::kAssertionViolation, std::move(msg));
    }
    static Status UnknownOutcome(std::string msg) {
        return Status(StatusCode::kUnknownOutcome, std::move(msg));
    }

    // A status decoded off a wire: the code as the integer it travelled as,
    // and a message the receiver chose. **A code outside the enum degrades
    // to IoError** rather than being trusted - nothing persists a
    // StatusCode and no on-disk format encodes one, so a stray integer is a
    // build disagreeing with itself. The one decode for every wire that
    // carries a code (remote steps, index builds): two switches drifted once
    // - one read kAlreadyExists as IoError - and this is what keeps a third
    // from drifting.
    static Status FromWire(std::uint32_t code, std::string msg) {
        switch (static_cast<StatusCode>(code)) {
            case StatusCode::kOk: return OK();
            case StatusCode::kInvalidArgument: return InvalidArgument(std::move(msg));
            case StatusCode::kOutOfSpace: return OutOfSpace(std::move(msg));
            case StatusCode::kNotFound: return NotFound(std::move(msg));
            case StatusCode::kAlreadyExists: return AlreadyExists(std::move(msg));
            case StatusCode::kOutOfRange: return OutOfRange(std::move(msg));
            case StatusCode::kCorruption: return Corruption(std::move(msg));
            case StatusCode::kIoError: return IoError(std::move(msg));
            case StatusCode::kTxnConflict: return TxnConflict(std::move(msg));
            case StatusCode::kUnsupported: return Unsupported(std::move(msg));
            case StatusCode::kCardinalityViolation: return CardinalityViolation(std::move(msg));
            case StatusCode::kResourceExhausted: return ResourceExhausted(std::move(msg));
            case StatusCode::kFkViolation: return FkViolation(std::move(msg));
            case StatusCode::kAssertionViolation: return AssertionViolation(std::move(msg));
            case StatusCode::kUnknownOutcome: return UnknownOutcome(std::move(msg));
        }
        return IoError(std::move(msg));
    }

    // The same failure, said with more context: "<prefix>: <message>",
    // keeping the code. For a layer that knows *which* column or relation a
    // lower layer's failure was about and would otherwise have to choose
    // between losing that fact and hard-coding the code it re-wraps with -
    // and re-wrapping with the wrong code is how a retryable failure stops
    // being retryable. Returns OK unchanged; there is nothing to say about
    // a success.
    Status WithContext(std::string_view prefix) const {
        if (ok()) return *this;
        return Status(code_, std::string(prefix) + ": " + message_);
    }

    bool ok() const noexcept { return code_ == StatusCode::kOk; }
    StatusCode code() const noexcept { return code_; }
    const std::string& message() const noexcept { return message_; }

    // Whether re-issuing the statement that produced this could succeed.
    bool retryable() const noexcept { return IsRetryable(code_); }

private:
    Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}

    StatusCode code_;
    std::string message_;
};

// Holds either a T or a non-ok Status, never both. Mirrors absl/RocksDB
// StatusOr: fallible constructors/factories return this instead of
// throwing or returning a half-valid T.
template <typename T>
class [[nodiscard]] StatusOr {
public:
    StatusOr(Status status) : status_(std::move(status)) {
        // A StatusOr built from a Status must carry an error: constructing
        // one from Status::OK() with no value would leave value() UB-prone.
    }
    StatusOr(T value) : status_(Status::OK()), value_(std::move(value)) {}

    bool ok() const noexcept { return status_.ok(); }
    const Status& status() const noexcept { return status_; }

    // Whether a T is actually held. This agrees with ok() for every
    // StatusOr built as the two constructors document, and disagrees in
    // exactly one case: one built from Status::OK(), which the Status
    // constructor forbids in a comment and nothing enforces. value() would
    // dereference an empty optional there.
    //
    // It exists because of the relation walks
    // (storage/heap/heap_chain.hpp): they hand out a caller-written
    // callback returning StatusOr<VisitControl>, and `return
    // Status::OK();` is the natural thing to write in one - it is what
    // every visitor said before the walks became stoppable. Checking here
    // turns that mistake into a reported error rather than undefined
    // behaviour inside a scan.
    bool has_value() const noexcept { return value_.has_value(); }

    T& value() { return *value_; }
    const T& value() const { return *value_; }

private:
    Status status_;
    std::optional<T> value_;
};

}  // namespace kds
