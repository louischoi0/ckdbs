# The latency matrix: baseline before the p50-p99 work

Measured 2026-08-06 with `tools/latency_matrix.py` on branch
`improve-wal-group-committer` at `a3f77c7`, against the **server** over a
socket. 250 users, 100 assets, 8 s measured, reporter off, fresh data file
per configuration.

Host: AMD EPYC 7571, 2 cores, xfs on an EBS volume, **load average 0.16** —
which is the point of this file, and the reason its numbers disagree with
the ones taken earlier.

All figures microseconds. `p99/p50` is the number the work is about.

## 1 connection

| configuration | TPS | txn p50 | p95 | p99 | ratio | insert p50 | p95 | p99 | ratio |
|---|---|---|---|---|---|---|---|---|---|
| ckdbs `group` (shipped) | 200.8 | 5145 | 6337 | 7406 | **1.4×** | 1038 | 2150 | 2220 | 2.1× |
| ckdbs `group`, no checkpointer | 202.5 | 5137 | 6312 | 6625 | 1.3× | 1035 | 2138 | 2219 | 2.1× |
| ckdbs `relaxed` | 1,610.0 | 504 | 742 | 2752 | 5.5× | 122 | 149 | 2207 | **18.1×** |
| ckdbs `relaxed`, no checkpointer | 1,608.5 | 503 | 745 | 2760 | 5.5× | 121 | 150 | 2214 | 18.2× |
| **PostgreSQL** `synchronous_commit=on` | 212.9 | 4641 | 4960 | 5353 | **1.2×** | 1140 | 1230 | 1322 | 1.2× |

## 4 connections

| configuration | TPS | txn p50 | p95 | p99 | ratio | insert p50 | p95 | p99 | ratio |
|---|---|---|---|---|---|---|---|---|---|
| ckdbs `group` (shipped) | **213.7** | 14710 | 29160 | 32580 | 2.2× | 3949 | 7932 | 9422 | 2.4× |
| ckdbs `group`, no checkpointer | 215.0 | 14693 | 28872 | 30826 | 2.1× | 3948 | 7919 | 8901 | 2.3× |
| ckdbs `relaxed` | 2,874.9 | 926 | 3553 | 6333 | 6.8× | 210 | 647 | 2760 | 13.2× |
| ckdbs `relaxed`, no checkpointer | 2,957.7 | 920 | 3450 | 5731 | 6.2× | 209 | 622 | 2651 | 13.4× |
| **PostgreSQL** `synchronous_commit=on` | **491.5** | 8036 | 8858 | 9854 | **1.2×** | 1975 | 2348 | 2672 | 1.4× |

---

## What this says

**1. The problem is throughput scaling, not tail latency.** At one
connection ckdbs is within 6% of PostgreSQL's TPS and its tail is 1.4×
against 1.2×. At four connections PostgreSQL more than doubles — 212.9 →
491.5 TPS — and **ckdbs gains 6%**, 200.8 → 213.7. Per-statement latency
grows almost exactly linearly with the connection count (insert p50 1038 →
3949, ~3.8× for 4×), which is the signature of a queue: every commit
performs its own `fsync` on the one reactor thread, so throughput is pinned
at one commit per device sync however many clients ask.

That is W1's target, and it is worth more than the latency framing
suggested: the fix is not "make the tail flatter", it is "stop serializing
commits".

**2. `relaxed` scales and `group` does not.** With the sync out of the way,
four connections take ckdbs from 1,610 to 2,875 TPS (1.8×, on 2 cores). So
the reactor is not the ceiling — the serialized durability point is.

**3. There is a ~2.2 ms floor under the tail that nothing here explains.**
Under `relaxed` at one connection the median is 122 µs and p99 is 2,207 µs,
and the same ~2.2 ms appears in every configuration including at four
connections. It is independent of the durability class and of the
checkpointer. It is not the commit path and not maintenance; it has no
attribution yet, and it is what makes `relaxed`'s ratio 18× while `group`'s
is 2.1× — the same absolute stall against an 8× smaller median.

