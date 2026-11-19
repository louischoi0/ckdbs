// What a compiled-chain cache could remove, priced in-process (CX-S6).
//
// `instructions/v3.2.0/workorder-cx-managed-plans.md` CX-S6 builds a cache
// keyed `(fetch_id, oids, plan, variant, cache_built_at, catalog_version)`
// only if the compile is a material share of a repeated statement's latency
// (CX-Q10: 5 % of p50 at `cores = 1`). `sys.statement_stats`' `compile_us`
// runs from the head of the call, so it holds the lex, the parse and the
// fingerprint - which a cache keyed by the fetch id must still pay, the key
// being made of them - beside the compile proper. This prices the two
// apart for shapes like the S6 cell's - not its exact texts, indexes or
// `join_order` (the results file's §7 lists the differences):
//
//   parse     `parser::Parser::Parse`, which lexes and fingerprints as it
//             goes: what every statement pays, cached or not;
//   compile   `exec::Compile` over the parsed statement: binding, the
//             access ladder, the chain - the most a cache could remove,
//             before the cost of binding a cached chain's literals again.
//
// Each shape's compile runs with one set of options: none (one compile, as
// a captured order equal to the written one costs, less the map lookup), a
// captured order that is not the written one (two compiles: the written,
// then the captured; no S6 cell ran this), an administrator's plan of two
// variants under `search` off (the second lex of `AdminPlan::Choose`, then
// the variant's compile where it reorders), a one-relation plan (the
// unpinned compile and the pinned one).
//
// Not measured: the dispatcher around the compile (the read borrow, the
// name resolution under a minted view - this passes none), a cache's own
// costs (its lookup and the literal rebinding it would need), and the
// compile with its caches cold: each loop compiles back to back.
//
// Median of `--repeats` runs of `--iterations` statements each, ns per
// statement, the minimum beside it. A Release build, or the number is
// Debug's (`bench/README.md` rule 1). Literals cycle through 16 values, so
// no run reads one text alone.
//
// Run:
//     cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_BENCH=ON
//     cmake --build build-release -j --target kds_compile_split_bench
//     ./build-release/kds_compile_split_bench [--iterations N] [--repeats K]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/admin_plans.hpp"
#include "kds/exec/captured_plans.hpp"
#include "kds/exec/join_order.hpp"
#include "kds/exec/step_compiler.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using namespace kds;

struct Options {
    std::size_t iterations = 20000;
    int repeats = 7;
};

Options ParseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--iterations" && i + 1 < argc) {
            options.iterations = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--repeats" && i + 1 < argc) {
            options.repeats = std::atoi(argv[++i]);
        }
    }
    return options;
}

struct Sample {
    double median = 0;
    double min = 0;
};

Sample Summarize(std::vector<double> runs) {
    std::sort(runs.begin(), runs.end());
    return {runs[runs.size() / 2], runs.front()};
}

class Instance {
public:
    Instance() {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        if (!boot.ok()) Die("bootstrap", boot.status().message());
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                            exec::Budget(), /*recorder=*/nullptr, /*replay=*/false,
                            /*access_statistics=*/false, /*cabins=*/nullptr, &*mgr_);
    }
    void Must(const std::string& sql) {
        const std::string out = dispatcher_->Dispatch(sql).response;
        if (out.rfind("ERR", 0) == 0) Die(sql, out);
    }
    catalog::Catalog& catalog() { return boot_->catalog; }
    exec::AdminPlans& admin() { return dispatcher_->admin_plans(); }

    [[noreturn]] static void Die(const std::string& what, const std::string& why) {
        std::fprintf(stderr, "%s: %s\n", what.c_str(), why.c_str());
        std::exit(1);
    }

private:
    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<server::CommandDispatcher> dispatcher_;
};

void Load(Instance& db) {
    db.Must("CREATE TABLE pb_bars (id int64, sym int64, close int64) BTREE");
    for (int i = 1; i <= 2000; ++i) {
        db.Must("INSERT INTO pb_bars VALUES (" + std::to_string(i) + ", " + std::to_string(i % 50) +
                ", " + std::to_string(i * 3) + ")");
    }
    db.Must("CREATE TABLE pc_users (id int64, member_code int64) BTREE");
    db.Must("CREATE TABLE pc_books (id int64, shelf int64) BTREE");
    db.Must("CREATE TABLE pc_loans (id int64, user_id int64, book_id int64) BTREE");
    for (int i = 1; i <= 200; ++i) {
        db.Must("INSERT INTO pc_users VALUES (" + std::to_string(i) + ", " + std::to_string(i * 7) +
                ")");
        db.Must("INSERT INTO pc_books VALUES (" + std::to_string(i) + ", " + std::to_string(i % 9) +
                ")");
    }
    for (int i = 1; i <= 2000; ++i) {
        db.Must("INSERT INTO pc_loans VALUES (" + std::to_string(i) + ", " +
                std::to_string(1 + i % 200) + ", " + std::to_string(1 + i % 199) + ")");
    }
}

// One shape: its text for a literal, and the options its compile runs with.
struct Shape {
    const char* name;
    std::string (*text)(int literal);
    const exec::PlanOptions* options;
};

