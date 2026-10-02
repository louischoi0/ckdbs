#include "kds/exec/functions.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <cctype>
#include <cstdlib>
#include <vector>

#include "kds/catalog/well_known.hpp"

namespace kds::exec {

namespace {

// Microseconds in a day: 86,400 s x 1,000,000. TY4 stores a DATE as days and
// a TIMESTAMP as microseconds, both since 1970-01-01 UTC.
constexpr std::int64_t kMicrosPerDay = 86'400LL * 1'000'000LL;

parser::AstValue IntValue(std::int64_t v) {
    parser::AstValue out;
    out.type = parser::ValueType::kInt;
    out.int_val = v;
    return out;
}

// DATE(timestamp): the UTC day the instant falls on. **Floor**, not
// truncation: 1969-12-31 23:00 is day -1, and C++'s `/` would give 0.
parser::AstValue EvaluateDate(std::span<const parser::AstValue* const> args,
                              const StatementContext&) {
    const std::int64_t us = args[0]->int_val;
    std::int64_t days = us / kMicrosPerDay;
    if (us % kMicrosPerDay < 0) --days;
    return IntValue(days);
}

// NOW(): the statement's instant, the same for every call in it.
parser::AstValue EvaluateNow(std::span<const parser::AstValue* const>,
                             const StatementContext& context) {
    return IntValue(context.now_us);
}

constexpr std::array<FunctionEntry, 2> kBuiltins = {{
    {"date", 1, Purity::kImmutable, catalog::kTypeValTimestamp, catalog::kTypeValDate,
     &EvaluateDate},
    {"now", 0, Purity::kStable, 0, catalog::kTypeValTimestamp, &EvaluateNow},
}};

static_assert(std::all_of(kBuiltins.begin(), kBuiltins.end(),
                          [](const FunctionEntry& e) { return e.arity <= kMaxFunctionArgs; }));

// Registered by `ScopedTestFunction` only. Searched after the built-ins, so a
// test cannot shadow a shipped function.
std::vector<const FunctionEntry*>& TestEntries() {
    static std::vector<const FunctionEntry*> entries;
    return entries;
}

// ScopedStatementClockForTest's pin; empty means the wall clock.
std::optional<std::int64_t>& PinnedClock() {
    static std::optional<std::int64_t> pinned;
    return pinned;
}

bool IEquals(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

}  // namespace

DeterminismClass ClassOf(Purity purity) noexcept {
    switch (purity) {
        case Purity::kImmutable: return DeterminismClass::kD0;
        case Purity::kStable:
        case Purity::kVolatileStatement: return DeterminismClass::kD1;
        case Purity::kVolatileRow: return DeterminismClass::kD2;
    }
    return DeterminismClass::kD2;  // an unknown purity is the conservative end
}

const FunctionEntry* FindFunction(std::string_view name) noexcept {
    for (const FunctionEntry& entry : kBuiltins) {
        if (IEquals(entry.name, name)) return &entry;
    }
    for (const FunctionEntry* entry : TestEntries()) {
        if (IEquals(entry->name, name)) return entry;
    }
    return nullptr;
}

StatementContext StatementContextNow() {
    StatementContext out;
    if (PinnedClock().has_value()) {
        out.now_us = *PinnedClock();
        return out;
    }
    out.now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();
    return out;
}

ScopedStatementClockForTest::ScopedStatementClockForTest(std::int64_t now_us) {
    PinnedClock() = now_us;
}
ScopedStatementClockForTest::~ScopedStatementClockForTest() { PinnedClock().reset(); }
void ScopedStatementClockForTest::Set(std::int64_t now_us) { PinnedClock() = now_us; }

ScopedTestFunction::ScopedTestFunction(const FunctionEntry& entry) {
    // A test entry is held to the same bound the built-ins are asserted to.
    if (entry.arity > kMaxFunctionArgs) std::abort();
    TestEntries().push_back(&entry);
}

ScopedTestFunction::~ScopedTestFunction() { TestEntries().pop_back(); }

}  // namespace kds::exec