## A correction this file exists to make

An earlier analysis in this session, taken while the host was at **load
average 3.2** (this box builds and runs the test suite), concluded that

- disabling the checkpointer cut relaxed p95 by **6.9×** and doubled TPS, and
- the ckdbs tail ran to 10× the median where PostgreSQL's was 1.3×.

**Neither reproduces on a quiet host.** The checkpointer's effect here is
inside the noise in all four configurations — 32,580 against 30,826 µs at
`group`, 6,333 against 5,731 at `relaxed` — and the `group` tail is 2.1×,
not 10×. The load-average check in `latency_matrix.py` exists because of
this: a statement preempted by another process produces outliers that look
exactly like engine stalls, and attributing them costs a work item aimed at
the wrong component.

W2 of the plan ("get the checkpoint's WAL fsync off the statement thread")
was justified by the first of those numbers and is **not supported by this
baseline**. The mechanism is real — `Checkpointer::Step()` → `FlushPages` →
`AwaitWalGate` → `EnsureDurable` does sync inline — but it costs nothing
measurable at this scale, and it should be re-priced against a workload with
a larger dirty set rather than built on the strength of a contaminated
measurement.

## Reproducing

```
tools/latency_matrix.py --seconds 8 --users 250 --traders 1,4 --pg --json before.json
```

It refuses to run on tmpfs (where every durability class measures the same)
and on a host above 0.6 runnable processes per core, which are the two ways
this measurement has already gone wrong. `--force` overrides both and says
so in the output.

---

# W1: the group committer

Measured 2026-08-06, same harness, same host, same parameters. The change:
a committing statement stages its `TXN_COMMIT` and **parks** instead of
calling `DrainOnce`/`EnsureDurable` on its own stack, and the reactor syncs
once per iteration after every runnable statement has had its turn
(`Scheduler::SetPostTaskHook`).

## What moved

| configuration | conns | TPS before | after | | insert p50 | insert p99 |
|---|---|---|---|---|---|---|
| ckdbs `group` | 1 | 200.9 | 197.5 | **−2%** | 1035 → 1051 | 2242 → 2338 |
| ckdbs `group` | 4 | 213.7 | **388.6** | **+82%** | 3949 → **2124** | 9422 → **4681** |
| ckdbs `relaxed` | 1 | 1,611.9 | 1,580.8 | −2% | 121 → 121 | 2208 → 2165 |
| ckdbs `relaxed` | 4 | 2,874.9 | 2,958.9 | +3% | 210 → 207 | 2760 → 2691 |
| PostgreSQL | 1 | 212.9 | 212.7 | −0% | 1140 → 1138 | 1322 → 1322 |
| PostgreSQL | 4 | 491.5 | 494.4 | +1% | 1975 → 1972 | 2672 → 2635 |

**The controls are what make this attributable.** PostgreSQL and `relaxed`
were measured in the same runs and did not move: `relaxed` has no durability
wait to batch, and PostgreSQL is a different process entirely. Only the
configuration with a serialized commit changed, and it changed by 82%.

## It scales now, which is the actual fix

| connections | ckdbs before W1 | ckdbs after | PostgreSQL |
|---|---|---|---|
| 1 | 200.8 | 197.5 | 212.7 |
| 4 | 213.7 (**+6%**) | 388.6 (+97%) | 494.4 (+132%) |
| 8 | — | 660.6 (+70%) | 918.9 (+86%) |

Before, ckdbs answered 6% more transactions with four times the clients —
one commit per device sync however many asked. It now scales at roughly
PostgreSQL's rate from a lower base: 72-79% of its throughput at 4 and 8
connections, against 43% before.

## What it cost

**1-2% at one connection**, consistently across `group` and `group-nockpt`
(p50 1035 → 1051 µs). That is the extra reactor iteration a parked statement
waits through: staging, giving the core back, the hook syncing, and being
resumed on the next pass. It is the predicted price and it is the right
trade — the single-connection case was already at parity with PostgreSQL and
the four-connection case was not.

