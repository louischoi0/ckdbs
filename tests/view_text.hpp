#pragma once

// A `sys` view's newline-protocol reply, read back as a table (BO-S2): every
// `SHOW` is a `SELECT` over a view (SV1), so a test that read a `SHOW`'s
// `key=value` line reads the view's columns instead.
//
// The reply is a header line of column names and one line per row, each
// comma-joined, the lines joined by the two-character "\n" escape
// (`docs/spec/client-manual.md` §2). The text surface cannot say where a
// value holding a comma ends, so a row with more fields than columns keeps
// the surplus in its last column: a test that reads such a column (a
// `decimal(p,s)` type, an index's key list) projects it last.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace kds::testing_view {

struct ViewText {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;

    // The column's index; a missing column fails the test and answers npos.
    std::size_t Col(std::string_view name) const {
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (columns[i] == name) return i;
        }
        ADD_FAILURE() << "the view has no column '" << name << "'";
        return std::string::npos;
    }

    // Every row's `column`, in row order.
    std::vector<std::string> Column(std::string_view column) const {
        std::vector<std::string> out;
        const std::size_t c = Col(column);
        if (c == std::string::npos) return out;
        for (const auto& row : rows) out.push_back(row[c]);
        return out;
    }

    // `column` on the first row whose `key_col` is `key`; nullopt when no
    // row matches.
    std::optional<std::string> Find(std::string_view key_col, std::string_view key,
                                    std::string_view column) const {
        const std::size_t k = Col(key_col);
        const std::size_t c = Col(column);
        if (k == std::string::npos || c == std::string::npos) return std::nullopt;
        for (const auto& row : rows) {
            if (row[k] == key) return row[c];
        }
        return std::nullopt;
    }
};

inline std::vector<std::string> SplitOn(std::string_view s, std::string_view sep) {
    std::vector<std::string> out;
    while (true) {
        const std::size_t at = s.find(sep);
        out.emplace_back(s.substr(0, at));
        if (at == std::string_view::npos) return out;
        s.remove_prefix(at + sep.size());
    }
}

inline ViewText ReadViewText(std::string_view reply) {
    ViewText table;
    if (reply.rfind("ERR", 0) == 0) {
        ADD_FAILURE() << "a view read was refused: " << reply;
        return table;
    }
    const std::vector<std::string> lines = SplitOn(reply, "\\n");
    table.columns = SplitOn(lines.front(), ",");
    for (std::size_t i = 1; i < lines.size(); ++i) {
        std::vector<std::string> row = SplitOn(lines[i], ",");
        while (row.size() > table.columns.size() && table.columns.size() > 0) {
            row[row.size() - 2] += "," + row.back();
            row.pop_back();
        }
        EXPECT_EQ(row.size(), table.columns.size()) << lines[i];
        table.rows.push_back(std::move(row));
    }
    return table;
}

// One `sys.meta` field's value out of a `SHOW META` (or `SELECT * FROM
// sys.meta`) reply; nullopt when the field is absent (the absent-rather-
// than-zeroed rule). Read by hand rather than through `ReadViewText`: a value
// may hold commas (`ddl_transactional`'s list), and `section` and `field`
// never do, so a row is split at its first two.
inline std::optional<std::string> MetaField(std::string_view meta_reply, std::string_view field) {
    if (meta_reply.rfind("section,field,value", 0) != 0) {
        ADD_FAILURE() << "not a sys.meta reply: " << meta_reply;
        return std::nullopt;
    }
    const std::vector<std::string> lines = SplitOn(meta_reply, "\\n");
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const std::string_view line = lines[i];
        const std::size_t a = line.find(',');
        const std::size_t b = a == std::string_view::npos ? a : line.find(',', a + 1);
        if (b == std::string_view::npos) continue;
        if (line.substr(a + 1, b - a - 1) == field) return std::string(line.substr(b + 1));
    }
    return std::nullopt;
}

// `column` of relation `table`'s row in a view keyed by `name` -
// `sys.tables` (`next_id`, `clustered_type`, `oid`, `root_page_id`) or
// `sys.relation_stats` (the budget columns, `height`, `leaves`) - read
// through `d.Dispatch`; nullopt when the relation has no row.
template <class Dispatcher>
std::optional<std::string> RelationField(Dispatcher& d, std::string_view view,
                                         std::string_view table, std::string_view column) {
    const std::string reply =
        d.Dispatch("SELECT name, " + std::string(column) + " FROM sys." + std::string(view) +
                   " WHERE name = '" + std::string(table) + "'")
            .response;
    return ReadViewText(reply).Find("name", table, column);
}

}  // namespace kds::testing_view
