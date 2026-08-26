# Three writer cores, and what rotation actually buys — the matrix at `v2.1.0`

**Rotation over three writer cores runs the workload at 1.051× the
single-core configuration against a 3× ceiling — and its own control, which
does nothing cross-core at all, runs at 1.071×.** At identical workload the
four-core server with every relation on core 0 does 4,206 stmt/s and the same
server with relations rotated over cores 1–3 does 4,159, so **rotation is
0.989× of not rotating**. The ~5% that the headline cell shows against
`cores = 1` is bought by running a four-core server, not by using peer cores
as writers.

The reason is not the device, not CPU, and not the cross-core machinery.
A core's commits are drained by a group committer that turns whatever has
accumulated into one device `fdatasync`
(`src/server/expeditor.cpp:1650-1657`, and the same on every peer at
`src/server/core_runtime.cpp:718-734`). **Spreading N writing sessions over
W cores divides that batch by W.** Every multi-core cell measured here runs
at 965–1,071 commits per second *per writer core* — the volume's
single-stream `fdatasync` rate, measured independently at 1,066/s — while
the single-core arm scales linearly with sessions because its batch grows.
The added cores spend W times the device's sync budget buying back the
batching the split destroyed.

Rotation therefore wins only where sessions-per-core falls to 1 and there was
no batch to lose. **That cell exists, and its gain is real.** One relation per
writer core runs 1.927× against `cores = 1`, with insert p50 halving from
1,944 µs to 984 µs — and against the control that isolates the four-core
server (C2: the same three relations, the same four-core server, every
relation on core 0) it runs **1.751×**, 4,053 stmt/s against 2,315. The
four-core-server artifact accounts for only 1.067× of it. So at one writing
session per core rotation delivers a genuine 1.751× of a 3× ceiling, and at
two it delivers nothing.

**Every phase of this run has now been measured**, including the two that
were published unrun in this file's first version. Two of them changed what it
says: the drain-cadence sweep **refuted** the mechanism §6 first proposed
(§6a), and the PW7 comparison had to be re-shaped before it tested anything
(§8a), where it then reproduced PW7's collapse and its fix — 0.765 against
1.081 — on two independent trees. §2 records those corrections.

---

## 1. The run

| | |
|---|---|
| Version | **`v2.1.0`** — annotated tag on `main` at `5ad2455`, named by the operator |
| `git describe --tags` at the measured commit | `v2.1.0` |
| Worktree | `bench-v2.1.0` |
| Date | 2026-08-25 / 2026-08-26 UTC |
| Host CPU | AMD EPYC 7R32, **4 logical / 2 physical cores**, 2 threads/core, 1 socket, 1 NUMA node, **SMT on** |
| Kernel | `7.0.0-1006-aws`, 7 GiB RAM |
| Data device | `nvme0n1p1`, **ext4**, `rw,relatime,discard,errors=remount-ro,commit=30`, non-rotational, scheduler `[none]` |
| `--workdir` | `/home/ubuntu/mcbench` — a block device. **`--force` was never passed.** `/tmp` is tmpfs on this box and was deliberately not used |
| Build | `build-release` only. `cmake -DCMAKE_BUILD_TYPE=Release -G Ninja`, g++ 15.2.0, `-Wall -Wextra -O3 -DNDEBUG`, C++20 |
| Test suite | 2635 tests, **2634 pass** (§9c) |
| Overhead | **not measured** — suspended for v2-stage work by the operator's 2026-08-24 amendment |

**The `cores = 4` ceiling is 3×, not 4×.** `AssignOwnerCore` returns
`kSystemCore + 1 + (relation_seq % (core_count - 1))` under `kRotate`
(`include/kds/catalog/core_placement.hpp:96-104`, **source-read** on this
tree at `5ad2455`), rotating over the **non-system cores only**. At
`cores = 4` the writer cores are 1, 2 and 3, and core 0 owns no rotated
relation. Every ratio below is against that 3× ceiling: 1.051× is 35% of
what is achievable here, not 26% of an imaginary 4×.

**Read the host line next to every number.** Four logical CPUs are two
physical cores with SMT. Rotation's three writer cores are not three
independent CPUs, and the top of any scaling curve is bounded by that before
it is bounded by anything architectural.

`steal` is nil on this box — 14–21 jiffies *total per CPU since boot*
(~0.005%) — so `busy = total − idle − iowait` is genuinely the engine's work
and the usual EC2 steal caveat does not bite here. **measured**,
`/proc/stat`.

Every claim below is tagged **measured** (with its invocation) or
**source-read** (with `path:line`, read on `bench-v2.1.0` at `5ad2455`).
They are never mixed in one sentence.

---

## 2. What ran, and what this file corrected about itself

Every phase the first version of this document listed as unrun has since been
run. The table is kept as the record of what was owed:

| Phase | State |
|---|---|
| **C2 control** (3 relations, all on core 0) | run, 5 reps — §7 |
| **H2** to five reps | run — §4 |
| **PW7 before/after**, three trees | run — §8 |
| **P7**, PW7's own four-sessions-on-one-core shape | run, 3 trees x 5 reps — §8 |
| **Per-core CPU attributed to the insert phase** | run — §5a |
| **Restart ownership at three writer cores** | run, **PASS** — §9b |
| **Drain cadence sweep** | run — §6a, and it **refuted** a claim this file made |

Three corrections this run made to itself, recorded because a benchmark that
only ever confirms itself is not measuring:

1. **The cadence sweep refuted §6's mechanism as originally written.** The
   first version attributed the per-core commit cap to the 1 ms
   `wal_drain_interval_us`. Varying that interval over a 10x range does not
   move throughput (§6a). The cap is the device's single-stream `fdatasync`
   *latency*, which is ~0.94 ms and merely coincides with the 1 ms default.
   The per-core-sync-bound conclusion survives; the attribution to the
   configured interval does not, and is withdrawn.
2. **The PW7 comparison the run instructions prescribe tests nothing.** H1
   spreads two sessions over three cores; PW7's collapse needs four sessions
   on **one** peer core. On H1 all three trees are indistinguishable. The
   shape-matched cell (P7) shows the defect and the fix plainly (§8).
3. **Three probes were measuring nothing at all on their first run.** They
   used `INSERT INTO t (cols) VALUES (...)`; ckdbs's Keystone pk is implicit
   and a column list is refused (`tools/multicore_benchmark.py:288`). Every
   insert errored. A fourth defect followed: the per-core probe did not retry
   the peer's retryable first-INSERT lease refusal (PW1b) and divided by rows
   it *intended* to insert rather than rows that landed, overstating the peer
   arm. Both fixed; the numbers below are from the fixed runs. **No number
   from the broken runs appears anywhere in this file.**

Not measurable by this harness at all — see §10.

## 3. The gate: `fdatasync` overlaps on this volume

PW6 §7 left open whether two cores can overlap `fdatasync` on one volume. If
they cannot, every ingest ratio below belongs to the I/O backend decision
rather than to the cross-core architecture. It was run first, as a blocking
gate.

**measured** — `build-release/fdatasync_probe /home/ubuntu/mcbench/probe 5 3`
(5 reps of 3 s per arm, arms interleaved, 8192-byte page, one file and one fd
per thread):

| threads | median syncs/s | min | max | vs N=1 |
|---|---|---|---|---|
| 1 | 1,066.1 | 1,060.2 | 1,074.9 | 1.000 |
| 2 | 2,138.1 | 2,136.7 | 2,148.8 | 2.005 |
| 3 | 3,203.8 | 3,192.4 | 3,211.4 | 3.005 |
| 4 | 3,898.6 | 3,892.5 | 3,911.0 | **3.657** |

**N=4 : N=1 = 3.657×**, near-linear to N=3, with spread under 1.5% at every
arm. The gate's first branch holds: the device overlaps, and §4 reads as
architecture rather than as an I/O ceiling.

Two things bound how this number may be quoted, both stated in the probe's
own header:

- **Separate files means separate inodes.** ext4 serialises `fdatasync` on
  the inode lock, so this answers *"N cores, N WAL streams"* — the engine's
  shape — and **not** *"N cores, one shared file"*, which is a materially
  worse number this run does not have.
- The single-stream figure, **1,066 syncs/s**, turns out to be the more
  important half. §6 shows every writer core landing on it.

---

## 4. The matrix

Harness: `tools/multicore_benchmark.py`, driven by `bench/run_benchv2.py`.
Each cell starts two fresh servers on fresh data files — `cores = 1`, then
`cores = N` — and compares them. Phases: insert, point-select by pk, update
by pk, delete the odd half, one scan per relation.

`--tables` is **two relations per writer core at every curve point** (2, 4, 6
for `cores` 2, 3, 4). The run instructions say `3 × (cores − 1)` "or
nearest"; a curve whose per-core load changes between points measures load as
well as cores, so per-core load was held constant instead. Every cell's table
count is still a multiple of `cores − 1`, which is the requirement that
matters — otherwise one writer core carries an extra relation and the
imbalance reads as poor scaling.

