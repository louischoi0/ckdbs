# AF-T5 — namespace placement measured on a workload that actually crosses

Measured 2026-09-02 on `worktree-workorder-wf-te-t5`,
`build-release` (`-DCMAKE_BUILD_TYPE=Release`),
`bench/af_namespace_grouping_probe.py`, three interleaved arms, 5 reps per
point, data file on a block device, `errors=0` in every run.

**Two commits, and a bridge cell that licenses reading them together.**
§3's first five cells were measured at **`v2.7.0-99-g775e79d`**; the
7-group cells §3a adds were measured at **`v2.7.0-105-g154df22`**, after
that branch merged `origin/main`'s AH-T6 follow-ups, which touched
`command_dispatcher.cpp` and `txn_2pc_service.cpp`. `g3-c4` was therefore
**re-run at the second commit** so the two tables are not being compared
across an unmeasured gap: `namespace/creating` reads 1.391× at the first
and 1.443× at the second, `rotate/creating` 1.320× and 1.423×. The
absolute control moved (5,734 → 5,446 join/s, ~5% slower) and the ratios
did not, which is what the bridge exists to establish.

**The verdict, because AF-T5 asked for it either way:** namespace placement
**is** an answer to AE-8 rather than a convenience — against single-core
placement it is 1.12× at two groups, 1.38–1.44× at three and **1.95× at
seven**, the last being the sizing AF actually exists for. Against *blind*
rotation the answer is conditional, and the condition is the one thing this
sweep settles:

> **Keeping a wired pair together pays when cores are scarce relative to
> declared groups, and pays nothing when they are not.** On four cores —
> three writer cores — `namespace` beats `rotate` at every group count, and
> the margin *widens* as groups pile up: 1.09× at one group, 1.06× at two,
> 1.01–1.05× at three, **1.22× at seven**. On eight cores, where every group
> can have a writer core of its own either way, it does not: 1.10× at two
> groups, 0.99× at three, **0.90× at seven**.

And it is **not free at one group, where it costs 19%** — the hop is real
and only parallelism pays for it.

---

## 1. AF-T5's own instruction rests on a premise that does not hold

AF-T5 says: *"The cell that matters is the one DA2's 0.51× came from,
re-run with the grouping supplied."* That cell cannot answer this question,
and the reason is worth more than the re-run would have been.

DA2's 0.51× is `bench/v2.1.0/results-shipping-pretasks-v2.1.0-10-g82a2749.md`
§6, whose driver is `tools/multicore_benchmark.py`. Its workload is INSERT /
point-SELECT / UPDATE / DELETE / scan on **independent** relations. There is
no join and no foreign key in it, so **nothing in that workload ever crosses
a core.** A placement policy cannot move a number that was never about
placement — and §6a of that same file says so outright, in its own words:
*"the multi-core arm sits at 4,204–4,571 stmt/s at every table count, while
the single-core arm climbs from 4,410 to 8,625 … The ratio's collapse from
1.04 to 0.51 is entirely the denominator."*

There is a second, duller reason the literal re-run would have measured
nothing: that driver creates every relation **unqualified**, so all of them
land in `public`, and `AssignOwnerCore` never rotates an undeclared
namespace (`core_placement.hpp`, NS10 clause 1). `placement = namespace`
against it is `placement = creating` with a different word in the config.

So DA2's number is not a baseline for this one and **must not be diffed
against it**: it prices a different workload under a different question.
`tools/multicore_benchmark.py` now accepts `--placement namespace` and says
in its own `--help` that the two are indistinguishable there.

**What replaces it.** `k` groups, each a *wired* pair — `head_<g>(id, tag)`
and `line_<g>(id, head_id, amt)`, both BTREE — joined on every read:

```sql
SELECT h.tag, l.amt FROM head_<g> AS h JOIN line_<g> AS l
    ON l.head_id = h.id WHERE h.id = <i>
```

