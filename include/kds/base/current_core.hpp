#pragma once

#include <cstdint>

// **Which core the calling thread is**, for the structures that are about to
// stop having one of their own (AM-S2 step 3).
//
// Every per-core object in this engine has carried its core id as a member,
// which was exact while each core had its own copy of everything. The shared
// buffer pool ends that for the frame table: one `DevicePageStore` serves
// every core, so a member `core_id_` would answer "whose store is this"
// where the caller is asking "who am I" - and `page_latch.hpp` writes the
// asking core into the latch word, so a shared store stamping its own id
// would record core 0 for every holder and make the owner field a lie.
//
// **This is exact rather than a convenience, and the premise is checkable.**
// The engine runs one reactor per thread and pins it there
// (`docs/spec/sched.md` §2); the only `std::thread`s it creates are the WAL
// writer's, which touches no page store, and `Expeditor`'s per-core workers.
// A `PageRef` never crosses threads, so a latch is always released by the
// core that took it.
//
// **Zero is the honest default**, not a fallback: a thread that never
// declared itself is the startup/mount thread, which does its work before
// any peer worker exists and is core 0's by construction. A test that drives
// a reactor by hand is in the same position.
//
// Why not thread the id through every accessor: `store()` alone has 337 call
// sites, and each would carry an argument that is constant for the life of
// the thread. Why not a per-core view object wrapping the store: the same
// 337 sites, plus a second name for the store.

namespace kds {

namespace detail {
inline thread_local std::uint32_t g_current_core = 0;
}  // namespace detail

// The core this thread is the reactor for. 0 where nothing said otherwise.
inline std::uint32_t CurrentCore() noexcept { return detail::g_current_core; }

// Declared once per reactor thread, before it does any work. Cheap enough to
// re-declare every iteration, which is what `Scheduler::RunOnce` does so a
// reactor driven by hand is covered by the same statement as one in `Run`.
inline void SetCurrentCore(std::uint32_t core) noexcept { detail::g_current_core = core; }

// For a scope that must act as another core - a fixture, or a mount pass
// that stamps a peer's pages before that peer has a thread. Restores on the
// way out, so a caller cannot leak an identity into the rest of its thread.
class CurrentCoreGuard {
public:
    explicit CurrentCoreGuard(std::uint32_t core) noexcept : saved_(CurrentCore()) {
        SetCurrentCore(core);
    }
    ~CurrentCoreGuard() { SetCurrentCore(saved_); }
    CurrentCoreGuard(const CurrentCoreGuard&) = delete;
    CurrentCoreGuard& operator=(const CurrentCoreGuard&) = delete;

private:
    std::uint32_t saved_;
};

}  // namespace kds
