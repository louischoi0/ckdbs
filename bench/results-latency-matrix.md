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
