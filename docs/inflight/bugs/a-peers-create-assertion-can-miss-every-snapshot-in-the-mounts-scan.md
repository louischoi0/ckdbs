# A peer's CREATE ASSERTION can miss every snapshot in the mount's scan

**Found by reading, not reproduced.** Verified at `5574a7e` on
`ay-q7-mark`, by the review of AY-Q7's mark
(`instructions/v3.0.0/raft-marks-2026-09-30.md` §14).

## What is wrong

An assertion's recovery base is an `ASSERT_SNAPSHOT` run. Two writers make
one:

- **Its own publish.** `CreateAssertion` logs a run before it inserts the
  catalog row (`src/exec/assertion_catalog.cpp:560-578`, then `:609`).
- **Core 0's checkpoints.** `Checkpointer::Start` logs a run for every
  assertion in the registry's `live_` right after `CHECKPOINT_BEGIN`
  (`src/wal/checkpointer.cpp:91-124`, `src/exec/assertion_check.cpp:118-128`).

The assertion enters `live_` only at `enforcer_->Adopt`
(`src/server/command_dispatcher.cpp:2834`), after `CreateAssertion` has
returned. On a peer core that is a window in which core 0's checkpoint can
run. `CheckpointGate` orders checkpoints against each other, not against
DDL.

The failing sequence:

1. A peer's `CREATE ASSERTION` logs its publish run at LSN `P` and commits.
2. Before the peer's `Adopt`, core 0 writes `CHECKPOINT_BEGIN` and its
   snapshot run. The run is whole but does not include the new assertion.
3. `RedoStartFrom` takes the minimum of `BEGIN` and the dirty pages'
   recLSNs, and ignores open transactions (`src/wal/analysis.cpp:23-31`).
   If every page dirtied before `P` is clean by then, and every core's
   latest redo start is past `P`, the anchor's `redo_start_lsn` moves
   past `P`.
4. A crash follows before core 0's next checkpoint completes.

The mount scans from `redo_start_lsn`
(`src/server/expeditor.cpp:1041-1053`). It finds no whole run for the
assertion, so the assertion is not recovered.

## What it costs

**A refusal, not a wrong answer.** The mount calls `NoteUnenforceable`.
The relation's writes are then refused `CannotEnforce` on every core until
`DROP ASSERTION` and `CREATE ASSERTION`. No checkpoint snapshots an
assertion outside `live_`, so a restart does not clear it.

If the crash in step 4 cuts core 0's next run, the mount discards that run
too (AY-S8, AY-Q7 (B)), so it ends in the same refusal. Before AY-S8 it was
adopted and under-counted: a **quiet wrong answer**.

## Reproduction

Not reproduced. A cell would need:

- a two-core rig with a `CREATE ASSERTION` on core 1, paused between
  `CreateAssertion` and `Adopt`;
- a completed core-0 checkpoint whose redo start passes the publish run;
- a crash, then a mount.

## The fix

The fix is not settled and not scheduled. Two shapes close the window:

- adopt into the registry before the publish run is logged, so any later
  core-0 snapshot covers the assertion;
- or hold the checkpoint gate across publish and adopt.

Either is a change to `docs/spec/assertion.md` §7/§8.1. No work order
carries it.
