// How long the frame table's structure latch is held, and what a hold costs
// the fetches on other cores (DO-S0, `instructions/v3.2.0/workorder-do-frame-table-long-holds.md`).
//
// ---- What is measured ------------------------------------------------------
//
// One armed `DevicePageStore` over one `FilePageDevice` - the `cores > 1`
// shape - filled with `--frames` resident pages, every one dirty (a
// checkpoint's worst case: every frame visited is also copied). Three phases:
//
//   fill     the pages created one by one; every insert that rehashes the
//            frame table (H3) is one walk-hold, and its length is read from
//            `Contention`'s walk tally as a delta, so it is exact
//   walk     `DirtyPagesWithRecLsn` (H1) and `DirtyPageIds` (H2) called
//            `--walks` times each with nothing else running: the hold from
//            the tally, and the call's whole time beside it (the sort, once
//            DO's E1 moved it, is in the second and not the first)
//   contend  `--readers` threads, each a core of its own, fetch random
//            resident pages for `--seconds` while the main thread runs H1
//            every `--every-ms` (0: none, the fetches alone); each fetch's
//            latency is recorded, with the
//            frame-table waits and their longest. `--readers 0` skips it
//
// With `-DKDS_LATCH_HOLD_CENSUS=ON` the census build's per-kind holds are
// printed too, and the contend phase prints structure-latch holds per fetch.
//
// A Release build, or the number is Debug's (`bench/README.md` rule 1). The
// pages stay resident and are never written back, so the file grows sparse
// and RSS is `--frames` times 8 KiB.
//
// Run:
//     cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_BENCH=ON
//     cmake --build build-release -j --target kds_frame_table_hold_bench
//     ./build-release/kds_frame_table_hold_bench --frames 65536 --file <path>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "kds/base/contention.hpp"
#include "kds/base/current_core.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/file_page_device.hpp"
#include "kds/storage/page_header.hpp"

