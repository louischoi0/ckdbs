# RW-B cell 1 — the R1 cell at the woken reactor

**Measured 2026-08-26** in worktree `v2.3.0-reactor-wake` at
**`v2.2.1-4-g656f744`** (the "after" arm) against **`bce12d0`**
(`v2.2.1-3-gbce12d0`, the "before" arm), both built `Release` and both run on
the same 8-CPU host within the same hour. The version is `2.3.0` by the
operator's naming; **no `v2.3.0` tag exists yet**, so both arms name
themselves by `git describe --tags` off the `v2.2.1` line, per the
version-management rule that nothing is back-filled.

Order: `instructions/v2.3.0-reactor-wake.md` §5 cell 1. Judges **claim 1**
("the R1 penalty is the block") and reports on **claim 3** in passing.

> **Scope, stated first.** This is one cell of RW-B, not the run. Cells 2
> (the knob sweep), 4 (the hot-path cost at load) and 5 (the commit path)
> are **not run**, and nothing here says the wake is free at load or that
> the commit path is unharmed — only that the R1 penalty is gone. Three reps
> per arm, 2,000 rows, no null cell. Read the spreads below before quoting
> the ratio.

---

## 1. What was run

```
bench/single_relation_probe.py --server <build-release/kds_server> \
    --workdir <fresh> --arm multi --cores 4 --sessions 1 --rows 2000 \
    --seat <owner|foreign> --arrival-core -1 --json <out>
```

Six invocations per arm-pair, **interleaved** `foreign, owner, foreign,
owner, …` with a fresh server and a fresh data file each time (the driver's
own rule — a second run on the same file measures a taller btree). Default
`group` durability, `wal_drain_interval_us` 1000. The box was idled to
`loadavg < 1.2` before each pair began.

The "before" binary is the engine at `bce12d0` built from a clean
`git archive` extract, not from a reverted working tree: the point of the
control is the engine as it stood, and a partially-reverted tree is neither
engine.

## 2. The result

| arm | seated ips | seated p50 | shipped ips | shipped p50 | **ratio** | **shipped − seated p50** | arrival cpu |
|---|---|---|---|---|---|---|---|
| **before** (`bce12d0`) | 1,389.4 | 713.0 µs | 578.1 | 1,728.8 µs | **0.416** | **+1,015.8 µs** | 0.883 |
| **after** (`656f744`) | 1,479.1 | 652.9 µs | 1,464.8 | 658.7 µs | **0.990** | **+5.8 µs** | 0.878 |

Medians of three reps. Per-rep spreads: before, shipped 578–582 and seated
1,384–1,488; after, shipped 1,376–1,529 and seated 1,284–1,673.

**The before arm reproduces SS-B**, which is what makes the after arm worth
reading: `bench/v2.2.0/results-shipping-ssb-v2.2.0-11-g982e133.md` published
0.429 at K = 1 and a shipped-minus-seated delta of 1,064 µs; this box, two
commits later and a different day, gives 0.416 and 1,016 µs. That is the
order's G1 gate — re-measure the premise before building the fix — passed
rather than assumed.

**Claim 1 is upheld.** The order asked for ≥ 0.90 and the cell reads 0.990.
The statistic that carries it is not the ratio but the **latency delta**:
+1,015.8 µs → **+5.8 µs**, a factor of 175. The seated arm's spread (±13% on
the after arm) is wide enough that a ratio near 1.0 should be read as "at
parity, within this cell's noise" and not as a third digit; the delta is a
difference of within-run medians and sits three orders of magnitude outside
that noise.

## 3. Why this is the block and not something else

Two independent readings, both already in hand:

- **The mechanism was source-read before it was measured** — nothing in the
  ring or the scheduler ended an `epoll_wait` block
  (`src/sched/scheduler.cpp:319` → `epoll_io_backend.cpp:88`; the ring seen
  only by `DrainInbox()` at `:62`, called at `:353`). The fix adds exactly
  one thing: an eventfd the epoll set already watches, written by a peer's
  send.
- **The end-to-end test fails by waiting out a 30-second block** when the
  wake is removed, and it was run that way to prove it can
  (`tests/reactor_wake_test.cpp`,
  `AMessageToASleepingReactorDoesNotWaitOutItsBlock`; the control measured
  10,001 ms against a 1,000 ms bound before the watchdog cut it short).

## 4. What did **not** move, and it is the honest half

**Arrival-core CPU is 0.883 before and 0.878 after.** One parked waiter
still burns ~88% of a core, exactly as SS-B §7 measured it. That is the
*other* half of `sched.md` §4's finding — `IdleTimeoutMs` counting a parked
coroutine as runnable — and this work does not touch it. Claim 3 ("the
arrival core's 89% is the ready-queue misclassification alone") is therefore
**not judged here**; it belongs to RW3, which is unbuilt.

Worth stating plainly because the two are easy to conflate: **the wake fixed
the latency, not the spin.** A shipped statement no longer waits a
millisecond; the core waiting for it still spins while it waits. On a host
with spare CPUs — this one — that costs nothing measurable, which is why
throughput reaches parity with the spin fully intact.

## 5. What this cell hands onward

- **`docs/spec/crosscore.md` §9's routing decision** inherits a new number.
  SS-B handed it "shipping costs ~2× in the R1 regime, and D6 ships
  unconditionally, so that penalty is being paid today". At `656f744` the R1
  penalty is **1.0×**, so the argument for a load-aware ship-or-refuse
  policy loses its measured motivation. The decision stays the operator's;
  what changed is the input.
- **`docs/inflight/known-gaps.md`'s idle-block entry** can close on this
  number once the rest of RW-B runs. It is amended, not closed, today.
- **RW6 (the sub-millisecond block) has no case yet.** The order gated it on
  a residual in cell 2's knob sweep; a 5.8 µs delta leaves no room for the
  1 ms rounding floor to be costing anything on this path. Cell 2 still
  decides it.

## 6. Reproducing

Both arms, the analysis, and the raw per-rep JSON are under
`/home/ubuntu/rw-b/` on the measuring host and are **not** archived here:
the scenario-archive rule covers scenario drivers, and this is a narrower
measurement. To re-run, the invocation in §1 is complete as written — the
only thing it needs that this file does not carry is a quiet box.
