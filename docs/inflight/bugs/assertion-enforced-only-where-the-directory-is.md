# A write on a core holding no Bound Cabin for the relation is admitted unchecked

**Found** 2026-09-09 by the `critics-developer` pass on AT-S5
(`091be8c` on `m3-at`), as its D1. **Not fixed.** Verified at `091be8c`; the
fix's shape is decided (operator, 2026-09-09) and its stage is **AT-S5d**.

**Cost: a quiet wrong answer.** The constraint reports enforcing and does
not enforce, and the two cores' pictures of the same group diverge — one
holds a directory whose aggregate says `count = 1` while the relation holds
two rows.

## The wrong behaviour

`exec::AssertionEnforcer` is a **by-value member of `CommandDispatcher`**
(`include/kds/server/command_dispatcher.hpp:2893`), so there is one per
core, and its own header says so: *"core-local, no latches (§6.1). The
registry is dispatcher-owned and memory-resident"*
(`include/kds/exec/assertion_check.hpp:66-69`).

`AdmitInsert` answers `OK` for an oid its `by_oid_` has never heard of
(`src/exec/assertion_check.cpp:181-182`), which is the right answer for a
relation carrying no assertion and the wrong one for a relation whose
assertion lives on another core. The only refusal is `CannotEnforce`
(`src/server/command_dispatcher.cpp:6923`), and `unenforceable_` is
populated only by a core that **owns** the relation and failed to revive —
`ResumeAssertionsAfterRecovery`'s owner filter counts every other core's
assertion `assertions_foreign` and adopts and notes nothing
(`src/server/mount_recovery.cpp:265-269`). So a core that never owned the
relation is false for both predicates and the write goes through.

Until AT-S5 nothing reached that state: the write was routed to the
relation's owner, which is the core that holds the directory. AT-S5 made a
write run where the session is and unified none of the per-core structures
the routing had made sound.

## Reproduction

`tests/expeditor_test.cpp:504-572`
(`ExpeditorTest.APeerThatOwnsAnAssertionMountsAndComesUpEnforcingIt`), one
of the four cells AT-S5 left failing and the only one that is a defect
rather than a retired claim. Two cores, `placement = kRotate`, so `cap` and
its `CHECK COUNT(*) <= 1` land on core 1; remount; core 1 reports
`assertions_enforcing == 1`; then a second `INSERT INTO cap VALUES (7)`
over the debug text port — which is **core 0's** listener — is admitted
where it must be refused `ASSERTION_VIOLATION`.

## The fix, and why it is a stage rather than a patch

One `AssertionEnforcer` for the instance, owned by `Expeditor` and handed to
every core through `CoreRuntime::Config`, which is the idiom `LockTable`
(`include/kds/txn/lock_table.hpp:114-117`) and `InstanceVisibility` already
use. What makes it a stage is the seam it opens rather than the plumbing:
the admission and the reservation are two calls with page work between them
(`command_dispatcher.cpp:8102` and `:8237`), atomic today only because
nothing on one cooperative core suspends between them. Sharing the
directory needs the group delta taken **with the check**, under a latch that
never spans a page fetch, or two cores both admit at count 0 and both
reserve. `assertion.md` §6.1's "all Bound Cabin state lives on that
relation's owner core" is the sentence that goes with it.

## Owner

`instructions/v3.0.0/workorder-at-m3-uniformity.md`, AT-S5d.
`docs/spec/assertion.md` §6.1 and `docs/spec/crosscore.md`'s assertion
bullet (§6a) are the two specs that state the retiring rule.
