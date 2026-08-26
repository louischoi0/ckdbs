#pragma once

// sim/reply.hpp — parsing the dispatcher's one-line wire replies
// (docs/spec/client-manual.md; the exact shapes are pinned by the dispatcher's
// own tests). A reply is one line; multi-row content is joined with the
// literal two-character escape `\n`, never a raw newline byte.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kds::sim {

// Header line first, then one entry per row. An empty result is the header
// alone.
inline std::vector<std::string> SplitEscapedLines(std::string_view reply) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at <= reply.size()) {
        const std::size_t next = reply.find("\\n", at);
        if (next == std::string_view::npos) {
            out.emplace_back(reply.substr(at));
            break;
        }
        out.emplace_back(reply.substr(at, next - at));
        at = next + 2;
    }
    return out;
}

inline bool IsErr(std::string_view reply) { return reply.rfind("ERR", 0) == 0; }

inline std::optional<std::uint64_t> ParseField(std::string_view reply, std::string_view key) {
    const std::size_t at = reply.find(key);
    if (at == std::string_view::npos) return std::nullopt;
    std::uint64_t value = 0;
    bool any = false;
    for (std::size_t i = at + key.size();
         i < reply.size() && reply[i] >= '0' && reply[i] <= '9'; ++i) {
        value = value * 10 + static_cast<std::uint64_t>(reply[i] - '0');
        any = true;
    }
    if (!any) return std::nullopt;
    return value;
}

// "INSERTED oid=<n> id=<n> page=<n> slot=<n>" -> the assigned id.
inline std::optional<std::uint64_t> ParseInsertedId(std::string_view reply) {
    if (reply.rfind("INSERTED ", 0) != 0) return std::nullopt;
    return ParseField(reply, " id=");
}

// The full placement an INSERTED reply reports — what the corruption tests
// use to find a tuple's bytes without re-deriving the engine's placement.
struct InsertedAt {
    std::uint64_t id = 0;
    std::uint32_t page = 0;
    std::uint16_t slot = 0;
};

inline std::optional<InsertedAt> ParseInserted(std::string_view reply) {
    if (reply.rfind("INSERTED ", 0) != 0) return std::nullopt;
    const auto id = ParseField(reply, " id=");
    const auto page = ParseField(reply, " page=");
    const auto slot = ParseField(reply, " slot=");
    if (!id || !page || !slot) return std::nullopt;
    return InsertedAt{*id, static_cast<std::uint32_t>(*page),
                      static_cast<std::uint16_t>(*slot)};
}

}  // namespace kds::sim
