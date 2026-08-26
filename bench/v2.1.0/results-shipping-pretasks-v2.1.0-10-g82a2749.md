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

## 2. What ran, and what this run corrected about its own instructions

| task | state |
|---|---|
| **T1a** transaction-wrapped bulk insert, batch 1/10/100/1000 × cores 2/4/8 | run, 5 reps — §4 |
| **T1b** one relation, 1/2/4/8 sessions × cores 1/2/4/8 | run, 5 reps — §5 |
| **T2** the crossover curve, cores 2/4/8 | run, 5 reps — §6 |
| **T3** discriminating the four-core-server effect | run — §7 |
| **T4** the parked-coroutine price, and the group-accounting instrument | run — §8 |
| **T5** the cross-core write refusal counters, and a baseline reading | run — §9 |
| **T6** the shipping × group-commit memo | `docs/memo-shipping-and-group-commit.md` |
| the device gate, the harness gate | run — §3 |

Four things this run corrected, recorded because a run that only ever
confirms its own instructions is not measuring:

1. **T5's premise was stale.** The instructions say the §6 counters are
   *"specified and **unbuilt** (source-read, no implementation sites)"* and
   ask for the known undercount to be stated: *"the peer-listener guard
   refuses foreign writes before parsing, so that class is invisible"*. On
   this tree at `82a2749` the class and **both recording sites** exist
   (`CheckWriteAffinity`, `src/server/command_dispatcher.cpp:2954` and
   `:2963`, **source-read**); what was missing was any way to *read* them
   from outside the process. And the undercount named is retired: that
   pre-parse guard was `PeerWriteRefused`, **deleted at PW1c-5** on
   2026-08-24, whose own workplan row says the change *"reverses PW5's
   recorded undercount"*. `docs/crosscore.md` §6 still asserted it and has
   been corrected in place. T5 became "expose, and state the undercount that
   is real now" — §9.
2. **T2's sweep measures the busiest core, not the average.** The
   instructions ask for "fractional sessions-per-core" points at 1.33 and
   1.67. On a thread-per-core engine there is no fractional load: at
   `tables = 4` over three writer cores the cores hold 2, 1 and 1. §6 reports
   the curve as asked *and* reports what it turns out to measure, which is a
   step at the first core to take a second session.
3. **The T1 orchestrator's quiet-load threshold stalled the sweep it was
   guarding.** A fixed `0.6` one-minute load, inherited from a 4-CPU host,
   is below what an 8-CPU box carries just after a benchmark, so every cell
   waited out `wait_quiet`'s 180 s timeout. It scales with the CPU count now,
   and the orchestrator gained resume so an interrupted sweep re-uses
   finished repetitions instead of re-measuring them.
4. **T4's own instrument shipped a dangling pointer, caught before it
   produced a number.** The first form set the dispatcher's scheduler view to
   a function-local reactor in `Expeditor::Serve` and cleared it after the
   worker join — with **twenty** early `return`s in between, any of which
   would have left a member dispatcher pointing at freed stack, readable
   through the public `dispatcher()` accessor. A `critics-developer` review
   found it; it is a scope guard now, and the same review found a test
   assertion that could not fail (`find("=1")` matches `ddl_durable=1`). §8a
   records both, because an instrument's credibility is the whole of its
   value.

---

## 3. The two gates, run before anything is read

### 3a. The device: this volume overlaps four `fdatasync` streams and then gets worse

`bench/v2.1.0` §3 ran this as a blocking gate, because if the device cannot
overlap concurrent syncs then every multi-core ingest ratio belongs to the
I/O backend rather than to the architecture. It must be re-run here: the host
and the volume are both different.

**measured** — `build-release/fdatasync_probe /home/ubuntu/mcbench2 5 3
1,2,3,4,5,6,7,8` (5 reps of 3 s per arm, arms **interleaved**, 8192-byte
page, one file and one fd per thread):

