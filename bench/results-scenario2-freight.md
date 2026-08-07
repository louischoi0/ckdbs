# What a booking costs, and what a transaction buys

Measurements for `docs/scenario2-freight.md` (`S2-02`). Driver:
`tools/scenario2_freight.py`. **This is not `S2-06`.** One booker in one
process, so nothing contends and nothing conflicts; the numbers below price
the *transaction*, not the workload. The contended run, the reporter and the
PostgreSQL twin are `S2-03` through `S2-06`.

## Device, build and configuration

**A block device, not tmpfs.** `/tmp` on this machine is tmpfs, and
`bench/results-cabin.md` records what measuring a write workload there does
to the answer: with fsync free the numbers describe a machine nobody runs.
Every run below is on the xfs volume.

| | |
|---|---|
| device | `/dev/nvme0n1p1` — Amazon EBS gp3, non-rotational, 8 GB, xfs |
| build | `-DCMAKE_BUILD_TYPE=Release` (`-O3 -DNDEBUG`), gcc 11.5.0 |
| kernel | 6.18.38-73.137.amzn2023.x86_64 |
| server | `cores = 1`, durability `group` (default) |
| client | one connection, one booker process |
| scale | 200 organizations, 40 ships, 400 voyages, 5,000 cargos |
| work | `--bookings 1500`, `--seed 1`, identical in every run |
| fresh | a new server **and a new data file per configuration** |

Latencies include the Python client's socket cost, which on a release build
is most of a small statement — a pk lookup is ~130 µs end to end, of which
the engine is a small part. Read differences under ~20 µs on the per-
statement rows as noise, and see "the noise floor" below for the throughput
rows.

Every configuration wrote the same thing: 1,500 freights and 8,425 charge
rows, 5.6 fees per booking. Every one passed `--verify 25` — 100 invariant
checks, 0 failures.

## The matrix

One knob at a time against the baseline (`BEGIN`/`COMMIT`, `--capacity-mode
cached`, no `--fk`, no `--cabin`, server-default isolation).

| configuration | TPS | vs baseline | what changed |
|---|---:|---:|---|
| **baseline** | **273.9 / 281.0** | — | measured twice, two fresh files |
| `--capacity-mode scan` | 262.3 / 275.8 | −2 to −4% | capacity re-derived by `SUM` over the ledger |
| `--fk` | 272.5 | −0.5% | three foreign keys declared |
| `--cabin` | 279.8 | +2.2% | Cabin on `recipes.cargo_type` |
| `--isolation repeatable-read` | 279.6 | +2.1% | one read view per transaction |
| **`--no-txn`** | **77.3** | **−72%** | eight statements, eight transactions |

### The noise floor is ±2%

The `repeatable-read` run is an accidental control and the most useful row in
the table. RR changes *when a read view is taken*; on a single connection with
no concurrent writer it cannot change what any statement reads or does. It
came out **+2.1%**. So +2.2% for the Cabin is not a Cabin result, and −0.5%
for the foreign keys is not an FK result — both are inside the same band, and
this workload as configured cannot resolve either. What it can resolve is
`--no-txn`, which is off the scale.

## The finding: a transaction is a 3.5× speedup, not a tax

Autocommit is the slowest configuration by a factor of 3.5, and the
per-statement means say exactly why:

| statement | baseline | `--no-txn` | ratio |
|---|---:|---:|---:|
| cargo-lookup | 145 µs | 205 µs | 1.4× |
| credit-lookup | 134 µs | 160 µs | 1.2× |
| capacity-read | 133 µs | 134 µs | 1.0× |
| recipe-read | 157 µs | 173 µs | 1.1× |
| **freight-insert** | **130 µs** | **1,518 µs** | **11.7×** |
| **charge-insert** | **110 µs** | **1,357 µs** | **12.3×** |
| **operation-update** | **117 µs** | **1,405 µs** | **12.0×** |
| **org-update** | **114 µs** | **1,491 µs** | **13.1×** |
| commit | 1,811 µs | — | |
| **booking (whole)** | **3,635 µs** | **12,893 µs** | 3.5× |

The four reads are unchanged. Every *write* is 12× slower, because under
autocommit each one is its own transaction and therefore its own durability
point. Under `BEGIN`/`COMMIT` the booking pays **one** — the 1,811 µs
`COMMIT` — and amortizes it over ~9.6 writes (one freight, 5.6 charges, two
updates).

That is half the booking. The whole eight-statement read/write body costs
about 1.5 ms and the durability point costs 1.8 ms, so on this engine, at
this durability class, **the transaction boundary is the dominant cost of a
booking and the statements inside it are the cheap part.** It also inverts
the usual framing of S2-2: explicit transactions were chosen for
*correctness*, and they turn out to be the fast path as well. The correctness
argument does not need the speed, but nothing here trades against it.

## `--capacity-mode`: the derived column buys 2–4%, at this size

