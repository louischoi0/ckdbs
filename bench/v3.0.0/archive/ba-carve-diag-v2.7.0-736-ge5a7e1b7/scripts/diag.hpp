#pragma once
// DIAG only (relaxed-carve-regression): never landed.
#include <chrono>
#include <cstdio>
namespace kds {
inline long long DiagMs() {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace kds
#define KDS_DIAG(...) std::fprintf(stderr, __VA_ARGS__)