| cell | invocation delta | reps | ratio median | spread |
|---|---|---|---|---|
| **H1** headline | `--cores 4 --tables 6 --rows 2000 --placement rotate --peer-listeners` | 5 | **1.051** | 1.008–1.070 |
| **H2** | H1 at `--rows 20000` | 5 | **1.024** | 1.019–1.030 |
| **H3** | H1 at `--tables 3` | 5 | **1.927** | 1.886–1.947 |
| **H4a** | `--cores 2 --tables 2` | 5 | **1.034** | 0.983–1.052 |
| **H4b** | `--cores 3 --tables 4` | 5 | **1.058** | 1.005–1.080 |
| **C1** control | `--cores 4 --tables 6 --placement creating`, no peer listeners | 5 | **1.071** | 0.988–1.093 |
| **C2** control | `--cores 4 --tables 3 --placement creating` | 5 | **1.067** | 1.016–1.103 |
| **P7** PW7's shape | `--cores 2 --tables 4 --placement rotate --peer-listeners` | 5 | **1.081** | 1.022–1.091 |

Verbatim, the headline invocation:

```
tools/multicore_benchmark.py --server build-release/kds_server \
    --cores 4 --tables 6 --rows 2000 \
    --placement rotate --peer-listeners --workdir /home/ubuntu/mcbench/H1-r1 \
    --port 15600
```

P7 is not a scaling cell: at `cores = 2` there is one writer core, so its
ceiling is 1× and its purpose is §8a's before/after, not throughput.

**Every cell against the 3× ceiling**: H1 1.051 (35% of ceiling), H2 1.024,
H3 1.927 (64%), H4b 1.058 against a 2× ceiling at two writer cores, H4a 1.034
against a 1× ceiling — H4a has exactly one writer core, so parity is its
honest expectation and it meets it.

**H2 answers its question, now on five reps** (1.019, 1.019, 1.024, 1.027,
1.030 — a 1.1% spread, the tightest cell in the matrix). Amortising
per-statement fixed costs over ten times the rows does not raise the ratio; it
lowers it slightly, to 1.024 against H1's 1.051. Insert p50 barely moves
either — 2,021 µs single against 1,952 multi, against H1's 2,000/1,910 at a
tenth the rows. **Whatever bounds this workload is not a per-statement fixed
cost**, which is what §6 then identifies.

**The comparison baseline.** `bench/results-multicore.md`'s 1.05× is *not*
quoted here as "the old speedup". It is a parity baseline from before the
peer writer existed — core 0 served everything — and it measures absence of
regression, not parallel gain. The only comparisons made are `cores = 1`
versus `cores = N` on one commit, and cell against cell at identical
workload.

---

## 5. Latency, per phase

p50/p99 in microseconds, median across reps. `n` is per run and matters: the
scan phase issues **one statement per relation**, so its percentiles rest on
2–6 samples and are marked accordingly.

### H1 — 6 relations, 3 writer cores, 2 sessions per core

| phase | single p50 | multi p50 | p50 gain | single p99 | multi p99 | n |
|---|---|---|---|---|---|---|
| insert | 2,000 | 1,910 | 1.047 | 2,425 | 2,218 | 12,001 |
| point-select | 255 | 255 | 1.000 | 1,109 | 1,018 | 12,000 |
| update | 2,030 | 1,941 | 1.046 | 2,230 | 2,203 | 12,000 |
| delete | 2,005 | 1,934 | 1.037 | 2,212 | 2,623 | 6,000 |
| scan | 1,453 | 1,372 | 1.059 | 1,580 | 1,451 | **6** |

### H3 — 3 relations, 3 writer cores, **1 session per core**

| phase | single p50 | multi p50 | p50 gain | single p99 | multi p99 | n |
|---|---|---|---|---|---|---|
| insert | 1,944 | **984** | **1.976** | 2,226 | 1,192 | 6,001 |
| point-select | 93 | 124 | **0.750** | 251 | 285 | 6,000 |
| update | 1,944 | 1,032 | 1.884 | 2,098 | 1,209 | 6,000 |
| delete | 1,938 | 1,025 | 1.891 | 2,128 | 1,210 | 3,000 |
| scan | 1,396 | 488 | 2.861 | 1,497 | 525 | **3** |

The insert p50 halving — 1,944 → 984 µs — is the whole of H3's gain and is
reproducible across all five reps (984–993 µs). It is the signature of §6.

**The point-SELECT gets worse under rotation at low session counts**:
93 → 124 µs in H3 (0.750), reproducible across five reps; 137 → 143 in H4b;
255 → 255 in H1; 71 → 66 in H4a. §4.2 of the run instructions asks this
against PW7's 48 µs figure — but that figure
(`docs/inflight/in-progress/workplan-peer-writer.md:325`, **source-read**) is *three writing
sessions on one peer core*, not three writer cores, so it is not the same
shape. The nearest cell here is H3's 124 µs at one session per core.

### 5a. Which cores actually do work — core 0 is not a bottleneck

**measured** — `bench/percore_insert_probe.py --cores 4 --tables 6 --rows 2000`,
which runs one configuration and one phase and samples only that window (the
matrix sampler spans both configurations and cannot attribute). 12,000 rows
inserted in both arms, **0 errors**, 58 retryable lease refusals retried on
the peer arm, every relation back at 2,000.