One session per group, all from **one core-0 listener**: no peer listeners,
so no SO_REUSEPORT accept distribution and no session-core hunting enters
the numbers, and the three arms differ in placement and in nothing else.
The measured window is the **slowest group's join phase** (`max`, not mean —
`bench/v2.1.0` §6's own reasoning: the run finishes when the busiest core
does). The load is untimed and reported separately.

**No previous number exists for this shape.** This is the first measurement
of a join over a *declared* group in this engine, so the delta column below
is against this run's own control arm and nothing else.

---

## 2. The topology is measured, not assumed

Every arm reads each relation's owner back out of the catalog through
`DESCRIBE`'s `owner_core=`, so what a run claims about placement is what the
catalog says. At three groups on four cores, one rep:

| arm | head_0 | line_0 | head_1 | line_1 | head_2 | line_2 | groups split | cores used |
|---|---|---|---|---|---|---|---|---|
| `creating` | 0 | 0 | 0 | 0 | 0 | 0 | 0/3 | 1 |
| `rotate` | 3 | **1** | 2 | **3** | 1 | **2** | **3/3** | 3 |
| `namespace` | 1 | 1 | 2 | 2 | 3 | 3 | 0/3 | 3 |

That table is the whole mechanism in one picture: **rotation and namespace
placement use the same cores — three of them — and differ only in whether a
group's own pair is on one of them.** Rotation splits every pair it is
given, which is AF-4's "parallelism spent in the wrong place" made visible.

---

## 3. The numbers

Median of 5 reps; `join/s` is over the slowest group's join window;
latencies are per-join, in microseconds.

| cell | cores | groups | arm | join/s | range | p0 | p25 | p50 | p90 | p99 | p100 | split | cores used |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| g1-c4 | 4 | 1 | `creating` | **4611** | 4275–4665 | 202.5 | 208.9 | 211.9 | 228.3 | 263.3 | 378.5 | 0/1 | 1 |
| g1-c4 | 4 | 1 | `rotate` | 3403 | 3266–3563 | 260.4 | 287.8 | 293.5 | 314.1 | 336.8 | 472.8 | 1/1 | 2 |
| g1-c4 | 4 | 1 | `namespace` | 3715 | 3544–3939 | 237.2 | 247.1 | 273.2 | 291.3 | 310.1 | 435.9 | 0/1 | 1 |
| g2-c4 | 4 | 2 | `creating` | 5794 | 5689–5982 | 267.5 | 317.3 | 323.3 | 400.0 | 417.6 | 12325.1 | 0/2 | 1 |
| g2-c4 | 4 | 2 | `rotate` | 6151 | 5937–6750 | 264.7 | 313.6 | 323.6 | 341.2 | 384.5 | 748.3 | 2/2 | 3 |
| g2-c4 | 4 | 2 | `namespace` | **6502** | 6332–6678 | 232.7 | 282.3 | 301.4 | 328.8 | 356.5 | 566.3 | 0/2 | 2 |
| g2-c8 | 8 | 2 | `creating` | 5702 | 5370–5836 | 240.7 | 328.5 | 341.7 | 365.2 | 434.6 | 18091.4 | 0/2 | 1 |
| g2-c8 | 8 | 2 | `rotate` | 5782 | 5681–5988 | 281.3 | 329.6 | 339.6 | 371.6 | 402.2 | 1088.3 | 2/2 | 4 |
| g2-c8 | 8 | 2 | `namespace` | **6383** | 6306–6640 | 233.3 | 290.4 | 305.1 | 348.2 | 374.4 | 700.9 | 0/2 | 2 |
| g3-c4 | 4 | 3 | `creating` | 5734 | 5534–5866 | 244.4 | 332.6 | 380.8 | 687.0 | 775.0 | 1169.6 | 0/3 | 1 |
| g3-c4 | 4 | 3 | `rotate` | 7569 | 7407–8085 | 269.6 | 359.2 | 374.8 | 455.5 | 496.0 | 1857.8 | 3/3 | 3 |
| g3-c4 | 4 | 3 | `namespace` | **7974** | 7568–8193 | 241.2 | 325.5 | 354.2 | 424.0 | 488.3 | 10281.0 | 0/3 | 3 |
| g3-c8 | 8 | 3 | `creating` | 5658 | 5486–5949 | 220.9 | 332.5 | 351.8 | 690.2 | 779.1 | 1557.7 | 0/3 | 1 |
| g3-c8 | 8 | 3 | `rotate` | **7872** | 7754–7946 | 286.9 | 354.2 | 369.4 | 407.0 | 479.1 | 3373.3 | 3/3 | 6 |
| g3-c8 | 8 | 3 | `namespace` | 7787 | 7756–7919 | 241.4 | 339.4 | 362.5 | 427.7 | 494.2 | 752.3 | 0/3 | 3 |

