# RW-B cell 3 — the waiter population, and what stopping the spin costs

**Measured 2026-08-26** in worktree `v2.3.0-reactor-wake`, an **interleaved
A/B** of two binaries built from the same tree an hour apart:

- **pre** — `8048b5d` (`v2.2.1-11-g8048b5d`), the wake path built, the spin
  intact.
- **post** — `12c0ebb` (`v2.2.1-12-g12c0ebb`), RW3: a parked coroutine no
  longer counts as runnable.

Five reps each, `pre, post, pre, post, …`, one fresh server per invocation,
on the same 8-CPU box in one sitting. The version is `2.3.0` by the
operator's naming; no `v2.3.0` tag exists, so both arms name themselves off
the `v2.2.1` line.

Order: `instructions/v2.3.0-reactor-wake.md` §5 cell 3. Judges **claim 3**.

> **The headline is a trade, not a win, and it is stated that way
> throughout.** RW3 gives back a whole CPU core and costs latency. Which
> side of that is worth more is a property of the host, not of the engine,
> and this file does not decide it.

---

## 1. What was run

```
bench/single_relation_probe.py --server <pre|post>/kds_server \
    --workdir <fresh> --arm multi --cores 4 --sessions 1 --rows 2000 \
    --seat foreign --arrival-core -1 --json <out>
```

`--seat foreign` throughout: a shipped statement is what puts a parked
waiter on an arrival core, and that core is the subject. Box idled to
`loadavg < 1.2` before the sweep began.

**Why an interleaved A/B and not a comparison with the cell above.** The
first RW3 pass compared numbers taken an hour apart and read −8%; the seated
arm in that pass spread ±15% on its own. A change worth ~10% cannot be
measured against a box that moves that much between sittings, so both
binaries were run alternately in one.

## 2. The result

| rep | pre ips | pre p50 | pre p99 | pre arrival cpu | post ips | post p50 | post p99 | post arrival cpu |
|---|---|---|---|---|---|---|---|---|
| 1 | 1,406.3 | 693.6 | 1,120.0 | 0.910 | 1,238.1 | 765.6 | 1,577.8 | 0.032 |
| 2 | 1,445.9 | 671.1 | 1,257.6 | 0.862 | 1,656.6 | 546.0 | 1,351.3 | 0.034 |
| 3 | 1,209.4 | 807.5 | 1,442.5 | 0.784 | 1,193.0 | 787.6 | 1,634.4 | 0.030 |
| 4 | 1,415.0 | 695.4 | 1,155.5 | 0.915 | 1,311.9 | 726.9 | 1,495.0 | 0.026 |
| 5 | 1,248.3 | 811.0 | 1,449.7 | 0.810 | 1,266.1 | 719.4 | 1,534.5 | 0.051 |

| | pre (median) | post (median) | change |
|---|---|---|---|
| **arrival-core cpu** | **0.862** | **0.032** | **27× less** |
| throughput | 1,406.3 ips | 1,266.1 ips | **0.900×** |
| p50 | 695.4 µs | 726.9 µs | **+31.5 µs (+4.5%)** |
| p99 | 1,257.6 µs | 1,534.5 µs | **+276.9 µs (+22%)** |

## 3. Claim 3, judged

**Upheld, and not narrowly.** The order asked whether the arrival core's
~89% is the ready-queue misclassification alone, with a target below 0.10.
It reads **0.032** — the core was doing nothing but re-asking a predicate,
and the whole of it comes back. SS-B measured 3.1M polls/s at 0.059 µs each
and named the mechanism; removing the mechanism removes the number.

Every rep agrees: 0.784–0.915 before, 0.026–0.051 after, with no overlap
anywhere near the boundary. This is the one figure in the file that needs no
statistics.

## 4. What it costs, stated as plainly as what it buys

**Throughput 0.900×, p50 +31.5 µs, p99 +276.9 µs at one session per arrival
core.** The throughput ratio is the softest of the three — the arms' spreads
(1,209–1,446 and 1,193–1,657) overlap, and a median-of-five over that is
worth about one significant figure. The latency deltas are firmer, and the
p99 is the clearest signal in the table: **the tail grew more than the
median**, which is what paying a wakeup per statement looks like.

**The mechanism, and one hypothesis this run did not test.** A spinning
reactor notices its reply on the next poll, tens of nanoseconds later. A
sleeping one has to be woken: an eventfd write, an `epoll_wait` return, and
an OS context switch back onto a core that is now **97% idle** — and an idle
core on this class of CPU sits in a deep C-state whose exit latency is tens
to low hundreds of microseconds. That would account for both the +31 µs
median and the fatter tail, and it is exactly the effect
`bench/idle_wakers_probe.py` was written to chase for a different question.
**Unverified here.** The test that would settle it is the sweep repeated
with C-states pinned shallow (`cpupower idle-set -D 0`); if the delta
collapses, the cost is the platform's idle exit and not the engine's.

**An adaptive spin-then-sleep would be the engine-side answer** — spin for a
few microseconds before arming the block, so a reply that is already in
flight is caught without a syscall. It is not built, not in this order, and
is recorded here as the obvious next lever rather than as a plan.

## 5. Which way the trade goes is the host's property, not the engine's

On **this** box the spin was free: eight cores, one busy session, nothing
contending. So RW3 reads as a 10% throughput regression that bought an idle
core nobody wanted. That reading does not survive contact with a real
deployment, and SS-B said so before this change existed: *"No throughput
cost was observed only because CPUs were free — at S = 14 two arrival cores
sat at 0.921 busy against 0.058 seated"*, and *"they will cost throughput on
any host where the arrival cores are not idle."*

Two facts point the same way:

- **The cost shrinks under load.** In the first RW3 pass, at K = 4 sessions
  on one arrival core the shipped/seated ratio was **0.975** with arrival
  cpu 0.049 — the reactor has work, does not sleep, and pays nothing. The
  penalty is concentrated exactly where the reactor was idle anyway.
- **The saving does not.** A burned core is burned whether or not the box
  has one to spare, and it is invisible in a throughput number until the
  moment it is not.

So: **RW3 converts a hidden cost into a visible one.** That is an
improvement in what can be reasoned about, and on a box with spare cores it
is a small loss in what can be measured. The operator's call is whether the
latency is acceptable at K = 1; the answer at K ≥ 4 is that it barely
registers.

## 6. Gates

Full suite **2741/2741** at `12c0ebb`. `scripts/sim.sh` 171 runs, 2
failures — the same two the pristine tree at `bce12d0` produces, the
`chain-order`-under-injected-faults defect already in
`docs/inflight/known-gaps.md`, and no new one.

Raw per-rep JSON for both arms, and for the K = 1 / K = 4 sweep §5 cites,
is archived beside this file in `bench/v2.3.0/archive/cell3-parked/`
(added 2026-08-27 — `instructions/v2.3.0-reactor-wake.md` §5 asks for RW-B's
raw output to be archived, and this file's first version did not).

## 7. What is still unrun

Cells 2 (the knob sweep at the woken reactor), 4 (the hot-path cost of the
wake at load) and 5 (the commit path under PW6's four-writer shape) remain
unrun. Cell 5 matters most for this change specifically: the commit path is
the one D5 identified as breakable, it is covered by a unit test that fails
by a full second if the post-task hook's answer is wrong, and it has **not**
been measured end to end.
