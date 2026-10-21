# `ExpeditorTest` dispatches on a running peer from the test thread

**The wrong behaviour.** `ExpeditorTest.TwoCoresComeUpOnOneLogAndEachHoldsTheVolumesOwnImage`
(`tests/expeditor_test.cpp`) calls `peer.dispatcher().Dispatch("SHOW META")`
and `Dispatch("SHOW TABLES")` from the test's own thread after `db.Start()`
has put the peer's reactor on a thread of its own. A dispatcher is its
core's reactor's alone (`include/kds/sched/scheduler.hpp`'s threading
block), so the statement races the running reactor: TSan reports
`Scheduler::iterations()`, read by `MetaSections` on the test thread,
against `Scheduler::RunOnce()` writing it on the peer's thread. The defect
is in the test, not the engine: no production path dispatches on another
core's dispatcher.

**Verified at** `8ef3cd98` (`v3.3.0-72-g8ef3cd98`) on the `bs-test-hardening`
branch, under clang 18 TSan - the `tsan` configuration of
`scripts/bs/config.sh`, which BS-S1 adds there
(`instructions/v3.0.0/workorder-bs-test-hardening.md`); the Debug suite
does not see it.

Re-verified by source read at `7dd9b789`: the peer `Dispatch("SHOW META")` and
`Dispatch("SHOW TABLES")` calls still stand at `tests/expeditor_test.cpp:350`
and `:368`.

**Smallest reproduction.**
`scripts/bs/config.sh tsan build`, then
`build-bs-tsan/tests/kds_tests --gtest_filter='ExpeditorTest.TwoCoresComeUpOnOneLogAndEachHoldsTheVolumesOwnImage'`
under `scripts/bs/config.sh`'s `TSAN_OPTIONS`; the report is not certain on
every run.

**What it costs.** A cell whose reads race what they read: in the Debug
suite a torn counter could fail it or pass it without meaning either. It
also keeps the nightly's `tsan` suite red by one cell, named in the report.
No engine answer is affected.

**The fix, known and unscheduled.** Read the peer's image where it is
quiescent or on its own thread. The Expeditor has neither a window between
the peer's build and its thread's start nor a test inbox like
`TwoCoreRig::Post` (BS-S1), so the fix needs a test seam in engine code,
which BS-R2 makes a question of its own. BS's stage log names it.
