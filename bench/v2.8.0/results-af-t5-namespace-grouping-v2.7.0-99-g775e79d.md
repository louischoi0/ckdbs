# AF-T5 — namespace placement measured on a workload that actually crosses

Measured 2026-09-02 on `worktree-workorder-wf-te-t5` at
`v2.7.0-99-g775e79d`, `build-release` (`-DCMAKE_BUILD_TYPE=Release`),
`bench/af_namespace_grouping_probe.py`, three interleaved arms, 5 reps per
point, data file on a block device, `errors=0` in every run.

**The verdict, in one sentence, because AF-T5 asked for it either way:**
namespace placement **is** an answer to AE-8 rather than a convenience — it
beats single-core placement by **1.12× at two groups and 1.38-1.39× at
three**, and beats blind rotation by 5-10% in three of the four cells where
the two are separable — but it is **not free at one group, where it costs
19%**, and at eight cores with three groups it and rotation are a **tie on
throughput**, separated only by the tail. What it buys is parallelism
between declared groups, plus a p100 that does not blow out; it never buys
a cheaper single group.

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

**The last row is the one that keeps this honest.** At eight cores with
three groups the two spreading arms are a tie: 7,872 against 7,787 join/s,
ranges 7,754–7,946 and 7,756–7,919 — fully overlapping, so nothing here
resolves them. Rotation split all three pairs and used six cores to do it;
namespace used three and kept them together; the throughput is the same.
Read together with `g2-c8`'s 1.104×, that says the margin over rotation is
**not** a pure crossing cost — it is a contention effect that appears when
cores are scarce relative to the work and disappears when they are not.

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

**`namespace` beats `rotate` by 5–10% in four cells and ties in the fifth,
and the difference between those cases is contention, not crossing.** The
two arms differ in the catalog only in whether a group's pair is together,
so the margin is the price of splitting a wired pair — 1.09× at one group,
1.06× and 1.10× at two, 1.05× at three on four cores. At three groups on
*eight* cores it is 0.99×, a tie: rotation had six cores for six relations
and nothing was waiting behind anything, so the extra hop cost nothing that
the spare capacity did not absorb. The honest reading is that AF's
throughput advantage over blind rotation is real where cores are the scarce
resource and vanishes where they are not — and that the case AF actually
exists for, a machine with more declared groups than cores, is **not
measured here** (see §5).

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

- **The 7-group cells were queued and not run, and this is the gap that
  matters.** The sweep was stopped after five of nine cells — `g7-c4`,
  `g7-c8` and two row-count variants at 7 groups never ran. Every cell in
  this file therefore has **at most one group per writer core**, which is
  precisely the side of `bench/v2.1.0` §6's step where rotation still wins;
  the point where a core takes a second group — where §6 found blind
  rotation collapsing on the *unwired* workload, and where AF's grouping
  argument should matter most — is **unmeasured**. Nothing here says what
  happens past it, and the g3-c8 tie is a warning that the answer is not
  obvious. Re-running `bench/af_namespace_grouping_probe.py --groups 7` at
  4 and 8 cores is the owed cell.
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
