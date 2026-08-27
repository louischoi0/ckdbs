#include "kds/exec/type_literals.hpp"

#include <array>
#include <cstdlib>

// See type_literals.hpp for why these are free functions and why they are
// the only gate.

namespace kds::exec {

namespace {

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// Reads exactly `count` digits, rejecting anything else. Zero-padding is
// required rather than tolerated: accepting `2026-2-3` would make the
// literal's width depend on the value, and a fixed shape is what lets the
// whole parse be a sequence of position checks.
bool ReadFixedDigits(std::string_view text, std::size_t at, std::size_t count, int& out) {
    if (at + count > text.size()) return false;
    int value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const char c = text[at + i];
        if (!IsDigit(c)) return false;
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

bool IsLeapYear(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int DaysInMonth(int y, int m) {
    static constexpr std::array<int, 12> kDays{31, 28, 31, 30, 31, 30,
                                               31, 31, 30, 31, 30, 31};
    if (m == 2 && IsLeapYear(y)) return 29;
    return kDays[static_cast<std::size_t>(m - 1)];
}

// Days from 1970-01-01 to y-m-d, proleptic Gregorian. Howard Hinnant's
// `days_from_civil`: exact for the whole range this type admits, and
// branch-free apart from the era split.
//
// Written out rather than taken from <chrono>'s calendar types, which are
// C++20 but arrive with a locale and a formatting machinery this engine has
// no use for - and which would put a per-value allocation on a path that
// must not have one.
std::int32_t DaysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int32_t>(era * 146097 + static_cast<int>(doe) - 719468);
}

// The inverse, for rendering.
void CivilFromDays(std::int32_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int yr = static_cast<int>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y = yr + (m <= 2);
}

Status BadLiteral(std::string_view text, std::string_view want) {
    return Status::InvalidArgument("'" + std::string(text) + "' is not a valid " +
                                    std::string(want));
}

std::string Pad(int value, int width) {
    std::string digits = std::to_string(value);
    if (static_cast<int>(digits.size()) >= width) return digits;
    return std::string(static_cast<std::size_t>(width) - digits.size(), '0') + digits;
}

// The date half of a timestamp, shared so the two parsers cannot disagree
// about what a calendar date is.
Status ParseCivilDate(std::string_view text, std::int32_t& out) {
    int y = 0, m = 0, d = 0;
    if (text.size() < 10 || text[4] != '-' || text[7] != '-' ||
        !ReadFixedDigits(text, 0, 4, y) || !ReadFixedDigits(text, 5, 2, m) ||
        !ReadFixedDigits(text, 8, 2, d)) {
        return BadLiteral(text, "date (expected YYYY-MM-DD)");
    }
    if (m < 1 || m > 12) {
        return Status::InvalidArgument("month " + std::to_string(m) + " in '" +
                                        std::string(text) + "' is out of range 1..12");
    }
    if (d < 1 || d > DaysInMonth(y, m)) {
        return Status::InvalidArgument(
            "day " + std::to_string(d) + " in '" + std::string(text) +
            "' is out of range for that month (" + std::to_string(DaysInMonth(y, m)) +
            " days)");
    }
    out = DaysFromCivil(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
    return Status::OK();
}

}  // namespace

StatusOr<std::int32_t> ParseDateLiteral(std::string_view text) {
    if (text.size() != 10) {
        return BadLiteral(text, "date (expected exactly YYYY-MM-DD)");
    }
    std::int32_t day = 0;
    if (Status s = ParseCivilDate(text, day); !s.ok()) return s;
    if (day < kMinEpochDay || day > kMaxEpochDay) {
        return Status::OutOfRange("date '" + std::string(text) +
                                   "' is outside the supported range 1900-01-01..2999-12-31");
    }
    return day;
}

StatusOr<std::int64_t> ParseTimestampLiteral(std::string_view text) {
    std::int32_t day = 0;
    if (Status s = ParseCivilDate(text, day); !s.ok()) return s;

    // `YYYY-MM-DD HH:MM:SS` is nineteen characters; anything shorter is not
    // a timestamp. A date alone is deliberately *not* accepted and promoted
    // to midnight: the two types are different columns, and silently
    // widening one literal into the other is how a schema change stops
    // being visible.
    if (text.size() < 19 || (text[10] != ' ' && text[10] != 'T') || text[13] != ':' ||
        text[16] != ':') {
        return BadLiteral(text, "timestamp (expected YYYY-MM-DD HH:MM:SS[.ffffff])");
    }
    int hh = 0, mm = 0, ss = 0;
    if (!ReadFixedDigits(text, 11, 2, hh) || !ReadFixedDigits(text, 14, 2, mm) ||
        !ReadFixedDigits(text, 17, 2, ss)) {
        return BadLiteral(text, "timestamp (expected YYYY-MM-DD HH:MM:SS[.ffffff])");
    }
    // 24:00:00 is not accepted, and no leap second is: both are spellings
    // of an instant that already has one, and admitting a second spelling
    // means two stored values that render alike.
    if (hh > 23 || mm > 59 || ss > 59) {
        return Status::InvalidArgument("time in '" + std::string(text) +
                                        "' is out of range (00:00:00..23:59:59)");
    }

    std::int64_t micros = 0;
    if (text.size() > 19) {
        if (text[19] != '.') {
            return BadLiteral(text, "timestamp (expected '.' before fractional seconds)");
        }
        const std::string_view frac = text.substr(20);
        if (frac.empty() || frac.size() > 6) {
            return Status::InvalidArgument(
                "'" + std::string(text) +
                "' has " + std::to_string(frac.size()) +
                " fractional digits; a timestamp carries one to six (microseconds)");
        }
        for (char c : frac) {
            if (!IsDigit(c)) return BadLiteral(text, "timestamp (fractional part)");
            micros = micros * 10 + (c - '0');
        }
        // `.5` is half a second, not five microseconds - the fraction is
        // positional, so a short one scales up.
        for (std::size_t i = frac.size(); i < 6; ++i) micros *= 10;
    }

    const std::int64_t seconds =
        static_cast<std::int64_t>(day) * 86'400 + hh * 3'600 + mm * 60 + ss;
    const std::int64_t value = seconds * 1'000'000 + micros;
    if (value < kMinEpochMicros || value > kMaxEpochMicros) {
        return Status::OutOfRange("timestamp '" + std::string(text) +
                                   "' is outside the supported range");
    }
    return value;
}

Status CheckDecimalPrecisionScale(std::uint32_t precision, std::uint32_t scale) {
    if (precision < kMinDecimalPrecision || precision > kMaxDecimalPrecision) {
        return Status::InvalidArgument(
            "decimal precision " + std::to_string(precision) + " is outside 1.." +
            std::to_string(kMaxDecimalPrecision) +
            "; a wider decimal needs an int128 representation, which is a separate type "
            "rather than a widening of this one (docs/spec/types.md TY2)");
    }
    if (scale > precision) {
        return Status::InvalidArgument("decimal scale " + std::to_string(scale) +
                                        " exceeds its precision " + std::to_string(precision));
    }
    return Status::OK();
}

Status CheckDecimalWidePrecisionScale(std::uint32_t precision, std::uint32_t scale) {
    // Exclusive of the narrow bounds on purpose: one declaration selects
    // exactly one type, so a `decimal128(10, 2)` is refused toward the
    // 8-byte type rather than stored twice as wide.
    if (precision < kMinDecimalPrecisionWide || precision > kMaxDecimalPrecisionWide) {
        return Status::InvalidArgument(
            "wide decimal precision " + std::to_string(precision) + " is outside " +
            std::to_string(kMinDecimalPrecisionWide) + ".." +
            std::to_string(kMaxDecimalPrecisionWide) + "; up to " +
            std::to_string(kMaxDecimalPrecision) +
            " digits is the 8-byte decimal(p, s), and beyond " +
            std::to_string(kMaxDecimalPrecisionWide) + " digits exceeds an int128");
    }
    if (scale > precision) {
        return Status::InvalidArgument("decimal scale " + std::to_string(scale) +
                                        " exceeds its precision " + std::to_string(precision));
    }
    return Status::OK();
}

namespace {

// The one digit walk, shared by both widths (TY01's one-parser rule
// holding across the width split). `Acc` is the accumulator - int64 for
// the 8-byte type, Int128 for the 16-byte one - and `digit_cap` the
// representation's own ceiling, which backstops the column's `precision`
// so the multiply below can never overflow: an Acc that holds 10^cap - 1
// gains one decimal digit at a time and is range-checked before each.
template <typename Acc>
StatusOr<Acc> ParseDecimalInto(std::string_view text, std::uint8_t precision,
                               std::uint8_t scale, std::uint8_t digit_cap) {
    if (text.empty()) return BadLiteral(text, "decimal");

    std::size_t at = 0;
    bool negative = false;
    if (text[at] == '+' || text[at] == '-') {
        negative = text[at] == '-';
        ++at;
    }

    std::string_view whole = text.substr(at);
    std::string_view frac;
    if (const std::size_t dot = whole.find('.'); dot != std::string_view::npos) {
        frac = whole.substr(dot + 1);
        whole = whole.substr(0, dot);
    }
    if (whole.empty() && frac.empty()) return BadLiteral(text, "decimal");

    // **More fractional digits than the column's scale is an error.** The
    // alternative is rounding, and rounding a literal so it fits is a
    // silent wrong answer about a value someone wrote down exactly (TY6).
    if (frac.size() > scale) {
        return Status::InvalidArgument(
            "'" + std::string(text) + "' has " + std::to_string(frac.size()) +
            " fractional digits but the column's scale is " + std::to_string(scale) +
            "; this engine does not round a literal to make it fit");
    }

    Acc unscaled = 0;
    std::uint32_t digits = 0;
    const auto absorb = [&](std::string_view run) -> Status {
        for (char c : run) {
            if (!IsDigit(c)) return BadLiteral(text, "decimal");
            // Leading zeros are not significant, so `'0.05'` at scale 2 is
            // two digits and fits a decimal(2,2).
            if (digits != 0 || c != '0') ++digits;
            if (digits > digit_cap) {
                return Status::OutOfRange("'" + std::string(text) +
                                           "' has more significant digits than a decimal can "
                                           "hold (max " + std::to_string(digit_cap) + ")");
            }
            unscaled = unscaled * 10 + (c - '0');
        }
        return Status::OK();
    };
    if (Status s = absorb(whole); !s.ok()) return s;
    if (Status s = absorb(frac); !s.ok()) return s;

    // A literal shorter than the scale is exact and scales up: `'12.3'` at
    // scale 2 is 1230, because the scale is part of the value's meaning.
    for (std::size_t i = frac.size(); i < scale; ++i) {
        unscaled *= 10;
        ++digits;
    }
    if (digits > precision) {
        return Status::OutOfRange("'" + std::string(text) + "' needs " +
                                   std::to_string(digits) +
                                   " significant digits, more than the column's precision " +
                                   std::to_string(precision));
    }
    return negative ? -unscaled : unscaled;
}

}  // namespace

StatusOr<std::int64_t> ParseDecimalLiteral(std::string_view text, std::uint8_t precision,
                                            std::uint8_t scale) {
    if (Status s = CheckDecimalPrecisionScale(precision, scale); !s.ok()) return s;
    return ParseDecimalInto<std::int64_t>(text, precision, scale, kMaxDecimalPrecision);
}

StatusOr<Int128> ParseDecimalLiteralWide(std::string_view text, std::uint8_t precision,
                                         std::uint8_t scale) {
    if (Status s = CheckDecimalWidePrecisionScale(precision, scale); !s.ok()) return s;
    return ParseDecimalInto<Int128>(text, precision, scale, kMaxDecimalPrecisionWide);
}

std::string FormatDate(std::int32_t epoch_day) {
    int y = 0;
    unsigned m = 0, d = 0;
    CivilFromDays(epoch_day, y, m, d);
    return Pad(y, 4) + "-" + Pad(static_cast<int>(m), 2) + "-" + Pad(static_cast<int>(d), 2);
}

std::string FormatTimestamp(std::int64_t epoch_micros) {
    // Floor division, so an instant before the epoch renders as the day it
    // falls in rather than the one after it.
    std::int64_t day = epoch_micros / (86'400LL * 1'000'000);
    std::int64_t rest = epoch_micros % (86'400LL * 1'000'000);
    if (rest < 0) {
        rest += 86'400LL * 1'000'000;
        --day;
    }
    const std::int64_t micros = rest % 1'000'000;
    const std::int64_t seconds = rest / 1'000'000;

    std::string out = FormatDate(static_cast<std::int32_t>(day)) + " " +
                      Pad(static_cast<int>(seconds / 3600), 2) + ":" +
                      Pad(static_cast<int>((seconds / 60) % 60), 2) + ":" +
                      Pad(static_cast<int>(seconds % 60), 2);
    // Six digits when non-zero, none when zero (spec §3.3 `[PROPOSED]`).
    if (micros != 0) out += "." + Pad(static_cast<int>(micros), 6);
    return out;
}

std::string FormatDecimal(std::int64_t unscaled, std::uint8_t scale) {
    const bool negative = unscaled < 0;
    // Through unsigned, so INT64_MIN does not overflow its own negation.
    std::uint64_t magnitude = negative ? (~static_cast<std::uint64_t>(unscaled) + 1)
                                       : static_cast<std::uint64_t>(unscaled);
    std::string digits = std::to_string(magnitude);
    if (scale == 0) return negative ? "-" + digits : digits;

    if (digits.size() <= scale) {
        digits.insert(0, static_cast<std::size_t>(scale) + 1 - digits.size(), '0');
    }
    // **Exactly `scale` fractional digits, trailing zeros included.** `12.30`
    // and `12.3` are the same number and not the same value here: the scale
    // is part of what the column declared.
    digits.insert(digits.size() - scale, ".");
    return negative ? "-" + digits : digits;
}

std::string FormatDecimalWide(Int128 unscaled, std::uint8_t scale) {
    const bool negative = unscaled < 0;
    // Through unsigned, so the minimum value does not overflow its own
    // negation - the same trick the narrow formatter uses, one type wider.
    unsigned __int128 magnitude = negative
                                      ? (~static_cast<unsigned __int128>(unscaled) + 1)
                                      : static_cast<unsigned __int128>(unscaled);
    // std::to_string has no int128 overload, so the digits are peeled by
    // hand - at most 39 of them, and rendering runs once per emitted value
    // at the boundary, never per row scanned.
    std::string digits;
    do {
        digits.insert(digits.begin(), static_cast<char>('0' + static_cast<int>(magnitude % 10)));
        magnitude /= 10;
    } while (magnitude != 0);

    if (scale == 0) return negative ? "-" + digits : digits;
    if (digits.size() <= scale) {
        digits.insert(0, static_cast<std::size_t>(scale) + 1 - digits.size(), '0');
    }
    digits.insert(digits.size() - scale, ".");
    return negative ? "-" + digits : digits;
}

}  // namespace kds::exec
