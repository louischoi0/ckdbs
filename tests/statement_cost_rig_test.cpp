// **What a statement shape costs, recorded by two cores at once** (DF-S1,
// `instructions/v3.2.0/workorder-df-statistics-coverage.md` §2 part A, §5).
//
// Two claims no single-dispatcher cell reaches, on the two-core rig's real
// reactors over one log:
//
//   1. A statement that runs inside a task of its own session is timed by
//      that session's statistics and owns the log it appended (DF-Q6 (a),
//      DF-Q12 (a)), and one the test thread dispatches outside any task is
//      counted and not timed (DF-R7).
//   2. Both cores record one shape, and refusals, while each reads the three
//      views: a shape is one row with the sums, its histogram sums to its
//      calls, and a refusal is its core's row. Armed for `tsan`: the views
//      read every core's part while its core writes it.

#include "two_core_rig.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/parser/fingerprint.hpp"
#include "kds/server/session.hpp"
#include "kds/stats/refusal_stats.hpp"
#include "kds/stats/statement_stats.hpp"
#include "kds/wire/error_registry.hpp"

namespace kds::server {
namespace {

using namespace std::chrono_literals;

struct Script {
    Session session;
    std::vector<std::string> lines;
    std::vector<std::string> replies;
    std::atomic<bool> done{false};
};

sched::Coro RunScript(CommandDispatcher& d, Script& s) {
    for (const std::string& line : s.lines) {
        DispatchOutcome out;
        co_await d.DispatchAsync(line, &s.session, &out);
        s.replies.push_back(std::move(out.response));
    }
    s.done.store(true, std::memory_order_release);
    co_return Status::OK();
}

// Submits `script` on `core` as a task of the script's own session - the
// arrangement the listener makes for a connection (`tcp_server.cpp`).
void SubmitAsItsSession(TwoCoreRig& rig, std::uint32_t core, Script& script) {
    auto task = sched::MakeCoroTask(sched::SchedulingGroup::kForeground,
                                    RunScript(rig.core(core).dispatcher(), script));
    task->wait_state_mut().session = script.session.WaitStats();
    rig.core(core).scheduler().Submit(std::move(task));
}

const stats::StatementShape* ShapeOf(const std::vector<stats::StatementShape>& shapes,
                                     std::string_view kind, std::string_view statement) {
    const auto fingerprint = parser::FingerprintOf(statement);
    const std::uint64_t fetch_id = fingerprint.has_value() ? fingerprint->fetch_id : 0;
    for (const stats::StatementShape& s : shapes) {
        if (s.kind == kind && s.fetch_id == fetch_id) return &s;
    }
    return nullptr;
}

class StatementCostRig : public ::testing::Test {
protected:
    void SetUp() override {
        auto opened = TwoCoreRig::Open({});
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        rig_ = std::move(opened.value());
        ASSERT_EQ(rig_->core(0)
                      .dispatcher()
                      .Dispatch("CREATE TABLE r (id int64, v int64) BTREE")
                      .response.substr(0, 3),
                  "CRE");
        ASSERT_EQ(rig_->core(0)
                      .dispatcher()
                      .Dispatch("INSERT INTO r (id, v) VALUES (1, 1), (2, 2)")
                      .response.rfind("INSERTED", 0),
                  0u);
        ASSERT_TRUE(rig_->store().Flush().ok());
        stats::StatementStats::ResetForTest();
        stats::RefusalStats::ResetForTest();
    }
    void TearDown() override {
        if (rig_ != nullptr) rig_->Stop();
        stats::StatementStats::ResetForTest();
        stats::RefusalStats::ResetForTest();
    }

