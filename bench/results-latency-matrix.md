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