Ratios against each cell's own control:

| cell | `rotate` / `creating` | `namespace` / `creating` | `namespace` / `rotate` |
|---|---|---|---|
| g1-c4 (1 group) | 0.738× | **0.806×** | 1.092× |
| g2-c4 (2 groups) | 1.061× | **1.122×** | 1.057× |
| g2-c8 (2 groups) | 1.014× | **1.119×** | 1.104× |
| g3-c4 (3 groups) | 1.320× | **1.391×** | 1.053× |
| g3-c8 (3 groups) | 1.391× | **1.376×** | 0.989× |

**The g3-c8 row is the one that keeps this honest.** At eight cores with
three groups the two spreading arms are a tie: 7,872 against 7,787 join/s,
ranges 7,754–7,946 and 7,756–7,919 — fully overlapping, so nothing here
resolves them. Rotation split all three pairs and used six cores to do it;
namespace used three and kept them together; the throughput is the same.
Read together with `g2-c8`'s 1.104×, that says the margin over rotation is
**not** a pure crossing cost — it is a contention effect that appears when
cores are scarce relative to the work and disappears when they are not.

### 3a. The 7-group cells, at `v2.7.0-105-g154df22`

The cells above all sit at **at most one declared group per writer core**,
which is the side of `bench/v2.1.0` §6's step where rotation still wins.
These three cross it. Same probe, same arms, same 5 reps; a second commit,
and `g3-c4` re-run as the bridge (see the head of this file).

| cell | cores | groups | groups per writer core | arm | join/s | range | p0 | p25 | p50 | p90 | p99 | p100 | split | cores used |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| g3-c4′ | 4 | 3 | 1.00 | `creating` | 5446 | 5139–5568 | 232.4 | 339.9 | 397.5 | 743.5 | 811.7 | 12654.7 | 0/3 | 1 |
| g3-c4′ | 4 | 3 | 1.00 | `rotate` | 7750 | 7532–8067 | 280.8 | 361.5 | 374.1 | 425.7 | 514.0 | 1091.5 | 3/3 | 3 |
| g3-c4′ | 4 | 3 | 1.00 | `namespace` | **7861** | 7666–8009 | 246.0 | 346.4 | 364.5 | 423.2 | 489.7 | 1371.6 | 0/3 | 3 |
| g7-c4 | 4 | 7 | **2.33** | `creating` | 5609 | 3226–5682 | 293.9 | 1041.8 | 1058.8 | 2106.0 | 2188.7 | 3009.5 | 0/7 | 1 |
| g7-c4 | 4 | 7 | **2.33** | `rotate` | 8954 | 8786–9126 | 294.4 | 608.0 | 695.2 | 1034.6 | 1413.7 | 2330.6 | 7/7 | 3 |
| g7-c4 | 4 | 7 | **2.33** | `namespace` | **10962** | 5085–11161 | 257.9 | 505.2 | 545.5 | 728.5 | 1081.2 | 13194.3 | 0/7 | 3 |
| g7-c8 | 8 | 7 | 1.00 | `creating` | 5386 | 5295–5592 | 261.3 | 1055.8 | 1087.4 | 2148.3 | 2273.3 | 3244.3 | 0/7 | 1 |
| g7-c8 | 8 | 7 | 1.00 | `rotate` | **8655** | 7740–8808 | 295.3 | 633.7 | 747.4 | 1080.4 | 1624.0 | 19389.6 | 7/7 | 7 |
| g7-c8 | 8 | 7 | 1.00 | `namespace` | 7792 | 6106–8797 | 269.7 | 649.6 | 808.3 | 1274.7 | 2148.0 | 18573.7 | 0/7 | 7 |