| threads | median syncs/s | min | max | vs N=1 |
|---|---|---|---|---|
| 1 | 1,117.7 | 1,055.7 | 1,192.2 | 1.000 |
| 2 | 1,970.1 | 1,873.1 | 2,021.7 | 1.763 |
| 3 | 2,800.9 | 2,773.3 | 2,819.1 | 2.506 |
| 4 | **3,767.5** | 3,735.4 | 3,819.4 | **3.371** |
| 5 | 3,326.2 | 3,282.4 | 3,367.1 | 2.976 |
| 6 | 3,104.8 | 3,099.8 | 3,108.4 | 2.778 |
| 7 | 3,101.7 | 3,099.3 | 3,170.3 | 2.775 |
| 8 | 3,102.3 | 3,101.8 | 3,107.7 | 2.776 |

**The overlap peaks at four streams and then declines — and four is this
host's physical core count.** Past N=4 the aggregate falls by 18% and then
sits flat at ~3,100/s through N=8. The single-stream figure is
**1,118 syncs/s**, a 0.89 ms sync.

This is a stronger statement than v2.1.0's gate could make: that host was
measured only to N=4 (its logical count) and read 3.657× as "near-linear to
N=3". Here the same shape is visible with its top: **near-linear to N=3, a
knee at N=4, and a decline after it.** The probe's own caveat still bounds
the quote — separate files means separate inodes, so this answers *"N cores,
N WAL streams"*, the engine's shape, and not *"N cores, one shared file"*.

**Read T1a's `cores = 8` cell against this and it stops being a puzzle.**
Seven writer cores commit 3,400/s aggregate (§4) — the device's 7-stream
ceiling of 3,102 to within noise — while the single-core arm, batching 14
sessions onto **one** stream, does 3,482/s. Both arms hit the same wall from
opposite directions: one by opening more streams, one by putting more commits
in each trip. **On this host the aggregate sync ceiling is ~3,100–3,770/s
however it is reached**, which is the fact any scaling claim about writer
cores has to clear first.

### 3b. The harness: what a CPython driver can do at all

T1a's batched cells reach tens of thousands of inserts per second, and at
that rate the number may be describing the driver rather than the engine.

**measured** — `bench/client_ceiling_probe.py --threads 1,2,4,6,8,14
--seconds 3`, `cores = 1`, three arms per thread count:

| threads | PING ops/s | point-SELECT ops/s | autocommit INSERT ops/s |
|---|---|---|---|
| 1 | 50,549 | 40,999 | 943 |
| 2 | **107,853** | 84,033 | 1,097 |
| 4 | 86,106 | 85,070 | 1,802 |
| 6 | 63,467 | 61,585 | 3,194 |
| 8 | 56,411 | 56,029 | 3,274 |
| 14 | 56,141 | 55,976 | 7,452 |

**At fourteen threads the harness tops out near 56,000 statements/s**, and
T1a's `cores = 8, batch = 1000` cell measured **52,581** — 94% of it. Those
cells are therefore reported as **unresolved**: the engine may be faster and
this driver cannot say so.

The attribution is checkable rather than assumed. This probe runs
`cores = 1`, so its 56,141 is one reactor plus CPython; T1a's 52,581 has
**seven** writer reactors available. Seven reactors not beating one is not a
statement about reactors, so the constraint is on the client side of the
socket. The unbatched cells are nowhere near it — a `batch = 1` cell runs at
876–3,482 ips against a 56,000 ceiling — so **every conclusion in §4 and §5
about commit batching is drawn from cells with two orders of magnitude of
headroom**, and only the `batch ≥ 100` ratios are withheld.

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

---

## 6. T2 — the crossover is a step, not a slope, and it is the *busiest* core that sets it

`bench/v2.1.0` §7 measured rotation winning **1.751×** at one writing session
per writer core and **0.989×** at two, and §11-1 left the boundary between
them "bracketed but not located". Locating it is what any placement policy
needs, and what shipping needs in order to decide when to ship rather than
refuse under load.

The sweep is by table count, since the driver gives each relation exactly one
writing session and rotation spreads relations over the `cores - 1`
non-system cores: `sessions per writer core = tables / (cores - 1)`.

