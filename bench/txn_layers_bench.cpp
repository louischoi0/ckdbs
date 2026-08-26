// Where a business transaction's time goes, layer by layer.
//
// `tools/scenario0_stockmarket.py` reports TPS for a four-statement transaction:
//
//     INSERT INTO trades ...   the buy leg   (HEAP relation, logged)
//     INSERT INTO trades ...   the sell leg
//     UPDATE accounts SET ...  buyer         (BTREE relation, logged since txn)
//     UPDATE accounts SET ...  seller
//
// That tool measures the whole stack through a socket, which answers "how
// fast" and not "why". This binary runs the *same four statements* against
// the same schema in-process and prices each layer, so a change can be
// aimed rather than guessed at.
//
// ---- How the attribution is done, and what it is worth -------------------
//
// Two independent measurements, deliberately not one:
//
//   1. **End to end**, through `CommandDispatcher::Dispatch` - the same
//      call the server makes, minus the socket. This is the authority: it
//      is what a statement actually costs.
//   2. **Per layer**, by calling each component directly in its own loop
//      with the same inputs the dispatcher gives it.
//
// The second does not add up to the first, and the difference is reported
// rather than hidden: it is dispatcher overhead plus whatever a layer does
// that this file does not reproduce. A layer's number is a *lower bound* on
// what removing it would save - which is the useful direction, since it
// stops an optimization being justified by a cost it would not actually
// remove.
//
// Durability is separated by running the same statements under four
// configurations (unlogged / relaxed / group / strict), because on a real
// device that is the axis everything else is dwarfed by, and a per-layer
// budget taken under one of them says nothing about the others.
//
// Run:
//     cmake --build build-release -j && ./build-release/kds_txn_bench
//     KDS_BENCH_DIR=/mnt/somewhere ./build-release/kds_txn_bench

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

#include "kds/base/log.hpp"
#include "kds/bootstrap/bootstrap.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/budget.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/step_compiler.hpp"
#include "kds/parser/fingerprint.hpp"
#include "kds/parser/lexer.hpp"
#include "kds/parser/parser.hpp"
#include "kds/sched/clock.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/file_page_device.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/wal/file_log_device.hpp"
#include "kds/wal/manager.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using kds::Status;

double MicrosSince(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

// p50 rather than the mean, for the reason the stress tool's own report
// gives: a mean over a run that includes a segment roll or a page split
// describes neither the common case nor the tail.
double Percentile(std::vector<double>& samples, double p) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    const std::size_t at =
        std::min(samples.size() - 1, static_cast<std::size_t>(p * samples.size()));
    return samples[at];
}

struct Row {
    std::string layer;
    double p50 = 0.0;
    double p99 = 0.0;
    std::string note;
};

void PrintTable(const std::string& title, const std::vector<Row>& rows) {
    std::printf("\n%s\n", title.c_str());
    std::printf("%-34s %10s %10s  %s\n", "layer", "p50 (us)", "p99 (us)", "note");
    std::printf("%-34s %10s %10s  %s\n", "----------------------------------", "----------",
                "----------", "----");
    for (const Row& row : rows) {
        std::printf("%-34s %10.2f %10.2f  %s\n", row.layer.c_str(), row.p50, row.p99,
                    row.note.c_str());
    }
}

