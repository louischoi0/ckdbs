#include "sim/view_diff.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "kds/exec/view_absorb.hpp"
#include "sim/instance.hpp"
#include "sim/rng.hpp"

namespace kds::sim {
namespace {

// The relations the views read. `r2.k` is its tuple key and `r1.a` ranges
// over it, so a join view on it matches every row; `b` and `d` are nullable, so a
// predicate's unknown is generated too.
constexpr const char* kSchema[] = {
    "CREATE TABLE r1 (id int64, a int64, b int64 NULL)",
    "CREATE TABLE r2 (k int64, c int64, d int64 NULL)",
};
constexpr std::int64_t kR1Rows = 40;
constexpr std::int64_t kR2Rows = 10;
// b, c, d over [0, 9]; a and k over [1, 10]; id over [1, 40].
constexpr std::int64_t kValues = 10;
constexpr std::size_t kQueriesPerView = 8;

// A value, NULL one time in five where the column admits it.
std::string Value(Rng& rng, bool nullable) {
    if (nullable && rng.Below(5) == 0) return "NULL";
    return std::to_string(rng.Below(kValues));
}

// A literal over the column's own range, so a bound on `id` is not a bound
// that keeps a quarter of the rows.
std::int64_t Literal(Rng& rng, const std::string& column) {
    const bool id = column.size() >= 2 && column.compare(column.size() - 2, 2, "id") == 0;
    return static_cast<std::int64_t>(rng.Below(id ? kR1Rows + 1 : kValues));
}

const char* Compare(Rng& rng) {
    static constexpr const char* kOps[] = {"=", "!=", "<", "<=", ">", ">="};
    return kOps[rng.Below(6)];
}

// One conjunct over `columns`, whose last one is nullable: a comparison, a
// `BETWEEN`, an `OR` of two comparisons, or an `IS [NOT] NULL`.
std::string Term(Rng& rng, const std::vector<std::string>& columns) {
    const std::string& col = columns[rng.Below(columns.size())];
    const std::int64_t value = Literal(rng, col);
    const std::string lit = std::to_string(value);
    switch (rng.Below(5)) {
        case 0: {
            const std::int64_t other = Literal(rng, col);
            return col + " BETWEEN " + std::to_string(std::min(value, other)) + " AND " +
                   std::to_string(std::max(value, other));
        }
        case 1: {
            const std::string& other = columns[rng.Below(columns.size())];
            return "(" + col + " " + Compare(rng) + " " + lit + " OR " + other + " " +
                   Compare(rng) + " " + std::to_string(Literal(rng, other)) + ")";
        }
        case 2:
            return columns.back() + (rng.Below(2) == 0 ? " IS NULL" : " IS NOT NULL");
        default:
            return col + " " + Compare(rng) + " " + lit;
    }
}

// ` WHERE <one or two conjuncts>`, or nothing one time in four.
std::string Where(Rng& rng, const std::vector<std::string>& columns) {
    if (rng.Below(4) == 0) return "";
    std::string out = " WHERE " + Term(rng, columns);
    if (rng.Below(2) == 0) out += " AND " + Term(rng, columns);
    return out;
}

struct Body {
    std::string text;     // as `CREATE VIEW` stores it
    std::string list;     // ` (c0, ...)` where the view names its columns, else empty
    std::string derived;  // as a derived table writes it: the list as aliases
    std::vector<std::string> columns;  // the view's, as a reader names them
    std::uint32_t depth = 1;           // DP-Q8's level: 1 over relations alone
};

std::string Joined(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) out += (out.empty() ? "" : ", ") + item;
    return out;
}

// A body over relations, `items` its select list and `names` their names.
// One time in three the view's column list permutes the names (DP-Q15), so a
// reader's `a` is the body's `id`: absorption must read every name through
// the view's map, never by its spelling.
Body OverRelations(Rng& rng, const std::vector<std::string>& items,
                   std::vector<std::string> names, const std::string& rest) {
    Body out;
    out.text = "SELECT " + Joined(items) + rest;
    out.derived = out.text;
    if (rng.Below(3) == 0) {
        for (std::size_t i = names.size(); i > 1; --i) std::swap(names[i - 1], names[rng.Below(i)]);
        out.list = " (" + Joined(names) + ")";
        std::vector<std::string> aliased;
        for (std::size_t i = 0; i < items.size(); ++i) aliased.push_back(items[i] + " AS " + names[i]);
        out.derived = "SELECT " + Joined(aliased) + rest;
    }
    out.columns = std::move(names);
    return out;
}

// A subquery conjunct for a body over `r1` (DP-Q1 admits one): an
// uncorrelated `IN`, a `NOT IN` over a nullable column, or an `EXISTS`
// correlated on `r1.a` - the body's own names, which absorption keeps apart
// from the reader's (DP-Q23).
std::string BodySubquery(Rng& rng) {
    const std::string lit = std::to_string(rng.Below(kValues));
    switch (rng.Below(3)) {
        case 0:
            return std::string("a IN (SELECT k FROM r2 WHERE c ") + Compare(rng) + " " + lit + ")";
        case 1:
            return "b NOT IN (SELECT d FROM r2 WHERE k <= " +
                   std::to_string(rng.Range(1, kR2Rows)) + ")";
        default:
            return std::string("EXISTS (SELECT k FROM r2 WHERE r2.k = r1.a AND r2.c ") +
                   Compare(rng) + " " + lit + ")";
    }
}

Body RandomBody(Rng& rng, const std::vector<Body>& earlier) {
    // One time in three, a view over an earlier view (DP-Q8): the native
    // side absorbs it inside out, the derived side nests a derived table.
    // Half of those over the latest view, so chains grow deep; none over a
    // view at the depth bound, whose create would be refused.
    if (!earlier.empty() && rng.Below(3) == 0) {
        const std::size_t j =
            rng.Below(2) == 0 ? earlier.size() - 1 : rng.Below(earlier.size());
        const Body& under = earlier[j];
        if (under.depth < exec::kMaxViewDepth) {
            Body out;
            out.text = "SELECT " + Joined(under.columns) + " FROM v" + std::to_string(j) +
                       Where(rng, under.columns);
            out.derived = out.text;
            out.columns = under.columns;
            out.depth = under.depth + 1;
            return out;
        }
    }
    if (rng.Below(2) == 0) {
        std::string where = Where(rng, {"a", "id", "b"});
        if (rng.Below(3) == 0) where += (where.empty() ? " WHERE " : " AND ") + BodySubquery(rng);
        return OverRelations(rng, {"id", "a", "b"}, {"id", "a", "b"}, " FROM r1" + where);
    }
    // On r2's tuple key, or one time in four on two nullable columns, where
    // a NULL key joins nothing.
    const char* on = rng.Below(4) == 0 ? " ON r1.b = r2.d" : " ON r1.a = r2.k";
    return OverRelations(rng, {"r1.id", "r1.a", "r2.c", "r2.d"}, {"id", "a", "c", "d"},
                         std::string(" FROM r1 JOIN r2") + on +
                             Where(rng, {"r1.a", "r2.c", "r1.id", "r2.d"}));
}

// The reply with its rows sorted under its heading: for a statement whose
// emission order is no contract (a native `GROUP BY` takes no `ORDER BY`).
std::string SortedRows(const std::string& reply) {
    static constexpr std::string_view kRow = "\\n";  // the reply's escaped row break
    if (reply.rfind("ERR", 0) == 0) return reply;
    std::vector<std::string> rows;
    for (std::size_t at = 0;;) {
        const std::size_t next = reply.find(kRow, at);
        rows.push_back(reply.substr(at, next == std::string::npos ? next : next - at));
        if (next == std::string::npos) break;
        at = next + kRow.size();
    }
    std::sort(rows.begin() + 1, rows.end());
    std::string out = rows[0];
    for (std::size_t i = 1; i < rows.size(); ++i) out += std::string(kRow) + rows[i];
    return out;
}

// One outer statement over the view `name`, written twice: with `name` in
// its FROM, and with the body as a derived table of that name. Each sorts by
// every column it returns but the fold, whose rows are compared sorted.
struct Outer {
    std::string through;
    std::string derived;
    bool sorted = false;
    std::string binding;  // the view's in `through`, which its plan's paths start with
};

Outer RandomOuter(Rng& rng, const std::string& name, const Body& body) {
    std::vector<std::string> mine;  // the view's columns, qualified
    for (const std::string& c : body.columns) mine.push_back(name + "." + c);
    const std::string all = Joined(body.columns);
    const std::string as_derived = "(" + body.derived + ")";
    std::string head;  // up to the FROM item
    std::string tail;  // after it
    bool sorted = false;
    // The relation a view is joined to: aliased, or under the name a body
    // reads it by, so a body's `r2` and the reader's must stay apart (DP-Q23).
    const bool aliased = rng.Below(2) == 0;
    const std::string x = aliased ? "x" : "r2";
    const std::string partner = aliased ? "r2 AS x" : "r2";
    const std::vector<std::string> theirs{x + ".k", x + ".c"};
    switch (rng.Below(7)) {
        case 0: {  // the view second, beside a relation: its ON travels with
                   // the body's first relation
            std::vector<std::string> cols = theirs;
            cols.insert(cols.end(), mine.begin(), mine.end());
            std::vector<std::string> filter = cols;
            filter.push_back(x + ".d");  // Term's nullable last column
            head = "SELECT " + Joined(cols) + " FROM " + partner + " JOIN ";
            tail = " ON " + x + ".k = " + name + ".a" + Where(rng, filter) + " ORDER BY " +
                   Joined(cols);
            break;
        }
        case 1: {  // the view first, a relation joined to it
            std::vector<std::string> cols = mine;
            cols.insert(cols.end(), theirs.begin(), theirs.end());
            std::vector<std::string> filter = cols;
            filter.push_back(x + ".d");
            head = "SELECT " + Joined(cols) + " FROM ";
            tail = " JOIN " + partner + " ON " + name + ".a = " + x + ".k" + Where(rng, filter) +
                   " ORDER BY " + Joined(cols);
            break;
        }
        case 2:  // a fold over the view's rows
            head = "SELECT a, COUNT(*), SUM(id), MIN(" + body.columns.back() + ") FROM ";
            tail = Where(rng, body.columns) + " GROUP BY a";
            sorted = true;
            break;
        case 3: {  // a subquery predicate beside the view: [NOT] IN over a
                   // nullable list, or [NOT] EXISTS correlated on its column
            const std::string filter =
                std::string(" WHERE c ") + Compare(rng) + " " + std::to_string(rng.Below(kValues));
            head = "SELECT " + all + " FROM ";
            const bool negated = rng.Below(2) == 0;
            if (rng.Below(2) == 0) {
                tail = std::string(" WHERE a ") + (negated ? "NOT IN" : "IN") + " (SELECT " +
                       (rng.Below(2) == 0 ? "k" : "d") + " FROM r2" + filter + ")";
            } else {
                tail = std::string(" WHERE ") + (negated ? "NOT EXISTS" : "EXISTS") +
                       " (SELECT k FROM r2" + filter + " AND r2.k = " + name + ".a)";
            }
            tail += " ORDER BY " + all;
            break;
        }
        case 4: {  // the view read inside a subquery
            const std::string where = Where(rng, body.columns);
            const auto over = [&](const std::string& from) {
                return "SELECT k, c FROM r2 WHERE k IN (SELECT a FROM " + from + where +
                       ") ORDER BY k, c";
            };
            return {over(name), over(as_derived + " AS " + name), false, name};
        }
        case 5: {  // the view read twice, each reading's names its own
            std::vector<std::string> cols;
            for (const std::string& c : body.columns) cols.push_back("p." + c);
            cols.push_back("q.id");
            const std::string where = Where(rng, cols);
            const auto over = [&](const std::string& from) {
                return "SELECT " + Joined(cols) + " FROM " + from + " AS p JOIN " + from +
                       " AS q ON p.a = q.a" + where + " ORDER BY " + Joined(cols);
            };
            return {over(name), over(as_derived), false, "p"};
        }
        default:
            head = "SELECT " + all + " FROM ";
            tail = Where(rng, body.columns) + " ORDER BY " + all;
            break;
    }
    return {head + name + tail, head + as_derived + " AS " + name + tail, sorted, name};
}

std::string Failure(const SimConfig& config, std::size_t iteration, const std::string& what) {
    return "seed=" + std::to_string(config.seed) + " iteration=" + std::to_string(iteration) +
           ": " + what;
}

}  // namespace

