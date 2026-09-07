// The instance read view's two costs, priced in-process (AN-S5).
//
// `instructions/v3.0.0/workorder-an-read-view.md` AN-S5 asks for two
// numbers beside the scenario cells: **the mint's cost as ns per
// statement** - which the commit-LSN view is expected to make smaller, since
// a mint is one atomic load and a walk of the live set where it used to
// fill a 64-entry array - and against it **the window lookup's cost per
// tuple** at 0, 8 and 64 live transactions per core, which is where the
// view spends what the mint saved: a writer above the floor costs
// `ReadView::Visible` one latched lookup (`read_view.hpp`).
//
// ---- What is measured, and what is not ------------------------------------
//
// Every number is one call on one thread against one
// `TransactionManager` over its own `InstanceVisibility`, unlogged - the
// shape every socket-free fixture runs. That isolates the predicate's own
// arithmetic from the page span, the decode and the socket a scenario cell
// pays around it; it says nothing about contention on the window latch,
// which needs two reactors and is the two-core rig's (AV). The four
// branches of `Visible` are priced separately, because a workload's mix of
// them is what decides its per-tuple cost:
//
//   bootstrap   t == kAlwaysVisibleTrxId          one comparison
//   own         t == own_trx_id                   one comparison
//   floor       t <  Floor()                      one atomic load
//   window hit  committed, above the floor        one latch + one hash lookup
//   window miss live, above the floor             one latch + one hash miss
//
// The live count moves two things: the mint's `live_` walk (the Cabin's
// one bit) and the window's size. Each is priced at 0, 8 and 64 live
// transactions held open on the manager.
//
// ---- Reading the numbers ----------------------------------------------------
//
// Median of `--repeats` runs of `--iterations` calls each, ns per call, with
// the minimum beside it; the spread between them is this box's own noise
// floor for a loop this short. A Release build, or the number is Debug's
// (`bench/README.md` rule 1). The loop's result is folded into a checksum
// that is printed, so the compiler cannot hoist the call.
//
// Run:
//     cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_BENCH=ON
//     cmake --build build-release -j --target kds_read_view_bench
//     ./build-release/kds_read_view_bench [--iterations N] [--repeats K]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "kds/server/superblock.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/read_view.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::uint64_t iterations = 2'000'000;
    int repeats = 5;
};

Options ParseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--iterations" && i + 1 < argc) {
            options.iterations = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--repeats" && i + 1 < argc) {
            options.repeats = std::atoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "usage: kds_read_view_bench [--iterations N] [--repeats K]\n"
                "  N calls per run (default 2000000), K runs per cell (default 5); the\n"
                "  table reports the median and the minimum ns per call over the K runs.\n");
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown argument %s\n", arg.c_str());
            std::exit(2);
        }
    }
    return options;
}

// One manager over its own visibility, unlogged, with `live` transactions
// held open and `committed` transactions committed and still in the window
// (the floor cannot pass them while anything is live below them, and the
// bench keeps one transaction live below every commit for exactly that).
struct Rig {
    kds::server::SuperBlock superblock =
        kds::server::SuperBlock::CreateFresh(/*now_unix_seconds=*/1000);
    kds::storage::InMemoryPageStore store{128};
    std::unique_ptr<kds::txn::TrxIdSequence> ids;
    std::unique_ptr<kds::txn::UndoLog> undo;
    std::unique_ptr<kds::txn::TransactionManager> manager;
    std::vector<kds::txn::Transaction*> live;
    std::vector<std::uint64_t> committed;
    // A transaction begun before every commit and held open, so the floor
    // stays below the committed ids and the window keeps them.
    kds::txn::Transaction* anchor = nullptr;