class ScratchDir {
public:
    ScratchDir() {
        const char* base = std::getenv("KDS_BENCH_DIR");
        path_ = std::filesystem::path(base != nullptr ? base
                                                      : std::filesystem::temp_directory_path()) /
                ("kds-txn-bench-" + std::to_string(::getpid()));
        std::filesystem::create_directories(path_);
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    std::string File(const std::string& name) const { return (path_ / name).string(); }
    const std::filesystem::path& path() const { return path_; }
    std::string Sub(const std::string& name) const {
        const auto p = path_ / name;
        std::filesystem::create_directories(p);
        return p.string();
    }

private:
    std::filesystem::path path_;
};

void Fatal(const kds::Status& s, const char* what) {
    std::fprintf(stderr, "txn bench: %s: %s\n", what, s.message().c_str());
    std::exit(1);
}

// The stress tool's schema, verbatim in the parts that matter: `accounts`
// clustered (every access is `WHERE id = <n>`), `trades` a heap (insert
// only, never probed by pk).
constexpr const char* kCreateUsers =
    "CREATE TABLE users (id int64, name varchar, country varchar, tier int32, "
    "created_day int32) BTREE";
constexpr const char* kCreateAssets =
    "CREATE TABLE assets (id int64, symbol varchar, asset_class int32, last_price int64) BTREE";
constexpr const char* kCreateAccounts =
    "CREATE TABLE accounts (id int64, user_id int64, balance int64, asset_qty int64, "
    "trade_count int64, opened_day int32) BTREE";
constexpr const char* kCreateTrades =
    "CREATE TABLE trades (id int64, account_id int64, asset_id int64, side int32, qty int64, "
    "price int64, trade_day int32) HEAP";

// One database, wired the way `server::Expeditor` wires the real one.
class Instance {
public:
    Instance(const ScratchDir& scratch, const std::string& tag, bool logged,
             kds::wal::DurabilityClass durability, bool foreign_keys, bool capture_phases = false)
        : durability_(durability) {
        if (capture_phases) {
            // The dispatcher's own per-phase instrumentation reports through
            // the debug `[query]` line, so capturing it means giving this
            // dispatcher a logger and reading the lines back. In situ, which
            // is what makes it better evidence than the direct-call loops
            // below: it prices each phase *as the statement runs it*.
            logger_.emplace(&sink_, wall_clock_, kds::LogLevel::kDebug);
        }
        auto device = kds::storage::FilePageDevice::Open(scratch.File(tag + ".db"));
        if (!device.ok()) Fatal(device.status(), "opening the data file");
        device_ = std::move(device.value());

        auto store = kds::storage::DevicePageStore::Open(*device_, kds::server::kFirstUserPageId);
        if (!store.ok()) Fatal(store.status(), "opening the page store");
        store_ = std::move(store.value());

        auto boot = kds::bootstrap::BootstrapDatabase(*store_, /*now_unix_seconds=*/4000);
        if (!boot.ok()) Fatal(boot.status(), "bootstrapping");
        database_.emplace(std::move(boot.value()));

        if (logged) {
            auto log_device = kds::wal::FileLogDevice::Open(scratch.Sub(tag + "-wal"), 0);
            if (!log_device.ok()) Fatal(log_device.status(), "opening the log device");
            log_device_ = std::move(log_device.value());

            auto wal = kds::wal::WalManager::Open(log_device_.get(), clock_, /*core_id=*/0);
            if (!wal.ok()) Fatal(wal.status(), "opening the wal manager");
            wal_ = std::move(wal.value());
            store_->SetWalGate(wal_.get());
        }

        trx_ids_.emplace(database_->superblock, [] { return kds::Status::OK(); });
        undo_.emplace(*store_, wal_ ? &*wal_ : nullptr);
        manager_.emplace(*trx_ids_, *undo_, *store_, wal_ ? &*wal_ : nullptr);

        dispatcher_.emplace(database_->superblock, database_->catalog, *store_,
                            logger_ ? &*logger_ : nullptr, &clock_, wal_ ? &*wal_ : nullptr,
                            durability, kds::exec::Budget(),
                            /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/false, /*cabins=*/nullptr, &*manager_);

        Run(kCreateUsers);
        Run(kCreateAssets);
        if (foreign_keys) {
            // The same schema with the relationships the workload already
            // has in its data, declared: `accounts.user_id -> users`, and
            // `trades.account_id -> accounts`.
            Run("CREATE TABLE accounts (id int64, user_id int64 REFERENCES users, "
                "balance int64, asset_qty int64, trade_count int64, opened_day int32) BTREE");
            Run("CREATE TABLE trades (id int64, account_id int64 REFERENCES accounts, "
                "asset_id int64, side int32, qty int64, price int64, trade_day int32) HEAP");
        } else {
            Run(kCreateAccounts);
            Run(kCreateTrades);
        }
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    // The load phase: `users` and `accounts`, one account per user so a
    // trade's two legs always name live rows.
    void Load(int accounts) {
        for (int i = 0; i < accounts; ++i) {
            Run("INSERT INTO users VALUES ('u" + std::to_string(i) + "', 'kr', 1, 0)");
            Run("INSERT INTO accounts VALUES (" + std::to_string(i + 1) +
                ", 1000000, 500, 0, 0)");
        }
        Run("SYNC");
    }

    kds::server::CommandDispatcher& dispatcher() { return *dispatcher_; }
    kds::wal::WalManager* wal() { return wal_ ? &*wal_ : nullptr; }
    std::vector<std::string>& log_lines() { return sink_.lines; }
    kds::catalog::Catalog& catalog() { return database_->catalog; }
    kds::storage::DevicePageStore& store() { return *store_; }
    kds::wal::DurabilityClass durability() const { return durability_; }

private:
    kds::sched::SystemClock clock_;
    kds::SystemWallClock wall_clock_;
    kds::MemoryLogSink sink_;
    std::optional<kds::Logger> logger_;
    kds::wal::DurabilityClass durability_;
    std::unique_ptr<kds::storage::PageDevice> device_;
    std::unique_ptr<kds::storage::DevicePageStore> store_;
    std::optional<kds::bootstrap::BootstrapResult> database_;
    std::unique_ptr<kds::wal::LogDevice> log_device_;
    std::unique_ptr<kds::wal::WalManager> wal_;
    std::optional<kds::txn::TrxIdSequence> trx_ids_;
    std::optional<kds::txn::UndoLog> undo_;
    std::optional<kds::txn::TransactionManager> manager_;
    std::optional<kds::server::CommandDispatcher> dispatcher_;
};

// ---- The transaction, statement by statement -----------------------------

struct Statements {
    std::string buy_leg;
    std::string sell_leg;
    std::string buyer_update;
    std::string seller_update;
};

Statements MakeStatements(int account, int day) {
    Statements out;
    const std::string acct = std::to_string(account);
    out.buy_leg = "INSERT INTO trades VALUES (" + acct + ", 7, 0, 10, 250, " +
                  std::to_string(day) + ")";
    out.sell_leg = "INSERT INTO trades VALUES (" + acct + ", 7, 1, 10, 250, " +
                   std::to_string(day) + ")";
    out.buyer_update = "UPDATE accounts SET balance = 999000, asset_qty = 10, trade_count = 1 "
                       "WHERE id = " + acct;
    out.seller_update = "UPDATE accounts SET balance = 1001000, asset_qty = 0, trade_count = 2 "
                        "WHERE id = " + acct;
    return out;
}

const char* DurabilityName(kds::wal::DurabilityClass d) {
    switch (d) {
        case kds::wal::DurabilityClass::kStrict: return "strict";
        case kds::wal::DurabilityClass::kGroup: return "group";
        case kds::wal::DurabilityClass::kRelaxed: return "relaxed";
    }
    return "?";
}

// End-to-end, one row per statement kind, under one configuration.
void MeasureEndToEnd(Instance& db, int accounts, int iterations, const std::string& title) {
    std::vector<double> insert, update, whole;
    for (int i = 0; i < iterations; ++i) {
        const Statements s = MakeStatements(1 + (i % accounts), i);

        const auto txn_start = Clock::now();
        const auto t0 = Clock::now();
        db.Run(s.buy_leg);
        insert.push_back(MicrosSince(t0));
        db.Run(s.sell_leg);

        const auto t1 = Clock::now();
        db.Run(s.buyer_update);
        update.push_back(MicrosSince(t1));
        db.Run(s.seller_update);

        whole.push_back(MicrosSince(txn_start));
    }

    std::vector<Row> rows;
    rows.push_back({"INSERT trades (one leg)", Percentile(insert, 0.5), Percentile(insert, 0.99),
                    "heap append, logged"});
    rows.push_back({"UPDATE accounts (one side)", Percentile(update, 0.5),
                    Percentile(update, 0.99), "btree descent, in-place, logged"});
    rows.push_back({"whole transaction (4 stmts)", Percentile(whole, 0.5), Percentile(whole, 0.99),
                    "what TPS is the reciprocal of"});
    PrintTable(title, rows);

    // **Validation, not decoration.** A durability class that did not
    // actually reach the device would make every row above a measurement of
    // the wrong thing, and the failure mode is silent: the numbers simply
    // come out fast. So the counters that say whether a sync happened are
    // printed beside them, along with what one sync costs on this device.
    if (kds::wal::WalManager* wal = db.wal(); wal != nullptr) {
        const kds::wal::WalStats& st = wal->stats();
        std::printf("  wal: records=%llu syncs=%llu strict=%llu group=%llu batches=%llu "
                    "relaxed=%llu mean_batch=%.1f\n",
                    static_cast<unsigned long long>(st.records_appended),
                    static_cast<unsigned long long>(st.syncs),
                    static_cast<unsigned long long>(st.strict_commits),
                    static_cast<unsigned long long>(st.group_commits),
                    static_cast<unsigned long long>(st.group_batches),
                    static_cast<unsigned long long>(st.relaxed_commits),
                    st.mean_group_batch_size());

        std::vector<double> syncs;
        for (int i = 0; i < 50; ++i) {
            db.Run("INSERT INTO trades VALUES (1, 7, 0, 1, 1, 1)");
            const auto t = Clock::now();
            if (Status s = wal->SyncAll(); !s.ok()) Fatal(s, "syncing");
            syncs.push_back(MicrosSince(t));
        }
        std::printf("  one wal->Sync() after one appended record: p50 %.2f us, p99 %.2f us\n",
                    Percentile(syncs, 0.5), Percentile(syncs, 0.99));
    }
}

// Per layer, by calling each component with the inputs the dispatcher gives
// it. See the header note on what these numbers are and are not.
void MeasureLayers(Instance& db, int iterations) {
    const Statements s = MakeStatements(1, 1);
    std::vector<Row> rows;

    // 1. Parse - the same call `InsertInner` makes first.
    {
        std::vector<double> insert_parse, update_parse;
        for (int i = 0; i < iterations; ++i) {
            auto t = Clock::now();
            auto parsed = kds::parser::Parse(s.buy_leg);
            insert_parse.push_back(MicrosSince(t));
            if (!parsed.ok()) Fatal(parsed.status(), "parsing the insert");

            t = Clock::now();
            auto parsed_update = kds::parser::Parse(s.buyer_update);
            update_parse.push_back(MicrosSince(t));
            if (!parsed_update.ok()) Fatal(parsed_update.status(), "parsing the update");
        }
        rows.push_back({"parse INSERT", Percentile(insert_parse, 0.5),
                        Percentile(insert_parse, 0.99), "lexer + AST, zero-copy tokens"});
        rows.push_back({"parse UPDATE", Percentile(update_parse, 0.5),
                        Percentile(update_parse, 0.99), ""});
    }

    // 1b. Lexing alone, against the full parse above.
    //
    // The split is the whole question for the statement path: parse is ~35%
    // of an unlogged INSERT, and what to do about it depends entirely on
    // whether the time is in the scanner or in building the AST. A slow
    // scanner is a scanner problem; a slow AST is an *allocation* problem,
    // since every name the AST keeps is a `std::string` copied at the
    // boundary (parser-v2.md I4) - and the arena that would fix that is
    // already specified and not built.
    {
        std::vector<double> insert_lex, update_lex;
        for (int i = 0; i < iterations; ++i) {
            auto t = Clock::now();
            {
                kds::parser::Lexer lexer(s.buy_leg);
                while (lexer.Peek().type != kds::parser::TokenType::kEof) lexer.Next();
            }
            insert_lex.push_back(MicrosSince(t));

            t = Clock::now();
            {
                kds::parser::Lexer lexer(s.buyer_update);
                while (lexer.Peek().type != kds::parser::TokenType::kEof) lexer.Next();
            }
            update_lex.push_back(MicrosSince(t));
        }
        rows.push_back({"  lex only INSERT", Percentile(insert_lex, 0.5),
                        Percentile(insert_lex, 0.99), "scanner, no AST"});
        rows.push_back({"  lex only UPDATE", Percentile(update_lex, 0.5),
                        Percentile(update_lex, 0.99), ""});
    }

    // 2. Catalog resolution, the cached path: name -> oid -> TableAccess.
    {
        std::vector<double> samples;
        for (int i = 0; i < iterations; ++i) {
            const auto t = Clock::now();
            auto oid = db.catalog().FindTableOidByName("trades");
            if (!oid.ok()) Fatal(oid.status(), "resolving trades");
            auto access = db.catalog().InitTableAccess(oid.value());
            if (!access.ok()) Fatal(access.status(), "opening trades");
            samples.push_back(MicrosSince(t));
        }
        rows.push_back({"catalog resolve (cached)", Percentile(samples, 0.5),
                        Percentile(samples, 0.99), "name -> oid -> TableAccess"});
    }

    // 3. The row-id sequence. **Not cached and never cacheable** - it is a
    //    read of the sys.tables page plus a write back to it.
    {
        std::vector<double> samples;
        auto oid = db.catalog().FindTableOidByName("trades");
        if (!oid.ok()) Fatal(oid.status(), "resolving trades");
        for (int i = 0; i < iterations; ++i) {
            const auto t = Clock::now();
            auto id = db.catalog().AllocateRowId(oid.value());
            if (!id.ok()) Fatal(id.status(), "allocating a row id");
            samples.push_back(MicrosSince(t));
        }
        rows.push_back({"AllocateRowId", Percentile(samples, 0.5), Percentile(samples, 0.99),
                        "sys.tables page read + write"});
    }

    // 4. Encoding one row against the schema constant.
    {
        auto oid = db.catalog().FindTableOidByName("trades");
        if (!oid.ok()) Fatal(oid.status(), "resolving trades");
        auto access = db.catalog().InitTableAccess(oid.value());
        if (!access.ok()) Fatal(access.status(), "opening trades");

        auto parsed = kds::parser::Parse(s.buy_leg);
        if (!parsed.ok()) Fatal(parsed.status(), "parsing the insert");
        const auto& stmt = std::get<kds::parser::InsertStmt>(parsed.value());

        std::vector<double> samples;
        for (int i = 0; i < iterations; ++i) {
            const auto t = Clock::now();
            auto encoded = kds::exec::EncodeRow(access.value()->schema, access.value()->layout,
                                                 /*id=*/1, stmt.rows.front(),
                                                 kds::exec::VarHeapSink{});
            samples.push_back(MicrosSince(t));
            if (!encoded.ok()) Fatal(encoded.status(), "encoding a row");
        }
        rows.push_back({"EncodeRow", Percentile(samples, 0.5), Percentile(samples, 0.99),
                        "fixed-length payload, no spill"});
    }

    // 5. The WHERE clause an UPDATE compiles, per statement.
    {
        auto oid = db.catalog().FindTableOidByName("accounts");
        if (!oid.ok()) Fatal(oid.status(), "resolving accounts");
        auto access = db.catalog().InitTableAccess(oid.value());
        if (!access.ok()) Fatal(access.status(), "opening accounts");

        auto parsed = kds::parser::Parse(s.buyer_update);
        if (!parsed.ok()) Fatal(parsed.status(), "parsing the update");
        const auto& stmt = std::get<kds::parser::UpdateStmt>(parsed.value());

        std::vector<double> samples;
        for (int i = 0; i < iterations; ++i) {
            const auto t = Clock::now();
            auto compiled =
                kds::exec::CompileWhere(db.catalog(), *access.value(), "accounts", stmt.where);
            samples.push_back(MicrosSince(t));
            if (!compiled.ok()) Fatal(compiled.status(), "compiling the where clause");
        }
        rows.push_back({"CompileWhere (UPDATE)", Percentile(samples, 0.5),
                        Percentile(samples, 0.99), "resolve names -> ColumnRef"});
    }

    // 6. The parse-time fingerprint, which every statement pays when
    //    Waystone recording is on. Off in this run; priced anyway.
    {
        std::vector<double> samples;
        for (int i = 0; i < iterations; ++i) {
            const auto t = Clock::now();
            volatile auto fp = kds::parser::FingerprintOf(s.buy_leg);
            (void)fp;
            samples.push_back(MicrosSince(t));
        }
        rows.push_back({"FingerprintOf (standalone)", Percentile(samples, 0.5),
                        Percentile(samples, 0.99), "free when folded into the parse"});
    }

    PrintTable("Per layer, called directly (lower bounds - see the file header)", rows);
}

// Per phase, **in situ**: the dispatcher marks each layer as the statement
// runs it (the `phaseprof` block in command_dispatcher.cpp) and reports the
// nanoseconds on its debug `[query]` line. This reads them back.
//
// Unlike the direct-call loops above, these numbers do add up - they are a
// partition of the statement's own server-side time, taken by the code that
// spends it.
void MeasurePhases(Instance& db, int accounts, int iterations, const std::string& title) {
    db.log_lines().clear();
    for (int i = 0; i < iterations; ++i) {
        const Statements s = MakeStatements(1 + (i % accounts), i);
        db.Run(s.buy_leg);
        db.Run(s.sell_leg);
        db.Run(s.buyer_update);
        db.Run(s.seller_update);
    }

    // `... "<sql>" -> ... in <n>us ph=scope:<ns>,parse:<ns>,...`
    struct Phase {
        std::string name;
        std::vector<double> insert;
        std::vector<double> update;
    };
    std::vector<Phase> phases;
    std::vector<double> insert_total, update_total;

    for (const std::string& line : db.log_lines()) {
        const std::size_t at = line.find(" ph=");
        if (at == std::string::npos) continue;
        const bool is_insert = line.find("\"INSERT") != std::string::npos;
        const bool is_update = line.find("\"UPDATE") != std::string::npos;
        if (!is_insert && !is_update) continue;

        std::size_t cursor = at + 4;
        std::size_t index = 0;
        double total = 0.0;
        while (cursor < line.size()) {
            const std::size_t colon = line.find(':', cursor);
            if (colon == std::string::npos) break;
            const std::string name = line.substr(cursor, colon - cursor);
            std::size_t comma = line.find(',', colon);
            if (comma == std::string::npos) comma = line.size();
            const double us = std::strtod(line.substr(colon + 1, comma - colon - 1).c_str(),
                                          nullptr) /
                              1000.0;
            if (index >= phases.size()) phases.push_back(Phase{name, {}, {}});
            (is_insert ? phases[index].insert : phases[index].update).push_back(us);
            total += us;
            cursor = comma + 1;
            ++index;
        }
        (is_insert ? insert_total : update_total).push_back(total);
    }

    std::vector<Row> rows;
    for (Phase& phase : phases) {
        if (Percentile(phase.insert, 0.5) == 0.0 && Percentile(phase.update, 0.5) == 0.0) continue;
        rows.push_back({"  " + phase.name + " (INSERT)", Percentile(phase.insert, 0.5),
                        Percentile(phase.insert, 0.99), ""});
        rows.push_back({"  " + phase.name + " (UPDATE)", Percentile(phase.update, 0.5),
                        Percentile(phase.update, 0.99), ""});
    }
    rows.push_back({"TOTAL (INSERT)", Percentile(insert_total, 0.5),
                    Percentile(insert_total, 0.99), "sum of the phases above"});
    rows.push_back({"TOTAL (UPDATE)", Percentile(update_total, 0.5),
                    Percentile(update_total, 0.99), ""});
    PrintTable(title, rows);
}

// ---- Where the tail comes from -------------------------------------------
//
// p50 says what a statement costs; p95 and p99 say what a *client* waits
// for, and on this workload they run 5-50x the median. That gap is either
// **structural** - something the engine does every Nth statement - or it is
// the device and the OS. The two want completely different fixes, so this
// separates them by measurement rather than by argument.
//
// Every statement is timed and tagged with what the engine did underneath
// it, read from outside: how many **pages were allocated** (page store),
// how many **WAL bytes** were written, and whether a **sync** happened. A
// relation growing a page is the structural event to look for - it logs a
// full 8 KB page image (the FPI `LogInsert` writes when a chain grows, and
// which `docs/spec/wal.md` prices at ~+50% log volume) and every 64th one also
// extends the file, since `EnsureCapacity` rounds to a whole extent.
struct Tagged {
    std::size_t index = 0;
    double us = 0.0;
    std::uint32_t pages_allocated = 0;  // during this statement
    std::uint64_t wal_bytes = 0;
    std::uint64_t syncs = 0;
};

void ReportTail(const std::string& what, std::vector<Tagged> samples) {
    if (samples.empty()) return;
    std::vector<Tagged> by_time = samples;
    std::sort(by_time.begin(), by_time.end(),
              [](const Tagged& a, const Tagged& b) { return a.us < b.us; });
    auto pct = [&](double p) {
        return by_time[std::min(by_time.size() - 1,
                                static_cast<std::size_t>(p * by_time.size()))];
    };

    const double p50 = pct(0.5).us;
    std::printf("  %-8s p50 %8.1f  p95 %8.1f  p99 %8.1f  max %8.1f us   p99/p50 = %.0fx\n",
                what.c_str(), p50, pct(0.95).us, pct(0.99).us, by_time.back().us,
                pct(0.99).us / std::max(0.001, p50));

    // The two populations, side by side. If growth statements are the tail,
    // their median alone is the answer and no further attribution is needed.
    std::vector<double> grew, flat;
    for (const Tagged& s : samples) {
        (s.pages_allocated > 0 ? grew : flat).push_back(s.us);
    }
    std::sort(grew.begin(), grew.end());
    std::sort(flat.begin(), flat.end());
    auto median = [](const std::vector<double>& v) {
        return v.empty() ? 0.0 : v[v.size() / 2];
    };
    std::printf("           allocated a page: n=%zu median %.1f us, max %.1f us\n", grew.size(),
                median(grew), grew.empty() ? 0.0 : grew.back());
    std::printf("           allocated none:   n=%zu median %.1f us, max %.1f us\n", flat.size(),
                median(flat), flat.empty() ? 0.0 : flat.back());

    // How much of the tail the structural event actually explains.
    const std::size_t tail_from =
        by_time.size() - std::max<std::size_t>(1, by_time.size() / 20);  // slowest 5%
    std::size_t tail_with_alloc = 0;
    std::uint64_t tail_bytes = 0;
    std::uint64_t body_bytes = 0;
    for (std::size_t i = 0; i < by_time.size(); ++i) {
        if (i >= tail_from) {
            if (by_time[i].pages_allocated > 0) ++tail_with_alloc;
            tail_bytes += by_time[i].wal_bytes;
        } else {
            body_bytes += by_time[i].wal_bytes;
        }
    }
    const std::size_t tail_n = by_time.size() - tail_from;
    std::printf("           slowest 5%%: %zu of %zu allocated a page; mean WAL %llu B "
                "vs %llu B for the rest\n",
                tail_with_alloc, tail_n,
                static_cast<unsigned long long>(tail_n == 0 ? 0 : tail_bytes / tail_n),
                static_cast<unsigned long long>(tail_from == 0 ? 0 : body_bytes / tail_from));

    // The extreme outliers, counted separately from the percentiles: a
    // handful of 10 ms statements in an otherwise 10 us population are not
    // the same phenomenon as a p95 twice the median, and averaging them
    // together is how one gets mistaken for the other.
    std::size_t over_20x = 0;
    std::size_t over_20x_with_alloc = 0;
    for (const Tagged& sample : samples) {
        if (sample.us <= 20.0 * p50) continue;
        ++over_20x;
        if (sample.pages_allocated > 0) ++over_20x_with_alloc;
    }
    std::printf("           beyond 20x the median: %zu of %zu statements (%zu allocated a "
                "page)\n",
                over_20x, samples.size(), over_20x_with_alloc);

    // Periodicity: structural events repeat at a fixed row count, a device
    // stall does not.
    std::vector<std::size_t> slow;
    for (std::size_t i = tail_from; i < by_time.size(); ++i) slow.push_back(by_time[i].index);
    std::sort(slow.begin(), slow.end());
    std::printf("           slowest 5%% at #:");
    for (std::size_t i = 0; i < slow.size() && i < 14; ++i) std::printf(" %zu", slow[i]);
    std::printf("\n           gaps:");
    for (std::size_t i = 1; i < slow.size() && i < 14; ++i) {
        std::printf(" %zu", slow[i] - slow[i - 1]);
    }
    std::printf("\n");
}

void MeasureTails(Instance& db, int accounts, int iterations, const std::string& title) {
    std::vector<Tagged> inserts, updates;
    std::uint32_t pages = db.store().allocated_pages();
    kds::wal::WalManager* wal = db.wal();
    std::uint64_t bytes = wal ? wal->stats().bytes_appended : 0;
    std::uint64_t syncs = wal ? wal->stats().syncs : 0;

    auto run = [&](const std::string& sql, std::vector<Tagged>& into, std::size_t index) {
        const auto t = Clock::now();
        db.Run(sql);
        Tagged sample;
        sample.index = index;
        sample.us = MicrosSince(t);
        const std::uint32_t now_pages = db.store().allocated_pages();
        sample.pages_allocated = now_pages - pages;
        pages = now_pages;
        if (wal != nullptr) {
            sample.wal_bytes = wal->stats().bytes_appended - bytes;
            bytes = wal->stats().bytes_appended;
            sample.syncs = wal->stats().syncs - syncs;
            syncs = wal->stats().syncs;
        }
        into.push_back(sample);
    };

    for (int i = 0; i < iterations; ++i) {
        const Statements s = MakeStatements(1 + (i % accounts), i);
        run(s.buy_leg, inserts, static_cast<std::size_t>(i) * 2);
        run(s.sell_leg, inserts, static_cast<std::size_t>(i) * 2 + 1);
        run(s.buyer_update, updates, static_cast<std::size_t>(i) * 2);
        run(s.seller_update, updates, static_cast<std::size_t>(i) * 2 + 1);
    }

    std::printf("\n%s\n", title.c_str());
    ReportTail("INSERT", std::move(inserts));
    ReportTail("UPDATE", std::move(updates));
}

// The same four statements inside **one** transaction, which is what the
// stress tool deliberately does not do. One commit instead of four is the
// largest single lever on this workload, and it needs no engine change - so
// it is measured rather than asserted.
void MeasureOneTransaction(Instance& db, int accounts, int iterations,
                           const std::string& title) {
    kds::server::Session session;
    std::vector<double> whole;
    for (int i = 0; i < iterations; ++i) {
        const Statements s = MakeStatements(1 + (i % accounts), i);
        const auto t = Clock::now();
        db.dispatcher().Dispatch("BEGIN", &session);
        db.dispatcher().Dispatch(s.buy_leg, &session);
        db.dispatcher().Dispatch(s.sell_leg, &session);
        db.dispatcher().Dispatch(s.buyer_update, &session);
        db.dispatcher().Dispatch(s.seller_update, &session);
        db.dispatcher().Dispatch("COMMIT", &session);
        whole.push_back(MicrosSince(t));
    }
    std::vector<Row> rows;
    rows.push_back({"whole transaction (BEGIN..COMMIT)", Percentile(whole, 0.5),
                    Percentile(whole, 0.99), "one durability point, not four"});
    PrintTable(title, rows);
}

}  // namespace

int main(int argc, char** argv) {
    int accounts = 200;
    int iterations = 400;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--accounts" && i + 1 < argc) accounts = std::atoi(argv[++i]);
        else if (arg == "--iterations" && i + 1 < argc) iterations = std::atoi(argv[++i]);
        else if (arg == "--help") {
            std::printf("usage: kds_txn_bench [--accounts N] [--iterations N]\n");
            return 0;
        }
    }