| cell | `rotate` / `creating` | `namespace` / `creating` | `namespace` / `rotate` |
|---|---|---|---|
| g3-c4′ (3 groups, 4 cores) | 1.423× | **1.443×** | 1.014× |
| g7-c4 (7 groups, 4 cores) | 1.596× | **1.954×** | **1.224×** |
| g7-c8 (7 groups, 8 cores) | **1.607×** | 1.447× | **0.900×** |

**g7-c4 is the cell AF was built for, and it is the strongest result in the
sweep.** Seven declared groups over three writer cores: `namespace` puts
them 3/2/2 — every core holds a second group, and one holds a third — while
`rotate` splits all seven pairs across the same three cores. Same cores,
same parallelism available; the only difference in the catalog is whether a
group's own pair is together, and it is worth **22.4%**. The p50 tells the
same story from the other side: 545.5 µs against 695.2 µs, because under
rotation both steps of every join cross.

**g7-c8 is the counter-cell, and it is why the verdict above is
conditional.** Seven groups over *seven* writer cores: every group gets a
core of its own under both policies, nothing is queued behind anything, and
`namespace` then **loses** to rotation, 7,792 against 8,655 (0.900×). The
ranges overlap (6,106–8,797 against 7,740–8,808), so the size of the loss is
not well resolved — but the sign is consistent with g3-c8's 0.989× and the
direction is unmistakable: **with spare cores, splitting a pair costs
nothing, and rotation's finer-grained spreading is at worst equal.**

**Two things in that cell are unexplained and are not glossed.** Its load
phase — untimed, outside the join window — is **29.7 s under `namespace`
against 13.4 s under `rotate`**, a 2.2× gap that appears at no other point
in the sweep and that a read benchmark has no business explaining. And both
spreading arms have a p100 near 19 ms there, an order of magnitude above
every other cell. Something about seven sessions on seven cores is
different, and this probe does not say what. **It is a finding to chase, not
a number to quote.**

**Variance, stated rather than smoothed.** Two arms carry exactly one
outlying rep out of five — `creating` at g7-c4 (3,226 against 5,607–5,682)
and `namespace` at g7-c4 (5,085 against 10,905–11,161). They fall in
different reps, so this is host noise rather than a within-rep effect, and
the medians reported above exclude both. The `namespace` outlier is also
where g7-c4's 13.2 ms p100 comes from.

### 3b. The load anomaly — **this section's mechanism is retracted**

> **RETRACTED 2026-09-02, the same day it was written.** §3b explained
> g7-c8's load gap as `group` durability's batch falling to one on a core
> with a single committing session, where `rotate`'s cores had two. **A
> direct measurement falsifies exactly that** (`bench/peer_group_batch_probe.py`,
> archived beside this file): **one and two committing sessions batch
> identically — 1.000 against 1.000** — so the batch is not what separates
> the two layouts. (The batch does move, at four sessions and above; §3c has
> the sweep, and correcting its own first reading.) The measured facts below
> stand; the *reason* given for them does not, and g7-c8's 2.35× returns to
> **unexplained**.

### 3b (as written, mechanism false): a batch of one

§3a reported g7-c8's load phase at 29.7 s under `namespace` against 13.4 s
under `rotate` and called it unexplained. It is explained, and the cause is
not the crossing — it is **`group` durability losing its batch**.