**measured** — `bench/run_t2.py` (`tools/multicore_benchmark.py`,
`--placement rotate --peer-listeners`, `--rows 2000`), 5 reps per point,
`errors=0` and every relation's survivor count verified in every run:

| cores | writer cores | tables | sessions per writer core | max on one core | ratio (median) | spread | multi stmt/s | single stmt/s |
|---|---|---|---|---|---|---|---|---|
| 2 | 1 | 1 | 1.00 | 1 | 1.204 | 1.143–1.361 | 1,429 | 1,175 |
| 2 | 1 | 2 | 2.00 | 2 | 1.168 | 1.160–1.245 | 1,450 | 1,244 |
| 4 | 3 | 3 | 1.00 | 1 | **1.999** | 1.959–2.036 | 3,686 | 1,836 |
| 4 | 3 | 4 | 1.33 | **2** | 1.170 | 1.077–1.235 | 2,815 | 2,475 |
| 4 | 3 | 5 | 1.67 | 2 | 1.172 | 1.063–1.193 | 3,484 | 2,960 |
| 4 | 3 | 6 | 2.00 | 2 | 1.118 | 1.032–1.133 | 3,974 | 3,533 |
| 8 | 7 | 7 | 1.00 | 1 | 1.036 | 1.023–1.109 | 4,571 | 4,410 |
| 8 | 7 | 9 | 1.29 | **2** | **0.804** | 0.754–0.812 | 4,204 | 5,234 |
| 8 | 7 | 12 | 1.71 | 2 | **0.595** | 0.580–0.631 | 4,399 | 7,383 |
| 8 | 7 | 14 | 2.00 | 2 | **0.506** | 0.495–0.513 | 4,365 | 8,625 |

5 reps per point, `errors=0` in all 50 runs, every relation's survivor count
verified in every run.

**The curve does not slope; it steps.** At exactly one session per writer
core rotation wins ~2×. The moment the average passes 1.00 — which on a
thread-per-core engine means *one* core has taken a second relation — the
ratio collapses to ~1.1 and stays there through 2.00. A fractional average is
not a fractional load: at `cores = 4, tables = 4` the three writer cores hold
2, 1 and 1 relations, and the run's wall clock is the two-relation core's.

**That is §6's law, and the law is about a core, not about an average.** A
core with one session commits once per sync; a core with two commits twice
and takes twice as long, while its idle neighbours finish early and wait. The
aggregate is therefore set by `max` sessions per writer core, and every
intermediate point on this sweep has the same max — 2 — which is exactly why
they have the same ratio.

**So the crossover is located, and it is not a number between 1 and 2.** It
is the first core to receive a second session. A placement policy that keeps
`max` at one session per writer core gets the whole 2×; one that lets any
core take two gives back nearly all of it, however good the average looks.
`bench/v2.1.0`'s bracket — 1.751× at 1.00 and 0.989× at 2.00 — was not
bracketing a slope; it was measuring the two sides of a step.

**No policy is proposed here.** Placement is `docs/crosscore.md` §9's open
decision and this is an input to it.

### 6a. At seven writer cores the multi-core arm is pinned, and that is the whole curve

The `cores = 8` rows are the clearest thing this run measured, because one
column does not move: **the multi-core arm sits at 4,204–4,571 stmt/s at
every table count**, while the single-core arm climbs from 4,410 to 8,625 as
sessions are added. The ratio's collapse from 1.04 to 0.51 is entirely the
denominator.

The pinned number is the device's, and it can be checked against §3a
directly. This workload is 5/7 commit-bound (insert + update + delete out of
insert / point-select / update / delete / scan), so 4,365 stmt/s is
**3,117 commits/s** — against §3a's measured 8-stream ceiling of
**3,102/s**. The seven writer cores are not slow; they are *at the device's
limit for seven concurrent streams*, which is 18% below what four streams
manage.

The single-core arm has no such limit because it is not opening streams — it
is filling one. Fourteen sessions on core 0 commit 6,158/s through a device
that syncs 1,118 times a second, which is a batch of 5.5 commits per sync.