## What did not move

The **~2.2 ms floor** under `relaxed`'s tail (p50 121 µs, p99 2,165 µs) is
untouched, as expected: it was never the commit path. It remains the
unattributed item.

And the checkpointer still makes no measurable difference at any connection
count, in either direction, which is the second run to say so.

---

# The ~2.2 ms floor, attributed

The unexplained item above is **D3's loss-window sync, performed on the
reactor thread**. Found by recording every statement's arrival time as well
as its duration (`stress_business.py --latency-trace`), because a percentile
says how bad a tail is and never *when* it happened - and "when" is the
whole answer here.

Under `relaxed`, one connection, 10 s, 62,204 statements:

```
statements >= 1 ms: 824 (1.32%)   trade-insert 435, account-update 389
median gap between them: 12.2 ms
first slow statements at t = 0.0062, 0.0183, 0.0305, 0.0426, 0.0547 s
```

Evenly spaced to the millisecond, hitting both statement kinds in proportion
to their frequency. That is a timer, not a data-dependent event - no page
split, no chain growth, no allocation is that regular.

`WalManagerConfig::relaxed_flush_interval_ns` is **10 ms**, and the observed
period is 10 ms plus the ~2.2 ms the sync itself takes. Confirmed by moving
the knob:

| `relaxed_flush_interval` | stall period | statements >= 1 ms | insert p99 | TPS |
|---|---|---|---|---|
| 10 ms (default) | 12.2 ms | 1.32% | 2,169 us | 1,555 |
| 50 ms | 52.7 ms | 0.29% | **177 us** | 1,770 |

The period tracks the setting exactly. It is now a config key,
`relaxed_flush_interval_us`, exposed for that reason as much as for
durability.

## What this is, and is not

**It is the D3 bound being honoured.** `relaxed` promises a commit is
durable within the loss window, and this is the sync that makes that true.
Nothing is broken.

**What is wrong is who pays.** The sync runs on the single reactor thread,
so the statement in flight when it fires is charged the device's full
latency - 2.2 ms against a 122 us median. The cost belongs to the system and
lands on an unlucky client.

**Tuning moves it and cannot remove it.** A longer window means fewer,
equally deep stalls and more data at risk; a shorter one means more of them.
The trade is a straight line and the depth is fixed by the device. Removing
it needs the fsync off the reactor thread - a WAL writer thread, or async
submission - which is W1's shape applied to maintenance rather than commits,
and which touches the thread-per-core model (`docs/rules.md` section 3).
That is a design decision, not a tuning one.

**It is invisible under `group`, and was always there.** `DrainOnce()` syncs
on a pending commit *or* on the interval, so with continuous traffic the
commit branch fires first and the interval branch rarely runs. `group`'s
~2.2 ms tail is the commit sync - the same device cost by a different route.

## The default is not changed

10 ms stays. Raising it is a durability decision - how much committed work a
`relaxed` instance may lose - and this measurement is not the argument for
making it. It is the argument for knowing what the current value costs.

---

# The WAL writer thread

Measured 2026-08-06, same harness, quiet host. `wal::WalWriter` is a thread
that owns the device sync; the reactor's `Append` and `Flush` stay
single-threaded `WalStream` work, and the shared state is two atomics plus a
mutex never held across an `fsync`.

**The first version gave it every sync, and that was wrong.** Measured:

| | conns | before | writer takes *every* sync |
|---|---|---|---|
| `relaxed` insert p99 | 1 | 2,165 us | **194 us** |
| `group` insert p99 | 1 | 2,338 us | **4,750 us** |
| `group` insert p99 | 4 | 4,681 us | **8,165 us** |

