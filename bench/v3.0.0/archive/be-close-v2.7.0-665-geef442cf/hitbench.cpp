// Hit-path cost of DevicePageStore::GetForRead on a resident pool: the
// per-page price a resident scan pays. Built against e4b107af and 0da17f9f
// (both take Open(device, first_new_page_id)).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/storage/page_header.hpp"

using namespace kds;
using namespace kds::storage;

int main(int argc, char** argv) {
    const std::size_t pages = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 8192;
    const int passes = argc > 2 ? std::atoi(argv[2]) : 200;
    auto device = MemoryPageDevice::Create(256, 0);
#ifdef KDS_HAS_CAPACITY
    auto store = DevicePageStore::Open(*device.value(), FrameCapacity{1u << 20}, 16);
#else
    auto store = DevicePageStore::Open(*device.value(), 16);
#endif
    std::vector<PageId> ids;
    for (std::size_t i = 0; i < pages; ++i) {
        auto made = store.value()->CreateNew();
        FormatPage(made.value().second.bytes(), PageType::kHeap);
        ids.push_back(made.value().first);
    }
    (void)store.value()->Sync();
    std::vector<double> per_pass;
    std::uint64_t sink = 0;
    for (int p = 0; p < passes; ++p) {
        const auto t0 = std::chrono::steady_clock::now();
        for (const PageId id : ids) {
            auto ref = store.value()->GetForRead(id);
            sink += static_cast<std::uint64_t>(ref.value().bytes()[100]);
        }
        const auto t1 = std::chrono::steady_clock::now();
        per_pass.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / pages);
    }
    std::sort(per_pass.begin(), per_pass.end());
    std::printf("pages=%zu passes=%d ns/hit p0=%.1f p50=%.1f p90=%.1f (sink %llu)\n", pages, passes,
                per_pass.front(), per_pass[per_pass.size() / 2], per_pass[per_pass.size() * 9 / 10],
                static_cast<unsigned long long>(sink));
}
