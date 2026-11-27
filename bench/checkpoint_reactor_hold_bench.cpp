// How long one cadence checkpoint run holds its own core's reactor, piece by
// piece (DZ-S0, `instructions/v3.2.0/workorder-dz-checkpoint-reactor-residue.md`).
//
// ---- What is measured ------------------------------------------------------
//
// One `DevicePageStore` over one `FilePageDevice`, filled with `--frames`
// resident pages, every one dirty - a checkpoint's worst case, since every
// page the walk visits is also written. `--readers 0` leaves the store
// unarmed, the `cores = 1` shape; `--readers R` arms it for `R + 1` pinners
// and runs R threads, each a core of its own, fetching random resident pages
// for the run's whole length, the `cores = R + 1` shape. The data-sync
// thread runs, as a server's does.
//
// One `Checkpointer` run task is submitted to one `Scheduler` on a real
// clock, beside a `foreground` task that yields every poll and records the
// longest gap between two of its polls - what a session on the checkpointing
// core waits. The reactor is turned until the run ends. Reported:
//
//   longest_system_poll   the scheduler's own longest `system` poll: the bar
//                         DZ-R5 reads (one hold of the reactor by the run)
//   fg_gap                the foreground task's longest gap between polls
//   start / step / publish_batch / maps  the run report's longest occurrence
//                         of each piece (`CheckpointRunReport`)
//   h1 / h2               the two dirty-table walks timed directly, with the
//                         readers still running, after the run
//
// **What is cut**: the pages carry no LSN, so no write is gated on the log
// and no step defers; the anchor is the in-memory one, so the anchor page's
// one write and sync are absent. The log is a file log in `<file>.wal`, its
// writer thread started. Each is a piece of
// the run this bench does not price, said in the results file.
//
// A Release build, or the number is Debug's (`bench/README.md` rule 1). RSS
// is `--frames` times 8 KiB, and every run writes that much to `--file`.
//
// Run:
//     cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_BENCH=ON
//     cmake --build build-release -j --target kds_checkpoint_reactor_hold_bench
//     ./build-release/kds_checkpoint_reactor_hold_bench --frames 65536 --file <path>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "kds/base/current_core.hpp"
#include "kds/sched/checkpoint_census.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/coro.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/file_page_device.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/page_store_checkpoint_target.hpp"
#include "kds/wal/checkpointer.hpp"
#include "kds/wal/file_log_device.hpp"

namespace {

using kds::CheckpointPhase;
using kds::PageId;
using kds::storage::DevicePageStore;
using kds::storage::FilePageDevice;
using Clock = std::chrono::steady_clock;

int Fail(const std::string& what) {
    std::fprintf(stderr, "checkpoint_reactor_hold_bench: %s\n", what.c_str());
    return 1;
}

double Ms(std::uint64_t ns) { return static_cast<double>(ns) / 1e6; }

// The store's target, each call's longest time recorded: which piece of the
// run a long poll is.
class TimedTarget final : public kds::wal::CheckpointTarget {
public:
    enum Piece { kDirtyTable, kWritePages, kDirtyPages, kHeaderlessMaps, kFreeMaps, kRequestSync, kPieces };
    static constexpr const char* kNames[kPieces] = {"dirty_table", "write_pages", "dirty_pages",
                                                    "headerless_maps", "free_maps", "request_sync"};
    explicit TimedTarget(kds::storage::PageStoreCheckpointTarget& inner) : inner_(inner) {}

    template <typename F>
    auto Time(Piece p, F&& f) const {
        const auto t0 = Clock::now();
        auto out = f();
        const auto ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
        max_[p] = std::max(max_[p], ns);
        return out;
    }

    std::vector<kds::wal::CheckpointDirtyPage> DirtyTable() const override {
        return Time(kDirtyTable, [&] { return inner_.DirtyTable(); });
    }
    kds::Status FlushPages(std::span<const PageId> ids) override { return inner_.FlushPages(ids); }
    kds::StatusOr<std::size_t> WritePages(std::span<const PageId> ids, kds::wal::Lsn* defer) override {
        return Time(kWritePages, [&] { return inner_.WritePages(ids, defer); });
    }
    std::uint64_t RequestSync() override {
        return Time(kRequestSync, [&] { return inner_.RequestSync(); });
    }
    bool SyncDone(std::uint64_t t) override { return inner_.SyncDone(t); }
    kds::Status SyncResult(std::uint64_t t) override { return inner_.SyncResult(t); }
    std::vector<PageId> DirtyPages() override {
        return Time(kDirtyPages, [&] { return inner_.DirtyPages(); });
    }
    kds::StatusOr<std::uint64_t> WriteHeaderlessMaps() override {
        return Time(kHeaderlessMaps, [&] { return inner_.WriteHeaderlessMaps(); });
    }
    kds::Status WriteFreeMaps() override {
        return Time(kFreeMaps, [&] { return inner_.WriteFreeMaps(); });
    }
    // The store's sliced collection (DZ-S1), each slice timed into the piece
    // the whole call was. Without this the run takes `CheckpointTarget`'s
    // default - `DirtyTable` and `DirtyPages` whole, in one poll each - and
    // the bench measures the walk DZ-S1 replaced.
    std::unique_ptr<kds::wal::DirtyCollection> CollectDirty(kds::wal::DirtyCollectionFor use) override {
        const Piece piece = use == kds::wal::DirtyCollectionFor::kStart ? kDirtyTable : kDirtyPages;
        return std::make_unique<TimedCollection>(*this, piece, inner_.CollectDirty(use));
    }

