// The per-miss cost of reclaiming past the frame budget, priced in the
// engine (BE-S1's premise, `instructions/v3.0.0/workorder-be-bounded-pool.md`).
//
// BE's §1.2 timed a standalone copy of the sweep's collect-and-sort loop:
// 7.5 ms at 131,072 frames and 89.9 ms at 786,432. That copy had no engine,
// no latch and no frame table, so BE-S1 asks for the same number from the
// real `DevicePageStore`: if the engine's per-miss cost were under a tenth of
// the standalone figure, the slot array BE-S2 builds would be answering a
// cost that is not there.
//
// ---- What is measured, and what is not ------------------------------------
//
// One thread, one `DevicePageStore` over one `FilePageDevice`, unarmed (the
// `cores = 1` shape). Setup creates `2 * budget` formatted pages, writes them
// back and drops them in batches, so the measured pass starts from an empty
// pool over a file that holds every page. The pass then reads every page in
// id order through `GetForRead` - the shape of a read-only scan over a
// relation twice the budget - and times each call:
//
//   below   the first `budget` reads: a fault into a pool with room
//   past    every read after that: a fault that pays the inline sweep
//
// `--past N` stops after N reads past the budget (0 = the whole second half),
// because at the engine's own rate the whole half can take hours; the total
// is then extrapolated from the sample and printed as such. The device read
// is in both columns, so `past - below` is the sweep. Page-cache state is
// whatever setup left: the file was just written, so reads mostly hit the
// kernel's cache - which makes the sweep a larger share, not a smaller one.
//
// A Release build, or the number is Debug's (`bench/README.md` rule 1).
//
// Run:
//     cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DKDS_BUILD_BENCH=ON
//     cmake --build build-release -j --target kds_pool_sweep_bench
//     ./build-release/kds_pool_sweep_bench --budget 131072 --file <path> [--past N]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "kds/storage/device_page_store.hpp"
#include "kds/storage/file_page_device.hpp"
#include "kds/storage/page_header.hpp"

namespace {

using kds::storage::DevicePageStore;
using kds::storage::FilePageDevice;
using kds::PageId;
using Clock = std::chrono::steady_clock;

// Pages created per setup batch before they are written back and dropped, so
// setup's own residency stays at one batch (128 MiB).
constexpr std::size_t kSetupBatch = 16384;

long RssKiB() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) return std::strtol(line.c_str() + 6, nullptr, 10);
    }
    return -1;
}

double Percentile(std::vector<double>& v, double p) {
    if (v.empty()) return 0;
    const auto k = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

void Report(const char* name, std::vector<double> us) {
    if (us.empty()) {
        std::printf("%-6s n=0\n", name);
        return;
    }
    double sum = 0;
    for (double x : us) sum += x;
    const double p0 = *std::min_element(us.begin(), us.end());
    const double p100 = *std::max_element(us.begin(), us.end());
    std::printf("%-6s n=%zu  p0=%.1f p25=%.1f p50=%.1f p90=%.1f p99=%.1f max=%.1f mean=%.1f us"
                "  total=%.3f s\n",
                name, us.size(), p0, Percentile(us, 0.25), Percentile(us, 0.50),
                Percentile(us, 0.90), Percentile(us, 0.99), p100, sum / us.size(), sum / 1e6);
}

int Fail(const std::string& what) {
    std::fprintf(stderr, "pool_sweep_bench: %s\n", what.c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t budget = 0;
    std::size_t past_limit = 0;
    std::string path;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--budget") == 0) budget = std::strtoull(argv[i + 1], nullptr, 10);
        else if (std::strcmp(argv[i], "--past") == 0) past_limit = std::strtoull(argv[i + 1], nullptr, 10);
        else if (std::strcmp(argv[i], "--file") == 0) path = argv[i + 1];
    }
    if (budget == 0 || path.empty()) {
        return Fail("usage: --budget N --file PATH [--past N]");
    }
    const std::size_t pages = 2 * budget;

    auto device = FilePageDevice::Open(path);
    if (!device.ok()) return Fail(device.status().message());

    std::vector<PageId> ids;
    ids.reserve(pages);
    {
        auto store = DevicePageStore::Open(*device.value());
        if (!store.ok()) return Fail(store.status().message());
        const auto start = Clock::now();
        std::vector<PageId> batch;
        while (ids.size() < pages) {
            auto created = store.value()->CreateNew();
            if (!created.ok()) return Fail(created.status().message());
            kds::storage::FormatPage(created.value().second.bytes(), kds::PageType::kHeap);
            batch.push_back(created.value().first);
            ids.push_back(created.value().first);
            if (batch.size() == kSetupBatch || ids.size() == pages) {
                created.value().second.Release();
                if (auto s = store.value()->Flush(); !s.ok()) return Fail(s.message());
                if (auto s = store.value()->EvictClean(batch); !s.ok()) return Fail(s.message());
                batch.clear();
            }
        }
        if (auto s = store.value()->Sync(); !s.ok()) return Fail(s.message());
        std::printf("setup: %zu pages written in %.1f s\n", pages,
                    std::chrono::duration<double>(Clock::now() - start).count());
    }

    auto store = DevicePageStore::Open(*device.value());
    if (!store.ok()) return Fail(store.status().message());
    store.value()->SetFrameBudget(budget);

    std::vector<double> below;
    std::vector<double> past;
    below.reserve(budget);
    past.reserve(past_limit != 0 ? past_limit : budget);
    const long rss_before = RssKiB();
    for (std::size_t i = 0; i < pages; ++i) {
        const auto t0 = Clock::now();
        auto ref = store.value()->GetForRead(ids[i]);
        const auto t1 = Clock::now();
        if (!ref.ok()) return Fail(ref.status().message());
        const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        (i < budget ? below : past).push_back(us);
        if (i >= budget && past_limit != 0 && past.size() == past_limit) break;
    }
    const long rss_after = RssKiB();

    std::printf("budget=%zu pages=%zu resident=%zu rss_before=%ld KiB rss_after=%ld KiB\n",
                budget, pages, store.value()->resident_pages(), rss_before, rss_after);
    Report("below", below);
    Report("past", past);
    if (!past.empty() && past.size() < budget) {
        double sum = 0;
        for (double x : past) sum += x;
        std::printf("past: sampled %zu of %zu; the whole half extrapolates to %.1f s\n",
                    past.size(), budget, sum / 1e6 * static_cast<double>(budget) / past.size());
    }
    return 0;
}