`kds.conf.sample` states the mechanism in its own words: D2 is *"one fsync
amortized over a batch of concurrent committers … **a batch of one is a
batch**"*. The batch is over the committers **on one core**. So what decides
a write workload's sync count is not how many cores a placement uses but how
many *sessions* each core is committing for — a number `distinct_owner_cores`
cannot show, and which the probe now reports as `max_sessions_per_owner_core`.

g7-c8 is the one cell where the two placements differ in it:

| arm | groups | writer cores used | **sessions per core** | why |
|---|---|---|---|---|
| `creating` | 7 | 1 (core 0) | **7** | every session commits on core 0 |
| `rotate` | 7 | 7 | **2** | a core's two relations come from two *different* groups |
| `namespace` | 7 | 7 | **1** | a group's pair is on one core, so one core serves one session |

**The A/B that settles it** (3 reps, same probe, `--durability`):

| cell | class | `creating` load | `rotate` load | `namespace` load | load `ns`/`rot` |
|---|---|---|---|---|---|
| g7-c8 | `group` (default) | 12.02 s | 13.54 s | **31.86 s** | **2.35×** |
| g7-c8 | `relaxed` | 1.51 s | 1.78 s | **1.81 s** | **1.02×** |
| g3-c8 (control) | `relaxed` | 0.40 s | 0.71 s | 0.63 s | 0.88× |

Take the per-commit sync away and **the gap vanishes** — 2.35× becomes 1.02×.
The load also collapses from ~13–32 s to ~1.8 s, so under `group` this phase
is almost entirely fsync wait, which is what makes the sync *count* the whole
story.

**And the sync count tracks sessions-per-core monotonically**, which is the
confirmation rather than the hypothesis: 7 sessions per core → 12.02 s,
2 → 13.54 s, 1 → **31.86 s**. Going from a batch of two to a batch of one
doubles the syncs, and the measured ratio is 2.35×.

**What this says about the engine, and it is not about namespaces.** It is a
property of D2 meeting *any* placement that reduces sessions per core — but
**AF's policy is the thing that makes it reachable by design**, because
co-locating a group's relations is exactly what removes a core's second
committer. A relation layout chosen to keep joins local can therefore double
a write workload's fsync count against a layout that scatters it, and the
shipped default is the one that co-locates. Nothing in the engine reports
sessions per core, so an operator cannot see this coming.

The negative control behaves as the mechanism requires: at g3-c8 both
placements already give every core one session, and there both classes move
the two arms together with no gap to explain.

**One correction to §3a from this run.** g7-c8's join-side `namespace/rotate`
reads 0.969× over these 3 reps against 0.900× over §3a's 5 — so the
*throughput* loss at one group per writer core is smaller than §3a's number
suggests and sits close to parity. The direction is unchanged; the size was
noise-dominated, as §3a's overlapping ranges already warned.

### 3c. What the retraction found: D2's batch needs four committers, not two

`SHOW META` gained `wal_group_commits` / `wal_group_batches` /
`wal_mean_group_batch` so §3b's claim could be *seen* rather than inferred.
Pointing it at the claim killed it.

**This section was itself wrong once and is corrected in place.** Its first
version, from a two-point measurement, said D2 forms *no* batch at all.
Sweeping the concurrency says otherwise, and the sweep is the honest
instrument: one and two sessions was a boundary case, and stopping there
called a threshold an absence.

**The measurement.** One namespace's two relations, co-located on one core
by NS10; `peer_listeners = on` so sessions can be accepted on that core;
300 inserts per session, at 1, 2, 4 and 8 sessions committing at once. Same
core, same relations, same statement count, same class.

| sessions committing | commits | syncs | **mean batch** | commits/s |
|---|---|---|---|---|
| 1 | 300 | 301 | **1.000** | 693–804 |
| 2 | 600 | 600 | **1.000** | 804–818 |
| 4 | 1200 | 600 | **2.000** | 1069–1597 |
| 8 | 2400 | 600 | **~4.000** | 2839–2900 |