`operations.booked_cbm` exists so the capacity check is a pk lookup instead
of an aggregate over the freight ledger. It works as intended per statement —

| | baseline (`cached`) | `scan` |
|---|---:|---:|
| capacity-read | 133 µs | 248 µs (**+87%**) |
| booking | 3,635 µs | 3,796 µs (+4.4%) |
| TPS | 273.9 / 281.0 | 262.3 / 275.8 |

— and buys 2–4% of throughput, because the read it replaces is 4% of a
booking dominated by its commit.

**This number does not generalize, and the shape of the workload says which
way it moves.** The `scan` read is a `FilterScan` over the whole `freights`
relation, so its cost grows with the ledger while the `cached` read stays one
descent. At 1,500 freights the relation is a handful of pages; at 100,000 it
is not. Read +87% as a floor measured on a nearly empty ledger, and expect
the gap to widen roughly linearly with rows booked. `S2-06` should measure it
at two scales rather than one, which is the only way the derived column's
maintenance cost — one more column in two `UPDATE`s — can be priced against
what it saves.

## What the file grew, and why 71% of it is Waystone

Pages persisted at clean shutdown, same 1,500 bookings, same writes:

| configuration | pages | file | data pages | Waystone pages |
|---|---:|---:|---:|---:|
| `cached` | 792 | 7.86 MB | 230 | **562** |
| `scan` | 434 | 4.72 MB | 230 | **204** |
| `cached`, `waystone_recording = off` | **230** | 1.84 MB | 230 | 0 |
| `scan`, `waystone_recording = off` | **230** | 1.84 MB | 230 | 0 |

Both reproduce exactly across repeated runs. The two modes write **identical
rows** — 1,500 freights, 8,425 charges, 1,500 pairs of updates — so all 562
and 204 extra pages are Waystone state, and switching recording off collapses
both to the same 230.

The mechanism is the step-kind trust table doing precisely what it is
specified to do. In `cached` mode the capacity read is `SELECT booked_cbm
FROM operations WHERE id = <n>` — a pk equality, **lookup-class**, therefore
trail-replayable and therefore recorded, against 400 voyages drawn
repeatedly, which is exactly the "same instance seen twice" that `n = 2`
recording is waiting for. In `scan` mode the same check is `SELECT SUM(cbm)
FROM freights WHERE operation_id = <n>` — **search-class**, never recorded.
The other two lookups behave as the model predicts too: `organizations` has
200 rows probed over and over and earns trails in both modes, while `cargos`
is drawn without replacement, so no cargo id is ever seen twice and no trail
is ever recorded for it.

**Recording cost no measurable throughput** — 281.0 TPS with it off against
273.9 / 281.0 with it on, inside the ±2% band. So on this workload Waystone
is free in time and expensive in space: **4.6 MB of trail state against
1.8 MB of data, a 3.4× multiplier on the data file**, for a run of 1,500
transactions that never replayed a trail often enough to show up in the
timing.

This is the first measured number for an open decision that has been open
since the feature landed — `docs/waystone-concpets.md` §9's retention and
eviction, and workplan items P15–P17, "retention, decay and epoch bump sites
do not [exist]". Nothing bounds instances per pattern, and this run shows
what unbounded means with a 400-value argument space on one relation. Two
things follow for whoever picks that up:

1. **The cost is per *distinct argument*, not per statement.** 5,000 cargo
   lookups recorded nothing; 400 voyages recorded 562 pages. The axis to
   bound is instance cardinality.
2. **A physical-design choice moves it.** Adding the derived column changed
   the capacity check from search-class to lookup-class, and that alone
   tripled the data file. Nothing about the schema decision hinted at it.

## Rejections are not exercised here

Every run above ended with 2 capacity rejections and 1 credit rejection out
of ~1,503 attempts. That is not a finding about the limits — it is the work
target stopping the run at 1,500 commits before accumulation binds, at a
scale where the fleet and the credit book are sized to 5,000 cargos.

The rejection axes do work, and shorter runs against a smaller pool show all
three outcomes together (1,705 committed / 111 capacity / 295 credit at 20
organizations and 200 voyages). Pricing rejection properly — how much a
rolled-back read-only prefix costs against a committed booking — belongs with
`S2-03`, where the same accounting has to separate a rejection from a
conflict.

## What this run does not answer

- **Whether `--txn` and `--no-txn` differ in *correctness* here.** They do not
  and cannot: one booker has no second writer, so `--no-txn` passed all 100
  invariant checks. The contrast §4 is written for needs `S2-03`.
- **What a conflict costs.** Zero occurred, by construction.
- **The Cabin and the foreign keys.** Both are inside the noise band at this
  scale. The Cabin served 1,496 probes against 8 misses over 8 observed
  values — the structure is doing its job; the recipe read is simply 4% of a
  booking, so serving it perfectly cannot show up. A workload that reads
  recipes more than it commits would be needed to price it.
- **Anything about more than one core.** `cores = 1` throughout.
