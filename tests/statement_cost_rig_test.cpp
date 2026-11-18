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
    // One row for the shape both cores ran, with the sums.
    const stats::StatementShape* read = ShapeOf(shapes, "SELECT", select);
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(read->calls, 2u * kRounds);
    EXPECT_EQ(read->rows, 2u * kRounds);
    EXPECT_EQ(read->refused, 0u);
    EXPECT_EQ(read->executed_calls, 2u * kRounds);
    EXPECT_GE(read->rows_examined, 2u * kRounds);
    std::uint64_t in_buckets = 0;
    for (std::uint64_t n : read->latency) in_buckets += n;
    EXPECT_EQ(in_buckets, read->calls) << "a shape's histogram sums to its calls";

    // The handed-on walk keeps what its head compiled and what its end
    // counted: the outcome is replaced by the walk's reply in between.
    const stats::StatementShape* walked = ShapeOf(shapes, "SELECT", scan);
    ASSERT_NE(walked, nullptr);
    EXPECT_EQ(walked->calls, static_cast<std::uint64_t>(kRounds));
    EXPECT_EQ(walked->rows, 2u * kRounds) << "two rows a call";
    EXPECT_EQ(walked->executed_calls, static_cast<std::uint64_t>(kRounds));
    EXPECT_GE(walked->rows_examined, 2u * kRounds);
    EXPECT_EQ(walked->compiled_calls, static_cast<std::uint64_t>(kRounds));
    EXPECT_EQ(walked->timed_calls, static_cast<std::uint64_t>(kRounds));

    const stats::StatementShape* write =
        ShapeOf(shapes, "UPDATE", "UPDATE r SET v = v + 1 WHERE id = 2");
    ASSERT_NE(write, nullptr);
    EXPECT_EQ(write->calls, static_cast<std::uint64_t>(kRounds));
    EXPECT_EQ(write->rows, static_cast<std::uint64_t>(kRounds));

    const stats::StatementShape* refused =
        ShapeOf(shapes, "INSERT", "INSERT INTO r (id, v) VALUES (1, 9)");
    ASSERT_NE(refused, nullptr);
    EXPECT_EQ(refused->refused, static_cast<std::uint64_t>(kRounds));
    EXPECT_EQ(refused->conflicts, 0u) << "a bound key is no lost race";

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