namespace {

using kds::Contention;
using kds::LatchKind;
using kds::Longest;
using kds::PageId;
using kds::Tally;
using kds::storage::DevicePageStore;
using kds::storage::FilePageDevice;
using Clock = std::chrono::steady_clock;

double Percentile(std::vector<double>& v, double p) {
    if (v.empty()) return 0;
    const auto k = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

void Report(const char* name, std::vector<double> us) {
    if (us.empty()) {
        std::printf("%-14s n=0\n", name);
        return;
    }
    double sum = 0;
    for (double x : us) sum += x;
    const double p100 = *std::max_element(us.begin(), us.end());
    const double p0 = *std::min_element(us.begin(), us.end());
    std::printf("%-14s n=%zu p0=%.2f p25=%.2f p50=%.2f p90=%.2f p99=%.2f p999=%.2f max=%.2f "
                "mean=%.2f us\n",
                name, us.size(), p0, Percentile(us, 0.25), Percentile(us, 0.50),
                Percentile(us, 0.90), Percentile(us, 0.99), Percentile(us, 0.999), p100,
                sum / static_cast<double>(us.size()));
}

// The walk tally's running total, in nanoseconds, and how many walks it holds.
struct WalkTotal {
    std::uint64_t walks;
    std::uint64_t ns;
};
WalkTotal ReadWalks() {
    const auto snap = Contention::Read();
    return {snap.of(Tally::kFrameTableWalks), snap.of(Tally::kFrameTableWalkNs)};
}

int Fail(const std::string& what) {
    std::fprintf(stderr, "frame_table_hold_bench: %s\n", what.c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t frames = 0;
    std::size_t walks = 20;
    std::size_t readers = 0;
    double seconds = 3;
    std::size_t every_ms = 100;
    std::string path;
    for (int i = 1; i + 1 < argc; i += 2) {
        const auto v = [&] { return std::strtoull(argv[i + 1], nullptr, 10); };
        if (std::strcmp(argv[i], "--frames") == 0) frames = v();
        else if (std::strcmp(argv[i], "--walks") == 0) walks = v();
        else if (std::strcmp(argv[i], "--readers") == 0) readers = v();
        else if (std::strcmp(argv[i], "--seconds") == 0) seconds = std::strtod(argv[i + 1], nullptr);
        else if (std::strcmp(argv[i], "--every-ms") == 0) every_ms = v();
        else if (std::strcmp(argv[i], "--file") == 0) path = argv[i + 1];
    }
    if (frames == 0 || path.empty()) {
        return Fail("usage: --frames N --file PATH [--walks N] [--readers R] [--seconds S] "
                    "[--every-ms M]");
    }

    auto device = FilePageDevice::Open(path);
    if (!device.ok()) return Fail(device.status().message());
    // Headroom past the fill, so no reservation sweeps or writes back.
    auto opened = DevicePageStore::Open(*device.value(),
                                        kds::storage::FrameCapacity{frames + frames / 8 + 4096});
    if (!opened.ok()) return Fail(opened.status().message());
    DevicePageStore& store = *opened.value();
    store.SetLatchArmed(true, static_cast<std::uint32_t>(readers + 1));

    // ---- fill -------------------------------------------------------------
    std::vector<PageId> ids;
    ids.reserve(frames);
    std::vector<double> rehash_us;
    const auto fill_start = Clock::now();
    for (std::size_t i = 0; i < frames; ++i) {
        const WalkTotal before = ReadWalks();
        auto created = store.CreateNew();
        if (!created.ok()) return Fail(created.status().message());
        if (const WalkTotal after = ReadWalks(); after.walks != before.walks) {
            rehash_us.push_back(static_cast<double>(after.ns - before.ns) / 1e3);
        }
        kds::storage::FormatPage(created.value().second.bytes(), kds::PageType::kHeap);
        ids.push_back(created.value().first);
    }
    std::printf("frames=%zu resident=%zu fill_s=%.2f (two tally reads an insert included)\n", frames, store.resident_pages(),
                std::chrono::duration<double>(Clock::now() - fill_start).count());
    Report("fill_rehash", rehash_us);

    // ---- walk -------------------------------------------------------------
    const auto time_walks = [&](const char* hold_name, const char* call_name, auto&& call) {
        std::vector<double> hold_us;
        std::vector<double> call_us;
        for (std::size_t i = 0; i < walks; ++i) {
            const std::uint64_t ns0 = ReadWalks().ns;
            const auto t0 = Clock::now();
            const std::size_t n = call();
            const auto t1 = Clock::now();
            if (n != frames) {
                std::fprintf(stderr, "%s returned %zu, expected %zu\n", call_name, n, frames);
            }
            hold_us.push_back(static_cast<double>(ReadWalks().ns - ns0) / 1e3);
            call_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        Report(hold_name, hold_us);
        Report(call_name, call_us);
    };
    time_walks("h1_hold", "h1_call", [&] { return store.DirtyPagesWithRecLsn().size(); });
    time_walks("h2_hold", "h2_call", [&] { return store.DirtyPageIds().size(); });

    // ---- contend ----------------------------------------------------------
    if (readers != 0) {
        const auto before = Contention::Read();
        std::atomic<bool> stop{false};
        std::vector<std::vector<double>> lat(readers);
        std::vector<std::thread> threads;
        for (std::size_t r = 0; r < readers; ++r) {
            threads.emplace_back([&, r] {
                kds::SetCurrentCore(static_cast<std::uint32_t>(r + 1));
                std::mt19937_64 rng(r + 1);
                std::uniform_int_distribution<std::size_t> pick(0, ids.size() - 1);
                lat[r].reserve(1 << 24);
                while (!stop.load(std::memory_order_relaxed)) {
                    const PageId id = ids[pick(rng)];
                    const auto t0 = Clock::now();
                    {
                        auto ref = store.GetForRead(id);
                        if (!ref.ok()) std::abort();
                    }
                    lat[r].push_back(
                        std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
                }
            });
        }
        std::vector<double> hold_us;
        std::vector<double> call_us;  // the walking thread's own time: its reactor's stall
        const auto end = Clock::now() + std::chrono::duration<double>(seconds);
        if (every_ms == 0) std::this_thread::sleep_until(end);  // fetches alone
        while (every_ms != 0 && Clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(every_ms));
            const std::uint64_t ns0 = ReadWalks().ns;
            const auto t0 = Clock::now();
            (void)store.DirtyPagesWithRecLsn();
            call_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
            hold_us.push_back(static_cast<double>(ReadWalks().ns - ns0) / 1e3);
        }
        stop.store(true);
        for (auto& t : threads) t.join();
        const auto after = Contention::Read();
        std::vector<double> all;
        for (auto& v : lat) all.insert(all.end(), v.begin(), v.end());
        const std::size_t fetches = all.size();
        std::printf("contend readers=%zu seconds=%.1f every_ms=%zu fetches=%zu fetches_per_s=%.0f\n",
                    readers, seconds, every_ms, fetches, static_cast<double>(fetches) / seconds);
        Report("contend_h1", hold_us);
        Report("contend_h1_call", call_us);
        Report("contend_fetch", std::move(all));
        const auto k = LatchKind::kFrameTable;
        std::printf("contend frame_table_waits=%llu frame_table_wait_us=%.1f\n",
                    static_cast<unsigned long long>(after.waits_of(k) - before.waits_of(k)),
                    static_cast<double>(after.wait_ns_of(k) - before.wait_ns_of(k)) / 1e3);
#ifdef KDS_LATCH_HOLD_CENSUS
        const auto i = static_cast<std::size_t>(k);
        const double holds = static_cast<double>(after.holds[i] - before.holds[i]);
        std::printf("contend frame_table_holds=%.0f holds_per_fetch=%.3f hold_ns_per_fetch=%.1f "
                    "hold_longest_us=%.1f\n",
                    holds, holds / static_cast<double>(fetches),
                    static_cast<double>(after.hold_ns[i] - before.hold_ns[i]) /
                        static_cast<double>(fetches),
                    static_cast<double>(after.hold_longest_ns[i]) / 1e3);
#endif
    }
#ifdef KDS_LATCH_HOLD_CENSUS
    // ---- count (census build) ---------------------------------------------
    // Structure-latch holds a fetch, exactly, on one thread with images on
    // (the server's state after its mount): a read hit, then a write hit.
    {
        store.EnableFetchImages(true);
        const auto i = static_cast<std::size_t>(LatchKind::kFrameTable);
        constexpr std::size_t kFetches = 1'000'000;
        const auto count = [&](const char* name, bool write) {
            const auto before = Contention::Read();
            for (std::size_t k = 0; k < kFetches; ++k) {
                const PageId id = ids[k % ids.size()];
                auto ref = write ? store.Get(id) : store.GetForRead(id);
                if (!ref.ok()) std::abort();
            }
            const auto after = Contention::Read();
            std::printf("count %s holds_per_fetch=%.3f hold_ns_per_fetch=%.1f\n", name,
                        static_cast<double>(after.holds[i] - before.holds[i]) /
                            static_cast<double>(kFetches),
                        static_cast<double>(after.hold_ns[i] - before.hold_ns[i]) /
                            static_cast<double>(kFetches));
        };
        count("read_hit", false);
        count("write_hit", true);
    }
#endif
    const auto end_state = Contention::Read();
    std::printf("walk_longest_us=%.1f walks=%llu over_100us=%llu over_1ms=%llu over_10ms=%llu\n",
                static_cast<double>(end_state.of(Longest::kFrameTableWalkNs)) / 1e3,
                static_cast<unsigned long long>(end_state.of(Tally::kFrameTableWalks)),
                static_cast<unsigned long long>(end_state.of(Tally::kFrameTableWalksOver100us)),
                static_cast<unsigned long long>(end_state.of(Tally::kFrameTableWalksOver1ms)),
                static_cast<unsigned long long>(end_state.of(Tally::kFrameTableWalksOver10ms)));
    return 0;
}