| core | rotated over 1-3 | everything on core 0 |
|---|---|---|
| cpu0 | **8.9%** | 4.2% |
| cpu1 | 14.3% | 5.5% |
| cpu2 | 13.9% | 7.5% |
| cpu3 | 13.2% | 5.0% |

Insert-only throughput 3,286.5 ips rotated against 2,944.1 — **1.116x**,
slightly better than the full workload's 1.051 because the insert phase has
no read to slow down.

**Core 0 is the least busy core under rotation**, at 8.9% against the writer
cores' 13-14%, and nothing anywhere is near saturated. So the answer to the
run instructions' question is: core 0 is idle, not a bottleneck, and the
constraint is not any core's CPU. (Idle windows on the same mounts: 1.6-3.7%.)

**Scan scaling is reported separately and is weak evidence.** H3's 2.861×
(1,396 → 488 µs) reproduces across five reps and is large enough to be real;
H1's 1.059×, H4b's 1.027× and H4a's 1.001× are within noise of a two-to-six
sample percentile. **A read-scaling claim needs its own measurement and this
run does not make one.**

---

## 6. The mechanism: rotation divides the group-commit batch

This is what the matrix is actually measuring, and it is the finding.

**source-read.** `src/server/expeditor.cpp:1650-1657` installs a group
committer as a post-task hook — *"once per reactor iteration, after every
runnable statement has staged whatever it is going to stage, so one device
sync covers all of them"* — and a committing statement parks rather than
syncing on its own stack. `src/server/core_runtime.cpp:718-734` installs the
same drain on **every peer core**, and `src/server/expeditor.cpp:1274` gives
each peer the expeditor's interval. `wal_drain_interval_ns` defaults to 1 ms
(`include/kds/server/expeditor.hpp:424`; `wal_drain_interval_us = 1000` in
`kds.conf.sample:186`).

**measured.** The workload is 5/7 commit-bound — insert + update + delete is
30,001 of 42,007 statements at `--tables 6 --rows 2000`, and the same 0.714
fraction at every table count because the phases scale together. Backing
commits per second out of each cell's aggregate:

| cell | sessions | writer cores | multi commits/s **per writer core** | single-core commits/s | measured ratio |
|---|---|---|---|---|---|
| H4a | 2 | 1 | 1,071 | 1,029 | 1.034 |
| H4b | 4 | 2 | 1,046 | 1,981 | 1.058 |
| H1 | 6 | 3 | 990 | 2,801 | 1.051 |
| H3 | 3 | 3 | 965 | 1,497 | 1.927 |

**Every multi-core cell runs at 965–1,071 commits/s per writer core.** That
is §3's independently measured single-stream `fdatasync` rate — 1,066/s — to
within 10%. A peer core commits at one sync per commit and is capped there.

**The single-core arm instead scales linearly with sessions**, at roughly
470 × sessions, because core 0's group committer amortises more commits into
one device sync as sessions accumulate: the implied batch is 1.0 at two
sessions, 1.4 at three, 1.9 at four and 2.6 at six.

So the predicted ratio is `1000 × writer_cores / (470 × sessions)`, which
gives H1 1.06 (measured 1.051), H4a 1.06 (1.034), H4b 1.06 (1.058) and
H3 2.13 (1.927). **Four cells, one expression.**

The latency signature is the same fact seen from the other side: one writing
session on a core commits in ~1 sync (984 µs), two or more in ~2 (1,910–2,030
µs), and adding a third through sixth session to core 0 does not make it
worse — the batch absorbs them.

It also explains why the multi-core aggregate is nearly constant at
4,053–4,159 stmt/s whether the run has three relations or six: three writer
cores at ~1,000 commits/s each is the ceiling, and adding relations to those
same cores adds nothing.

**The reading.** Rotation does not fail to scale for want of parallelism. It
*defeats group commit*, and the W cores then spend W times the device's sync
budget to buy back the batching the split destroyed. The win survives only
where sessions-per-core falls to 1 and there was no batch to lose.

**No constant was changed in response, and none is proposed here.**
`wal_drain_interval_us` trades durability latency against throughput and is
the operator's to set.

### 6a. The cadence sweep, which refuted this section as first written

The first version of this file attributed the per-core cap to the 1 ms
`wal_drain_interval_us` default, and predicted that shortening the interval
would raise throughput. **measured** —
`bench/drain_cadence_probe.py --tables 6 --rows 1000 --cores 4`, zero rows
lost at every setting:

| `wal_drain_interval_us` | single-core ips | multi-core ips | ratio |
|---|---|---|---|
| 1000 (default) | 3,723.6 | 3,554.1 | 0.954 |
| 500 | 3,376.2 | 3,624.0 | 1.073 |
| 250 | 3,356.7 | 3,642.8 | 1.085 |
| 100 | 4,061.3 | 3,626.3 | 0.893 |