The floor it was built for vanished, and `group`'s tail doubled. The two
classes differ in **who waits**: nobody waits for D3's loss-window tick, and
every committer waits for a commit sync. Handing a waited-on sync to another
thread adds a wake-up to a latency somebody is measuring - visible as a
doubled p99 against a median that barely moved (1,051 -> 1,087 us), which is
what an occasional slow wake-up looks like on a 2-core host with no spare
core to schedule the writer on.

## The rule that shipped: the writer takes the syncs nobody waits for

D3's interval tick goes to the writer. Commit syncs, `SYNC`, shutdown and
the checkpoint gate stay on the reactor. `durable_lsn()` became the **max of
two watermarks**, since both can now advance it and both are monotonic.

| | conns | baseline | W1 | writer-everywhere | **split** |
|---|---|---|---|---|---|
| `relaxed` insert p99 | 1 | 2,208 | 2,165 | 194 | **202 us** |
| `relaxed` TPS | 1 | 1,612 | 1,581 | 1,881 | **1,890** |
| `relaxed` insert p99 | 4 | 2,760 | 2,691 | 839 | **937 us** |
| `relaxed` TPS | 4 | 2,875 | 2,959 | 3,488 | **3,443** |
| `group` insert p99 | 1 | 2,242 | 2,338 | 4,750 | **2,372 us** |
| `group` insert p99 | 4 | 9,422 | 4,681 | 8,165 | **4,920 us** |
| `group` TPS | 4 | 214 | 389 | 404 | **381** |

The win is kept and the regression is gone. PostgreSQL, measured in every
run, moves by ~2% across all four - which is the drift these comparisons
carry.

**`relaxed` at one connection now answers p50 122 us / p99 202 us - a ratio
of 1.7x**, against PostgreSQL's 1.2x and its own 18x at the start of this
work. That ratio was the point of the plan.

## What is not claimed

The writer-everywhere regression is a **2-core** result. A host with a core
to spare may well schedule the writer fast enough that a waited-on sync is
cheaper off-thread, which would make the split unnecessary. Nothing here
tests that, and the split is what the available evidence supports.

Verified under ThreadSanitizer: a server built with `-fsanitize=thread`,
driven by 3 traders plus the reporter and then 4 traders under `--txn`,
followed by `SYNC` and a clean shutdown - **zero warnings**, balances
verified both times. The three sharing points are the segment table (a
mutex, copied out and synced outside it), the durable watermark (atomic),
and the ring (reactor-only, untouched by the writer).

---

# W3, the drain bound: built, measured, reverted

The plan carried a fourth item - bound the WAL drain per tick, the way the
checkpointer bounds itself at `pages_per_step = 64`. Its justification was
preventive: "once W1 lands, the drain is the thing standing between a parked
committer and its reply, so an unbounded one reintroduces the convoy in a
new place." Nothing had measured it.

## What the drain actually costs

Two configurations, 4 connections, read from the sync line's `durable_lsn`
deltas:

| | syncs | bytes made durable per sync |
|---|---|---|
| `group` | 6,746 | p50 **424 B**, p95 680 B, p99 768 B, max 16,760 B |
| `relaxed` | 34 | p50 **1,018,112 B**, p95 1,026,472 B, max 1,096,648 B |

Under `group` there is nothing to bound: the post-task hook drains every
reactor iteration, so each drain writes a few hundred bytes and the ring is
empty every time. A cap would have capped nothing.

Under `relaxed` the drain wrote **~1 MB in one call**, on the reactor
thread. `DrainOnce()` only flushed when the 10 ms sync interval fired, so
the ring was allowed to fill for the whole window first.

## The fix that followed, and why it did not help

A byte cap was the wrong instrument; the two cadences are the point. D3
bounds when bytes must be **durable** and says nothing about when they must
be **written**, and writing is cheap - a page-cache write, no device round
trip. So: flush whatever is staged on every tick, sync only when the window
is up. No tuning parameter, and a ring emptied every iteration cannot grow
large, which is a bound by construction.

Measured against the split design in the section above:

