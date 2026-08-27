#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "kds/base/int128.hpp"
#include "kds/base/status.hpp"

// The text forms of `DATE`, `TIMESTAMP` and `DECIMAL(p,s)`
// (docs/spec/types.md TY3, TY7; workplan TY01).
//
// ---- One parser per type, two callers, zero drift ----------------------
//
// Each of these is called from **both** `EncodeOneValue` - storing a value -
// and the step compiler - coercing a literal in a predicate against a typed
// column (spec §3.1). That is the whole reason they are free functions in a
// header of their own rather than arms of the encoder: a predicate that
// accepted a literal the encoder would reject, or read it differently,
// would make `WHERE d = '2026-02-30'` and `INSERT ... '2026-02-30'`
// disagree about what the database contains.
//
// ---- Validation happens here and nowhere else --------------------------
//
// TY7: encode is the only gate. These functions do the whole check - shape,
// field ranges, calendar validity, and the type's own range - so a stored
// value is proven at the boundary and **decode never re-validates**. That
// is the principle the codec already runs on (`row_codec.cpp`), and it is
// what keeps the read path free of work the write path already did.
//
// ---- Values are integers, and stay integers ---------------------------
//
// TY5: a date *is* an integer - days since the epoch - and a timestamp is
// microseconds since it, UTC. Only the rendering differs, and rendering
// happens at the emission boundary. Nothing here produces a string, and
// nothing downstream needs one to compare, group, range-scan or fold these
// types: they take the int arm everywhere.
//
// Errors carry no byte position. The caller has it - a literal's offset in
// the statement, or the column being written - and adds it with
// `Status::WithContext`, which is how the positioned messages spec §2
// promises are built without every parser taking an offset it cannot check.

namespace kds::exec {

// ---- Ranges (spec §6.1, `[PROPOSED]`) ----------------------------------
//
// Wide enough for every workload that asked, narrow enough that a
// mistyped year is an error rather than a stored surprise. Both edges are
// pinned by the round-trip corpus, so moving them is a visible change.
inline constexpr std::int32_t kMinEpochDay = -25567;   // 1900-01-01
inline constexpr std::int32_t kMaxEpochDay = 376199;   // 2999-12-31

// The same window in microseconds, which is what makes a TIMESTAMP's range
// a restatement of a DATE's rather than a second decision.
inline constexpr std::int64_t kMinEpochMicros =
    static_cast<std::int64_t>(kMinEpochDay) * 86'400 * 1'000'000;
inline constexpr std::int64_t kMaxEpochMicros =
    (static_cast<std::int64_t>(kMaxEpochDay) + 1) * 86'400 * 1'000'000 - 1;

// DECIMAL's bounds (TY2). `p > 18` is a *separate type* carrying an
// int128 - a different schema constant, so the two coexist - and never a
// widening of this one. Built 2026-08-07: `decimal(p, s)` with `p >= 19`
// selects `kTypeValDecimalWide` at the one DDL site, and the wide bounds
// below are exclusive of the narrow ones on purpose - one declaration
// selects exactly one type, so `decimal128(10, 2)` is refused toward
// `decimal(10, 2)` rather than stored twice as wide.
inline constexpr std::uint8_t kMinDecimalPrecision = 1;
inline constexpr std::uint8_t kMaxDecimalPrecision = 18;
inline constexpr std::uint8_t kMinDecimalPrecisionWide = 19;
// 38, because 10^38 - 1 < 2^127: every unscaled value of a p <= 38 column
// fits the int128 with its sign, and 39 digits would not.
inline constexpr std::uint8_t kMaxDecimalPrecisionWide = 38;

// `YYYY-MM-DD` -> days since 1970-01-01.
//
// Strict: exactly ten characters, four-digit year, zero-padded month and
// day, and a real calendar date - `'2026-02-30'` is an error, not February
// 30th silently becoming March 2nd.
StatusOr<std::int32_t> ParseDateLiteral(std::string_view text);

// `YYYY-MM-DD HH:MM:SS[.ffffff]` -> microseconds since 1970-01-01 UTC.
//
// **Always UTC.** There is no session time zone and no conversion (TY4);
// what the stored instant means in a local calendar is the client's act.
// The fractional part is optional and may carry one to six digits; more is
// an error rather than a silent truncation, for the reason a decimal's
// extra digits are.
StatusOr<std::int64_t> ParseTimestampLiteral(std::string_view text);

// A decimal string -> the unscaled integer at `scale`.
//
// `'12.34'` at scale 2 is 1234; `'12.3'` at scale 2 is 1230, because the
// scale is part of the value's meaning and a shorter literal is exact.
// `'12.345'` at scale 2 is an **error**: rounding a literal to make it fit
// is a silent wrong answer about someone's money (TY6).
//
// `precision` bounds the total significant digits, so a value too large for
// the declared column is refused here rather than stored and found later.
StatusOr<std::int64_t> ParseDecimalLiteral(std::string_view text, std::uint8_t precision,
                                           std::uint8_t scale);

// Checks a declared `DECIMAL(p, s)` against TY2's bounds. Split out because
// the DDL checks it once per column and the literal parser assumes it.
Status CheckDecimalPrecisionScale(std::uint32_t precision, std::uint32_t scale);

// The wide siblings (TY2's separate type). One parser body serves both
// widths - the digit walk is shared and only the accumulator and the cap
// differ - which is TY01's one-parser rule holding across the width split:
// a literal that stores and a literal that compares cannot drift, and
// neither can the two widths' ideas of what a decimal string is.
Status CheckDecimalWidePrecisionScale(std::uint32_t precision, std::uint32_t scale);
StatusOr<Int128> ParseDecimalLiteralWide(std::string_view text, std::uint8_t precision,
                                         std::uint8_t scale);

// ---- Rendering (spec §3.3) ---------------------------------------------
//
// The inverse of the three parsers, exact for every value they accept -
// which is what makes the round-trip corpus a corpus rather than a sample.
std::string FormatDate(std::int32_t epoch_day);
std::string FormatTimestamp(std::int64_t epoch_micros);
std::string FormatDecimal(std::int64_t unscaled, std::uint8_t scale);
std::string FormatDecimalWide(Int128 unscaled, std::uint8_t scale);

}  // namespace kds::exec