    ScratchDir scratch;
    std::printf("ckdbs transaction layer benchmark\n");
    std::printf("accounts=%d iterations=%d\n", accounts, iterations);
    // **Printed because getting this wrong invalidates everything below.**
    // The default is the system temp directory, which on many hosts is
    // tmpfs - where fsync is free and every durability class measures the
    // same. Set KDS_BENCH_DIR to a real device. (`df -T <path>` tells you
    // which you got.)
    std::printf("scratch=%s  <- must be a real device, not tmpfs\n",
                scratch.path().c_str());

    // Durability, the axis everything else is measured under. Same
    // statements, same data, four configurations.
    struct Config {
        const char* tag;
        bool logged;
        kds::wal::DurabilityClass durability;
    };
    const Config configs[] = {
        {"unlogged", false, kds::wal::DurabilityClass::kRelaxed},
        {"relaxed", true, kds::wal::DurabilityClass::kRelaxed},
        {"group", true, kds::wal::DurabilityClass::kGroup},
        {"strict", true, kds::wal::DurabilityClass::kStrict},
    };

    for (const Config& config : configs) {
        Instance db(scratch, config.tag, config.logged, config.durability,
                    /*foreign_keys=*/false);
        db.Load(accounts);
        const std::string title =
            std::string("End to end - ") +
            (config.logged ? std::string("wal ") + DurabilityName(config.durability)
                           : std::string("unlogged (no WAL manager)"));
        MeasureEndToEnd(db, accounts, iterations, title);
    }

