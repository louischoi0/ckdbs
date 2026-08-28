#pragma once

#include <cstddef>
#include <string_view>

// The one UTF-8-safe truncation, hoisted at R6-3 when it gained its second
// caller - `statement_ship_service.cpp` wrote it for the shipped reply's
// diagnostic message, and `txn_2pc_service.cpp`'s participant reply needs
// exactly the same cut. R6-1's header named this hoist and its condition
// ("better hoisted once R6-3 is its second caller than duplicated now").
//
// Why it is not a plain `substr`: the newline protocol's strings are UTF-8
// (`docs/spec/protocol.md` §2) and at least one client decodes them strictly
// (`tools/multicore_benchmark.py`, the driver this version is measured
// with), so a message cut through a multi-byte character - engine messages
// carry '§' routinely - would reach that client as a decode error rather
// than as the shortened diagnostic it is meant to be.

namespace kds::server {

// The longest prefix of `text` that fits in `cap` bytes and does not end
// inside a UTF-8 sequence.
inline std::size_t Utf8PrefixLen(std::string_view text, std::size_t cap) noexcept {
    if (text.size() <= cap) return text.size();
    // `cap` is the first *dropped* byte. While that byte is a continuation
    // (10xxxxxx) the cut lands mid-character, so step back until the byte
    // after the cut starts one.
    std::size_t len = cap;
    while (len > 0 && (static_cast<unsigned char>(text[len]) & 0xC0) == 0x80) --len;
    return len;
}

}  // namespace kds::server