    void Print() const {
        for (int i = 0; i < kPieces; ++i) {
            std::printf("%s_max_ms=%.2f%s", kNames[i], Ms(max_[i]), i + 1 == kPieces ? "\n" : " ");
        }
    }

private:
    class TimedCollection final : public kds::wal::DirtyCollection {
    public:
        TimedCollection(const TimedTarget& target, Piece piece,
                        std::unique_ptr<kds::wal::DirtyCollection> inner)
            : target_(target), piece_(piece), inner_(std::move(inner)) {}
        bool Advance() override { return target_.Time(piece_, [&] { return inner_->Advance(); }); }
        std::vector<kds::wal::CheckpointDirtyPage>& Table() override { return inner_->Table(); }

    private:
        const TimedTarget& target_;
        Piece piece_;
        std::unique_ptr<kds::wal::DirtyCollection> inner_;
    };

    kds::storage::PageStoreCheckpointTarget& inner_;
    mutable std::uint64_t max_[kPieces] = {};
};

}  // namespace

int main(int argc, char** argv) {
    std::size_t frames = 0;
    std::size_t readers = 0;
    std::size_t pages_per_step = 64;
    std::string path;
    for (int i = 1; i + 1 < argc; i += 2) {
        const auto v = [&] { return std::strtoull(argv[i + 1], nullptr, 10); };
        if (std::strcmp(argv[i], "--frames") == 0) frames = v();
        else if (std::strcmp(argv[i], "--readers") == 0) readers = v();
        else if (std::strcmp(argv[i], "--pages-per-step") == 0) pages_per_step = v();
        else if (std::strcmp(argv[i], "--file") == 0) path = argv[i + 1];
    }
    // `--file`'s log directory, `<file>.wal`, must not exist: a fresh log.
    if (frames == 0 || path.empty()) {
        return Fail("usage: --frames N --file PATH [--readers R] [--pages-per-step P]");
    }

    auto device = FilePageDevice::Open(path);
    if (!device.ok()) return Fail(device.status().message());
    auto opened = DevicePageStore::Open(*device.value(),
                                        kds::storage::FrameCapacity{frames + frames / 8 + 4096});
    if (!opened.ok()) return Fail(opened.status().message());
    DevicePageStore& store = *opened.value();
    if (readers != 0) store.SetLatchArmed(true, static_cast<std::uint32_t>(readers + 1));

    std::vector<PageId> ids;
    ids.reserve(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        auto created = store.CreateNew();
        if (!created.ok()) return Fail(created.status().message());
        kds::storage::FormatPage(created.value().second.bytes(), kds::PageType::kHeap);
        ids.push_back(created.value().first);
    }
    store.data_sync().Start();

    kds::sched::SystemClock clock;
    kds::sched::NullIoBackend io;
    kds::sched::Scheduler reactor(clock, io);
    // The log on the device beside the data file, its writer thread syncing
    // as a server's does - so a park on the log costs the reactor nothing.
    auto log = kds::wal::FileLogDevice::Open(path + ".wal");
    if (!log.ok()) return Fail(log.status().message());
    auto wal = kds::wal::WalManager::Open(log.value().get(), clock, 0);
    if (!wal.ok()) return Fail(wal.status().message());
    wal.value()->StartWriter();
    kds::storage::PageStoreCheckpointTarget store_target(store);
    TimedTarget target(store_target);
    kds::wal::NoActiveTransactions txns;
    kds::wal::InMemoryCheckpointAnchor anchor;
    kds::wal::Checkpointer checkpointer(*wal.value(), target, txns, anchor,
                                        kds::wal::CheckpointerConfig{.pages_per_step = pages_per_step});

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> fetches{0};
    std::vector<std::thread> threads;
    for (std::size_t r = 0; r < readers; ++r) {
        threads.emplace_back([&, r] {
            kds::SetCurrentCore(static_cast<std::uint32_t>(r + 1));
            std::mt19937_64 rng(r + 1);
            std::uniform_int_distribution<std::size_t> pick(0, ids.size() - 1);
            std::uint64_t n = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                auto ref = store.GetForRead(ids[pick(rng)]);
                if (!ref.ok()) std::abort();
                ++n;
            }
            fetches.fetch_add(n);
        });
    }

    // The session on the checkpointing core: a poll a turn, its longest gap.
    std::uint64_t fg_gap_max = 0;
    std::uint64_t fg_polls = 0;
    bool fg_stop = false;
    auto foreground = [&]() -> kds::sched::Coro {
        std::uint64_t last = clock.Now();
        while (!fg_stop) {
            const std::uint64_t now = clock.Now();
            fg_gap_max = std::max(fg_gap_max, now - last);
            last = now;
            ++fg_polls;
            co_await kds::sched::Yield{};
        }
        co_return kds::Status::OK();
    };
    reactor.Submit(kds::sched::MakeCoroTask(kds::sched::SchedulingGroup::kForeground, foreground()));

    const auto run_start = Clock::now();
    checkpointer.SubmitRun(reactor, nullptr, 0);
    // Where the longest turn fell: the run report's progress around it.
    std::uint64_t turn_max = 0;
    std::string turn_at;
    while (checkpointer.run_in_flight()) {
        const kds::CheckpointRunReport before = checkpointer.task_report();
        const auto t0 = clock.Now();
        reactor.RunOnce();
        const std::uint64_t ns = clock.Now() - t0;
        if (ns > turn_max) {
            const kds::CheckpointRunReport& after = checkpointer.task_report();
            turn_max = ns;
            turn_at = "steps " + std::to_string(before.steps) + "->" + std::to_string(after.steps) +
                      " pages " + std::to_string(before.pages) + "->" + std::to_string(after.pages) +
                      " publish_n " +
                      std::to_string(before.count[static_cast<std::size_t>(CheckpointPhase::kPublishWriteback)]) +
                      "->" +
                      std::to_string(after.count[static_cast<std::size_t>(CheckpointPhase::kPublishWriteback)]) +
                      " end_ns " + std::to_string(after.end_ns != 0) +
                      " in_progress " + std::to_string(checkpointer.in_progress());
        }
    }
    const double run_s = std::chrono::duration<double>(Clock::now() - run_start).count();
    fg_stop = true;
    reactor.RunOnce();

    // The walks alone, readers still running: what the run's start and its
    // publish's collection each cost the reactor. Everything is clean now,
    // so the walks visit every frame and emit none; re-dirty first.
    for (PageId id : ids) {
        auto ref = store.Get(id);
        if (!ref.ok()) return Fail(ref.status().message());
        ref.value().MarkDirty();
    }
    std::uint64_t h1_max = 0;
    std::uint64_t h2_max = 0;
    for (int i = 0; i < 3; ++i) {
        auto t0 = clock.Now();
        (void)store.DirtyPagesWithRecLsn();
        h1_max = std::max(h1_max, clock.Now() - t0);
        t0 = clock.Now();
        (void)store.DirtyPageIds();
        h2_max = std::max(h2_max, clock.Now() - t0);
    }
    stop.store(true);
    for (auto& t : threads) t.join();

    const kds::CheckpointRunReport& r = checkpointer.task_report();
    const auto phase_max = [&](CheckpointPhase p) { return r.max_ns[static_cast<std::size_t>(p)]; };
    const auto phase_n = [&](CheckpointPhase p) { return r.count[static_cast<std::size_t>(p)]; };
    std::printf(
        "frames=%zu readers=%zu pages_per_step=%zu completed=%d run_s=%.2f pages=%llu steps=%llu\n",
        frames, readers, pages_per_step, r.completed ? 1 : 0, run_s,
        static_cast<unsigned long long>(r.pages), static_cast<unsigned long long>(r.steps));
    std::printf(
        "longest_system_poll_ms=%.2f system_polls_over_10ms=%llu system_polls_over_100ms=%llu "
        "fg_gap_max_ms=%.2f fg_polls=%llu\n",
        Ms(reactor.longest_poll_ns(kds::sched::SchedulingGroup::kSystem)),
        static_cast<unsigned long long>(reactor.polls_over_10ms(kds::sched::SchedulingGroup::kSystem)),
        static_cast<unsigned long long>(reactor.polls_over_100ms(kds::sched::SchedulingGroup::kSystem)),
        Ms(fg_gap_max), static_cast<unsigned long long>(fg_polls));
    std::printf(
        "start_max_ms=%.2f step_max_ms=%.2f step_write_max_ms=%.2f publish_batch_max_ms=%.2f "
        "publish_batches=%llu data_park_ms=%.1f\n",
        Ms(phase_max(CheckpointPhase::kStart)), Ms(r.step_max_ns), Ms(r.step_page_write_max_ns),
        Ms(phase_max(CheckpointPhase::kPublishWriteback)),
        static_cast<unsigned long long>(phase_n(CheckpointPhase::kPublishWriteback)),
        Ms(r.data_park_ns));
    std::printf("h1_call_max_ms=%.2f h2_call_max_ms=%.2f reader_fetches=%llu\n", Ms(h1_max),
                Ms(h2_max), static_cast<unsigned long long>(fetches.load()));
    target.Print();
    std::printf("longest_turn_ms=%.2f at: %s\n", Ms(turn_max), turn_at.c_str());
    store.data_sync().Stop();
    return 0;
}
