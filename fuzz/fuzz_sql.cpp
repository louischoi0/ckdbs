// BS-S5 target 1: SQL text (instructions/v3.0.0/workorder-bs-test-hardening.md).
//
// Every input through the lexer, both grammars and the fingerprint, then
// dispatched on a fresh instance (fuzz_instance.hpp) - the commands the
// dispatcher reads by hand included. The obligation is
// parser_fuzz_test.cpp's, widened to execution: a `Status` or rows for every
// input, never a crash, a hang or an out-of-bounds read; and the two
// fingerprint paths agree on what lexes.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "fuzz_instance.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/parser/parser.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view sql(reinterpret_cast<const char*>(data), size);
    {
        kds::parser::Parser parser(sql);
        const auto parsed = parser.Parse();
        // The parse-time and standalone fingerprints never disagree
        // (parser_fuzz_test.cpp's property, over every input reached here).
        if (parsed.ok()) {
            const auto during = parser.fingerprint();
            const auto apart = kds::parser::FingerprintOf(sql);
            if (during.has_value() != apart.has_value()) __builtin_trap();
            if (during.has_value() &&
                (during->pattern_id != apart->pattern_id || during->arg_hash != apart->arg_hash ||
                 during->literal_count != apart->literal_count ||
                 during->param_count != apart->param_count)) {
                __builtin_trap();
            }
        }
    }
    {
        kds::parser::Parser parser(sql);
        (void)parser.ParseQuery();
    }
    // Every input, accepted by a grammar or not: the commands the
    // dispatcher reads itself (SHOW, BEGIN, SET, SYNC, ...) are past both.
    {
        kds::fuzz::Instance instance;
        (void)instance.Run(sql);
        // And the instance serves after whatever the input did to it.
        (void)instance.Run("SELECT * FROM t");
    }
    return 0;
}