| | conns | before | after W3 |
|---|---|---|---|
| `relaxed` TPS | 1 | 1,890 | 1,828 |
| `relaxed` insert p99 | 1 | 202 us | 199 us |
| `relaxed` TPS | 4 | 3,443 | 3,384 |
| `relaxed` insert p99 | 4 | 937 us | 984 us |
| `group` insert p99 | 4 | 4,920 us | 4,823 us |
| *PostgreSQL TPS* | *4* | *483* | *491* |

**Nothing moved.** Every difference is inside the drift PostgreSQL shows in
the same runs. If anything the two `relaxed` TPS figures are 2-3% lower,
which would be the extra `pwrite` per reactor iteration - too small to
separate from noise at this magnitude.

**So it was reverted.** The error worth naming is the one in the reasoning:
the flush *size* was measured and its *cost* was assumed. A megabyte of
sequential writing into page cache is one syscall at memory bandwidth, on
the order of 100-200 us, against a p95 of ~520 us and a p99 of ~940 us - it
was never the dominant term.

The speculative case for keeping it - that at ten times the throughput the
accumulated write would be 10 MB and would matter - is the same species of
argument that produced W2 and W3's original framing, both of which were
wrong. It can be rebuilt in an afternoon if a workload ever shows the drain
costing something.

## The scoreboard for the plan

| item | outcome |
|---|---|
| W0, the harness | built; falsified a finding from the session that proposed it |
| W1, the group committer | built; +82% TPS at 4 connections |
| the ~2.2 ms floor | attributed to D3's loss-window sync; the interval is now a config key |
| the WAL writer thread | built, then split by *who waits*; `relaxed` p99 2,208 -> 202 us |
| W2, the checkpoint sync | never built - its evidence came from a host at load average 3.2 |
| W3, the drain bound | built, measured, reverted - no effect |

Two of the five items were aimed at nothing, and both were caught by
measuring rather than by argument.

---

# Current state

Measured at `41cef92` on a quiet host (load 0.17), same harness and
parameters as the baseline at the top of this file. `trade-insert`
microseconds; PostgreSQL measured in the same runs as the control.

| configuration | conns | TPS then | now | | p99 then | now | | ratio then | now |
|---|---|---|---|---|---|---|---|---|---|
| ckdbs `group` | 1 | 201 | 198 | −2% | 2,242 | 2,310 | +3% | 2.2× | 2.2× |
| ckdbs `relaxed` | 1 | 1,612 | **1,829** | +13% | 2,208 | **187** | **−92%** | 18.2× | **1.5×** |
| ckdbs `group` | 4 | 214 | **390** | **+83%** | 9,422 | **4,377** | **−54%** | 2.4× | 2.1× |
| ckdbs `relaxed` | 4 | 2,875 | **3,556** | +24% | 2,760 | **768** | −72% | 13.2× | **3.6×** |
| PostgreSQL | 1 | 213 | 212 | −0% | 1,322 | 1,346 | +2% | 1.2× | 1.2× |
| PostgreSQL | 4 | 491 | 495 | +1% | 2,672 | 2,626 | −2% | 1.4× | 1.3× |

PostgreSQL moving by 2% or less across every row is what says the rest is
the engine and not the box.

**Where the two engines now stand.** At one connection `relaxed` answers
1,829 TPS against PostgreSQL's 212 with a tighter tail ratio (1.5× against
1.2×, at an eighth of the latency); `group` - the durable, shipped default -
answers 198 against 212, at parity. At four connections `group` reaches 390
against 494, or **79% of PostgreSQL**, where it began at 43%.

**What is left is not durability.** The remaining gap at four connections is
general throughput on two cores against a process-per-connection engine, and
`group`'s tail ratio (2.1×) is now within sight of PostgreSQL's (1.3×) while
its median is the same fsync. The next honest target is elsewhere: the
statement path itself, where parse was measured at 32-38% of an unlogged
statement (`results-txn-layers.md` §4) and is no longer hidden behind a
device.