**So the two arms are the same law at two operating points**, and the
crossing point is where a core's batch is one: below it, more streams win;
above it, a fuller batch wins, and it wins by more the more sessions there
are. At `cores = 8, tables = 14` concentrating beats spreading by **1.98×**
(8,625 against 4,365) on identical work.

**This is the measured half of what T6's memo predicts about statement
shipping.** Shipping re-concentrates commits onto owner cores; the arm that
does that here is the one that wins at every session count above one per
core, and the margin grows with load.

---

## 7. T3 — the four-core-server effect: one candidate dead, one harness bias found, the rest not separable

`bench/v2.1.0` §7 measured `cores = 4` with **everything on core 0 and
nothing cross-core happening at all** beating `cores = 1` by 1.071× aggregate
and 1.457× on insert p50, and §11-3 left it undiscriminated with three named
candidates: four WAL anchors, per-core extent leases, background work moving
off core 0. It is currently a larger effect than rotation's whole
contribution, so every multi-core ratio needs it subtracted.

### 7a. The null cell, which changes how every other ratio is read

The sweep's first cell is `cores = 1` against `cores = 1` — the same driver,
two identical servers, compared to each other. It should be 1.000.

**measured** — `bench/run_t3.py --cores 1,2,3,4,8 --tables 6 --rows 2000`,
`--placement creating`, no peer listeners, 5 reps per cell, `errors=0`
throughout:

| cores | ratio (median) | spread | insert p50 gain | N-core stmt/s | one-core stmt/s |
|---|---|---|---|---|---|
| **1 (null)** | **1.099** | 0.970–1.115 | 1.186 | 3,761 | 3,469 |
| 2 | 1.131 | 1.078–1.195 | 1.340 | 3,913 | 3,479 |
| 3 | 1.183 | 1.130–1.227 | 1.300 | 4,161 | 3,506 |
| 4 | 1.202 | 1.142–1.307 | 1.325 | 4,293 | 3,516 |
| 8 | 1.340 | 1.318–1.354 | 1.461 | 4,659 | 3,474 |

**The null cell is not 1.000; it is 1.099.** The driver starts its
`single-core` arm first and its second arm afterwards on the same box, and
the second arm is ~10% faster for reasons that have nothing to do with what
is being compared. Every A/B ratio this driver produces carries that, and
**`bench/v2.1.0` never ran this cell** — so its C1 (1.071) and C2 (1.067)
cannot be separated from an ordering bias by anything in that file. That is a
statement about the harness, not about that run's conclusions: H3's 1.751×
was computed as a *cross-cell* comparison of absolute aggregates, which the
bias does not touch.

Divided through by the null cell, the residual four-core-server effect on
this host is **1.03× at 2 cores, 1.08× at 3, 1.09× at 4 and 1.22× at 8** —
real, monotone in the core count, and roughly a third of what the raw ratios
show.

### 7b. The candidate configuration cannot reach — and it is dead

The three candidates §11-3 names are all *engine* state, and none of them has
a knob: `wal_anchor_count` is a **high-water mark of anchor slots ever
published** (`src/server/superblock.cpp:174`, **source-read**), not a
setting; per-core extent leases are unconditional above `cores = 1`; and
background work has no toggle.

There is a fourth candidate that list does not name, and it *is* testable
from outside: **the peer reactors are idle but not asleep**. A reactor with
nothing to do blocks in `PollReady` for at most `max_idle_block_ms` = 10 ms,
so every peer core wakes ~100 times a second forever — which on a server CPU
is the difference between a deep C-state and a merely idle core, and which
would scale with the core count exactly as the measured effect does.

**measured** — `bench/idle_wakers_probe.py --wakers 3,7 --cores-arms 1,4,8
--tables 6 --rows 2000 --reps 3`, insert phase only, arms **interleaved by
repetition** so drift cannot land on one of them, helper processes pinned one
per otherwise-unused CPU and doing nothing but `sleep(10 ms)` in a loop:

| arm | ips (median) | spread | insert p50 | against `cores = 1` |
|---|---|---|---|---|
| `cores = 1`, no wakers | 2,743 | 2,519–2,864 | 2,198 µs | 1.000 |
| `cores = 1`, **3 wakers** | 2,628 | 2,588–3,080 | 2,256 µs | **0.958** |
| `cores = 1`, **7 wakers** | 2,661 | 2,633–3,196 | 2,234 µs | **0.970** |
| `cores = 4`, no wakers | 2,965 | 2,837–3,388 | 1,966 µs | **1.081** |
| `cores = 8`, no wakers | 3,090 | 3,045–3,879 | 1,891 µs | **1.127** |

**Supplying the wake cadence from outside the process reproduces none of the
effect** — it is slightly *negative*, which is what adding processes to a box
usually is — while the same probe's `cores = 4` and `cores = 8` arms show
1.081× and 1.127× in the same interleaved run. The candidate is dead by
measurement rather than by argument, and this probe's ratios are the
cleanest estimate of the residual effect in this document, because it has no
A/B ordering to be biased by.

### 7c. What `SHOW META` says, and where this stops

**measured**, one `SHOW META` per arm after the same workload: across
`cores` 1, 2, 3, 4 and 8 every field is identical — `undo_pages_live=2`,
`undo_pages_recycled=64`, `recovery_records=0`, `catalog_marks_purged=0` —
**except `wal_anchor_count`, which is the core count**. And that one cannot
be the cause: with `creating` placement only core 0 ever writes, so the other
anchors are allocated slots nothing touches.

Per-core CPU says the same thing from the other side: nothing is near
saturated in any arm — every CPU sits at 1.3–7.1% busy, and core 0 is not
distinguishable from the rest, because the driver's own threads float across
the box while the single reactor is pinned.

**So this is where T3 stops, exactly as its instructions require.** Of the
four candidates, one is dead (the idle-wake cadence), one is visible but
excluded by its own semantics (`wal_anchor_count`), and the remaining two —
per-core extent leases and background work — have no configuration that
separates them on this engine. Finding out would mean patching the engine,
which a measurement run may not do.

**What this run does establish about it**: the effect is real, it is
**1.08–1.13×** rather than the 1.20–1.34× the raw A/B shows, it grows with
the core count, and **any future multi-core ratio on this harness must be
divided by a null cell before it is quoted** — which is the finding with the
longest reach, because it applies to every number this driver has ever
produced.

---

## 8. T4 — the reactor's time, and why the parked population could not be built

Statement shipping parks a waiter on the **arrival** core for every shipped
statement while the **owner** core executes it. `bench/v2.1.0` §8 measured
what that does to a reactor today — the trx-id refill leg spanning
19,000–24,000 reactor iterations under load, because `IdleTimeoutMs` returns
0 while any ready queue is non-empty (`src/sched/scheduler.cpp:196-199`,
**source-read**) — and §11-5 recorded that the *accounting* half of the
question could not be answered from outside the process at all.

### 8a. The instrument — the one engine change this run is allowed

`bench/v2.1.0` §11-5 states the problem exactly: the per-group counter is
`std::array<std::uint64_t, kNumSchedulingGroups> consumed_ns_` at
`include/kds/sched/scheduler.hpp:250`, **a private member with no accessor**,
and `SHOW META` does not print it — so reporting whether group accounting
diverges from wall time *"requires adding instrumentation to the engine,
which is a code change and outside what a measurement run may do"*. It was
left owed to whoever next touched `docs/sched.md` §4.

It is paid here. `SHOW META` now prints, per core:

```
sched_wall_us=<reactor wall clock since its first iteration>
sched_iterations=<RunOnce calls>
sched_<group>_polled_us=<cumulative time inside that group's task polls>
sched_<group>_polls=<how many polls it got>
sched_<group>_consumed_us=<the share law's own counter>
```

for `foreground`, `maintenance` and `system`.