**The prediction failed.** Over a 10x range of the interval the multi-core arm
is flat at 3,554-3,643 ips, and the single-core arm's variation is noise with
no trend. The configured cadence is not what paces a committing INSERT.

The explanation is in the same source already cited: the group committer is
installed as a **post-task hook** — *"once per reactor iteration"* — and only
*additionally* on the timer (`src/server/expeditor.cpp:1650-1660`,
**source-read**). The hook is the live path; the timer is a backstop. So the
per-core cap is the device's single-stream `fdatasync` **latency**, ~0.94 ms
from §3's 1,066/s, which merely coincides with the 1 ms default closely
enough to look causal.

**What survives and what is withdrawn.** The measured fact — every writer core
runs at 965-1,071 commits/s, which is §3's single-stream sync rate — is
unchanged, and so is the batching account of why splitting sessions costs.
The attribution of that cap to the configured interval is **withdrawn**. A
constant this run might have been tempted to propose would have been the
wrong lever.

---

## 7. The control is not at parity, and it bounds every ratio above

The run instructions call C1 the control, say parity is the honest
expectation, and add: *"A control that shows speedup means the harness is
measuring something other than what it claims."*

**It does.** **measured** — C1
(`--cores 4 --tables 6 --rows 2000 --placement creating`, no peer listeners),
5 reps: ratio median **1.071**, spread 0.988–1.093, insert p50 gain 1.457
(2,006 → 1,377 µs). In both arms every relation is core 0's and every session
is on core 0, so nothing cross-core happens at all.

**H1's headline 1.051× is below its own control's 1.071×.**

The cleanest form is a direct cross-cell comparison at identical workload
(6 relations × 2,000 rows), since all four are absolute aggregates from the
same driver:

| configuration | aggregate stmt/s |
|---|---|
| `cores = 1` (H1 single arm) | 3,923 |
| `cores = 1` (C1 single arm) | 3,938 |
| `cores = 4`, every relation on core 0 (C1 multi arm) | **4,206** |
| `cores = 4`, rotated over 3 writer cores (H1 multi arm) | **4,159** |

**Rotation runs at 0.989× of not rotating on the same four-core server.**
The ~5% H1 shows against `cores = 1` is bought by running a four-core server
— whatever that buys — and none of it is attributable to using peer cores as
writers.

**What the four-core server buys is not established by this run.** It shows
up in C1's insert p50 (1.457×). Candidate explanations — more WAL anchors
(`SHOW META` reports `wal_anchor_count=4` at `cores = 4`), per-core extent
leases, background work moving off core 0 — are **not discriminated here**.
Stated as an open observation, not an explanation.

### The 3-relation control resolves H3, in rotation's favour

H3's 1.927× is measured against a `cores = 1` baseline and carries the same
confound, so the matching control was run: **C2**
(`--cores 4 --tables 3 --rows 2000 --placement creating`), no peer listeners,
5 reps. **measured**: ratio median **1.067**, spread 1.016–1.103, `errors=0`,
every rep verified clean.

The absolute aggregates settle it, all three at 3 relations × 2,000 rows:

| configuration | aggregate stmt/s | insert p50 |
|---|---|---|
| `cores = 1` | 2,117 | 1,935 µs |
| `cores = 4`, every relation on core 0 (**C2**) | **2,315** | 1,236 µs |
| `cores = 4`, rotated over 3 writer cores (**H3**) | **4,053** | **984 µs** |

**Rotation delivers 4,053 / 2,315 = 1.751× over the equivalent control.** The
four-core-server artifact accounts for 1.067× of H3's 1.927×; the remaining
1.751× is rotation. **H3's gain is real**, and it is 58% of the 3× ceiling.

This confirms §6 rather than undermining it, and the arithmetic closes: core 0
serving three sessions does 2,117 × 0.714 = 1,512 commits/s at `cores = 1` and
2,315 × 0.714 = 1,653 at `cores = 4` — a batch of ~1.4 against the volume's
1,066/s single-stream rate. Three peers at one session each do
4,053 × 0.714 = 2,894, or 965 per core with no batching at all. And
3 × 965 / 1,653 = **1.75**, which is what was measured.

