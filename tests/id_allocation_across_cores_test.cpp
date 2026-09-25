#include "two_core_rig.hpp"

#include <string>

#include <gtest/gtest.h>

#include "kds/server/session.hpp"

// AT-S10b (AT-S4's allocators, folded into AT-S10): every core issues its
// own ids - a transaction-id window carved from the instance's one ceiling,
// a row id bumped in place on the relation's `sys.tables` mark - where a
// peer used to issue from blocks core 0 carved and leased to it over the
// ring. These cells run both cores' writes into one relation at once, on
// the rig's real threads.

namespace kds::server {
namespace {

using namespace std::chrono_literals;

constexpr int kRowsPerCore = 200;

struct Writer {
    Session session;
    DispatchOutcome out;
    std::string first_refusal;
    std::atomic<bool> done{false};
};

sched::Coro InsertMany(CommandDispatcher& d, Writer& w) {
    for (int i = 0; i < kRowsPerCore; ++i) {
        co_await d.DispatchAsync("INSERT INTO ids VALUES (" + std::to_string(i) + ")",
                                 &w.session, &w.out);
        if (w.out.response.rfind("INSERTED", 0) != 0 && w.first_refusal.empty()) {
            w.first_refusal = w.out.response;
        }
    }
    w.done.store(true, std::memory_order_release);
    co_return Status::OK();
}

TEST(IdAllocationAcrossCores, TwoCoresWritingOneRelationIssueOneGaplessSequence) {
    TwoCoreRig::Options options;
    options.wal_drain_interval_ns = 1'000'000;
    auto opened = TwoCoreRig::Open(options);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto rig = std::move(opened.value());

    CommandDispatcher& d0 = rig->core(0).dispatcher();
    ASSERT_EQ(d0.Dispatch("CREATE TABLE ids (id int64, v int64) BTREE").response.substr(0, 3),
              "CRE");
    ASSERT_TRUE(rig->store().FlushPages(catalog::kEveryCatalogPage).ok());

    // Nothing is funded: both cores write on their first statement.
    Writer w0;
    Writer w1;
    rig->core(0).scheduler().Submit(
        sched::MakeCoroTask(sched::SchedulingGroup::kForeground, InsertMany(d0, w0)));
    rig->core(1).scheduler().Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground, InsertMany(rig->core(1).dispatcher(), w1)));
    rig->Start();
    ASSERT_TRUE(KickUntil(
        *rig, 0,
        [&] {
            rig->wakers().Kick(1);
            return w0.done.load(std::memory_order_acquire) &&
                   w1.done.load(std::memory_order_acquire);
        },
        20000ms));
    rig->Stop();

    EXPECT_TRUE(w0.first_refusal.empty()) << "core 0 refused a write: " << w0.first_refusal;
    EXPECT_TRUE(w1.first_refusal.empty()) << "core 1 refused a write: " << w1.first_refusal;

    // Every row, every id distinct - and **gapless**: the mark is one
    // sequence both cores bump, so no id is skipped. A block cached on
    // either core would leave its unspent remainder as a gap, and a block
    // leased ahead of the mark would put one core's ids above the other's
    // later ones. The primary key refuses a duplicate, so a count of every
    // row written is also a count of distinct ids.
    const std::string reply = d0.Dispatch("SELECT COUNT(*), MIN(id), MAX(id) FROM ids").response;
    const std::string kHeader = "count(*),min(id),max(id)\\n";
    ASSERT_EQ(reply.rfind(kHeader, 0), 0u) << reply;
    const std::string row = reply.substr(kHeader.size());
    const std::size_t c1 = row.find(',');
    const std::size_t c2 = row.find(',', c1 + 1);
    ASSERT_NE(c2, std::string::npos) << reply;
    const std::uint64_t count = std::stoull(row.substr(0, c1));
    const std::uint64_t lo = std::stoull(row.substr(c1 + 1, c2 - c1 - 1));
    const std::uint64_t hi = std::stoull(row.substr(c2 + 1));
    EXPECT_EQ(count, static_cast<std::uint64_t>(2 * kRowsPerCore)) << reply;
    EXPECT_EQ(hi - lo + 1, count) << "the ids are not one gapless sequence: " << reply;
}

}  // namespace
}  // namespace kds::server