**There are two counters per group, and the distinction is the whole reason
the accessor could not simply expose `consumed_ns_`.** That counter is the
share law's *input* and is **halved periodically**
(`Scheduler::MaybeDecayConsumedRuntime`, so history does not dominate the
pick) — it says what the next pick will weigh, never how much time a group
has had. `polled_us` and `polls` are new, cumulative and never decayed. So

    sched_wall_us − Σ sched_<group>_polled_us

is the reactor time charged to **no** group — the `PollReady` idle block on a
quiet reactor, the WAL drain's `fdatasync` on a committing one, timer
callbacks, the io drain — which is the quantity `docs/sched.md` §4's last
bullet names and could not previously be read. And a **spin** has a
signature of its own here: `polls` climbing while `polled_us` does not,
because a parked coroutine answers `kSuspended` in nanoseconds.

The cost is two integer adds per task poll, on the poll path, and one pointer
compare per `SHOW META`. It is stated rather than measured: the operator's
2026-08-24 amendment suspends the interleaved A/B overhead measurement for
v2-stage work, so this run does **not** claim the instrument is free.

The block is **absent** rather than zeroed where no reactor is attached — a
socket-free test has no answer, and printing zeroes would be one. That is the
rule the recovery block already follows.

**Its own review is part of the deliverable**, because an instrument that is
wrong is worse than no instrument. A `critics-developer` pass over the change
found three defects before any number was taken with it: the dangling view on
`Serve`'s twenty early returns (§2's correction 4), a comment in
`core_runtime.cpp` that asserted the opposite of what `~CoreRuntime` does
(the destructor resets the scheduler *ahead* of the dispatcher, which the
header documents at length), and a test assertion — `find("=1")` — that
`SHOW META` satisfies through `ddl_durable=1` whatever the counters say. All
three are fixed; the accessors' concurrency protocol is now written into
`scheduler.hpp`'s protocol block, and `docs/client-manual.md`'s field list
carries both new blocks.


### 8b. The measurement: 94–98% of a loaded reactor's wall time is charged to no group

**measured** — `bench/reactor_accounting_probe.py`, 3 reps per cell, one
`SHOW META` reading session held per core for the whole run (the counters are
core-local, so a delta must come from the same reactor twice), an idle window
and a loaded window on the same mount, `errors=0` and `retries=0` in every
cell:

| cores | sessions | owner | ips | insert p50 | owner polls | owner polled | **unaccounted** | polls/s | ns per poll |
|---|---|---|---|---|---|---|---|---|---|
| 4 | 1 | 3 | 864 | 1,147 µs | 12,701 | 0.172 s | **97.1%** | 2,116 | 13,548 |
| 4 | 4 | 3 | 1,886 | 2,165 µs | 22,698 | 0.228 s | **96.2%** | 3,781 | 10,080 |
| 4 | 8 | 3 | 3,603 | 2,242 µs | 43,354 | 0.338 s | **94.4%** | 7,221 | 7,578 |
| 8 | 1 | 5 | 981 | 990 µs | 12,647 | 0.148 s | **97.5%** | 2,107 | 10,620 |
| 8 | 4 | 5 | 2,040 | 1,915 µs | 24,559 | 0.182 s | **97.0%** | 4,090 | 7,378 |
| 8 | 8 | 5 | 4,069 | 1,916 µs | 48,958 | 0.254 s | **95.8%** | 8,154 | 4,923 |

Windows are 6 s loaded and 4 s idle; "unaccounted" is
`sched_wall_us − Σ sched_<group>_polled_us` over `sched_wall_us`.

**The gap `docs/sched.md` §4 names is 94–98%.** A writing core spends between
0.15 and 0.34 seconds of every six inside task polls; the rest — the WAL
drain's `fdatasync` above all — is charged to no scheduling group at all. The
share law is therefore arbitrating over **4–6%** of the reactor's time, which
is the quantitative form of PW7's finding that a low-share group's debt took
hundreds of iterations to clear: the debt is denominated in a currency the
core barely spends.

**The system core is quieter still.** Core 0 under the same load takes
13–51 polls in six seconds and is 98.7–99.6% unaccounted — it is not doing
the work, and it is not spinning either.

