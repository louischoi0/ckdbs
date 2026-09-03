# `tests/core_runtime_test.cpp` models a topology its volume does not have

**Found 2026-09-03 at `f027a3c`. Open.** Cost: a **test-coverage** defect,
not a wrong answer in the engine — but it is the kind that hides wrong
answers, and it already did.

## What is wrong

`CoreRuntimeTest::SetUp` calls `bootstrap::BootstrapDatabase`, which writes
a **single-stream** volume — every volume this build creates is one
(`src/bootstrap/bootstrap.cpp`). Most cells in the file then model a peer
under **per-core streams**: opening its own `wal-<core>-*`, running its own
`RecoverCoreAtMount`, publishing its own anchor. **No real instance can be
in that combination**, and a peer on a single-stream volume that is handed
no stream to attach to is refused at `Open` — correctly.

## Why it went unnoticed until now

A peer's `superblock_` used to be a *default-constructed* copy, whose
`log_topology` read `kPerCoreStreams` (0) whatever the volume said, and
`CoreRuntime::Config` carried the topology as a separate field the fixture
left at its default. Two independent sources both said "per-core" and
neither was the volume, so the contradiction had nothing to surface
against.

Handing a peer the volume's **whole image** (AL-S9 review, `f027a3c`) made
it surface all at once: **123 of 3253 cells failed**, three of them with a
segfault, every one of them refusing for the same right reason.

## The reproduction

Delete the `SetLogTopology(kPerCoreStreams)` override in
`CoreRuntimeTest::SetUp` so `ConfigFor` hands the volume's real image, and
run `ctest -R CoreRuntime`. The refusals are the correct behaviour of a
peer on a single-stream volume.

## What is in the tree instead

The override, named as a symptom in both `SetUp` and
`superblock.hpp`'s `SetLogTopology`. It keeps every *other* field of the
image true on a peer — which is the half of the AL-S9 fix that was never in
question — while the topology stays what the cells were written against.

## What the fix is

Rebuild the fixture so a peer **attaches to core 0's shared stream**, the
way `Expeditor` wires it, and keep a per-core arm only for cells that
genuinely test the legacy topology — those need a volume that says so, and
`BootstrapDatabase` has no parameter for it today. That is a stage of work,
not an edit: it changes what ~100 cells exercise, and several assert on a
peer's own recovery, which does not run under one stream.

It belongs with the `Expeditor` cell in
`../known-gaps.md`: both are the same shape — the tests model the assembly
instead of running it.