So the two regimes are one law seen at two points. **At one writing session
per core there is no batch to lose and rotation wins 1.751×. At two, core 0's
batching keeps pace and rotation wins nothing** (§7's 0.989× at 6 relations).
The crossover is between one and two sessions per writer core.

---

## 8. The wait breakdown: the leg PW7 fixed is at zero

**measured**, H1 rep 1, from each peer's `SHOW META`. The field list is
`src/server/command_dispatcher.cpp:618-631` (**source-read**): requests and
grants per lease kind, and the longest wait split into submit→sent,
sent→grant, and grant→resumed, each in nanoseconds *and* in reactor
iterations.

| core | rowid | trxid | extent |
|---|---|---|---|
| 1 | 2/2, wait_max 0.7 ms | 3/3, 10.5 ms | 1/1, 5.6 ms |
| 2 | 2/2, 0.6 ms | 3/3, 10.4 ms | 1/1, 5.8 ms |
| 3 | 2/2, 0.7 ms | 3/3, 10.4 ms | 1/1, 5.9 ms |

**The submit leg is 0.0 ms over 0 reactor iterations on every kind, on all
three cores.** That is the leg PW7 traced at 546 ms over 395 iterations
before its two floors. §8a is the before/after that shows it is the floors
doing it.

The remaining wait is sent→grant, and the trxid leg's 10.4 ms spans
**18,822–23,965 reactor iterations**. That is PW7's second open item visible
at three writer cores: `Scheduler::IdleTimeoutMs` returns 0 whenever any ready
queue is non-empty and a parked task still occupies one
(`src/sched/scheduler.cpp:196-199`, **source-read**), so a reactor with a
parked coroutine spins rather than blocking.

### 8a. PW7 before and after, on three trees — and the shape that matters

Three trees were measured, because the tree the run instructions name as
"before" cannot answer the question alone:
`include/kds/server/lease_refill_stats.hpp` is **absent at `9c0528a`** and
present at HEAD (**source-read**) — PW7 shipped its lag instrument and its fix
in one commit, `2c6ae23`. So a third tree was built: **HEAD with the two
floors disabled in `src/sched/scheduler.cpp` and everything else, instrument
included, left alone**. (Reverting the file wholesale does not compile: the
floors changed `PickNextGroup`'s signature. Floor one is disabled by letting a
re-queued suspended task become eligible again within the round, floor two by
removing the guaranteed per-group poll.)

**On H1 the comparison is null, and that is a statement about H1, not about
the floors.** Medians over 5 reps each, zero rows lost in every arm: HEAD with
floors **1.048**, `9c0528a` **1.057**, HEAD floors-disabled **1.027**. All
noise. H1 puts *two* sessions on each of three cores; PW7's collapse needs
*four sessions on one peer core*. **The prescribed comparison does not provoke
the defect it is meant to test.**

So the shape was matched. Cell **P7** — `--cores 2 --tables 4`, four writing
sessions on the single writer core, PW7's own shape — 5 reps per tree,
`errors=0` and every relation at its row count everywhere:

| tree | median | range | multi-core aggregate |
|---|---|---|---|
| HEAD, floors **present** | **1.081** | 1.022–1.091 | 2,982 stmt/s |
| `9c0528a`, pre-floors | **0.765** | 0.730–0.779 | 2,088 |
| HEAD, floors **disabled** | **0.742** | 0.714–0.860 | 1,993 |

**PW7's collapse is reproduced and the floors remove it** — a 37% throughput
recovery at the shape that provokes it. PW7's own record
(`docs/inflight/in-progress/workplan-peer-writer.md:325`, **source-read**) reports the four-writer
cell at **0.61–0.80x before** and 1.034x after; this run measures 0.73–0.78
before and 1.081 after, on a host that can actually run three writer cores.

**The two "before" arms agree**, which is what makes this attributable.
`9c0528a` differs from HEAD by ~20 unrelated commits (the key-mode deletion,
PW1c-6b, PW3b); the floors-disabled tree differs by exactly the two floors.
Both collapse to the same place, so the effect is the floors and not the
intervening work.

The instrument says the same thing directly. Trx-id refill on H1, floors
present against floors disabled (**measured**, `SHOW META`):

| core | floors present | floors disabled |
|---|---|---|
| 1 | 9.2 ms, **submit 0.0 ms / 0 iters** | **930.7 ms, submit 924.4 ms / 934 iters** |
| 2 | 9.1 ms, submit 0.0 ms / 0 iters | 93.4 ms, submit 18.2 ms / 17 iters |
| 3 | 9.1 ms, submit 0.0 ms / 0 iters | 422.6 ms, submit 26.6 ms / 27 iters |

That is PW7's signature exactly — a submit→sent leg of hundreds of
milliseconds spanning hundreds of reactor iterations — and **it is present on
H1 even though H1's throughput does not move.** The stall is real on this
shape; the workload simply does not convert it into throughput. Which is
precisely why the lag legs, and not the ratio, are the diagnostic.

**That spin is load-conditional, not idle-conditional.** **measured**,
`bench/idle_cpu_probe.py --cores 4 --seconds 15`: baseline with no server
running is cpu0–3 at 2.8 / 1.8 / 1.0 / 1.0%; the instance mounted and idle is
3.9 / 2.3 / 1.6 / 1.4%; four idle sessions, one per core, is 2.9 / 2.1 / 1.6
/ 1.8%. At rest the reactors block as intended and the spin costs nothing
measurable. It needs a parked coroutine, which needs a lease refill in
flight.

Per-core CPU **under load** was sampled across each whole driver invocation —
H1 gives cpu0 11.1%, cpu1 13.9%, cpu2 13.2%, cpu3 14.4%; H3 gives 6.9 / 9.2 /
9.9 / 8.1%. **Nothing is CPU-saturated in any cell.** But that window spans
both the `cores = 1` and `cores = N` configurations, so it cannot attribute
work to a configuration, and it therefore does **not** answer the run
instructions' "is core 0 idle or a bottleneck". `bench/percore_insert_probe.py`
answers it directly, and §5a carries the result: core 0 is the *least* busy
core under rotation.

---

## 9. Correctness

### 9a. Rows in equals rows out, every cell, every rep

A release build compiles `MayWrite`/`MayFault` out — they sit inside
`#ifndef NDEBUG` — so the driver's own per-relation `COUNT(*)` verify is the
only guard there is. **measured**: every rep of every cell reported
`verify: survivors as expected` and `errors=0`. **No rows were lost anywhere
in this run**, including the four-writer shapes where PW6 lost 1/13/51
INSERTs to the unretried extent lease.

Retries are visible and expected: H1 17–20 insert retries per rep, H3 6–10,
C1 none. Those are the retryable lease-refill refusals a peer answers its
first INSERT with (PW1b), and the driver's bounded retry is what keeps them
from becoming lost rows.

### 9b. Restart ownership at three writer cores: PASS

**measured** — `bench/restart_ownership_check.py --cores 4 --tables 6 --rows 2000`:
six rotated relations written from their owner cores (1, 2, 3), the instance
stopped gracefully with `STOP`, the same data file mounted again, every
relation re-read. **PASS, no problems.** All six back at 2,000 rows, the scan
count agreeing with the insert count, the point-SELECT returning the row that
was written, and `owner_core` unchanged across the restart (3/1/2/3/1/2).

This is the first exercise of PW1c-7's stamp-carried ownership at three writer
cores. Two details make it a test rather than a formality: the shutdown is
checked to be the graceful path, because a SIGTERM or SIGKILL would turn the
second mount into a crash-recovery test and a weaker claim; and the probe key
is **discovered** (by the last row's unique `balance`) rather than assumed to
be the row count, since the Keystone pk is engine-issued and assuming
`id == rows` would substitute an untested premise for the thing under test.
The discovered keys were in fact 2,000 on every relation.

### 9c. The suite

**measured**: `build-release/tests/kds_tests` — 2635 tests from 250 suites,
**2634 passed, 1 failed**, 28.1 s.

The failure is `TlsChannelTest.PlaintextGarbageIsFatal`
(`tests/tls_channel_test.cpp:216`). It asserts that OpenSSL queues **no**
alert for a first record that was never TLS (`EXPECT_TRUE(out.empty())`); on
this box's **OpenSSL 3.5.5** an alert *is* queued. The status is still
not-ok and `server_plain` is still empty — only the "no alert" half fails.
This is an upstream behaviour change, not an engine defect, and TLS is off in
every configuration measured here. It is stated rather than worked around.

### 9d. The instruments were reviewed before they produced numbers

Recorded because a benchmark's credibility is its instruments. A
`critics-developer` review of the three new tools caught five defects, all
fixed before any number below was measured:

- the `fdatasync` probe's start barrier was a 300 ms sleep, so at N=4 each
  thread's 2 MiB of pre-allocation and its `fsync` landed inside everyone
  else's measured window — a bias against N=4 that scales with N, which is
  the exact artifact that fakes an absence of overlap and would have
  mis-gated §3. Replaced with a two-`std::latch` barrier;
- its sweep ran blocked rather than interleaved, so drift across the run
  landed wholly on one arm of the ratio;
- `run_benchv2.py`'s `REFILL_RE` matched a per-core line the driver never
  prints — it emits one joined `refills:` line — silently dropping PW7's
  entire lag instrument from every summary, which is §8's whole content;
- `restart_ownership_check.py`'s `count_of()` could never return a count (the
  wire reply `count(*)\n2000` carries no whitespace at all), so the exercise
  would have failed every run while reporting that the restart lost every
  row;
- its probe check was a substring test over the whole reply, and since
  `balance = id * 10` the reply for row 200 contains "2000" — it passed on
  the wrong row.

---

## 10. What this does not measure

Stated plainly so the headline is not quoted for something it cannot support.

- **Not the hot ascending-pk single-relation case.** This harness uses N
  non-interfering relations, one connection each. The case that motivates
  the stride-forest proposal — many writers contending on one relation's
  ascending key — is not exercised at all.
- **Not cross-core statement shipping**, which is unbuilt. Every relation
  here is written from a session the kernel accepted on its owner core.
- **Not `fdatasync` on one shared file.** §3 measures N streams on N inodes.
- **Not read scaling.** §5's scan numbers rest on 2–6 samples per run.
- **Not the PW7 floors' effect at three writer cores** — §8 observes the
  fixed engine only; no pre-floors tree was run.
- **No PostgreSQL comparison.** §2 of the run instructions enumerates the
  only valid comparisons — `cores = 1` versus `cores = N` on one commit, and
  PW7-before versus HEAD — and a PostgreSQL twin answers neither. It is also
  not installed on this host. This is a scope decision, stated, not an
  omission.
- **Overhead was not measured**, per the operator's 2026-08-24 amendment.

---

## 11. What this leaves open, and for whom

The run instructions state the fork this gates and say plainly that reporting
it is not deciding it. **This run lands in the middle band — materially below
the ceiling but above 1× — and it locates the gap.** The gap is the per-core
group committer, not the device (§3), not CPU (§8), and not the cross-core
machinery (§7's control does no cross-core work and outruns rotation).

Open, in the order that a next step would need them:

1. **Where the crossover actually sits.** C2 (§7) establishes that rotation
   wins 1.751× at one writing session per writer core and nothing at two.
   Between those two points is a boundary this run brackets but does not
   locate, and it is the number any placement policy would need. **Not
   decided here.**
2. **What actually sets the per-core commit cap.** §6a withdrew the cadence
   attribution: the cap tracks the device's single-stream `fdatasync`
   latency, and the drain interval is not the lever. Whether a batch can span
   cores at all, or whether per-core drains are inherent to thread-per-core,
   is the architecture question this run poses and does not answer.
3. **The four-core-server effect** C1 exposes (insert p50 1.457× with
   everything on core 0). Undiscriminated. Whatever it is, it is currently
   larger than anything rotation contributes.
4. **PW7's floors are validated (§8a)** at the shape that provokes the
   defect, on two independent "before" arms. What is *not* settled is why H1
   carries the 924 ms submit stall without losing throughput — a stall that
   large going unpriced is itself worth understanding.
5. **PW7's two open items** stand, and they stand differently.
   *The reactor spin* is now observed at three writer cores: the trx-id leg
   spans ~19,000–24,000 reactor iterations under load (§8) while costing
   nothing measurable at idle, so it is load-conditional.
   *`fdatasync` time charged to no scheduler group* is **not measurable from
   outside the process on this tree**, which is a stronger statement than
   "not attempted": the per-group counter is
   `std::array<std::uint64_t, kNumSchedulingGroups> consumed_ns_` at
   `include/kds/sched/scheduler.hpp:250`, a **private member with no
   accessor**, and `SHOW META` does not print it
   (`src/server/command_dispatcher.cpp`, **source-read**). Reporting whether
   group accounting diverges from wall time therefore requires *adding*
   instrumentation to the engine, which is a code change and outside what a
   measurement run may do. Owed by whoever next touches `docs/spec/sched.md` §4,
   not by this file.

One passage elsewhere is affected. `docs/inflight/known-gaps.md:738` states that
*"every cross-core number in `bench/` is a cost measured with the parallelism
removed, never a speedup"*. **That is now false**: the parallelism is present
here, and with its own control subtracted H3 measures a genuine **1.751×**
(§7). This is the first measured cross-core speedup in `bench/`. The passage
has been **retired in place** in `docs/inflight/known-gaps.md`, carrying both bounds
that go with the number — the gain appears only at one writing session per
writer core, and this host is 4 logical / 2 physical. The same edit records
the group-commit constraint (§6) as an entry of its own under *Concurrency
and multicore*, retires "PW6's number is unmeasured", and adds §8a's
validation to the PW7 lease-refill entry.

---

## Reproducing

The matrix, the probes and the cells are `bench/run_benchv2.py` (committed at
`f9b9203`). The gate is `tools/fdatasync_probe.cpp`, built standalone and
deliberately outside the CMake targets:

```
g++ -O2 -std=c++20 -pthread tools/fdatasync_probe.cpp -o build-release/fdatasync_probe
build-release/fdatasync_probe <dir-on-block-device> 5 3

bench/run_benchv2.py --server build-release/kds_server \
    --workdir ~/mcbench --archive bench/v2.1.0/archive/... --cells H1,H3,H4a,H4b,C1,C2 --reps 5
```

The raw driver output of this run — every per-run stdout and JSON, the
`fdatasync` probe's output, and the per-cell CPU samples — is archived beside
this file under `bench/v2.1.0/archive/multicore-writers-v2.1.0/`. No data
file and no WAL segment is archived.
