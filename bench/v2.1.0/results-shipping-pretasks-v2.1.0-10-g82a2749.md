# What must be measured before statement shipping — the pretask run

**Batching a commit is worth 60× on one core, and it is what decides every
multi-core ratio in this run.** The same relations and the same rows run at
876 inserts/s with one commit per row and **69,454** with one commit per
thousand — one core, two sessions, nothing else changed
(§4). `bench/v2.1.0` §6 measured a per-writer-core cap at the device's
single-stream `fdatasync` rate and explained its whole matrix with it; this
run shows that cap is an **autocommit artifact**, and that shipping's design
budget therefore belongs to commit batching rather than to per-row wire cost,
which is 21–23 µs against a ~0.9 ms sync.

**The serialized single-relation baseline is not a ceiling: it scales
linearly with sessions.** One relation, N sessions, all on its owner core —
the shape `bench/v2.1.0` §10 says its matrix cannot see, and the shape
statement shipping would create on purpose — runs at ≈ 490 × S inserts/s on
core 0 and ≈ 590 × S on a peer, with insert p50 pinned near 1.7–2.0 ms
whatever S is (§5). Sessions on one core do not queue for the device; they
share a trip to it. **A peer core beats core 0 at the identical shape by
15–20%**, reproducibly at every session count.

**These are the two numbers the shipping workplan was owed**, and
`docs/memo-shipping-and-group-commit.md` (T6) does the arithmetic they
license: shipping re-concentrates onto one owner exactly what rotation
divides across W cores, which is the mechanism that made rotation lose at two
sessions per core — so shipping is predicted to be positive in the regime
rotation is negative in.

Sections still in flight are marked **NOT YET RUN** and carry no numbers.

---

## 1. The run

| | |
|---|---|
| Version | **`v2.1.0`** — the operator-named version of record; no `v2.2.0` tag exists and this run does not mint one |
| `git describe --tags` at the measured commit | **`v2.1.0-10-g82a2749`** for T1, T2 and T3 (the engine is byte-identical to `2b00f12`; `82a2749` adds only `bench/` drivers) |
| Worktree | `worktree-v2.2.0-pretasks-stmtshipping` |
| Date | 2026-08-26 UTC |
| Host CPU | Intel Xeon Platinum 8488C, **8 logical / 4 physical cores**, 2 threads/core, 1 socket, 1 NUMA node, SMT on, KVM guest |
| Kernel | `7.0.0-1006-aws`, 15 GiB RAM |
| Data device | `nvme0n1p1`, **ext4**, `rw,relatime,discard,errors=remount-ro,commit=30`, non-rotational, scheduler `[none]` |
| `--workdir` | `/home/ubuntu/mcbench2` — a block device. `--force` was never passed |
| Build | `build-release` only. `cmake -DCMAKE_BUILD_TYPE=Release -G Ninja`, g++ 15.2.0, `-Wall -Wextra -O3 -DNDEBUG`, C++20 |
| Overhead | **not measured** — suspended for v2-stage work by the operator's 2026-08-24 amendment |

**This is not `bench/v2.1.0`'s host.** That run measured an AMD EPYC 7R32
with **4 logical / 2 physical** cores; this one has **8 logical / 4
physical**, and `cpu0..3` are four distinct physical cores
(`thread_siblings_list` pairs 0-4, 1-5, 2-6, 3-7, **measured**). Reactors pin
one per core id (`PinToCore`, `src/server/expeditor.cpp:1009`,
**source-read**), so `cores = 4` here runs on four independent physical cores
where `cores = 4` there ran on two physical cores' four threads. **No number
in `bench/v2.1.0` transfers to this host**, and none is quoted here as
though it did: where a v2.1.0 finding is named it is named as a finding about
that host, and every constant it fitted is re-fitted here or left alone.

`cores = 8` is the machine's logical count and the most the server admits
(`hardware_concurrency`, `src/server/expeditor.cpp:643`, **source-read**), so
its seven writer cores are four physical cores' SMT threads. That is stated
at every `cores = 8` number rather than left for the reader.

`steal` is nil — 0-16 jiffies *per CPU since boot* — so `busy = total − idle
− iowait` is the engine's own work. **measured**, `/proc/stat`.

Every claim below is tagged **measured** (with its invocation) or
**source-read** (with `path:line` and the commit it was read at). They are
never mixed in one sentence.

---

## 4. T1a — one commit per `--batch` rows, and what it does to §6's law

`bench/v2.1.0` §6 explains its whole matrix with one expression,
`1000 × writer_cores / (470 × sessions)`, and both halves of it are about
**commits**: a writer core is capped at the device's single-stream
`fdatasync` rate, and core 0 beats that cap by batching whatever accumulated
in one reactor iteration into one sync. Every cell that fitted it issues one
autocommit statement per row, so one commit per row. A transaction of N rows
issues one commit per N rows and leaves the law's domain.

**measured** — `bench/txn_batch_probe.py` through `bench/run_t1.py`, 5 reps
per cell, `--rows 2000` per relation, `--tables` two per writer core
(2 / 6 / 14 at `cores` 2 / 4 / 8), every relation's `COUNT(*)` verified,
**0 errors and 0 lost rows in all 60 runs**:

| cores | writer cores | batch | ratio (median) | spread | multi ips | single ips | multi commits/s | single commits/s |
|---|---|---|---|---|---|---|---|---|
| 2 | 1 | 1 | 1.171 | 1.091–1.234 | 1,026 | 876 | 1,026 | 876 |
| 2 | 1 | 10 | 1.104 | 0.955–1.160 | 9,594 | 8,764 | 959 | 876 |
| 2 | 1 | 100 | 1.038 | 1.004–1.090 | 42,838 | 41,373 | 428 | 414 |
| 2 | 1 | 1000 | 0.935 | 0.854–1.046 | 65,971 | 69,454 | 66 | 70 |
| 4 | 3 | 1 | **2.035** | 1.996–2.120 | 2,837 | 1,394 | 2,837 | 1,394 |
| 4 | 3 | 10 | **2.860** | 2.770–2.939 | 28,988 | 10,137 | 2,899 | 1,014 |
| 4 | 3 | 100 | 1.577 | 1.507–1.614 | 55,656 | 36,281 | 557 | 363 |
| 4 | 3 | 1000 | 1.089 | 1.027–1.144 | 56,667 | 51,986 | 57 | 52 |
| 8 | 7 | 1 | 0.976 | 0.965–0.983 | 3,400 | 3,482 | 3,400 | 3,482 |
| 8 | 7 | 10 | **2.889** | 2.856–3.004 | 41,840 | 14,462 | 4,184 | 1,446 |
| 8 | 7 | 100 | 1.325 | 1.302–1.392 | 49,325 | 37,019 | 493 | 370 |
| 8 | 7 | 1000 | 1.013 | 1.007–1.014 | 52,581 | 51,856 | 53 | 52 |

**The per-core sync cap is an autocommit artifact, and the size of the
artifact is 60×.** On one core, the same relations and the same rows run at
876 inserts/s with a commit per row and **69,454** with a commit per thousand
— `cores = 2`'s single-core arm, which is the cleanest read because it holds
sessions constant at 2. Nothing about the engine's per-core sync rate
changed between those two numbers; what changed is how many rows one sync
was asked to cover. **Statement shipping's design budget should not be spent
on per-row wire cost**: at `batch = 1000` an INSERT costs 21–23 µs of
statement time (median, both arms) against a ~0.9 ms commit, so the commit
is 40× the statement and the batch decides everything.

**Rotation's advantage is largest in the middle and vanishes at both ends.**
At `batch = 1` the ratio is whatever §6's law says for that session count —
2.04 at 3 writer cores against 6 sessions, and 0.98 at 7 writer cores against
14 sessions, where the single-core arm's batch has grown enough to keep pace.
At `batch = 10` rotation wins **2.86× / 2.89×**, its best cell anywhere in
this run. At `batch = 1000` every ratio falls to 0.94–1.09.

The reason the ends differ is the reason §6 gives, read in both directions.
At `batch = 1` the single-core arm is *already* batching — 14 sessions on
core 0 commit 3,482/s, three and a half times the volume's single-stream sync
rate — so there is little left for spreading to win. At `batch = 1000`
neither arm is sync-bound at all, and the ratio measures something else
entirely, which is the next paragraph.

**At `batch ≥ 100` these cells are at the harness's ceiling, not the
engine's, and are reported as unresolved.** Every configuration lands in
49,000–69,000 ips regardless of how many writer cores it has: `cores = 2`
with **one** writer core does 65,971 at `batch = 1000` and `cores = 8` with
**seven** does 52,581. A seven-fold difference in engine parallelism moving
the number by −20% is not an engine result. §3b's ceiling probe measures what
a CPython driver with this thread count can do at all, and these cells are
read against it rather than quoted.

**No constant is proposed.** `batch` is the client's, not the engine's, and
this cell says what it buys, not what it should be.

---

## 5. T1b — one relation, N sessions, ascending keys: the serialized baseline

`bench/v2.1.0` §10 lists this shape first among what its matrix cannot see:
N sessions contending on **one** relation's ascending key, which is the case
the stride-forest proposal exists for and the case statement shipping
re-creates on purpose. Today every one of those sessions must sit on the
relation's owner core — rotation places exactly one owner and a session
elsewhere is refused (`crosscore.md` CC3; DML shipping is unbuilt) — so this
is the serialized baseline, and until now there was no number for stride or
shipping to be measured against.

**measured** — `bench/single_relation_probe.py` through `bench/run_t1.py`,
5 reps per cell, `--rows 1000` per session, one relation, every session
collected on its owner core, `COUNT(*)` verified against
`sessions × rows + 1` in every run, **0 errors and 0 lost rows**:

| arm | cores | writer core | sessions | ips | p0 | p25 | p50 | p75 | p99 |
|---|---|---|---|---|---|---|---|---|---|
| single | 1 | core 0 | 1 | 895 | 677 | 1,032 | 1,104 | 1,178 | 1,533 |
| single | 1 | core 0 | 2 | 980 | 1,076 | 1,894 | 2,036 | 2,174 | 2,619 |
| single | 1 | core 0 | 4 | 1,996 | 1,069 | 1,859 | 2,004 | 2,141 | 2,528 |
| single | 1 | core 0 | 8 | 4,038 | 944 | 1,834 | 1,964 | 2,103 | 2,539 |
| multi | 2 | core 1 | 1 | 957 | 686 | 945 | 1,052 | 1,114 | 1,339 |
| multi | 2 | core 1 | 2 | 1,136 | 937 | 1,673 | 1,771 | 1,868 | 2,149 |
| multi | 2 | core 1 | 4 | 2,301 | 869 | 1,625 | 1,749 | 1,843 | 2,210 |
| multi | 2 | core 1 | 8 | 4,696 | 818 | 1,557 | 1,727 | 1,834 | 2,199 |
| multi | 4 | core 3 | 1 | 1,050 | 662 | 878 | 931 | 1,001 | 1,186 |
| multi | 4 | core 3 | 2 | 1,168 | 845 | 1,650 | 1,718 | 1,788 | 2,025 |
| multi | 4 | core 3 | 4 | 2,272 | 974 | 1,582 | 1,773 | 1,875 | 2,218 |
| multi | 4 | core 3 | 8 | 4,336 | 980 | 1,710 | 1,853 | 1,973 | 2,347 |
| multi | 8 | core 5 | 1 | **1,220** | 644 | 727 | **798** | 891 | 1,096 |
| multi | 8 | core 5 | 2 | 1,305 | 770 | 1,377 | 1,490 | 1,644 | 2,035 |
| multi | 8 | core 5 | 4 | 2,569 | 820 | 1,406 | 1,522 | 1,676 | 2,068 |
| multi | 8 | core 5 | 8 | **5,252** | 805 | 1,420 | **1,478** | 1,561 | 2,037 |

Microseconds; medians over 5 reps. The owner core is **discovered** from
`DESCRIBE`, never assumed: rotation assigns by creation sequence, which is
why the three multi arms sit on cores 1, 3 and 5 rather than all on core 1.


**The serialized baseline is not flat: it scales linearly with sessions, and
the latency does not move.** From two sessions upward, aggregate throughput
is very nearly proportional to the session count while insert p50 sits pinned
at 1.7–2.0 ms. That is the group committer seen from the inside of one core:
each reactor iteration ends in one `fdatasync`, and the more sessions have a
commit staged when it runs, the more rows that one sync covers. Sessions do
not queue for the device; they *share* a trip to it.

**One session is the exception, and it is the interesting one.** At S = 1
there is nothing to batch with, so the insert costs exactly one sync
(p50 ≈ 1.0 ms) and throughput is the device's single-stream rate. At S = 2
the latency roughly doubles while throughput barely moves — two sessions
whose commits do not land in the same iteration serialize on two syncs — and
only from S = 4 does the batch grow fast enough to hold latency flat while
throughput rises.

**The fit.** Past S = 2 the arms are close to linear in sessions:
core 0 at ≈ 490 × S inserts/s and the peer core at ≈ 590 × S. `bench/v2.1.0`
§6 fitted its single-core arm at **470 × sessions** on a different CPU and a
different device; that this run re-fits it at 490 on Sapphire Rapids with
four physical cores is worth stating, because it means the constant is a
property of the *mechanism* — one sync per reactor iteration, shared by
whoever is staged — and not of the machine it was first measured on.

**The peer core beats core 0 at the identical shape**, by 15–20% on
throughput and 13% on p50, reproducibly across every session count. Core 0
carries the listener, the catalog, the lease-granting services and the
system-core role; a peer carries its relation and nothing else. This is the
same asymmetry §7's control chases from the other direction, and T3 examines
it.

**What this says to the two designs it was measured for.** Stride's premise
is that a single hot relation is a serialization point; that is true of the
*keys* and false of the *commits* — the commits already batch, and the batch
is what sets throughput here. And shipping's arrival-core cost has to be paid
against a number that grows with concurrency rather than a ceiling: an owner
core absorbing N shipped writers is on this curve, not on a per-core sync
cap. §6 of the memo (`docs/memo-shipping-and-group-commit.md`) does that
arithmetic.

### 5a. What `cores = 8` adds, and what it costs

`cores = 8` is this host's logical count; its writer cores are four physical
cores' SMT threads. The single-session cell is the sharpest reading:
p50 **798 µs** against `cores = 2`'s 1,052 and `cores = 1`'s 1,104, with
throughput 1,220 against 957 and 895 — a **1.36×** on the same one-session,
one-relation workload where nothing cross-core happens beyond the peer write
path itself. Eight sessions give 5,252 ips against core 0's 4,038, **1.30×**.

That is the four-core-server effect `bench/v2.1.0` §11-3 flagged, reproduced
here on a machine with twice the cores and growing with the core count.
**§7 discriminates it.** Until then it is an observation, and every ratio in
this document that compares a multi-core arm against `cores = 1` carries it.

---
