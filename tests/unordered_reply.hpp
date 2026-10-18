#pragma once

#include <algorithm>
#include <string>
#include <vector>

// A reply with its rows sorted when its statement has no ORDER BY.
//
// Without ORDER BY the order rows arrive in is the join planner's, and two
// configurations that change a step's kind - an index, a Cabin - can change
// the order it chooses (`docs/spec/parser-v2.md` I12, CB-Q1 (a)). A suite
// comparing configurations compares the rows, then, not their order. The
// header line stays first. Replies are the debug protocol's text, rows
// separated by a literal `\n`.

namespace kds::testing_reply {

inline std::string Unordered(const std::string& reply, const std::string& sql) {
    if (sql.find("ORDER BY") != std::string::npos) return reply;
    const std::string sep = "\\n";
    std::vector<std::string> lines;
    std::size_t at = 0;
    for (std::size_t next; (next = reply.find(sep, at)) != std::string::npos; at = next + 2) {
        lines.push_back(reply.substr(at, next - at));
    }
    lines.push_back(reply.substr(at));
    std::sort(lines.begin() + 1, lines.end());
    std::string out = lines.front();
    for (std::size_t i = 1; i < lines.size(); ++i) out += sep + lines[i];
    return out;
}

}  // namespace kds::testing_reply
