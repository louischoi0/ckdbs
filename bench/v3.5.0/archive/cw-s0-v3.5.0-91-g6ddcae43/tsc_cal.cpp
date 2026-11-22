// CW-S0: the TSC's rate against steady_clock, and the cost an empty
// `rdtsc` bracket adds to what it brackets (the spike's Start/Stop pair).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <x86intrin.h>

int main() {
    using clk = std::chrono::steady_clock;
    const auto w0 = clk::now();
    const std::uint64_t t0 = __rdtsc();
    while (clk::now() - w0 < std::chrono::milliseconds(800)) {
    }
    const std::uint64_t t1 = __rdtsc();
    const double ns = std::chrono::duration<double, std::nano>(clk::now() - w0).count();
    const double ghz = double(t1 - t0) / ns;
    // An empty bracket: the ticks between two back-to-back reads, as the
    // spike's Stop sums them (the add after the second read is outside).
    std::atomic<std::uint64_t> sum{0};
    constexpr int kN = 20'000'000;
    for (int i = 0; i < kN; ++i) {
        const std::uint64_t a = __rdtsc();
        const std::uint64_t b = __rdtsc();
        sum.fetch_add(b - a, std::memory_order_relaxed);
    }
    std::printf("tsc_ghz=%.4f empty_bracket_ticks=%.2f empty_bracket_ns=%.2f\n", ghz,
                double(sum.load()) / kN, double(sum.load()) / kN / ghz);
}
