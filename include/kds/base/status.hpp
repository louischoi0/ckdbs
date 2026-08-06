#pragma once

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
    // A write lost a first-updater-wins race (docs/txn.md §5): the row was
    // written by a transaction still in flight, or one that committed after
    // the writer's read view. Appending is free - nothing persists a
    // StatusCode, and no on-disk format encodes one.
    //
    // The only **retryable** code in this enum, and the distinction matters
    // outward: it maps to wire::ErrorCategory::kTxnConflict with
    // retryable = 1, which docs/protocol.md §11 calls part of the
    // compatibility surface because client libraries build retry loops on
    // that bit. Every other code here means "this will fail the same way
    // again".
    kTxnConflict,
    // A form the language reserves but cannot execute (docs/parser-v2.md J2,
    // I18): table-position nesting, outer joins, over-depth sub-chains,
    // non-pk ORDER BY. Distinct from kInvalidArgument on purpose - the
    // statement is well-formed and the position it carries points at what
    // the engine will not do, not at a typo. A client that sees it should
    // rewrite the statement, never retry it.
    kUnsupported,
    // A scalar subquery returned more than one row (docs/parser-v2.md §2).
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
    // A write would leave a foreign key unsatisfied (docs/impl-foreign-keys.md
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
    // compatibility surface (docs/protocol.md §11) - one code wide.
    kFkViolation,
};

// True for a Status a caller may sensibly re-issue the same statement for.
// Spelled out here rather than at each call site so the wire layer's
// `retryable` bit and the engine's notion of retryable cannot drift.
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
