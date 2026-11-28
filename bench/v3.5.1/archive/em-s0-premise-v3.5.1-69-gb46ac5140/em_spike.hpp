#pragma once
// EM-S0 SPIKE - never committed to the source tree; archived beside the
// results so the run can be rebuilt. EA-S0's spike (bench/v3.4.17/archive/
// ea-s0-premise-v3.4.17-202-g481d101aa/ea_spike.hpp) with five more scopes:
// FindNamespaceOidByName (ns), KeptParse::Of (keptof), HolesDelimited (holes),
// parser::Bind (bind) and BoundStatement::Materialize (mat). `Report()` is
// appended to PING's reply.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
namespace kds::em_spike {
enum Cat { kParse, kFingerprint, kArguments, kSubstitute, kCount, kCompile, kNamespace, kKeptOf,
           kHoles, kBind, kMaterialize, kCats };
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
    static const char* names[kCats] = {"parse", "fp",     "args",  "subst", "count", "compile",
                                       "ns",    "keptof", "holes", "bind",  "mat"};
    std::string s;
    for (int i = 0; i < kCats; ++i) {
        s += std::string(" ") + names[i] + "_ns=" + std::to_string(g_ns[i].load()) + " " +
             names[i] + "_calls=" + std::to_string(g_calls[i].load());
    }
    return s;
}
}  // namespace kds::em_spike
