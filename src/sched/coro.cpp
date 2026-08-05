#include "kds/sched/coro.hpp"

namespace kds::sched {
namespace {

// Core-local rather than atomic, for the reason step_vm.cpp's page-span
// guard gives about its own counters: a coroutine runs on one core, and an
// atomic here would suggest a cross-core protocol that does not exist.
thread_local SuspendAuditFn g_audit = nullptr;
thread_local bool g_audit_tripped = false;
thread_local std::string_view g_audit_reason;

}  // namespace

void SetSuspendAudit(SuspendAuditFn fn) noexcept { g_audit = fn; }
SuspendAuditFn suspend_audit() noexcept { return g_audit; }

bool SuspendAuditTripped() noexcept { return g_audit_tripped; }
std::string_view SuspendAuditReason() noexcept { return g_audit_reason; }

void ResetSuspendAudit() noexcept {
    g_audit_tripped = false;
    g_audit_reason = {};
}

void NoteSuspension() noexcept {
    if (g_audit == nullptr) return;
    const std::string_view reason = g_audit();
    if (reason.empty()) return;
    // Recorded, not fatal. A suspension under a page span is a defect, and
    // the honest way to surface it is a test that reads this - aborting
    // would take out a server for a rule whose whole point is that it is
    // checkable before anything depends on it.
    g_audit_tripped = true;
    g_audit_reason = reason;
}

}  // namespace kds::sched
