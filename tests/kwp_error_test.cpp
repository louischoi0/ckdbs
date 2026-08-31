#include <array>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "kds/base/status.hpp"
#include "kds/wire/error_registry.hpp"
#include "kds/wire/kwp.hpp"

// The error registry (docs/spec/protocol.md §11, protocol-wp.md P12).
//
// **This file is a compatibility guard, not a unit test.** §11 makes the
// numbering and the `retryable` bit part of the surface a client library
// compiles against, so the assertions below are deliberately written as
// literals: a change that renumbers a category has to edit this file, and
// editing this file is the moment someone has to decide whether every
// deployed client is going to be renumbered with it.

namespace kds::wire {
namespace {

// ---- The golden list -----------------------------------------------------
//
// Every category, its number and its name, written out. Not derived from
// the enum - deriving it would make the guard agree with whatever the enum
// says, which is the one thing it must not do.
struct GoldenCategory {
    ErrorCategory category;
    std::uint16_t value;
    std::string_view name;
};

constexpr GoldenCategory kGolden[] = {
    {ErrorCategory::kInvalidArgument, 0, "INVALID_ARGUMENT"},
    {ErrorCategory::kNotFound, 1, "NOT_FOUND"},
    {ErrorCategory::kAlreadyExists, 2, "ALREADY_EXISTS"},
    {ErrorCategory::kOutOfSpace, 3, "OUT_OF_SPACE"},
    {ErrorCategory::kOutOfRange, 4, "OUT_OF_RANGE"},
    {ErrorCategory::kCorruption, 5, "CORRUPTION"},
    {ErrorCategory::kIoError, 6, "IO_ERROR"},
    {ErrorCategory::kInternal, 7, "INTERNAL"},
    {ErrorCategory::kProtocol, 8, "PROTOCOL"},
    {ErrorCategory::kUnsupported, 9, "UNSUPPORTED"},
    {ErrorCategory::kTxnConflict, 10, "TXN_CONFLICT"},
    {ErrorCategory::kCancelled, 11, "CANCELLED"},
    {ErrorCategory::kCardinalityViolation, 12, "CARDINALITY_VIOLATION"},
    {ErrorCategory::kResourceExhausted, 13, "RESOURCE_EXHAUSTED"},
    {ErrorCategory::kUnknownOutcome, 14, "UNKNOWN_OUTCOME"},
    {ErrorCategory::kFkViolation, 15, "FK_VIOLATION"},
    {ErrorCategory::kAssertionViolation, 16, "ASSERTION_VIOLATION"},
};

TEST(KwpErrorRegistryTest, TheCategoryNumberingIsFrozen) {
    for (const GoldenCategory& g : kGolden) {
        EXPECT_EQ(static_cast<std::uint16_t>(g.category), g.value)
            << g.name << " moved; the numbering is a compatibility surface (§11), so a new "
                         "category is appended and an existing one never renumbered";
        EXPECT_EQ(ErrorCategoryName(g.category), g.name);
    }
}

TEST(KwpErrorRegistryTest, EveryStatusCodeMapsToACategory) {
    // Written out rather than iterated, for the golden list's reason: a
    // code appended to status.hpp must fail here until someone decides
    // which category it earns. `kOk` is included because the mapping has
    // to answer for it - not because it is an error.
    struct Case {
        StatusCode code;
        ErrorCategory expect;
    };
    constexpr Case kCases[] = {
        {StatusCode::kOk, ErrorCategory::kInternal},
        {StatusCode::kInvalidArgument, ErrorCategory::kInvalidArgument},
        {StatusCode::kOutOfSpace, ErrorCategory::kOutOfSpace},
        {StatusCode::kNotFound, ErrorCategory::kNotFound},
        {StatusCode::kAlreadyExists, ErrorCategory::kAlreadyExists},
        {StatusCode::kOutOfRange, ErrorCategory::kOutOfRange},
        {StatusCode::kCorruption, ErrorCategory::kCorruption},
        {StatusCode::kIoError, ErrorCategory::kIoError},
        {StatusCode::kTxnConflict, ErrorCategory::kTxnConflict},
        {StatusCode::kUnsupported, ErrorCategory::kUnsupported},
        {StatusCode::kCardinalityViolation, ErrorCategory::kCardinalityViolation},
        {StatusCode::kResourceExhausted, ErrorCategory::kResourceExhausted},
        {StatusCode::kFkViolation, ErrorCategory::kFkViolation},
        {StatusCode::kAssertionViolation, ErrorCategory::kAssertionViolation},
        {StatusCode::kUnknownOutcome, ErrorCategory::kUnknownOutcome},
    };
    for (const Case& c : kCases) {
        EXPECT_EQ(CategoryOf(c.code), c.expect)
            << "StatusCode " << static_cast<int>(c.code) << " changed category";
    }
    // And nothing folds: no two engine codes share a category, which is R2
    // stated as a property rather than as advice. `kOk` is skipped - it is
    // not an engine failure and shares `kInternal` deliberately.
    for (const Case& a : kCases) {
        if (a.code == StatusCode::kOk) continue;
        for (const Case& b : kCases) {
            if (b.code == StatusCode::kOk || a.code == b.code) continue;
            EXPECT_NE(a.expect, b.expect)
                << "two engine codes now share one wire category; a client can no longer tell "
                   "them apart, and the message is not a surface";
        }
    }
}

TEST(KwpErrorRegistryTest, RetryableIsTheEnginesOwnAnswerAndOnlyOneCodeCarriesIt) {
    const WireError conflict = ErrorFromStatus(Status::TxnConflict("row 42 is held"));
    EXPECT_TRUE(conflict.retryable) << "§11 names TxnConflict = 1 explicitly";
    EXPECT_EQ(conflict.category(), ErrorCategory::kTxnConflict);
    EXPECT_EQ(conflict.severity, Severity::kError) << "an engine failure never closes a session";

    // The three that a client might *expect* to be retryable and must not
    // be: each one's reason is on its StatusCode in status.hpp.
    EXPECT_FALSE(ErrorFromStatus(Status::InvalidArgument("no")).retryable);
    EXPECT_FALSE(ErrorFromStatus(Status::ResourceExhausted("budget spent")).retryable);
    EXPECT_FALSE(ErrorFromStatus(Status::UnknownOutcome("no reply")).retryable)
        << "the one refusal that does not mean 'nothing happened'; a retry would insert twice";
}

TEST(KwpErrorRegistryTest, ACodePacksItsCategoryAndDetail) {
    const WireError e = ProtocolError(ProtocolDetail::kUnsupportedVersion, "no common version",
                                      Severity::kFatal);
    EXPECT_EQ(e.code, MakeErrorCode(ErrorCategory::kProtocol, 4));
    EXPECT_EQ(e.category(), ErrorCategory::kProtocol);
    EXPECT_EQ(e.detail_code(), 4);
    EXPECT_FALSE(e.retryable) << "a retry loop on a protocol defect is an infinite one";
    EXPECT_EQ(e.severity, Severity::kFatal);

    // A category with no detail is the common case and is not the same
    // number as detail 1.
    const WireError plain = ErrorFromStatus(Status::NotFound("no such relation"));
    EXPECT_EQ(plain.detail_code(), kNoDetail);
    EXPECT_NE(plain.code, MakeErrorCode(ErrorCategory::kNotFound, 1));
}

// ---- The payload ---------------------------------------------------------

TEST(KwpErrorRegistryTest, ThePayloadRoundTripsWithAndWithoutItsOptionalHalves) {
    WireError full = ErrorFromStatus(Status::InvalidArgument("bad token"));
    full.detail = "expected an identifier";
    full.position = 0;  // byte 0, the case a zero-means-absent encoding loses

    const auto bytes = EncodeError(full);
    auto back = DecodeError(bytes);
    ASSERT_TRUE(back.ok()) << back.status().message();
    EXPECT_EQ(back.value().code, full.code);
    EXPECT_EQ(back.value().retryable, full.retryable);
    EXPECT_EQ(back.value().severity, full.severity);
    EXPECT_EQ(back.value().message, full.message);
    ASSERT_TRUE(back.value().detail.has_value());
    EXPECT_EQ(*back.value().detail, "expected an identifier");
    ASSERT_TRUE(back.value().position.has_value());
    EXPECT_EQ(*back.value().position, 0u) << "position 0 must not decode as absent";

    const WireError bare = ErrorFromStatus(Status::NotFound("gone"));
    auto bare_back = DecodeError(EncodeError(bare));
    ASSERT_TRUE(bare_back.ok());
    EXPECT_FALSE(bare_back.value().detail.has_value());
    EXPECT_FALSE(bare_back.value().position.has_value());
    EXPECT_EQ(bare_back.value().message, "gone");

    // Present-and-empty is not absent, which is the whole reason the
    // encoding spends a sentinel rather than a length of zero.
    WireError empty_detail = bare;
    empty_detail.detail = std::string();
    auto empty_back = DecodeError(EncodeError(empty_detail));
    ASSERT_TRUE(empty_back.ok());
    ASSERT_TRUE(empty_back.value().detail.has_value());
    EXPECT_TRUE(empty_back.value().detail->empty());
}

TEST(KwpErrorRegistryTest, ATruncatedPayloadIsRefusedAtEveryLength) {
    WireError e = ErrorFromStatus(Status::Corruption("torn page"));
    e.detail = "page 7";
    e.position = 12;
    const auto bytes = EncodeError(e);
    ASSERT_TRUE(DecodeError(bytes).ok());
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        auto partial = DecodeError(std::span<const std::byte>(bytes.data(), n));
        EXPECT_FALSE(partial.ok()) << "a payload one byte short decoded as a whole error at n="
                                   << n;
    }
}

TEST(KwpErrorRegistryTest, AnUnknownSeverityDecodesFatal) {
    WireError e = ErrorFromStatus(Status::IoError("device"));
    auto bytes = EncodeError(e);
    // The severity byte sits at offset 5: code u32, retryable u8.
    bytes[5] = static_cast<std::byte>(200);
    auto back = DecodeError(bytes);
    ASSERT_TRUE(back.ok());
    EXPECT_EQ(back.value().severity, Severity::kFatal)
        << "a client that cannot tell whether the connection survives must assume it did not";
}

}  // namespace
}  // namespace kds::wire