    // Declared before the rig and so destroyed after it: a cell that fails
    // early leaves its reactors running until `TearDown` stops them, and a
    // script's session must outlive the task that books to it.
    Script scripts_[2];
    std::unique_ptr<TwoCoreRig> rig_;
};

#ifndef KDS_WAIT_INSTRUMENTATION_OFF
TEST_F(StatementCostRig, AStatementInItsSessionsTaskOwnsItsTimeAndTheLogItAppended) {
    constexpr int kInserts = 4;
    const std::string insert = "INSERT INTO r (id, v) VALUES (10, 10)";
    // Outside any task, from this thread: counted, with no time and no log.
    ASSERT_EQ(rig_->core(0).dispatcher().Dispatch(insert).response.rfind("INSERTED", 0), 0u);

    Script& script = scripts_[1];
    for (int i = 1; i <= kInserts; ++i) {
        script.lines.push_back("INSERT INTO r (id, v) VALUES (" + std::to_string(10 + i) + ", 1)");
    }
    // Core 1's manager counts what core 1 appended to the one stream.
    const wal::WalStats& log = rig_->core(1).wal().stats();
    const std::uint64_t log_before = log.bytes_appended;
    SubmitAsItsSession(*rig_, 1, script);
    rig_->Start();
    ASSERT_TRUE(KickUntil(*rig_, 1, [&] { return script.done.load(std::memory_order_acquire); },
                          8000ms));
    rig_->Stop();
    for (const std::string& reply : script.replies) {
        ASSERT_EQ(reply.rfind("INSERTED", 0), 0u) << reply;
    }
    const std::uint64_t log_appended = log.bytes_appended - log_before;

    const std::vector<stats::StatementShape> shapes = stats::StatementStats::Snapshot();
    const stats::StatementShape* shape = ShapeOf(shapes, "INSERT", insert);
    ASSERT_NE(shape, nullptr);
    EXPECT_EQ(shape->calls, static_cast<std::uint64_t>(kInserts) + 1);
    EXPECT_EQ(shape->rows, static_cast<std::uint64_t>(kInserts) + 1);
    EXPECT_EQ(shape->timed_calls, static_cast<std::uint64_t>(kInserts))
        << "the call outside a task is in no timed sum";
    EXPECT_GT(shape->timed.on_cpu_ns, 0u);
    // Each autocommit INSERT appends at least its row's record and its
    // commit's, and nothing a statement appended is more than the log took.
    EXPECT_GE(shape->timed.log_records, 2u * kInserts);
    EXPECT_GT(shape->timed.log_bytes, 0u);
    EXPECT_LE(shape->timed.log_bytes, log_appended);
}

TEST_F(StatementCostRig, TwoSessionsOfOneCoreEachOwnTheLogTheyAppended) {
    // DF-Q12 (a)'s point against a per-core delta: two sessions of one core
    // whose `group` commits park, so each statement's span holds the
    // other's appends. Booked to the polled task's session, the two shapes'
    // logs add up to what the core appended; read off the core, each would
    // also hold the other's and the sum would pass it.
    ASSERT_EQ(rig_->core(0)
                  .dispatcher()
                  .Dispatch("CREATE TABLE q (id int64, v int64) BTREE")
                  .response.substr(0, 3),
              "CRE");
    ASSERT_TRUE(rig_->store().Flush().ok());
    stats::StatementStats::ResetForTest();
    constexpr int kRounds = 12;
    const std::string narrow = "INSERT INTO r (id, v) VALUES (100, 1)";
    const std::string wide = "INSERT INTO q (id, v) VALUES (100, 1), (101, 1), (102, 1), (103, 1)";
    for (Script& script : scripts_) script.session.set_durability(wal::DurabilityClass::kGroup);
    for (int i = 0; i < kRounds; ++i) {
        const std::string id = std::to_string(100 + 4 * i);
        scripts_[0].lines.push_back("INSERT INTO r (id, v) VALUES (" + id + ", 1)");
        std::string rows;
        for (int k = 0; k < 4; ++k) {
            rows += (k == 0 ? "(" : ", (") + std::to_string(100 + 4 * i + k) + ", 1)";
        }
        scripts_[1].lines.push_back("INSERT INTO q (id, v) VALUES " + rows);
    }
    const wal::WalStats& log = rig_->core(1).wal().stats();
    const std::uint64_t bytes_before = log.bytes_appended;
    const std::uint64_t records_before = log.records_appended;
    SubmitAsItsSession(*rig_, 1, scripts_[0]);
    SubmitAsItsSession(*rig_, 1, scripts_[1]);
    rig_->Start();
    ASSERT_TRUE(KickUntil(
        *rig_, 1,
        [&] {
            return scripts_[0].done.load(std::memory_order_acquire) &&
                   scripts_[1].done.load(std::memory_order_acquire);
        },
        15000ms));
    rig_->Stop();
    for (const Script& script : scripts_) {
        for (const std::string& reply : script.replies) {
            ASSERT_EQ(reply.rfind("INSERTED", 0), 0u) << reply;
        }
    }

    const std::vector<stats::StatementShape> shapes = stats::StatementStats::Snapshot();
    const stats::StatementShape* a = ShapeOf(shapes, "INSERT", narrow);
    const stats::StatementShape* b = ShapeOf(shapes, "INSERT", wide);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(a, b);
    EXPECT_EQ(a->timed_calls, static_cast<std::uint64_t>(kRounds));
    EXPECT_EQ(b->timed_calls, static_cast<std::uint64_t>(kRounds));
    EXPECT_EQ(a->timed.log_records + b->timed.log_records, log.records_appended - records_before);
    EXPECT_EQ(a->timed.log_bytes + b->timed.log_bytes, log.bytes_appended - bytes_before);
    EXPECT_GT(b->timed.log_bytes, a->timed.log_bytes) << "four rows a statement against one";
    // Each `group` commit parked for its record to be durable: Commit time.
    EXPECT_GT(a->timed.class_ns[static_cast<std::size_t>(stats::WaitClass::kCommit)], 0u);
}
#endif

TEST_F(StatementCostRig, OneTextUnderTwoNamespacesOnTwoCoresIsARowABinding) {
    // DI-S1 (`stats/statement_binding.hpp`) on the real stack: a lock table,
    // a transaction manager and its views, each statement a task of its
    // session through `DispatchAsync`. One text, run under `USE east` on
    // core 0 and under `USE west` on core 1, is two statements and two
    // rows - a point read answered inline and a walk handed on, whose reply
    // is rebuilt over the outcome that carried the binding.
    CommandDispatcher& d = rig_->core(0).dispatcher();
    for (const char* sql : {"CREATE NAMESPACE east", "CREATE NAMESPACE west",
                            "CREATE TABLE east.acct (id int64, v int64) BTREE",
                            "CREATE TABLE west.acct (id int64, v int64) BTREE",
                            "INSERT INTO east.acct (id, v) VALUES (1, 10)",
                            "INSERT INTO west.acct (id, v) VALUES (1, 20), (2, 21), (3, 22)"}) {
        const std::string reply = d.Dispatch(sql).response;
        ASSERT_NE(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
    }
    ASSERT_TRUE(rig_->store().Flush().ok());
    stats::StatementStats::ResetForTest();

    const std::string point = "SELECT v FROM acct WHERE id = 1";
    const std::string walk = "SELECT id FROM acct WHERE v >= 0";
    scripts_[0].lines = {"USE east", point, walk, point};
    scripts_[1].lines = {"USE west", point, walk, walk, point, point};
    SubmitAsItsSession(*rig_, 0, scripts_[0]);
    SubmitAsItsSession(*rig_, 1, scripts_[1]);
    rig_->Start();
    for (std::uint32_t core = 0; core < 2; ++core) {
        ASSERT_TRUE(KickUntil(
            *rig_, core, [&] { return scripts_[core].done.load(std::memory_order_acquire); },
            15000ms))
            << "core " << core;
    }
    rig_->Stop();
    for (const Script& script : scripts_) {
        for (std::size_t i = 0; i < script.lines.size(); ++i) {
            ASSERT_NE(script.replies[i].rfind("ERR", 0), 0u)
                << script.lines[i] << ": " << script.replies[i];
        }
    }

    // {calls, rows} of each binding's row, `east` first: its namespace is
    // the older oid.
    const auto rows_of = [](std::string_view statement) {
        const std::uint64_t fetch_id = parser::FingerprintOf(statement)->fetch_id;
        std::vector<stats::StatementShape> rows;
        for (stats::StatementShape& s : stats::StatementStats::Snapshot()) {
            if (s.kind == "SELECT" && s.fetch_id == fetch_id) rows.push_back(std::move(s));
        }
        return rows;
    };
    const std::vector<stats::StatementShape> points = rows_of(point);
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(points[0].calls, 2u);
    EXPECT_EQ(points[0].rows, 2u);
    EXPECT_EQ(points[1].calls, 3u);
    EXPECT_EQ(points[1].rows, 3u);
    const std::vector<stats::StatementShape> walks = rows_of(walk);
    ASSERT_EQ(walks.size(), 2u);
    EXPECT_EQ(walks[0].calls, 1u);
    EXPECT_EQ(walks[0].rows, 1u);
    EXPECT_EQ(walks[1].calls, 2u);
    EXPECT_EQ(walks[1].rows, 6u);
    for (const std::vector<stats::StatementShape>* rows : {&points, &walks}) {
        for (const stats::StatementShape& row : *rows) {
            EXPECT_EQ(row.binding.n, 1u);
            EXPECT_NE(row.binding.namespace_oid, 0u);
        }
        EXPECT_NE((*rows)[0].binding.oids[0], (*rows)[1].binding.oids[0]);
        EXPECT_LT((*rows)[0].binding.namespace_oid, (*rows)[1].binding.namespace_oid);
    }
}

TEST_F(StatementCostRig, AConflictArmsSubqueryIsBoundWhetherOrNotARowConflicts) {
    // The arm runs its `SET` subquery at each conflicting row (DI-S1's
    // review, finding 3). What the statement bound must not follow that:
    // the same text is one statement whether its key was free or taken.
    CommandDispatcher& d = rig_->core(0).dispatcher();
    ASSERT_EQ(d.Dispatch("CREATE TABLE u (id int64, x int64) BTREE").response.substr(0, 3), "CRE");
    ASSERT_EQ(d.Dispatch("INSERT INTO u (id, x) VALUES (1, 7)").response.rfind("INSERTED", 0), 0u);
    stats::StatementStats::ResetForTest();
    const std::string upsert =
        "INSERT INTO r (id, v) VALUES (50, 1) "
        "ON CONFLICT (id) DO UPDATE SET v = (SELECT x FROM u WHERE id = 1)";
    const DispatchOutcome free_key = d.Dispatch(upsert);   // the arm did not run
    const DispatchOutcome taken_key = d.Dispatch(upsert);  // the arm ran
    ASSERT_EQ(free_key.response.rfind("ERR", 0), std::string::npos) << free_key.response;
    ASSERT_EQ(taken_key.response.rfind("ERR", 0), std::string::npos) << taken_key.response;
    EXPECT_EQ(d.Dispatch("SELECT v FROM r WHERE id = 50").response, "v\\n7") << "the arm ran";

    EXPECT_EQ(free_key.binding.n, 2u) << "the target, then the arm's subquery's relation";
    EXPECT_TRUE(free_key.binding == taken_key.binding);
    // Kept: the row found points into it.
    const std::vector<stats::StatementShape> shapes = stats::StatementStats::Snapshot();
    const stats::StatementShape* shape = ShapeOf(shapes, "INSERT", upsert);
    ASSERT_NE(shape, nullptr);
    EXPECT_EQ(shape->calls, 2u) << "one row for both";
}

TEST_F(StatementCostRig, BothCoresRecordOneShapeAndRefusalsWhileEachReadsTheViews) {
    constexpr int kRounds = 40;
    const std::string select = "SELECT v FROM r WHERE id = 1";
    // A walk, which `DispatchAsync` hands on and runs after the head returns.
    const std::string scan = "SELECT id FROM r WHERE v >= 0";
    const std::string views[] = {"SELECT * FROM sys.statement_stats",
                                 "SELECT * FROM sys.statement_histogram",
                                 "SELECT * FROM sys.refusal_stats"};
    Script (&scripts)[2] = scripts_;
    for (int round = 0; round < kRounds; ++round) {
        for (Script& script : scripts) {
            script.lines.push_back(select);
            script.lines.push_back(views[round % 3]);
        }
        scripts[0].lines.push_back(scan);
        // Core 1 also writes, and is refused a key that is bound.
        scripts[1].lines.push_back("UPDATE r SET v = v + 1 WHERE id = 2");
        scripts[1].lines.push_back("INSERT INTO r (id, v) VALUES (1, 9)");
    }
    SubmitAsItsSession(*rig_, 0, scripts[0]);
    SubmitAsItsSession(*rig_, 1, scripts[1]);
    rig_->Start();
    for (std::uint32_t core = 0; core < 2; ++core) {
        ASSERT_TRUE(KickUntil(
            *rig_, core, [&] { return scripts[core].done.load(std::memory_order_acquire); },
            15000ms))
            << "core " << core;
    }
    rig_->Stop();

    std::uint64_t refused_replies = 0;
    for (const Script& script : scripts) {
        for (std::size_t i = 0; i < script.lines.size(); ++i) {
            const bool refused = script.replies[i].rfind("ERR", 0) == 0;
            if (script.lines[i].rfind("INSERT", 0) == 0) {
                EXPECT_TRUE(refused) << script.replies[i];
                refused_replies += refused;
            } else {
                EXPECT_FALSE(refused) << script.lines[i] << ": " << script.replies[i];
            }
        }
    }
    ASSERT_EQ(refused_replies, static_cast<std::uint64_t>(kRounds));

    const std::vector<stats::StatementShape> shapes = stats::StatementStats::Snapshot();
    // **A record that met the other core's view read was not waited for**
    // (`docs/rules/rules.md` §3a, DL-S2): it is counted busy and is in no
    // shape. So every statement of both scripts is in a shape or in that
    // count, and a shape's calls fall short of what ran by no more than it.
    const std::uint64_t busy = stats::StatementStats::Busy();
    std::uint64_t recorded = 0;
    for (const stats::StatementShape& shape : shapes) recorded += shape.calls;
    EXPECT_EQ(recorded + busy, scripts[0].lines.size() + scripts[1].lines.size())
        << "a statement neither recorded nor counted busy";
    const auto ran = [&](const stats::StatementShape& shape, std::uint64_t statements) {
        EXPECT_LE(shape.calls, statements) << shape.shape_text;
        EXPECT_GE(shape.calls + busy, statements) << shape.shape_text;
    };

    // One row for the shape both cores ran, with the sums.
    const stats::StatementShape* read = ShapeOf(shapes, "SELECT", select);
    ASSERT_NE(read, nullptr);
    ran(*read, 2u * kRounds);
    EXPECT_EQ(read->rows, read->calls);
    EXPECT_EQ(read->refused, 0u);
    EXPECT_EQ(read->executed_calls, read->calls);
    EXPECT_GE(read->rows_examined, read->calls);
    std::uint64_t in_buckets = 0;
    for (std::uint64_t n : read->latency) in_buckets += n;
    EXPECT_EQ(in_buckets, read->calls) << "a shape's histogram sums to its calls";

    // The handed-on walk keeps what its head compiled and what its end
    // counted: the outcome is replaced by the walk's reply in between.
    const stats::StatementShape* walked = ShapeOf(shapes, "SELECT", scan);
    ASSERT_NE(walked, nullptr);
    ran(*walked, static_cast<std::uint64_t>(kRounds));
    EXPECT_EQ(walked->rows, 2u * walked->calls) << "two rows a call";
    EXPECT_EQ(walked->executed_calls, walked->calls);
    EXPECT_GE(walked->rows_examined, 2u * walked->calls);
    EXPECT_EQ(walked->compiled_calls, walked->calls);
    EXPECT_EQ(walked->timed_calls, walked->calls);

    const stats::StatementShape* write =
        ShapeOf(shapes, "UPDATE", "UPDATE r SET v = v + 1 WHERE id = 2");
    ASSERT_NE(write, nullptr);
    ran(*write, static_cast<std::uint64_t>(kRounds));
    EXPECT_EQ(write->rows, write->calls);

    const stats::StatementShape* refused =
        ShapeOf(shapes, "INSERT", "INSERT INTO r (id, v) VALUES (1, 9)");
    ASSERT_NE(refused, nullptr);
    ran(*refused, static_cast<std::uint64_t>(kRounds));
    EXPECT_EQ(refused->refused, refused->calls);
    EXPECT_EQ(refused->conflicts, 0u) << "a bound key is no lost race";

    // The refusal counts take no latch, so none is lost to a reader.
    // The refusals are core 1's, under the code its client was told.
    const std::vector<stats::RefusalCount> refusals = stats::RefusalStats::Snapshot();
    ASSERT_EQ(refusals.size(), 1u);
    EXPECT_EQ(refusals[0].core, 1u);
    EXPECT_EQ(refusals[0].code,
              static_cast<std::uint16_t>(wire::CategoryOf(StatusCode::kAlreadyExists)));
    EXPECT_EQ(refusals[0].count, static_cast<std::uint64_t>(kRounds));
}

}  // namespace
}  // namespace kds::server
