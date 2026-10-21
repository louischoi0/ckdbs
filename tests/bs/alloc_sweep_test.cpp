// BS-S3's allocation sweep (instructions/v3.0.0/workorder-bs-test-hardening.md).
//
// For each statement kind: fail its Nth allocation in a forked child, for a
// fixed budget of N (BS-A5: the first and last few and an even spread), and
// assert `sched.md` §3's fail-stop and what follows it -
//
//   1. the statement never comes back: the child stops at the allocation
//      (`base::InstallAllocationFailStop`, SIGABRT) or the cell fails;
//   2. the next mount recovers the files the stop left, and the statement's
//      relation reads as it stood **before** the statement or **after** it,
//      never anything else. Both are legal: `strict` makes every earlier
//      commit durable, and a stop after the statement's own commit record
//      reached the log is a statement that committed.
//
// One forked process per failure point, because a fail-stop ends the process
// (the census, `bs-s0-census.md` §3). A BS cell: labelled `bs-nightly`, so the
// push hook never runs it (§6.1).

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "alloc_counter.hpp"
#include "file_rig_crash.hpp"
#include "kds/base/fail_stop.hpp"

namespace kds::server {
namespace {

using test_support::CountAllocations;
using test_support::FailNthAllocation;

struct Kind {
    const char* name;
    std::vector<std::string> setup;
    std::string statement;
    // What must read as before or after: every reply as it was before the
    // statement, or every one as it was after it - an index probe beside the
    // walk it must agree with, a catalog view beside a read through the name.
    std::vector<std::string> checks;
};

// Points swept per kind: the first and last few, where setup and teardown
// paths live, and an even spread between.
constexpr std::size_t kEdge = 4;
constexpr std::size_t kSpread = 24;

std::vector<std::size_t> PointsOf(std::size_t total) {
    std::set<std::size_t> points;
    for (std::size_t n = 1; n <= std::min(total, kEdge); ++n) points.insert(n);
    for (std::size_t n = total > kEdge ? total - kEdge + 1 : 1; n <= total; ++n) points.insert(n);
    for (std::size_t i = 1; i <= kSpread && total > 0; ++i) {
        points.insert(std::max<std::size_t>(1, total * i / (kSpread + 1)));
    }
    return {points.begin(), points.end()};
}

// Every statement `strict`, so what answered is in the log's file.
std::string Run(CommandDispatcher& d, Session& s, const std::string& sql) {
    return d.Dispatch(sql, &s).response;
}

// The checks' replies, one after another; empty if one of them is refused.
std::string ReadChecks(CommandDispatcher& d, Session* s, const Kind& kind) {
    std::string all;
    for (const std::string& sql : kind.checks) {
        const std::string reply = d.Dispatch(sql, s).response;
        if (crash_rig::StartsWith(reply, "ERR")) return {};
        all += sql + " -> " + reply + "\n";
    }
    return all;
}

std::unique_ptr<TwoCoreRig> OpenAt(const std::filesystem::path& dir) {
    TwoCoreRig::Options options;
    options.file_backed = true;
    options.dir = dir;
    auto opened = TwoCoreRig::Open(options);
    return opened.ok() ? std::move(opened.value()) : nullptr;
}

// Exit codes a child reports when it does not stop where it should.
constexpr int kChildOpenFailed = 2;
constexpr int kChildSetupFailed = 3;
constexpr int kChildCameBack = 4;
// The child made fewer allocations than the reference run did, so its Nth
// never came: a point past the child's count, not a statement that
// survived a failure. A process's first statements warm what later ones
// reuse, so the two counts can differ by an allocation or two.
constexpr int kChildNeverReached = 5;

[[noreturn]] void Child(const Kind& kind, const std::filesystem::path& dir, std::size_t n) {
    const rlimit no_core{0, 0};
    ::setrlimit(RLIMIT_CORE, &no_core);  // a thousand aborts, no core files
    // Where the fail-stop's line lands, for the parent to read: an abort of
    // any other kind is not the stop being asked for.
    if (std::freopen((dir / "child.err").c_str(), "w", stderr) == nullptr) ::_exit(kChildOpenFailed);
    auto rig = OpenAt(dir);
    if (rig == nullptr) ::_exit(kChildOpenFailed);
    CommandDispatcher& d = rig->core(0).dispatcher();
    Session s;
    s.set_durability(wal::DurabilityClass::kStrict);
    for (const std::string& sql : kind.setup) {
        if (crash_rig::StartsWith(Run(d, s, sql), "ERR")) ::_exit(kChildSetupFailed);
    }
    // The reference run read the check before its counted statement; so does
    // the child, or the Nth allocation is counted over another history.
    if (ReadChecks(d, &s, kind).empty()) ::_exit(kChildSetupFailed);
    base::InstallAllocationFailStop();
    {
        FailNthAllocation fail(n);
        (void)Run(d, s, kind.statement);
        if (test_support::g_fail_after != 0) ::_exit(kChildNeverReached);
    }
    ::_exit(kChildCameBack);  // reaching here is the defect: the statement came back
}

void Sweep(const Kind& kind) {
    // The reference run: the allocation count, and both legal states.
    std::string before;
    std::string after;
    std::size_t total = 0;
    {
        crash_rig::TempDir dir;
        auto rig = OpenAt(dir.path);
        ASSERT_NE(rig, nullptr);
        CommandDispatcher& d = rig->core(0).dispatcher();
        Session s;
        s.set_durability(wal::DurabilityClass::kStrict);
        for (const std::string& sql : kind.setup) {
            ASSERT_FALSE(crash_rig::StartsWith(Run(d, s, sql), "ERR")) << sql;
        }
        before = ReadChecks(d, &s, kind);
        ASSERT_FALSE(before.empty()) << "a check is refused before the statement";
        std::string reply;
        {
            CountAllocations counting;
            reply = Run(d, s, kind.statement);
            total = counting.count();
        }
        ASSERT_FALSE(crash_rig::StartsWith(reply, "ERR")) << kind.statement << " -> " << reply;
        after = ReadChecks(d, &s, kind);
        ASSERT_FALSE(after.empty()) << "a check is refused after the statement";
    }
    ASSERT_GT(total, 0u) << kind.statement << " allocated nothing to fail";

    std::size_t stopped = 0;
    std::size_t unreached = 0;
    for (const std::size_t n : PointsOf(total)) {
        SCOPED_TRACE(std::string(kind.name) + " allocation " + std::to_string(n) + " of " +
                     std::to_string(total));
        crash_rig::TempDir dir;
        const pid_t pid = ::fork();
        ASSERT_GE(pid, 0);
        if (pid == 0) Child(kind, dir.path, n);
        int status = 0;
        ASSERT_EQ(::waitpid(pid, &status, 0), pid);
        if (WIFEXITED(status) && WEXITSTATUS(status) == kChildNeverReached) {
            ++unreached;
            continue;
        }
        ASSERT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT)
            << "the child did not stop at the allocation: "
            << (WIFEXITED(status) ? "exit " + std::to_string(WEXITSTATUS(status))
                                  : "signal " + std::to_string(WTERMSIG(status)));
        {
            std::ifstream err(dir.path / "child.err");
            const std::string text((std::istreambuf_iterator<char>(err)),
                                   std::istreambuf_iterator<char>());
            ASSERT_NE(text.find("allocation failed; stopping"), std::string::npos)
                << "the child aborted, but not at the fail-stop: " << text;
        }
        ++stopped;

        auto mounted = crash_rig::Mount(dir.path);
        ASSERT_TRUE(mounted.ok()) << "the mount refused the stop: " << mounted.status().message();
        const std::string seen = ReadChecks(mounted.value()->dispatcher(), nullptr, kind);
        EXPECT_TRUE(seen == before || seen == after)
            << "neither before nor after the statement:\n  seen   " << seen << "\n  before "
            << before << "\n  after  " << after;
    }
    EXPECT_GT(stopped, 0u);
    // Only the last few points may lie past the child's count.
    EXPECT_LE(unreached, kEdge) << unreached << " points past the child's allocation count";
}

const std::vector<std::string> kRows = {
    "CREATE TABLE t (id int64, v int64, s varchar NULL) BTREE",
    "INSERT INTO t VALUES (1, 10, 'a'), (2, 20, 'b'), (3, 30, 'c'), (4, 40, 'd')",
};

std::vector<std::string> With(std::vector<std::string> base, std::vector<std::string> more) {
    base.insert(base.end(), more.begin(), more.end());
    return base;
}

TEST(BsAllocSweep, AnInsert) {
    Sweep({"insert", kRows, "INSERT INTO t VALUES (5, 50, 'e')", {"SELECT * FROM t"}});
}

TEST(BsAllocSweep, AMultiRowInsert) {
    Sweep({"multi-row insert", kRows,
           "INSERT INTO t VALUES (5, 50, 'e'), (6, 60, 'f'), (7, 70, 'g'), (8, 80, 'h')",
           {"SELECT * FROM t"}});
}

TEST(BsAllocSweep, AnUpdateByKey) {
    Sweep({"update by key", kRows, "UPDATE t SET v = v + 1 WHERE id = 2", {"SELECT * FROM t"}});
}

TEST(BsAllocSweep, AnUpdateThatSpills) {
    // A value past the inline cell: the var-heap's path.
    Sweep({"update that spills", kRows,
           "UPDATE t SET s = '" + std::string(600, 'x') + "' WHERE id = 3", {"SELECT * FROM t"}});
}

TEST(BsAllocSweep, ADeleteByPredicate) {
    Sweep({"delete by predicate", kRows, "DELETE FROM t WHERE v >= 20",
           {"SELECT * FROM t", "SELECT COUNT(*) FROM t"}});
}

TEST(BsAllocSweep, AnInsertUnderAnIndex) {
    Sweep({"insert under an index", With(kRows, {"CREATE INDEX tv ON t (v)"}),
           "INSERT INTO t VALUES (5, 25, 'e')",
           {"SELECT * FROM t", "SELECT id FROM t WHERE v BETWEEN 20 AND 30"}});
}

TEST(BsAllocSweep, AChildInsertUnderAForeignKey) {
    Sweep({"child insert under a foreign key",
           With(kRows, {"CREATE TABLE c (id int64, pid int64 REFERENCES t) BTREE"}),
           "INSERT INTO c VALUES (1, 2)", {"SELECT * FROM c", "SELECT * FROM t"}});
}

TEST(BsAllocSweep, ACreateIndex) {
    Sweep({"create index", kRows, "CREATE INDEX tv ON t (v)",
           {"SELECT name FROM sys.indexes", "SELECT id FROM t WHERE v = 20", "SELECT * FROM t"}});
}

TEST(BsAllocSweep, ACreateTable) {
    Sweep({"create table", kRows, "CREATE TABLE u (id int64, w int64) BTREE",
           {"SELECT name FROM sys.tables", "SELECT * FROM t"}});
}

TEST(BsAllocSweep, AJoinWithAnAggregate) {
    // A read: nothing it does may change what the mount reads.
    Sweep({"join with an aggregate",
           With(kRows, {"CREATE TABLE c (id int64, pid int64) BTREE",
                        "INSERT INTO c VALUES (1, 1), (2, 1), (3, 2)"}),
           "SELECT t.v, COUNT(*) FROM t JOIN c AS c ON t.id = c.pid GROUP BY t.v",
           {"SELECT * FROM t", "SELECT * FROM c"}});
}

}  // namespace
}  // namespace kds::server
