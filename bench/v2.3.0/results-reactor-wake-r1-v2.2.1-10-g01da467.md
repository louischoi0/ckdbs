# RW-B cell 1 — the R1 cell at the woken reactor

**Measured 2026-08-26** in worktree `v2.3.0-reactor-wake`, three arms, all on
the same 8-CPU host within the same two hours, all `Release`:

- **before** — the engine at **`bce12d0`** (`v2.2.1-3-gbce12d0`), extracted
  clean with `git archive` and built from scratch. No wake path.
- **after (shipped)** — **`v2.2.1-10-g01da467`**, the merge that takes
  `origin/main`'s wake path (`c4732ba`, `1c52ded`, `sched/waker.hpp`).
- **after (discarded)** — `656f744`, this worktree's own independent
  implementation of the same mechanism, discarded in that merge. Reported
  because it is a second, independently written answer measuring the same
  thing, which is worth more than a footnote.

The version is `2.3.0` by the operator's naming; **no `v2.3.0` tag exists
yet**, so every arm names itself by `git describe --tags` off the `v2.2.1`
line, per the rule that nothing is back-filled.

Order: `instructions/v2.3.0-reactor-wake.md` §5 cell 1. Judges **claim 1**
("the R1 penalty is the block") and reports on **claim 3** in passing.

> **Scope, stated first.** One cell of RW-B, not the run. Cells 2 (the knob
> sweep), 4 (the hot-path cost at load) and 5 (the commit path) are **not
> run**; nothing here says the wake is free at load or that the commit path
> is unharmed. Three reps per arm, 2,000 rows, no null cell. Read §2's
> spreads before quoting a ratio.

---

## 1. What was run

```
bench/single_relation_probe.py --server <build>/kds_server \
    --workdir <fresh> --arm multi --cores 4 --sessions 1 --rows 2000 \
    --seat <owner|foreign> --arrival-core -1 --json <out>
```

Six invocations per arm, **interleaved** `foreign, owner, foreign, owner, …`
with a fresh server and a fresh data file each time (the driver's rule — a
second run on the same file measures a taller btree). `group` durability,
`wal_drain_interval_us` 1000, and the box idled to `loadavg < 1.2` before
each arm began.

The before arm is the engine as it stood, built from a clean extract rather
than a reverted working tree: a partly-reverted tree is neither engine.

## 2. The result

| arm | seated ips | seated p50 | shipped ips | shipped p50 | **ratio** | **shipped − seated p50** | arrival cpu |
|---|---|---|---|---|---|---|---|
| **before** `bce12d0` | 1,389.4 | 713.0 µs | 578.1 | 1,728.8 µs | **0.416** | **+1,015.8 µs** | 0.883 |
| **after** `01da467` (shipped) | 1,483.9 | 705.7 µs | 1,467.6 | 703.4 µs | **0.989** | **−2.3 µs** | 0.898 |
| after `656f744` (discarded) | 1,479.1 | 652.9 µs | 1,464.8 | 658.7 µs | 0.990 | +5.8 µs | 0.878 |

Medians of three reps. Per-rep spreads: before, shipped 578–582 / seated
1,384–1,488; after (shipped impl), shipped 1,376–1,529 / seated 1,284–1,673.

**The before arm reproduces SS-B**, which is what makes the rest worth
reading: `bench/v2.2.0/results-shipping-ssb-v2.2.0-11-g982e133.md` published
0.429 at K = 1 with a shipped-minus-seated delta of 1,064 µs; this box, a
different day and two commits later, gives 0.416 and 1,016 µs. That is the
order's G1 gate — re-measure the premise before building the fix — passed
rather than assumed.

**Claim 1 is upheld.** The order asked for ≥ 0.90; the shipped
implementation reads 0.989. The statistic that carries it is not the ratio
but the **latency delta**: **+1,015.8 µs → −2.3 µs**. A negative number
here means only that the difference has vanished into the run-to-run noise
of two ~700 µs medians — the honest reading is *zero*, not *faster*. The
seated arm's spread (±13%) is wide enough that a ratio near 1.0 should be
read as "at parity within this cell" and never as a third digit; the delta
is a difference of within-run medians and sits three orders of magnitude
outside that spread.

**Two independent implementations, one number.** 0.989 and 0.990, −2.3 µs
and +5.8 µs. The agreement is not a check on the measurement so much as on
the *diagnosis*: two sessions read the same SS-B finding, built the
mechanism differently — an eventfd owned by the backend against one owned by
the reactor — and recovered the same millisecond.

## 3. Why this is the block and not something else

- **The mechanism was source-read before it was measured.** Nothing in the
  ring or the scheduler ended an `epoll_wait` block; the ring is a store to
  shared memory and epoll cannot watch memory. The fix adds one thing: an
  fd the epoll set already watches, written by a peer's send.
- **The end-to-end test fails by waiting out its block** when the wake is
  removed. On the discarded implementation this was run as a deliberate
  negative control: 10,001 ms against a 1,000 ms bound, where the passing
  form takes under 1 ms.

## 4. What did **not** move, and it is the honest half

**Arrival-core CPU is 0.883 before and 0.898 after.** One parked waiter
still burns ~90% of a core, exactly as SS-B §7 measured it. That is the
*other* half of `sched.md` §4's finding — `IdleTimeoutMs` counting a parked
coroutine as runnable — and neither implementation touches it. Claim 3
("the arrival core's 89% is the ready-queue misclassification alone") is
therefore **not judged here**; it belongs to RW3, which is unbuilt.

Worth stating plainly, because the two are easy to conflate: **the wake
fixed the latency, not the spin.** A shipped statement no longer waits a
millisecond; the core waiting for it still spins while it waits. On a host
with spare CPUs — this one — that costs nothing measurable, which is why
throughput reaches parity with the spin fully intact. On a host without
them it would not.

## 5. What this cell hands onward

- **`docs/spec/crosscore.md` §9's routing decision** inherits a new number.
  SS-B handed it "shipping costs ~2× in the R1 regime, and D6 ships
  unconditionally, so that penalty is being paid today". At `01da467` the R1
  penalty is **1.0×**, so the case for a load-aware ship-or-refuse policy
  loses its measured motivation. The decision stays the operator's; the
  input changed.
- **`docs/inflight/known-gaps.md`'s idle-block entry** can close on this
  number once the rest of RW-B runs.
- **RW6 (the sub-millisecond block) has no case yet.** The order gated it on
  a residual in cell 2's knob sweep; a delta indistinguishable from zero
  leaves no room for the 1 ms rounding floor to be costing anything on this
  path. Cell 2 still decides it.

## 6. Reproducing

The invocation in §1 is complete as written, and the raw per-rep JSON for
all three arms is archived beside this file in
`bench/v2.3.0/archive/cell1-r1/`, prefixed by arm. The only thing it needs
that this file cannot carry is a quiet box.

**Corrected 2026-08-27.** This section previously said the output was *not*
archived, on the grounds that CLAUDE.md's archive rule covers scenario
drivers and this is a narrower measurement. That reading was wrong for this
series: `instructions/v2.3.0-reactor-wake.md` §5 asks for RW-B's raw driver
output archived beside its results, and an order's own instruction outranks
the general exemption. The files were back-filled rather than re-measured —
they are the same JSON the tables above were computed from.
