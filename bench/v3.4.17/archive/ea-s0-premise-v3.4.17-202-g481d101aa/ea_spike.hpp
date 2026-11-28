#pragma once
// EA-S0 SPIKE - never committed to the source tree; kept here so the run can
// be rebuilt. `apply_spike.py <worktree>` inserts one `Scope` at the head of
// Parser::Parse / ParseQuery / ParseConditions (parse), FingerprintOf (fp),
// ArgumentsOf (args), kwp_session's Substitute (subst) and PlaceholderCount
// (count), and exec::Compile (compile), and appends `Report()` to PING's reply.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
namespace kds::ea_spike {
enum Cat { kParse, kFingerprint, kArguments, kSubstitute, kCount, kCompile, kCats };
inline std::atomic<std::uint64_t> g_ns[kCats];
inline std::atomic<std::uint64_t> g_calls[kCats];
inline thread_local int t_depth[kCats];
struct Scope {
    Cat c;
    std::chrono::steady_clock::time_point t0;
    bool outer;
    explicit Scope(Cat cat) : c(cat), outer(t_depth[cat]++ == 0) {
        if (outer) t0 = std::chrono::steady_clock::now();
    }
    ~Scope() {
        if (outer) {
            const auto d = std::chrono::steady_clock::now() - t0;
            g_ns[c].fetch_add(static_cast<std::uint64_t>(
                                  std::chrono::duration_cast<std::chrono::nanoseconds>(d).count()),
                              std::memory_order_relaxed);
            g_calls[c].fetch_add(1, std::memory_order_relaxed);
        }
        --t_depth[c];
    }
};
inline std::string Report() {
    static const char* names[kCats] = {"parse", "fp", "args", "subst", "count", "compile"};
    std::string s;
    for (int i = 0; i < kCats; ++i) {
        s += std::string(" ") + names[i] + "_ns=" + std::to_string(g_ns[i].load()) + " " +
             names[i] + "_calls=" + std::to_string(g_calls[i].load());
    }
    return s;
}
}  // namespace kds::ea_spike