Reproduced on a peer core and on core 0 with the same shape. Two readings
matter:

- **The batch is `n/2` from four sessions upward, and flatly 1 below it.**
  D2 works, and the sync count is pinned from two sessions on: 600 syncs
  whether the core commits 600, 1,200 or 2,400 times.
- **Two concurrent committers on a core get none of it.** Batch 1.000 and
  818 commits/s against one session's 804 — a second session on a core buys
  essentially nothing, where a fourth doubles and an eighth quadruples.

**Why two is the boundary**, from the design: the reactor's post-task hook
syncs once per iteration, *after* the ready tasks (`Scheduler::SetPostTaskHook`),
and the sync runs on the reactor thread. A request that arrives during a
sync is polled on the next iteration — the one after the sync it just
missed. With two closed-loop clients that anti-phases into one commit per
iteration forever; with four or more, arrivals pile up during a sync and the
next iteration stages them together, which is exactly the `n/2` the table
shows.

**What this does to §3b.** The retraction stands, and it is the *specific*
mechanism that fails: §3b said `namespace` pays more because its cores have
one committing session where `rotate`'s have two. **Two sessions batch
exactly as badly as one** — 1.000 against 1.000 — so that cannot be the
difference between the arms. g7-c8's 2.35× is unexplained.

What the sweep rules in and out for a next attempt:

- **not** the batch size at one-versus-two sessions per core: measured
  equal;
- **is** sync-related, since `relaxed` collapses the gap to 1.02×;
- and the sweep *supports* §3b's replacement hypothesis without confirming
  it: a second session on a core adds 1.7% of throughput, which is the
  signature of a core that is idle waiting for a client round trip rather
  than busy. Confirming that needs per-core reactor utilisation during the
  load phase, which this probe does not collect.

**What survives.** `wal_mean_group_batch` earned its place twice over: it
falsified a mechanism, then corrected the falsification. `1.000` on a core
means that core's D2 is paying D1's device cost, and it is now visible
before a workload is designed around it.

---

## 4. What the engine looks like through these numbers

**The crossing costs ~62 µs on the p50 of a point join, and it is a
per-statement cost, not a per-row one.** At one group there is no
parallelism to win and every arm runs one session, so g1-c4 isolates the
crossing alone: `creating` p50 211.9 µs, `namespace` 273.2 µs — the same
join, one core-boundary hop, +61.3 µs. Rotation's p50 is 293.5 µs, +81.6 µs,
because its group is split and the *second* step crosses too. The
difference between those two deltas, ~20 µs, is what AF buys per crossed
step; the rest is the hop the group's owner needs whatever the policy.

**That is why `namespace` loses at one group and wins at two.** One group
pays the hop and has nothing to overlap it with (0.806×). Two groups pay the
same hop each and run on two cores at once, which is worth more than the hop
costs (1.122×). Three groups, three cores: 1.391×. The crossover is between
one and two groups, and it is a **statement about the session topology**, not
about the workload size — every session here dispatches from core 0, so the
hop is unavoidable the moment a relation leaves it.

**`namespace`'s margin over `rotate` is a function of one number: declared
groups per writer core.** The two arms differ in the catalog only in whether
a group's pair is together, so that margin *is* the price of splitting a
wired pair, and across the whole sweep it tracks contention rather than
crossing:

| groups per writer core | cells | `namespace` / `rotate` |
|---|---|---|
| 0.33 (1 group, 3 writer cores) | g1-c4 | 1.092× |
| 0.29–0.67 (2 groups) | g2-c8, g2-c4 | 1.104×, 1.057× |
| 0.43–1.00 (3 groups) | g3-c8, g3-c4, g3-c4′ | 0.989×, 1.053×, 1.014× |
| 1.00 (7 groups, 7 writer cores) | g7-c8 | **0.900×** |
| **2.33 (7 groups, 3 writer cores)** | **g7-c4** | **1.224×** |

