// BS-S7: the multi-session history checker (instructions/v3.0.0/workorder-bs-test-hardening.md).
//
// Two halves. The checker's own cells feed it histories with one known
// anomaly each, so a checker that cannot fail is caught (a gate that cannot
// fail is not a gate). The live cells run four sessions, two per core, on a
// started `TwoCoreRig`, each a seeded script of list-append transactions over
// a few keys - point and range reads, point and range appends, so the lock
// family's row and key-window units are both met - record every answer, and
// hand the history to the checker under the level they ran at. A BS cell:
// labelled `bs-nightly`, never in the push gate (§6.1).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "file_rig_crash.hpp"
#include "history.hpp"
#include "sim/rng.hpp"
#include "two_core_rig.hpp"
#include "kds/sched/coro.hpp"

namespace kds::server {
namespace {

using history::Op;
using history::Outcome;
using history::Txn;
using List = std::vector<std::uint64_t>;

Op Append(std::uint64_t key, std::uint64_t value) { return Op{true, key, value, {}}; }
Op Read(std::uint64_t key, List seen) { return Op{false, key, 0, std::move(seen)}; }

std::vector<std::string> Kinds(const history::Result& r) {
    std::vector<std::string> out;
    for (const auto& a : r.anomalies) out.push_back(a.kind);
    return out;
}

// ---- The checker, on histories with a known answer ------------------------

TEST(BsHistoryChecker, ASerialHistoryIsClean) {
    const std::vector<Txn> h = {
        {1, Outcome::kCommitted, {Append(1, 10), Append(2, 20)}},
        {2, Outcome::kCommitted, {Read(1, {10}), Append(1, 11)}},
        {3, Outcome::kCommitted, {Read(1, {10, 11}), Read(2, {20})}},
    };
    const auto r = history::Check(h, {{1, {10, 11}}, {2, {20}}}, /*forbid_g_single=*/true);
    EXPECT_TRUE(r.anomalies.empty()) << Kinds(r)[0];
    EXPECT_GT(r.edges, 0u);
}

TEST(BsHistoryChecker, WriteSkewIsAllowedAtBothLevels) {
    const std::vector<Txn> h = {
        {1, Outcome::kCommitted, {Read(1, {}), Read(2, {}), Append(1, 10)}},
        {2, Outcome::kCommitted, {Read(1, {}), Read(2, {}), Append(2, 20)}},
    };
    const auto r = history::Check(h, {{1, {10}}, {2, {20}}}, /*forbid_g_single=*/true);
    EXPECT_TRUE(r.anomalies.empty()) << Kinds(r)[0];
    EXPECT_EQ(r.rw_edges, 2u);
}

TEST(BsHistoryChecker, ReadSkewIsGSingleUnderRepeatableReadOnly) {
    const std::vector<Txn> h = {
        {1, Outcome::kCommitted, {Append(1, 10), Append(2, 20)}},
        {2, Outcome::kCommitted, {Read(1, {}), Read(2, {20})}},
    };
    const std::map<std::uint64_t, List> fin = {{1, {10}}, {2, {20}}};
    EXPECT_EQ(Kinds(history::Check(h, fin, true)), std::vector<std::string>{"G-single"});
    EXPECT_TRUE(history::Check(h, fin, false).anomalies.empty());
}

TEST(BsHistoryChecker, AnAbortedAppendSeenIsG1a) {
    const std::vector<Txn> h = {
        {1, Outcome::kAborted, {Append(1, 10)}},
        {2, Outcome::kCommitted, {Read(1, {10})}},
    };
    const auto r = history::Check(h, {{1, {}}}, false);
    EXPECT_EQ(Kinds(r).at(0), "G1a");
}

TEST(BsHistoryChecker, AnIntermediateAppendSeenIsG1b) {
    const std::vector<Txn> h = {
        {1, Outcome::kCommitted, {Append(1, 10), Append(1, 11)}},
        {2, Outcome::kCommitted, {Read(1, {10})}},
    };
    const auto r = history::Check(h, {{1, {10, 11}}}, false);
    EXPECT_EQ(Kinds(r).at(0), "G1b");
}

TEST(BsHistoryChecker, CrossedAppendOrdersAreG0) {
    const std::vector<Txn> h = {
        {1, Outcome::kCommitted, {Append(1, 10), Append(2, 20)}},
        {2, Outcome::kCommitted, {Append(1, 11), Append(2, 21)}},
    };
    const auto r = history::Check(h, {{1, {10, 11}}, {2, {21, 20}}}, false);
    EXPECT_EQ(Kinds(r).at(0), "G0");
}

TEST(BsHistoryChecker, ACircularReadIsG1c) {
    const std::vector<Txn> h = {
        {1, Outcome::kCommitted, {Append(1, 10), Read(2, {20})}},
        {2, Outcome::kCommitted, {Append(2, 20), Read(1, {10})}},
    };
    const auto r = history::Check(h, {{1, {10}}, {2, {20}}}, false);
    EXPECT_EQ(Kinds(r).at(0), "G1c");
}

TEST(BsHistoryChecker, ALostCommittedAppendIsAnOrderAnomaly) {
    const std::vector<Txn> h = {{1, Outcome::kCommitted, {Append(1, 10)}}};
    const auto r = history::Check(h, {{1, {}}}, false);
    EXPECT_EQ(Kinds(r), std::vector<std::string>{"order"});
}

TEST(BsHistoryChecker, AReadThatIsNoPrefixIsAnOrderAnomaly) {
    const std::vector<Txn> h = {
        {1, Outcome::kCommitted, {Append(1, 10)}},
        {2, Outcome::kCommitted, {Append(1, 11)}},
        {3, Outcome::kCommitted, {Read(1, {11})}},
    };
    EXPECT_EQ(Kinds(history::Check(h, {{1, {10, 11}}}, false)), std::vector<std::string>{"order"});
}

TEST(BsHistoryChecker, ATransactionMissingItsOwnAppendIsInternal) {
    const std::vector<Txn> h = {{1, Outcome::kCommitted, {Append(1, 10), Read(1, {})}}};
    EXPECT_EQ(Kinds(history::Check(h, {{1, {10}}}, false)), std::vector<std::string>{"internal"});
}

TEST(BsHistoryChecker, AShrinkingReadIsNotMonotonic) {
    const std::vector<Txn> h = {
        {1, Outcome::kCommitted, {Append(1, 10)}},
        {2, Outcome::kCommitted, {Read(1, {10}), Read(1, {})}},
    };
    EXPECT_EQ(Kinds(history::Check(h, {{1, {10}}}, false)), std::vector<std::string>{"monotonic"});
}

// A long fork: two readers see two writers' commits in opposite orders - two
// rw edges, neither adjacent to the other, which snapshot isolation forbids.
TEST(BsHistoryChecker, ALongForkIsGNonadjacentUnderRepeatableReadOnly) {
    const std::vector<Txn> h = {
        {1, Outcome::kCommitted, {Read(1, {}), Read(4, {40})}},
        {2, Outcome::kCommitted, {Append(1, 10), Append(2, 20)}},
        {3, Outcome::kCommitted, {Read(2, {20}), Read(3, {})}},
        {4, Outcome::kCommitted, {Append(3, 30), Append(4, 40)}},
    };
    const std::map<std::uint64_t, List> fin = {{1, {10}}, {2, {20}}, {3, {30}}, {4, {40}}};
    const auto kinds = Kinds(history::Check(h, fin, true));
    ASSERT_FALSE(kinds.empty());
    for (const auto& k : kinds) EXPECT_EQ(k, "G-nonadjacent");
    EXPECT_TRUE(history::Check(h, fin, false).anomalies.empty());
}

// An indeterminate transaction is what the final lists say: one whose appends
// are absent aborted - its read of its own append is no anomaly - one whose
// appends are present committed, and one in part present is the anomaly.
TEST(BsHistoryChecker, AnIndeterminateTransactionIsDecidedByTheFinalLists) {
    const std::vector<Txn> h = {
        {1, Outcome::kIndeterminate, {Append(1, 10), Read(1, {10})}},
        {2, Outcome::kIndeterminate, {Append(2, 20)}},
        {3, Outcome::kCommitted, {Read(2, {20})}},
    };
    EXPECT_TRUE(history::Check(h, {{1, {}}, {2, {20}}}, true).anomalies.empty());
    const std::vector<Txn> torn = {{1, Outcome::kIndeterminate, {Append(1, 10), Append(2, 20)}}};
    EXPECT_EQ(Kinds(history::Check(torn, {{1, {10}}, {2, {}}}, false)),
              std::vector<std::string>{"atomic"});
}

// ---- The live history -------------------------------------------------------

constexpr std::uint64_t kKeys = 6;
constexpr int kSessions = 4;

struct Planned {
    enum Kind : std::uint8_t { kReadPoint, kReadRange, kAppendPoint, kAppendRange } kind;
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;  // == lo for a point
    std::uint64_t value = 0;
};

struct SessionRun {
    Session session;
    std::vector<std::vector<Planned>> plan;
    bool repeatable_read = false;
    std::vector<Txn> txns;  // what it saw
    // The transaction under way, read by the test thread only once the
    // reactors are joined: one a crash cuts off is indeterminate.
    Txn current;
    bool in_txn = false;
    std::atomic<int> txns_done{0};
    std::atomic<bool> done{false};
    std::string failure;    // a reply the cell could not read
};

std::vector<std::string> Rows(const std::string& reply) {
    std::vector<std::string> rows;
    std::size_t at = reply.find("\\n");
    while (at != std::string::npos) {
        const std::size_t next = reply.find("\\n", at + 2);
        rows.push_back(reply.substr(at + 2, next == std::string::npos ? std::string::npos
                                                                     : next - at - 2));
        at = next;
    }
    return rows;
}

List Elements(const std::string& val) {
    List out;
    std::size_t at = 0;
    while (at < val.size()) {
        const std::size_t next = val.find(';', at);
        const std::string part = val.substr(at, next == std::string::npos ? std::string::npos
                                                                          : next - at);
        if (!part.empty()) out.push_back(std::stoull(part));
        if (next == std::string::npos) break;
        at = next + 1;
    }
    return out;
}

// `k,val` rows into each key's list.
std::map<std::uint64_t, List> Lists(const std::string& reply) {
    std::map<std::uint64_t, List> out;
    for (const std::string& row : Rows(reply)) {
        const std::size_t comma = row.find(',');
        out[std::stoull(row.substr(0, comma))] =
            Elements(comma == std::string::npos ? "" : row.substr(comma + 1));
    }
    return out;
}

std::string SqlOf(const Planned& p) {
    const std::string where = p.lo == p.hi
                                  ? "k = " + std::to_string(p.lo)
                                  : "k BETWEEN " + std::to_string(p.lo) + " AND " +
                                        std::to_string(p.hi);
    switch (p.kind) {
        case Planned::kReadPoint:
        case Planned::kReadRange:
            return "SELECT k, val FROM kv WHERE " + where;
        case Planned::kAppendPoint:
        case Planned::kAppendRange:
            return "UPDATE kv SET val = val || ';" + std::to_string(p.value) + "' WHERE " + where;
    }
    return {};
}

sched::Coro RunSession(CommandDispatcher& d, SessionRun& s) {
    for (std::size_t t = 0; t < s.plan.size(); ++t) {
        s.current = Txn{};  // its id is given when the history is assembled
        s.in_txn = true;
        Txn& txn = s.current;
        DispatchOutcome out;
        co_await d.DispatchAsync(s.repeatable_read ? "BEGIN ISOLATION LEVEL REPEATABLE READ"
                                                   : "BEGIN",
                                 &s.session, &out);
        bool failed = out.response.rfind("BEGIN", 0) != 0;
        for (const Planned& p : s.plan[t]) {
            if (failed) break;
            co_await d.DispatchAsync(SqlOf(p), &s.session, &out);
            const bool append = p.kind == Planned::kAppendPoint || p.kind == Planned::kAppendRange;
            if (out.response.rfind("ERR", 0) == 0) {
                // The statement's own appends count as the aborted
                // transaction's: one seen anywhere is G1a.
                if (append) {
                    for (std::uint64_t k = p.lo; k <= p.hi; ++k) txn.ops.push_back(Append(k, p.value));
                }
                failed = true;
                break;
            }
            if (append) {
                if (out.response != "UPDATED " + std::to_string(p.hi - p.lo + 1)) {
                    s.failure = SqlOf(p) + " -> " + out.response;
                }
                for (std::uint64_t k = p.lo; k <= p.hi; ++k) txn.ops.push_back(Append(k, p.value));
            } else {
                const auto lists = Lists(out.response);
                for (std::uint64_t k = p.lo; k <= p.hi; ++k) {
                    const auto it = lists.find(k);
                    if (it == lists.end()) {
                        s.failure = SqlOf(p) + " -> " + out.response;
                        continue;
                    }
                    txn.ops.push_back(Read(k, it->second));
                }
            }
        }
        if (failed) {
            co_await d.DispatchAsync("ROLLBACK", &s.session, &out);
            txn.outcome = Outcome::kAborted;
        } else {
            co_await d.DispatchAsync("COMMIT", &s.session, &out);
            txn.outcome = out.response.rfind("COMMIT", 0) == 0 ? Outcome::kCommitted
                                                               : Outcome::kIndeterminate;
        }
        s.txns.push_back(std::move(txn));
        s.in_txn = false;
        s.txns_done.fetch_add(1, std::memory_order_release);
    }
    s.done.store(true, std::memory_order_release);
    co_return Status::OK();
}

// `crash`: S4's crash composed with it (BS-A14). The rig is file-backed, every
// session commits `strict` - acknowledged is durable - and once half the
// transactions are done the reactors are joined mid-history and what a
// crash would leave is mounted through production's mount: the recovered
// lists must hold every acknowledged append, no aborted one, and either for
// a transaction the crash cut off.
// What the READ COMMITTED histories' G-single and G-nonadjacent come to,
// summed by the cell that asks for them.
std::size_t g_read_skew_seen = 0;

void RunHistory(std::uint64_t seed, bool repeatable_read, int txns_per_session,
                bool crash = false) {
    SCOPED_TRACE("seed " + std::to_string(seed) +
                 (repeatable_read ? " REPEATABLE READ" : " READ COMMITTED") +
                 (crash ? " crash" : ""));
    // Before the rig: a crash leaves sessions parked on its reactors, whose
    // frames name these until the rig is destroyed.
    std::vector<std::unique_ptr<SessionRun>> runs;
    TwoCoreRig::Options options;
    options.file_backed = crash;
    auto opened = TwoCoreRig::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    std::unique_ptr<TwoCoreRig> rig = std::move(opened.value());
    CommandDispatcher& d0 = rig->core(0).dispatcher();
    ASSERT_EQ(d0.Dispatch("CREATE TABLE kv (k int64, val varchar) BTREE").response.rfind("CREATED", 0),
              0u);
    for (std::uint64_t k = 1; k <= kKeys; ++k) {
        ASSERT_EQ(d0.Dispatch("INSERT INTO kv VALUES (" + std::to_string(k) + ", '')")
                      .response.rfind("INSERTED", 0),
                  0u);
    }

    sim::Rng rng(seed);
    for (int s = 0; s < kSessions; ++s) {
        auto run = std::make_unique<SessionRun>();
        run->repeatable_read = repeatable_read;
        if (crash) run->session.set_durability(wal::DurabilityClass::kStrict);
        for (int t = 0; t < txns_per_session; ++t) {
            std::vector<Planned> ops;
            const std::uint64_t n = 1 + rng.Below(4);
            for (std::uint64_t i = 0; i < n; ++i) {
                Planned p;
                const std::uint64_t pick = rng.Below(100);
                p.kind = pick < 30   ? Planned::kReadPoint
                         : pick < 40 ? Planned::kReadRange
                         : pick < 90 ? Planned::kAppendPoint
                                     : Planned::kAppendRange;
                p.lo = 1 + rng.Below(kKeys);
                p.hi = p.kind == Planned::kReadRange || p.kind == Planned::kAppendRange
                           ? std::min(kKeys, p.lo + rng.Below(3))
                           : p.lo;
                // Unique per key: session, transaction, op.
                p.value = (static_cast<std::uint64_t>(s + 1) * 1000 + t) * 10 + i;
                ops.push_back(p);
            }
            run->plan.push_back(std::move(ops));
        }
        rig->core(s % 2).scheduler().Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground, RunSession(rig->core(s % 2).dispatcher(), *run)));
        runs.push_back(std::move(run));
    }
    rig->Start();
    if (crash) {
        const int half = kSessions * txns_per_session / 2;
        ASSERT_TRUE(KickUntil(*rig, 0, [&] {
            rig->wakers().Kick(1);
            int done = 0;
            for (const auto& run : runs) done += run->txns_done.load(std::memory_order_acquire);
            return done >= half;
        }, std::chrono::milliseconds(300'000))) << "the history never reached its crash";
    } else {
        for (int s = 0; s < kSessions; ++s) {
            ASSERT_TRUE(KickUntil(*rig, static_cast<std::uint32_t>(s % 2),
                                  [&] { return runs[s]->done.load(std::memory_order_acquire); },
                                  std::chrono::milliseconds(300'000)))
                << "session " << s << " never finished";
        }
    }
    rig->Stop();
    crash_rig::TempDir image;
    std::unique_ptr<Expeditor> mounted;
    if (crash) {
        // The log as its writer left it, no flush: what an acknowledged
        // commit holds there is what its `strict` sync put there.
        ASSERT_TRUE(rig->Snapshot(image.path, /*flush_log=*/false).ok());
        auto opened_image = crash_rig::Mount(image.path);
        ASSERT_TRUE(opened_image.ok()) << opened_image.status().message();
        mounted = std::move(opened_image.value());
    }

    std::vector<Txn> all;
    std::uint64_t next_id = 1;
    std::size_t committed = 0;
    for (const auto& run : runs) {
        EXPECT_TRUE(run->failure.empty()) << run->failure;
        for (Txn t : run->txns) {
            t.id = next_id++;
            committed += t.outcome == Outcome::kCommitted;
            all.push_back(std::move(t));
        }
        if (run->in_txn) {
            Txn cut = run->current;
            cut.id = next_id++;
            cut.outcome = Outcome::kIndeterminate;
            all.push_back(std::move(cut));
        }
    }
    CommandDispatcher& reader = crash ? mounted->dispatcher() : d0;
    const auto fin = Lists(reader.Dispatch("SELECT k, val FROM kv").response);
    ASSERT_EQ(fin.size(), kKeys);
    const history::Result r = history::Check(all, fin, repeatable_read);
    for (const auto& a : r.anomalies) ADD_FAILURE() << a.kind << ": " << a.detail;
    // Not vacuous: transactions committed, and they depended on each other.
    EXPECT_GT(committed, all.size() / 4) << "too few commits to say anything";
    EXPECT_GT(r.edges, 0u);
    // What READ COMMITTED allows and the checker sees: G-single, read skew,
    // and G-nonadjacent.
    // A run of them here is what says the REPEATABLE READ cells' clean
    // verdicts are the engine's, not a checker that sees nothing.
    std::size_t g_single = 0;
    if (!repeatable_read) {
        for (const auto& a : history::Check(all, fin, /*forbid_g_single=*/true).anomalies) {
            g_single += a.kind == "G-single" || a.kind == "G-nonadjacent";
        }
    }
    g_read_skew_seen += g_single;
    std::printf("history seed=%llu %s%s: %zu txns, %zu committed, %zu edges (%zu rw)%s\n",
                static_cast<unsigned long long>(seed), repeatable_read ? "RR" : "RC",
                crash ? " crash" : "", all.size(),
                committed, r.edges, r.rw_edges,
                repeatable_read ? "" : (", G-single seen " + std::to_string(g_single)).c_str());
}

constexpr int kTxnsPerSession = 60;

TEST(BsHistory, RepeatableReadHistoriesShowOnlyWriteSkew) {
    for (std::uint64_t seed : {1u, 2u, 3u, 4u}) RunHistory(seed, /*repeatable_read=*/true, kTxnsPerSession);
}

TEST(BsHistory, ReadCommittedHistoriesShowNoDirtyOrCircularRead) {
    g_read_skew_seen = 0;
    for (std::uint64_t seed : {11u, 12u, 13u, 14u}) RunHistory(seed, /*repeatable_read=*/false, kTxnsPerSession);
    // The read skew READ COMMITTED allows, seen: what says the REPEATABLE
    // READ cells' silence is the engine's and not a checker that sees nothing.
    EXPECT_GT(g_read_skew_seen, 0u) << "four READ COMMITTED histories and no read skew in any";
}

TEST(BsHistory, AHistoryCutByACrashRecoversEveryAcknowledgedAppend) {
    for (std::uint64_t seed : {21u, 22u}) {
        RunHistory(seed, /*repeatable_read=*/true, kTxnsPerSession, /*crash=*/true);
        RunHistory(seed + 10, /*repeatable_read=*/false, kTxnsPerSession, /*crash=*/true);
    }
}

}  // namespace
}  // namespace kds::server