    explicit Rig(std::size_t live_count, std::size_t committed_count) {
        ids = std::make_unique<kds::txn::TrxIdSequence>(superblock);
        undo = std::make_unique<kds::txn::UndoLog>(store, /*wal=*/nullptr);
        manager = std::make_unique<kds::txn::TransactionManager>(*ids, *undo, store);
        anchor = manager->Begin(kds::txn::IsolationLevel::kReadCommitted).value();
        for (std::size_t i = 0; i < committed_count; ++i) {
            auto* txn = manager->Begin(kds::txn::IsolationLevel::kReadCommitted).value();
            committed.push_back(txn->id());
            (void)manager->Commit(*txn, kds::wal::DurabilityClass::kRelaxed);
            manager->Release(*txn);
        }
        for (std::size_t i = 0; i < live_count; ++i) {
            live.push_back(manager->Begin(kds::txn::IsolationLevel::kReadCommitted).value());
        }
    }
};

struct Cell {
    double median_ns = 0;
    double min_ns = 0;
};

template <typename Body>
Cell Time(const Options& options, Body&& body) {
    std::vector<double> runs;
    std::uint64_t checksum = 0;
    for (int r = 0; r < options.repeats; ++r) {
        const auto start = Clock::now();
        for (std::uint64_t i = 0; i < options.iterations; ++i) checksum += body(i);
        const double ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
        runs.push_back(ns / static_cast<double>(options.iterations));
    }
    std::sort(runs.begin(), runs.end());
    // Printed so the loop's result is observed and the call is not hoisted.
    std::printf("  (checksum %llu)\n", static_cast<unsigned long long>(checksum));
    return Cell{runs[runs.size() / 2], runs.front()};
}

void PrintRow(const char* name, std::size_t live, const Cell& cell) {
    std::printf("| %-34s | %4zu | %9.1f | %9.1f |\n", name, live, cell.median_ns, cell.min_ns);
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = ParseOptions(argc, argv);
    std::printf("read view bench: %llu calls per run, %d runs per cell, ns per call\n\n",
                static_cast<unsigned long long>(options.iterations), options.repeats);
    std::printf("| cell                               | live |    median |       min |\n");
    std::printf("|------------------------------------|------|-----------|-----------|\n");

    for (const std::size_t live : {std::size_t{0}, std::size_t{8}, std::size_t{64}}) {
        Rig rig(live, /*committed_count=*/64);
        kds::txn::TransactionManager& manager = *rig.manager;

        // The mint, as an autocommit statement takes it.
        PrintRow("mint (MintReadView)", live, Time(options, [&](std::uint64_t) {
                     return manager.MintReadView(kds::txn::kNoTrxId).snapshot_lsn;
                 }));

        const kds::txn::ReadView view = manager.MintReadView(kds::txn::kNoTrxId);
        const std::uint64_t below_floor = 1;  // never issued: below every floor
        const std::uint64_t hit = rig.committed[rig.committed.size() / 2];
        const std::uint64_t miss = live > 0 ? rig.live[live / 2]->id() : rig.anchor->id();

        PrintRow("visible: bootstrap id", live, Time(options, [&](std::uint64_t) {
                     return view.Visible(kds::txn::kAlwaysVisibleTrxId) ? 1u : 0u;
                 }));
        PrintRow("visible: below the floor", live, Time(options, [&](std::uint64_t) {
                     return view.Visible(below_floor) ? 1u : 0u;
                 }));
        PrintRow("visible: window hit (committed)", live, Time(options, [&](std::uint64_t) {
                     return view.Visible(hit) ? 1u : 0u;
                 }));
        PrintRow("visible: window miss (live)", live, Time(options, [&](std::uint64_t) {
                     return view.Visible(miss) ? 1u : 0u;
                 }));
        // Alternating hit and miss, which is closer to a scan over rows
        // written by a mix of committed and live writers.
        PrintRow("visible: hit/miss alternating", live, Time(options, [&](std::uint64_t i) {
                     return view.Visible((i & 1) ? hit : miss) ? 1u : 0u;
                 }));
    }
    return 0;
}