Read down the last column and the story is not "namespace always wins": it
is **at or below parity wherever a group can have a core to itself, and it
pulls away only once cores are shared.** g7-c4 is the only cell in the sweep
where a core holds a second declared group, and it is the only cell where
the margin exceeds 10%. That is the case AF exists for — a machine with more
wired groups than cores — and it is also the case an operator will actually
be in.

**The tail is where the arms separate most consistently, and it is not
subtle.** `creating`'s p100 is **12.3 ms at two groups on four cores and
18.1 ms at eight** — against 566–1,088 µs for the other two arms. Two
sessions serialised behind one reactor produce a tail an order of magnitude
worse than two sessions on two reactors, and the medians barely show it
(323.3 against 301.4 µs). Anyone reading only the median would price
single-core placement as roughly free here; the p100 says it is not.

**And in the one cell where throughput cannot separate `namespace` from
`rotate`, the tail still does**: at g3-c8 the two medians are a tie, but
rotation's p100 is **3,373 µs against namespace's 752 µs**, 4.5×. Splitting
a wired pair does not cost throughput when there is spare core capacity; it
still costs the worst join in the run. `namespace`'s own 10.3 ms p100 at
g3-c4 is a single rep's outlier in the other direction — its p99 there is
488 µs against `creating`'s 775 µs — which is why p100 is reported rather
than leaned on.

**The write half moves the other way, and it is worth stating because the
probe measures reads.** The untimed load (4,000 inserts per group, shipped
to the owner) is *faster* under both spreading arms once there are two
groups — 6.6 s (`namespace`) and 7.0 s (`rotate`) against 9.8 s
(`creating`) at g2-c4, and 6.6 / 6.7 / 9.7 s at g2-c8 — because each group's
writes commit on their own core's WAL stream. At one group it is the
expected loss: 5.69 s against 5.05 s. Same crossover, same reason. At three
groups the advantage narrows to almost nothing (9.0 / 9.1 / 10.0 s at eight
cores), which this probe does not explain and does not try to.

---

## 5. What was not run, and what this does not claim

- ~~**The 7-group cells were queued and not run**~~ — **run 2026-09-02**,
  §3a. What is still not measured is anything **past 2.33 groups per writer
  core**: g7-c4 is the only cell where a core holds more than one declared
  group, so the shape of the curve beyond it is one point, not a curve. The
  two row-count variants at 7 groups (`--rows 200` and `--rows 10000`) were
  queued and never ran either, so nothing here separates the per-statement
  fixed cost from the per-row one at that sizing.
- ~~**g7-c8's load anomaly is unexplained**~~ — **diagnosed, §3b**: it is
  `group` durability's batch falling to one, and the durability A/B closes
  it (2.35× under `group`, 1.02× under `relaxed`). What is *not* measured is
  how far the effect reaches: only one cell in this sweep has a placement
  that leaves a core with a single committing session, so the shape of the
  cost against sessions-per-core is three points, not a curve.
- **This is a read measurement.** The load numbers in §4 are a by-product
  taken outside the timed window, not a write benchmark.
- **No foreign key is declared in the workload.** A cross-owner FK adds a
  probe round per write (`foreign-keys.md` §2a) and makes a parent `DELETE`
  refuse (§3a); this workload joins, it does not reference. AF-T4's `WARN`
  exists precisely because that combination is priced elsewhere.
- **One session per group, every session on core 0.** The peer-listener
  shape — a session accepted on the owner's core — would remove the hop
  entirely for the group it belongs to and is a different measurement.
- **The numbers are this host's.** Eight cores, one quiet box; every cell's
  three arms are interleaved rep by rep, so an ordering bias would have to
  be a within-rep one to survive.
- **The write half in §4 is a by-product with one arm's caveat.** At three
  groups the load times converge (10.0 / 9.1 / 9.0 s at eight cores), where
  at two groups they did not (9.7 / 6.7 / 6.6 s) — three loaders saturate
  something the two did not, and this probe does not say what.