std::string PointRead(int k) { return "SELECT * FROM pb_bars WHERE id = " + std::to_string(k); }
std::string TwoJoin(int k) {
    return "SELECT l.book_id, u.member_code FROM pc_users AS u JOIN pc_loans AS l ON l.user_id = "
           "u.id WHERE u.member_code > " +
           std::to_string(k);
}
std::string ThreeJoin(int k) {
    return "SELECT l.id, u.member_code, b.shelf FROM pc_users AS u JOIN pc_loans AS l ON "
           "l.user_id = u.id JOIN pc_books AS b ON b.id = l.book_id WHERE u.member_code > " +
           std::to_string(k);
}
std::string OneRelation(int k) {
    return "SELECT * FROM pb_bars WHERE sym = " + std::to_string(k % 50);
}

struct Result {
    Sample parse;
    Sample compile;
};

Result Measure(Instance& db, const Shape& shape, const Options& options) {
    // Each literal's text, parsed once to check it, outside the timed loop.
    std::vector<std::string> texts;
    for (int k = 0; k < 16; ++k) texts.push_back(shape.text(k * 11 + 3));
    std::vector<std::unique_ptr<parser::Parser>> parsers;
    std::vector<parser::SelectStmt> stmts;
    for (const std::string& text : texts) {
        auto parser = std::make_unique<parser::Parser>(text);
        auto parsed = parser->Parse();
        if (!parsed.ok()) Instance::Die(text, parsed.status().message());
        stmts.push_back(std::get<parser::SelectStmt>(std::move(parsed.value())));
        parsers.push_back(std::move(parser));
    }
    const exec::StatementContext context = exec::StatementContextNow();
    std::vector<double> parse_runs;
    std::vector<double> compile_runs;
    std::uint64_t checksum = 0;
    for (int r = 0; r < options.repeats; ++r) {
        auto t0 = Clock::now();
        for (std::size_t i = 0; i < options.iterations; ++i) {
            parser::Parser parser(texts[i % texts.size()]);
            auto parsed = parser.Parse();
            checksum += parsed.ok() ? parser.fingerprint()->fetch_id & 1 : 7;
        }
        auto t1 = Clock::now();
        for (std::size_t i = 0; i < options.iterations; ++i) {
            const std::size_t at = i % texts.size();
            exec::PlanOptions plan;
            const exec::PlanOptions* used = nullptr;
            if (shape.options != nullptr) {
                plan = *shape.options;
                plan.statement = texts[at];
                plan.fetch_id = parsers[at]->fingerprint()->fetch_id;
                used = &plan;
            }
            auto chain = exec::Compile(db.catalog(), stmts[at], /*view=*/nullptr,
                                       /*declare=*/nullptr, &context, used);
            if (!chain.ok()) Instance::Die(texts[at], chain.status().message());
            checksum += chain.value().steps.size();
        }
        auto t2 = Clock::now();
        const double n = static_cast<double>(options.iterations);
        parse_runs.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / n);
        compile_runs.push_back(std::chrono::duration<double, std::nano>(t2 - t1).count() / n);
    }
    std::fprintf(stderr, "checksum %llu\n", static_cast<unsigned long long>(checksum));
    return {Summarize(parse_runs), Summarize(compile_runs)};
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = ParseOptions(argc, argv);
    Instance db;
    Load(db);

    // An administrator's plan of two variants over the two-relation join,
    // and a one-relation plan; each made through the dispatcher, as the S6
    // cell made them.
    db.Must("CREATE PLAN two FOR " + TwoJoin(100) +
            " VARIANT low WHEN ARG(1) < 100 ORDER (u, l) VARIANT high ELSE ORDER (l, u)");
    db.Must("CREATE PLAN one FOR " + OneRelation(3) +
            " VARIANT v ELSE ORDER (pb_bars) ACCESS pb_bars SCAN");

    exec::PlanOptions admin;
    admin.admin = &db.admin();
    admin.search = false;  // `join_order = written`: the plan alone chooses

    // A captured order that is not the written one: the written order's
    // compile, then the captured order's.
    exec::CapturedPlans captured;
    exec::PlanOptions fixed;
    fixed.captured = &captured;
    {
        const std::string three = ThreeJoin(3);  // the parser keeps a view of it
        parser::Parser parser(three);
        auto parsed = parser.Parse();
        auto chain = exec::Compile(db.catalog(), std::get<parser::SelectStmt>(parsed.value()),
                                   nullptr, nullptr, nullptr, nullptr);
        if (!chain.ok()) Instance::Die("three", chain.status().message());
        exec::CapturedKey key;
        key.fetch_id = parser.fingerprint()->fetch_id;
        key.n = 3;
        for (std::size_t w = 0; w < 3; ++w) key.oids[w] = chain.value().steps[w].rel_oid;
        exec::CapturedOrder order{};
        order[0] = 1;  // loans, users, books
        order[1] = 0;
        order[2] = 2;
        captured.Capture(key, order, ThreeJoin(3));
    }

    const Shape shapes[] = {
        {"pk point read (no options)", PointRead, nullptr},
        {"two-relation join, one compile", TwoJoin, nullptr},
        {"three-relation join, one compile", ThreeJoin, nullptr},
        {"three-relation join, captured reorder", ThreeJoin, &fixed},
        {"two-relation join, 2-variant plan", TwoJoin, &admin},
        {"one-relation statement, no plan", OneRelation, nullptr},
        {"one-relation statement, its plan", OneRelation, &admin},
    };
    std::printf("%-40s %14s %14s %14s %14s\n", "shape", "parse p50 ns", "parse min ns",
                "compile p50 ns", "compile min ns");
    for (const Shape& shape : shapes) {
        const Result result = Measure(db, shape, options);
        std::printf("%-40s %14.0f %14.0f %14.0f %14.0f\n", shape.name, result.parse.median,
                    result.parse.min, result.compile.median, result.compile.min);
    }
    return 0;
}