SimVerdict RunViewDifferential(const SimConfig& config) {
    SimVerdict verdict;
    const std::size_t views = std::max<std::size_t>(4, config.ops / 100);
    for (std::size_t iteration = 0; iteration < config.iterations; ++iteration) {
        ++verdict.iterations_run;
        Rng rng(config.seed * 1000003u + iteration);
        auto created = SimInstance::Create();
        if (!created.ok()) {
            verdict.ok = false;
            verdict.detail = "instance creation failed: " + created.status().message();
            return verdict;
        }
        SimInstance& instance = *created.value();
        const auto run = [&](const std::string& sql) {
            ++verdict.ops_run;
            return instance.Execute(sql);
        };
        const auto setup = [&](const std::string& sql) {
            const std::string reply = run(sql);
            if (reply.rfind("ERR", 0) != 0) return true;
            verdict.ok = false;
            verdict.detail = Failure(config, iteration, "setup [" + sql + "] -> " + reply);
            return false;
        };
        for (const char* ddl : kSchema) {
            if (!setup(ddl)) return verdict;
        }
        for (std::int64_t id = 1; id <= kR1Rows; ++id) {
            if (!setup("INSERT INTO r1 VALUES (" + std::to_string(id) + ", " +
                       std::to_string(rng.Range(1, kValues)) + ", " + Value(rng, true) + ")")) {
                return verdict;
            }
        }
        for (std::int64_t k = 1; k <= kR2Rows; ++k) {
            if (!setup("INSERT INTO r2 VALUES (" + std::to_string(k) + ", " + Value(rng, false) +
                       ", " + Value(rng, true) + ")")) {
                return verdict;
            }
        }

        std::vector<Body> made_views;
        for (std::size_t v = 0; v < views; ++v) {
            const Body body = RandomBody(rng, made_views);
            made_views.push_back(body);
            const std::string name = "v" + std::to_string(v);
            const std::string create = "CREATE VIEW " + name + body.list + " AS " + body.text;
            const std::string made = run(create);
            if (made.rfind("CREATED VIEW", 0) != 0 || made.find("WARNING") != std::string::npos) {
                verdict.ok = false;
                verdict.detail = Failure(config, iteration, "[" + create + "] -> " + made);
                return verdict;
            }
            for (std::size_t q = 0; q < kQueriesPerView; ++q) {
                const Outer outer = RandomOuter(rng, name, body);
                // The view's side must absorb natively - its plan names the
                // view's relations by their path (DP-Q7) - or the cell
                // compares the query layer with itself.
                const std::string plan = run("ANALYZE " + outer.through);
                const std::string absorbed_reply = run(outer.through);
                const std::string derived_reply = run(outer.derived);
                ++verdict.reads_checked;
                const bool native = plan.find(" AS " + outer.binding + ".") != std::string::npos;
                const bool same = outer.sorted
                                      ? SortedRows(absorbed_reply) == SortedRows(derived_reply)
                                      : absorbed_reply == derived_reply;
                if (native && same && absorbed_reply.rfind("ERR", 0) != 0) continue;
                verdict.ok = false;
                verdict.detail = Failure(
                    config, iteration,
                    std::string(native ? "DP-R1" : "not absorbed natively") + ": [" + create +
                        "]\n    through the view: [" + outer.through + "]\n      -> " +
                        absorbed_reply + "\n    as a derived table: [" + outer.derived +
                        "]\n      -> " + derived_reply + "\n    plan: " + plan);
                return verdict;
            }
        }
    }
    return verdict;
}

}  // namespace kds::sim
