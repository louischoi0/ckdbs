# RW-B cell 6 — correctness, at the tree the other five cells describe

**Run 2026-08-27** in worktree `v2.3.0-rwc1` at **`1a305ab`**
(`v2.2.1-15-g1a305ab`), which is the wake path, the park rule, D7's counters
and RW4's test — everything the v2.3.0 order builds — in one tree.

Order: `instructions/v2.3.0-reactor-wake.md` §5 cell 6. It judges no §1
claim; it is the gate the other five stand on, and its terms are: *"The full
suite green (the step gate, unsuspended); `scripts/sim.sh` 95 cells green
with RW4's new case; rows in = rows out in every throughput cell. **A hang
is a blocking finding and stops the chain** — this version's failure mode is
not a slow statement, it is a statement that never answers."*

> **All four pass.** Suite **2743/2743**, sim **171 runs / 0 failures**,
> **1,543,600 INSERTs in = 1,543,600 out** across cells 2 and 4, and nothing
> hung.

---

## 1. The unit suite

```
cmake --build build-release -j8 && build-release/tests/kds_tests
```

**2743 tests from 256 suites, 2743 passed, 1 disabled**, 28.2 s. Cell 3 last
ran this at `12c0ebb` and read 2741; the two added are this commit's —
`SchedulerWakeTest.TheBlockAndTheWakesAroundItAreCounted` (D7's counters) and
`SchedulerWakeTest.AParkedCoroutineWithOnlyARingWakeIsResumedPromptly`
(RW4's composed shape).

**The second one is the cell's real subject**, because it is the only test
in the tree whose failure mode is the one the order names. It parks a
coroutine on a flag that nothing but a peer's ring message sets, waits for
`parked_idle_blocks()` to confirm the reactor is actually asleep *on that
park* — a state nothing before RW3 could produce — then sends, and asserts
the resume inside 100 ms against a `max_idle_block_ms` of 1000. **The
deadline is the test's own**: a lost wake fails a named assertion with a
message, never a CI timeout. Measured resume: **0 ms**.

## 2. The simulation corpus

```
scripts/sim.sh          # every committed seed, plus 4 derived from today
```

**171 runs, 0 failures.** Every committed seed in
`tests/testdata/sim_seeds.txt` across `clean`/`sync-crash`/`crash` × faults
`none`/`io` × profiles `uniform`/`zipfian`/`colliding`, plus the on-vs-off
pairing, plus four date-derived fresh seeds (`20260827000`–`…003`).

**Cell 3 reported 2 failures here and this run reports none**, which is not
a fix: the fresh seeds are derived from the date, so the corpus differs day
to day, and the two failures that day were the `chain-order`-under-injected-
faults defect already in `docs/inflight/known-gaps.md`, reproduced equally by
the pristine tree at `bce12d0`. Nothing in this version touches it either
way.

**What the sim does *not* cover, said plainly**: the harness under `sim/`
builds a whole instance on crashable in-memory devices and drives it through
`CommandDispatcher` — it constructs **no reactor**, so it has no idle block
to interrupt and no wake path to exercise. Its value here is as a regression
gate over recovery and the storage engine while the scheduler changed
underneath them. `docs/spec/sched.md` §8 carries the same statement so that
nobody reads "sim green" as "the wake is simulated".

## 3. Rows in = rows out

| cell | runs | attempted | executed | refused | `verify` |
|---|---|---|---|---|---|
| 2 — the knob sweep and its probes | 66 | 223,600 | 223,600 | **0** | rows as expected, all |
| 4 — the hot-path cells | 84 | 1,320,000 | 1,320,000 | **0** | rows as expected, all |
| **total** | **150** | **1,543,600** | **1,543,600** | **0** | |

Both cells ran across the pre-wake and post binaries alternately, so the
count covers 75 runs of an engine with no wake path and 75 of one with it.

## 4. Hangs

**None.** The failure mode this version could have introduced is a statement
that never answers — a park whose only wake source is a message that no
longer arrives — and three independent things would have caught it: the
per-statement deadline in `single_relation_probe.py` (never reached in 150
runs), the sim's own crash/restart reconciliation, and RW4's test deadline.
D4's ceiling is the structural reason it cannot happen at all:
`max_idle_block_ms` bounds every block, so a lost wake is bounded latency
rather than a hang, and `docs/spec/sched.md` invariant 7a now says so.

## 5. What this cell does not cover

- **The interleaved A/B overhead measurement** the Session Workflow's step 3
  normally requires is *not* suspended for this version — the order says so —
  and it is cells 2, 4 and 5 rather than this one.
- **A sanitizer matrix.** SIM13's, unbuilt, unchanged by this version.
- **`cores > 4`.** Every cell here runs at 1 or 4 cores on an 8-logical
  host; the wake path is per-reactor and its cost model is per-send, but
  nothing measured says what happens at 8.
