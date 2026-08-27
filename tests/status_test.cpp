#include "kds/base/status.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

// The Status type is used by nearly every function in the engine and had no
// test of its own. It gets one now because `retryable()` is the first thing
// Status has ever asserted about a code *class* rather than a single value,
// and docs/spec/protocol.md §11 makes that bit part of the wire compatibility
// surface - "financial client libraries build retry loops on this bit".

namespace kds {
namespace {

// Every code except kOk, so a code added without a factory or without a
// retryability decision is noticed here rather than in the wire layer.
const std::vector<StatusCode>& AllErrorCodes() {
    static const std::vector<StatusCode> codes = {
        StatusCode::kInvalidArgument, StatusCode::kOutOfSpace,  StatusCode::kNotFound,
        StatusCode::kAlreadyExists,   StatusCode::kOutOfRange,  StatusCode::kCorruption,
        StatusCode::kIoError,         StatusCode::kTxnConflict, StatusCode::kUnsupported,
        StatusCode::kCardinalityViolation, StatusCode::kResourceExhausted,
        StatusCode::kFkViolation,          StatusCode::kAssertionViolation,
        StatusCode::kUnknownOutcome,
    };
    return codes;
}

Status Make(StatusCode code) {
    switch (code) {
        case StatusCode::kInvalidArgument: return Status::InvalidArgument("m");
        case StatusCode::kOutOfSpace:      return Status::OutOfSpace("m");
        case StatusCode::kNotFound:        return Status::NotFound("m");
        case StatusCode::kAlreadyExists:   return Status::AlreadyExists("m");
        case StatusCode::kOutOfRange:      return Status::OutOfRange("m");
        case StatusCode::kCorruption:      return Status::Corruption("m");
        case StatusCode::kIoError:         return Status::IoError("m");
        case StatusCode::kTxnConflict:     return Status::TxnConflict("m");
        case StatusCode::kUnsupported:     return Status::Unsupported("m");
        case StatusCode::kCardinalityViolation:
            return Status::CardinalityViolation("m");
        case StatusCode::kResourceExhausted:
            return Status::ResourceExhausted("m");
        case StatusCode::kFkViolation:     return Status::FkViolation("m");
        case StatusCode::kAssertionViolation:
            return Status::AssertionViolation("m");
        case StatusCode::kUnknownOutcome:
            return Status::UnknownOutcome("m");
        case StatusCode::kOk:              return Status::OK();
    }
    // Unreachable for a code in AllErrorCodes(); a new enumerator lands
    // here and fails the test below rather than silently defaulting.
    return Status::OK();
}

TEST(StatusTest, OkCarriesNoErrorAndIsNotRetryable) {
    const Status s = Status::OK();
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kOk);
    EXPECT_TRUE(s.message().empty());
    EXPECT_FALSE(s.retryable());
}

TEST(StatusTest, EveryErrorCodeHasAFactoryThatSetsItAndTheMessage) {
    // The guard the enum needs: a code appended without a factory shows up
    // as Make() returning OK for a code we asked to be an error.
    for (const StatusCode code : AllErrorCodes()) {
        const Status s = Make(code);
        EXPECT_FALSE(s.ok()) << "no factory for code " << static_cast<int>(code);
        EXPECT_EQ(s.code(), code);
        EXPECT_EQ(s.message(), "m");
    }
}

TEST(StatusTest, TxnConflictIsTheOnlyRetryableCode) {
    // Not "kTxnConflict is retryable" but "and nothing else is" - the
    // second half is what stops a future code from quietly joining the
    // retryable set and changing client retry behaviour.
    for (const StatusCode code : AllErrorCodes()) {
        const bool expected = (code == StatusCode::kTxnConflict);
        EXPECT_EQ(IsRetryable(code), expected) << "code " << static_cast<int>(code);
        EXPECT_EQ(Make(code).retryable(), expected) << "code " << static_cast<int>(code);
    }
}

TEST(StatusTest, TxnConflictCarriesItsReason) {
    // The message is what the newline protocol turns into
    // "ERR TXN_CONFLICT retryable=1 ..." (docs/spec/txn.md §5), so it must not
    // be dropped.
    const Status s = Status::TxnConflict("row id=42 was written by transaction 118");
    EXPECT_EQ(s.code(), StatusCode::kTxnConflict);
    EXPECT_TRUE(s.retryable());
    EXPECT_NE(s.message().find("id=42"), std::string::npos);
}

TEST(StatusTest, UnsupportedIsDistinctFromInvalidArgumentAndNotRetryable) {
    // docs/spec/parser-v2.md J2: a reserved-but-inexecutable form is well-formed
    // input the engine declines, not malformed input. Folding it into
    // kInvalidArgument would tell a client to go looking for a typo, and
    // retrying it would fail identically forever.
    const Status s = Status::Unsupported("derived tables are not supported at position 21");
    EXPECT_EQ(s.code(), StatusCode::kUnsupported);
    EXPECT_NE(s.code(), StatusCode::kInvalidArgument);
    EXPECT_FALSE(s.retryable());
    EXPECT_NE(s.message().find("position 21"), std::string::npos);
}

TEST(StatusTest, CardinalityViolationIsNotRetryable) {
    // docs/spec/parser-v2.md §2: a scalar subquery that returned more than one
    // row. Re-running it against unchanged data returns the same extra
    // rows, so retryable = 0 - even though, unlike every other code here,
    // it is a *runtime* verdict that a later data change could lift.
    const Status s = Status::CardinalityViolation("scalar subquery returned 3 rows");
    EXPECT_EQ(s.code(), StatusCode::kCardinalityViolation);
    EXPECT_FALSE(s.retryable());
    EXPECT_NE(s.message().find("3 rows"), std::string::npos);
}

TEST(StatusTest, ResourceExhaustedIsNotRetryable) {
    // A spent work budget (exec/budget.hpp). Re-running the statement does
    // the same work and stops in the same place, so retrying is pure
    // waste - the fix is a different statement, or a higher ceiling.
    const Status s = Status::ResourceExhausted("statement exceeded its row-touch budget of 100");
    EXPECT_EQ(s.code(), StatusCode::kResourceExhausted);
    EXPECT_FALSE(s.retryable());
    EXPECT_NE(s.message().find("row-touch budget"), std::string::npos);
}

TEST(StatusTest, AssertionViolationIsNotRetryable) {
    // docs/spec/assertion.md §4.4. The one race a retry could win - a
    // refusal caused by a reservation that later aborts - is §4.3's bounded
    // false rejection, accepted rather than encoded: granting the bit would
    // make every client spin on a group that is genuinely full. The enum
    // comment carries the full argument; this pins its conclusion.
    const Status s = Status::AssertionViolation(
        "assertion \"limit\" group (id=1): COUNT(*) would exceed bound 5");
    EXPECT_EQ(s.code(), StatusCode::kAssertionViolation);
    EXPECT_FALSE(s.retryable());
    EXPECT_NE(s.message().find("would exceed bound 5"), std::string::npos);
}

TEST(StatusOrTest, HoldsAValueOrAStatusNeverBoth) {
    StatusOr<int> ok(7);
    EXPECT_TRUE(ok.ok());
    EXPECT_EQ(ok.value(), 7);
    EXPECT_TRUE(ok.status().ok());

    StatusOr<int> bad(Status::TxnConflict("lost the race"));
    EXPECT_FALSE(bad.ok());
    EXPECT_EQ(bad.status().code(), StatusCode::kTxnConflict);
    EXPECT_TRUE(bad.status().retryable());
}

}  // namespace
}  // namespace kds
