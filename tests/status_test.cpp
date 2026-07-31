#include "kds/base/status.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

// The Status type is used by nearly every function in the engine and had no
// test of its own. It gets one now because `retryable()` is the first thing
// Status has ever asserted about a code *class* rather than a single value,
// and docs/protocol.md §11 makes that bit part of the wire compatibility
// surface - "financial client libraries build retry loops on this bit".

namespace kds {
namespace {

// Every code except kOk, so a code added without a factory or without a
// retryability decision is noticed here rather than in the wire layer.
const std::vector<StatusCode>& AllErrorCodes() {
    static const std::vector<StatusCode> codes = {
        StatusCode::kInvalidArgument, StatusCode::kOutOfSpace,  StatusCode::kNotFound,
        StatusCode::kAlreadyExists,   StatusCode::kOutOfRange,  StatusCode::kCorruption,
        StatusCode::kIoError,         StatusCode::kTxnConflict,
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
    // "ERR TXN_CONFLICT retryable=1 ..." (docs/txn.md §5), so it must not
    // be dropped.
    const Status s = Status::TxnConflict("row id=42 was written by transaction 118");
    EXPECT_EQ(s.code(), StatusCode::kTxnConflict);
    EXPECT_TRUE(s.retryable());
    EXPECT_NE(s.message().find("id=42"), std::string::npos);
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