**At idle every reactor takes exactly one poll per window and blocks.** The
idle arm reads 1 poll and 100% unaccounted on every core at every cell, which
is the same conclusion `bench/v2.1.0` §8a reached from per-core CPU (2.8–3.9%
at rest) and now reads directly off the engine.

**No spin is visible in this shape**, and the instrument is what makes that
sayable: a spin is polls climbing while polled time does not, and here polls
track the work — 2 polls per insert at every cell, with `ns per poll` falling
as sessions rise because more of each poll's cost is amortised. §8's
19,000-iteration refill legs need a *parked* coroutine, and this workload
produced no lease-refill retries at all.

### 8c. The parked population could not be built, and the reasons are measurements

T4 asks for a probe that **holds K coroutines parked** (K = 1, 4, 16). It
cannot be built from outside this engine, and three attempts each failed for
a reason worth recording rather than a reason worth retrying:

1. **A cross-core `SELECT` would park the reading core and does not run.**
   From a core-0 session against a peer-owned relation, both
   `SELECT * FROM t WHERE balance > 0` and `SELECT COUNT(*) FROM t` are
   refused: *"relation 't' is owned by core 3 and this statement is running
   on core 0; cross-core reads need the step pipeline, which is not built"*.
   The P4d pipeline exists in process; it is not reachable this way.
2. **A shipped `CREATE INDEX` parks correctly but cannot be repeated.**
   Looping build/drop on peer-owned relations drove a `cores = 4` instance
   into `ERR page id not found` on **every** subsequent write, permanently,
   after ~58 builds — while the identical churn on `cores = 1` ran 400 builds
   clean and grew the file from 32 MB to 70 MB (**measured**, twice). §8d
   states what that is.
