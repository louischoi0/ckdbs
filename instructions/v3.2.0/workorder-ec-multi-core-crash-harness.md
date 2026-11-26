# Work order EC - a multi-core instance crashed and mounted under a seed: the simulator given cores

Status: **WRITTEN 2026-11-26, not opened.** Written on worktree
`known-gaps-milestone-orders` (branch
`worktree-known-gaps-milestone-orders`) from `8274c951`
(`v3.4.17-148-g8274c951e`) on the operator's word of 2026-11-26
(`raft-marks-2026-11-26.md` §3). No stage is started; EC-S0 waits for the
operator's word and for the marks on §4's items. A `[source-read]` claim
with a `path:line` was read at `8274c951`; a claim taken from
`known-gaps.md` carries that entry's own verification commit and is not
re-verified here.

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`,
`[quiet-wrong]`, as in DT's order.

Related: `sim/`, `scripts/sim.sh`, `scripts/bs/`; `docs/spec/sched.md`;
`docs/spec/crosscore.md`; `docs/spec/wal.md` §11;
`docs/inflight/known-gaps.md`, "Testing", "Power cuts (BS-S4)", "Wait
events and ASH (CJ)"; `instructions/v3.0.0/workorder-bs-test-hardening.md`
(BS, closed narrow 2026-11-23);
`instructions/v3.0.0/workorder-av-two-core-rig.md` (AV, closed on
paper).

## 0. What EC is

`[source-read]` The simulator is one `CommandDispatcher` and one
`Session` on crashable in-memory devices (`sim/instance.hpp:11`,
`:123`). The engine it tests runs every statement on its session's core,
checkpoints each core in turn, and shares one pool, one log, one lock
table and one registry between them. **No cell crashes a multi-core
instance and mounts it** (`known-gaps.md:152`): "it needs a multi-core
crash harness that does not exist."

`[design]` EC builds that harness: N cores of one instance, driven on
one thread by a seeded schedule, crashed at a seed-chosen point, mounted
- at the same `cores` or another - and reconciled against the oracle.
It changes no engine behaviour.

## 1. The tree as read

`[source-read]` from `docs/inflight/known-gaps.md` at `8274c951`:

- **The rotated checkpoint gate is pinned in pieces and by no cell end
  to end** (`:152`): the simulator is one core, the two-core rig's gate
  is not rotated, and `SetRotation` is reached only by a running server.
  DH's "a crash between every pair of runs with a live transaction on
  each core" was not built.
- **`sim/` drives no core runtime** (`:2539`), so CJ's I2 is checked
  over unit cells.
- **The sim's constraint stream cannot kill a mutant that ignores held
  assertion contributions** (`:188`): it writes from one session in
  autocommit, so two admissions are never in flight at once.
- **A split page created after the split's first write is guarded by
  one unit cell, not the sim** (`:202`).
- **No cell drives a stale writeback against a re-faulted frame**
  (`:793`).
- **The simulated cuts are narrower than a disk's** (`:3436`): the log
  is cut as a suffix, never scattered; one pending image a page.
- **Cells that failed once or more under `-j8` load and did not
  reproduce** (`:174`-`:283`, `:2282`): a group-commit kick, a reclaim
  while serving, a spin-turn counter, an increment race, an id sequence,
  an expeditor's start, a release kick, a `strict` park. Each is a
  multi-core cell timed by a wall clock.
- **The sim's restore with `skip_recovery` runs no advisory discard**
  (`:2169`); **the sim harness has no Transition op, and generates one
  hidden-Keystone relation in autocommit** (`:1758`, `:1764`).

`[source-read]` in the code at `8274c951`: `SimInstance`
(`sim/instance.hpp:123`) holds one dispatcher (`:328`); the run's
configuration is `SimConfig` (`sim/loop.hpp:129`).

## 2. The mechanism `[design]`

1. **Cores without threads.** A simulated instance owns N per-core
   dispatchers over the instance's shared structures, as a server does.
   One OS thread runs them: a seeded scheduler picks which core's next
   task step runs, at the boundaries the engine already has - a task's
   yield, a park, a wake, a statement's end. Between two boundaries a
   core runs alone, which is the engine's own rule for a reactor.
2. **A virtual clock.** Every deadline, tick and cadence the harness
   drives - the fault net, the checkpoint interval, core 0's tick - is
   read from a clock the schedule advances. No cell waits on a wall
   clock, so a loaded host changes no outcome.
3. **Crash points.** The seed chooses an op and a boundary; the devices
   are cut as today (BS-S4's torn and reordered writes). The mount runs
   at a seed-chosen `cores`, since nothing on disk names a core.
4. **The oracle.** The reconciliation is the one-core oracle's, plus
   the list-append history checker over the interleaved sessions'
   commits.
5. **What it reaches that nothing does**: a crash between two cores'
   checkpoint runs with a transaction open on each; two admissions into
   one assertion group; a writer on one core against a DDL on another; a
   stale writeback against a re-faulted frame.

## 3. Rules

- **EC-R1. A seed is a run**: the same seed, binary and configuration
  give the same schedule, the same crash and the same verdict, on any
  host at any load.
- **EC-R2. The harness drives the engine's own code**: the dispatcher,
  the transaction manager, the lock table, the pool, the log and the
  checkpoint gate are the server's, not doubles. Where a seam is needed
  to drive one from a schedule, it is a seam in how a core is turned,
  never in what the engine decides.
- **EC-R3. A release server executes no instruction EC adds.** A seam
  that cannot be made free is compiled out of release, and the close
  measures a point statement to show it.
- **EC-R4. Every mutant the harness is built to kill is in
  `scripts/bs/mutants.sh` and dies**: `holds-ignored` first (`:188`), a
  checkpoint begin that omits a core's open transaction, a fold that
  moves past an unrun core.
- **EC-R5. The harness's own cost is bounded**: its corpus is planned
  inside the standing 20 minutes a stage, and what does not fit is cut
  and named.
- **EC-R6. What the schedule cannot produce is stated**: an interleaving
  inside one boundary - two threads in one latch's critical section - is
  `tsan`'s and the rigs', not EC's.
- The standing rules, as DT's order lists them.

## 4. Items for the operator (EC-Q), CLA's proposals

| | item | options | CLA proposes |
|---|---|---|---|
| EC-Q0 | Is EC opened, and with what scope? | (a) §2 whole; (b) two cores only; (c) the virtual clock for the existing rigs only | (a), S1 at two cores and S2 at N |
| EC-Q1 | Does EC-S0 gate? | (a) S0 is the census of what a core's turn reads outside its dispatcher - threads, clocks, descriptors, thread-locals; if a core cannot be turned from a schedule without changing what it decides, EC stops and reports the seam. (b) built regardless | (a) |
| EC-Q2 | The push gate | (a) outside it, under the `bs-nightly` label, run by `scripts/bs/nightly.sh`, with a short smoke corpus inside `scripts/sim.sh`; (b) all inside the gate | (a) |
| EC-Q3 | Threads that are real in a server (the WAL writer, the data-sync thread, the page reader) | (a) each run as a scheduled task of the harness, its completion a boundary; (b) left real, with EC-R1 weakened to "same verdict" | (a) |
| EC-Q4 | The flaky wall-clock cells | (a) each re-expressed on the harness where S0 shows it can be, the original kept until its replacement kills the same mutant; (b) outside EC | (a) |
| EC-Q5 | The scattered log cut and a second pending image a page (`:3436`) | (a) a last stage; (b) outside EC | (a) |
| EC-Q6 | The sim's missing ops (Transition, more than one heap, explicit transactions on heaps) | (a) outside EC; (b) inside | (a) |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **EC-S0** | The census of a core's turn and of every real thread; the boundaries listed from `sched.md` and the code; a spike that turns two dispatchers from one loop on the work branch | the census in §7; EC-Q1 read |
| **EC-S1** | Two cores, the virtual clock, the seeded schedule, crash and mount; the oracle over two sessions | EC-R1 shown over 1,000 seeds run twice; `holds-ignored` dies |
| **EC-S2** | N cores, the rotated gate, a mount at another `cores` | `:152`'s cell: a crash between every pair of runs with a live transaction on each core; the two checkpoint mutants die |
| **EC-S3** | The cells EC-Q4 marks; the cuts EC-Q5 marks | each replacement kills its original's mutant |
| **EC-S4** | The close: EC-R3 measured, the corpus's runtime, `sim/`'s README, `known-gaps.md` | the operator's word on landing |

## 6. What EC does not do

- No engine behaviour, format or setting changes.
- No replacement for `tsan`, the two-core rigs or the unit cells.
- No distributed or multi-instance simulation: one instance.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-26

On `known-gaps-milestone-orders` from `8274c951`, on the operator's word.
No stage is opened and no code is written. **Not reviewed**: the
`critics-developer` review was started and stopped before it reported,
so no line reference or reading in this order has a second reader behind
it.
