#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Deterministic crash points, for the RP7 correctness gate
// (`instructions/v2.4.0/2pc.md` §5: *"Kill -9 at each protocol point, on
// each side"*).
//
// Why this exists in the engine rather than in the harness. The protocol's
// windows are microseconds wide, so an external killer racing them lands in
// one essentially never - `bench/shipped_kill_recovery_probe.py` kills
// "wherever it lands" and says so, which answers a different question. The
// other way to stop a process at a named source line is a debugger, and
// this host has no `gdb`. What is left is the process killing itself at a
// line it names.
//
// What "crash" means here is exactly what `kill -9` means: `SIGKILL` to
// self. No destructor runs, no buffer is flushed, no `atexit` fires, and
// the WAL and the data file are left holding precisely the bytes the device
// had already taken. A crash point that called `std::exit` or `abort()`
// would be a weaker test - the first unwinds statics, and both let a
// libc-buffered stream drain.
//
// **Armed only by the environment**, never by config: `KDS_CRASH_POINT` is
// read once, and an instance that was not started with it in its
// environment cannot reach a kill. There is no config key, no `SHOW META`
// field and no wire surface, so nothing a client can send arms one.
//
// Cost when unarmed - which is every production process - is one load of a
// cached pointer and a null test, and every call site is on the cross-owner
// commit path. **No crash point sits on the one-owner path**, which is what
// keeps D1's fast path free of this facility as well as of the protocol.
//
// Syntax: `KDS_CRASH_POINT=<name>` fires at the first hit of `<name>`;
// `KDS_CRASH_POINT=<name>:<n>` fires at the n-th (1-based). The ordinal is
// what lets a two-participant transaction be killed after the *second*
// participant reached a point rather than the first.
namespace kds::base {

// What one `KDS_CRASH_POINT` spelling means. `name` empty is "nothing
// armed", which is every process that did not ask for a crash.
struct CrashPointArm {
    std::string name;
    std::uint64_t ordinal = 1;  // 1-based; which hit of `name` fires
};

// The spec parser, exposed because it is the only part of this file with a
// branch in it. Total: every input has a reading, and a trailing `:` or a
// non-numeric suffix is part of the *name* rather than a bad ordinal - a
// crash point may one day be named with a colon in it, and a spec that
// silently lost its tail would arm nothing while looking armed.
CrashPointArm ParseCrashPointArm(std::string_view spec) noexcept;

// Kills this process with `SIGKILL` if `name` is the armed point and this
// call is its armed occurrence. Returns to the caller otherwise, and always
// returns when nothing is armed.
void CrashPointHit(std::string_view name) noexcept;

// The armed point's name, or empty when nothing is armed. For tests of the
// facility itself; the engine calls `CrashPointHit` only.
std::string_view ArmedCrashPoint() noexcept;

}  // namespace kds::base