3. **A single build per parker is refused when the owner's extent lease is
   spent**, which after a bulk load it always is:
   *"extent lease: this core's lease of 64 pages is spent; a refill must be
   granted before it can allocate again"*. Retrying it — which the
   `TXN_CONFLICT retryable=1` spelling explicitly invites — made things
   worse, not better: 6,670 refused attempts in 30 seconds exhausted the
   **single-page free map** (*"no run of 64 contiguous free pages remains
   below the free map's coverage (65280 ids)"*), after which every core's
   refill failed and the instance never recovered.

So the deliverable T4 can honestly make is §8b's: the price of the *parked
population the engine creates for itself* — which is the population shipping
would multiply — rather than an injected one. **What remains unmeasured is
stated rather than implied**: nobody has priced K parked coroutines per core
at K = 16, and nothing here says what shipping's steady-state waiters cost.

### 8d. Two defects found on the way, reported because they bound shipping

Both are on the peer path, both were found by driving it harder than any test
does, and neither is worked around silently.

- **A refused shipped `CREATE INDEX` consumes free-map pages.** A retryable
  refusal is a statement the client is told to retry; here each refused
  attempt leaks the pages its build allocated before refusing, so a
  conforming retry loop walks a `cores = 4` instance from healthy to
  *"the free map is full"* in half a minute. The free map is one page
  covering **65,280 ids** (~510 MiB) and `docs/workplan-multi-free-map.md`
  is unbuilt — this run is evidence that its ceiling is reachable by a
  client doing what the wire tells it to do.
- **Sustained shipped builds leave a peer-owned relation permanently
  unwritable** (`ERR page id not found` on every later INSERT, not
  retryable), where the same churn on one core is clean at 400 builds. The
  mechanism is not established here and this run does not guess at it; the
  reproduction is `bench/parked_coroutine_probe.py`'s first form and the
  capacity probe archived beside it.

**Both matter to statement shipping specifically**, because the shipped-DDL
path is the exact shape shipping generalises: an arrival core parking on an
owner core's work. A design that ships DML through machinery whose DDL
sibling leaks on refusal would inherit the leak.

---

## 9. T5 — the cross-core write refusal counters, and their before-shipping reading

`docs/crosscore.md` §6 specifies a per-core counter keyed
(home core, target core, relation) and calls it *"the input the future
placement/2PC decision will be made from"*. **One instrument, two eras**:
read now, it says how often today's engine refuses a write because the
session is on the wrong core; read after shipping lands, the same counter
reports only the residue shipping cannot convert — a genuine multi-core
transaction, which is what 2PC would have to be designed for.

### 9a. What was actually missing, and what the undercount is now

The instructions describe the counters as *"specified and **unbuilt**
(source-read, no implementation sites)"*. On this tree the class
(`CrossCoreWriteCounters`, `include/kds/server/core_affinity.hpp`) and **both
recording sites** (`CheckWriteAffinity`, `src/server/command_dispatcher.cpp:2954`
and `:2963`) already existed, **source-read**. What was missing was any way
to *read* them from outside the process — which is the whole of what a metric
is for. `SHOW META` now prints, per core:

```
cross_core_write_refusals=<total>
cross_core_write_refusal_keys=<distinct (home,target,relation) keys>
cross_core_write_refusal_detail=<home>&gt;<target>:<oid>=<count>[,...]   (capped at 16, and says so)
```

**The undercount the instructions ask to be stated has been retired**, and
the one that is real now is stated in its place. The named class — *"the
peer-listener guard refuses foreign writes before parsing"* — was
`PeerWriteRefused`, **deleted at PW1c-5** on 2026-08-24, whose workplan row
says the change *"reverses PW5's recorded undercount"*. `docs/crosscore.md`
§6 still asserted it and is corrected in place. What the counter genuinely
cannot see today: **DDL on a peer** (`PeerDdlRefused` fires by verb before
any relation is resolved) and anything refused before resolution at all. The
two owner-core refusals — `RelationWriteRightsPending` and
`IndexBuildPending` — are excluded **by decision**: the write is not
cross-core, it is this core's own write waiting on a grant or a build window,
and counting it would inflate the 2PC evidence with cases 2PC does not
address.

### 9b. The baseline: four in five write statements are refused when the client does not route

Every driver in `bench/` hunts for a session on the relation's owner core,
because otherwise it could not write at all — so every one of them reports
zero refusals by construction. `bench/refusal_baseline_probe.py` deliberately
takes the sessions the kernel gives it and writes round-robin over every
relation, which is what an application that has never heard of core placement
does.

**measured**, `--tables 6 --rows 200`:

| cores | sessions | write attempts | accepted | refused | **refusal rate** | CC3 class | lease-refill class |
|---|---|---|---|---|---|---|---|
| 4 | 4 | 800 | 127 | 673 | **0.841** | 667 | 6 |
| 4 | 8 | 1,600 | 322 | 1,278 | **0.799** | 1,266 | 12 |
| 8 | 4 | 800 | 61 | 739 | **0.924** | 734 | 5 |
| 8 | 8 | 1,600 | 253 | 1,347 | **0.842** | 1,334 | 13 |

**The engine's counters agree with the driver's own count exactly, in all
four cells** — 667/667, 1,266/1,266, 734/734, 1,334/1,334 — which is what
turns the stated undercount from an assertion into a checked claim: no CC3
refusal escapes the counter, and the classes that do escape are the ones
named above.

The per-key detail is the shape §6 wants the 2PC decision made from. One
cell's core 0, verbatim:

```
cross_core_write_refusals=400 cross_core_write_refusal_keys=6
cross_core_write_refusal_detail=0>1:4004=68,0>1:4016=66,0>2:4008=66,
                                0>2:4020=66,0>3:4000=68,0>3:4012=66
```

— six relations, three target cores, evenly hit, from a home core that owns
none of them.

**The reading, for the era it records**: on this engine, with relations
rotated and sessions taken as they come, **80–92% of write statements are
refused**, and the fraction rises with the core count because a session's
chance of landing on the right core is 1/W. Every one of those refusals is a
statement that shipping would convert into work. After shipping lands, the
same counter should read the residue — writes that span *two* owners in one
transaction — and that residue, not this number, is what 2PC has to be
designed for.