    // The layer breakdown, taken under the configuration the server ships
    // with (`durability=group`).
    {
        Instance db(scratch, "layers", /*logged=*/true, kds::wal::DurabilityClass::kGroup,
                    /*foreign_keys=*/false);
        db.Load(accounts);
        MeasureLayers(db, iterations);
    }

    // The same phases, in situ, logged and unlogged - because which layer
    // dominates is a different answer under each, and only one of them is
    // what the server ships with.
    {
        Instance db(scratch, "phases-group", /*logged=*/true, kds::wal::DurabilityClass::kGroup,
                    /*foreign_keys=*/false, /*capture_phases=*/true);
        db.Load(accounts);
        MeasurePhases(db, accounts, iterations, "Per phase, in situ - wal group (as shipped)");
    }
    {
        Instance db(scratch, "phases-unlogged", /*logged=*/false,
                    kds::wal::DurabilityClass::kRelaxed, /*foreign_keys=*/false,
                    /*capture_phases=*/true);
        db.Load(accounts);
        MeasurePhases(db, accounts, iterations, "Per phase, in situ - unlogged (no WAL manager)");
    }

    // The tail, under both configurations: with the device in the loop and
    // without it, because a tail that survives `unlogged` is the engine's.
    {
        Instance db(scratch, "tails-group", /*logged=*/true, kds::wal::DurabilityClass::kGroup,
                    /*foreign_keys=*/false, /*capture_phases=*/true);
        db.Load(accounts);
        MeasureTails(db, accounts, iterations, "Tail attribution - wal group (as shipped)");
    }
    {
        Instance db(scratch, "tails-unlogged", /*logged=*/false,
                    kds::wal::DurabilityClass::kRelaxed, /*foreign_keys=*/false,
                    /*capture_phases=*/true);
        db.Load(accounts);
        MeasureTails(db, accounts, iterations, "Tail attribution - unlogged (no device sync)");
    }

    // One transaction instead of four autocommits.
    {
        Instance db(scratch, "one-txn", /*logged=*/true, kds::wal::DurabilityClass::kGroup,
                    /*foreign_keys=*/false);
        db.Load(accounts);
        MeasureOneTransaction(db, accounts, iterations,
                              "One BEGIN..COMMIT around all four statements - wal group");
    }

    // What the foreign keys added: the same four statements against a
    // schema that declares the two relationships the data already has.
    {
        Instance db(scratch, "fk", /*logged=*/true, kds::wal::DurabilityClass::kGroup,
                    /*foreign_keys=*/true);
        db.Load(accounts);
        MeasureEndToEnd(db, accounts, iterations,
                        "End to end - wal group, foreign keys declared and enforced");
    }

    return 0;
}
