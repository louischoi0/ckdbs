#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Deterministic crash points: the process killing itself at a source line
// it names, because a window microseconds wide is never hit by an external
// killer and this host has no `gdb`. Built for the 2PC gate (RP7,
// `instructions/v2.4.0/2pc.md` §5).
//
// **No engine call site exists**: the cross-owner commit path's points
// retired with 2PC (AT-S6; history at cf889b63). The facility and
// `tests/crash_point_test.cpp` remain for the next protocol point.
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
// Cost of a call when unarmed - every production process - is an
// out-of-line call, the once-guard's acquire load, and an `empty()` test.
//
// Syntax: `KDS_CRASH_POINT=<name>` fires at the first hit of `<name>`;
// `KDS_CRASH_POINT=<name>:<n>` fires at the n-th (1-based). The ordinal is
// what lets a process be killed at a point's n-th hit rather than its first.
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
// facility itself.
std::string_view ArmedCrashPoint() noexcept;

}  // namespace kds::base
