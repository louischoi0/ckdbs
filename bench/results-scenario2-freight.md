# Where a freight booking spends its time

**Half of it waits for one fsync.** That is the finding this document exists
to support, and most of what follows is the evidence for it plus what it
implies for the two engines being compared. A booking on KDS is eight
statements, four of them reads and four of them writes, and every one of
those eight is *faster* than PostgreSQL's equivalent — yet KDS finishes only
11% ahead, because it spends 50% of a booking in its commit where PostgreSQL
spends 28% in its own.

The workload is `docs/scenario2-freight.md`'s freight and cargo book, driven
by `tools/scenario2_freight.py`. How to run it: `bench/docs/README.md`.

## The run

| | |
|---|---|
| executed | **2026-08-07 00:35:40 → 00:38:58 UTC** |
| branch | `feat-additional-types` |
| repository HEAD | `e4f15e1` — tree clean |
| **binary measured** | `build-release/kds_server`, built 2026-08-06 23:47:41 UTC, i.e. the tree that became **`8e0f8d5`**. HEAD's later commits (`ccb0521`, `a32c1f6`, `e4f15e1`) include engine source that **is not in the measured binary** |
| device | `/dev/nvme0n1p1` — Amazon EBS gp3, non-rotational, 8 GB, xfs. Not tmpfs |
| build | `-DCMAKE_BUILD_TYPE=Release` (`-O3 -DNDEBUG`), gcc 11.5.0 |
| kernel | 6.18.38-73.137.amzn2023.x86_64 |
| KDS server | `cores = 1`, durability `group`, all other keys default |
| PostgreSQL | 17, scratch cluster on port 15433, **default tuning**, `synchronous_commit = on` |
| client | one connection per engine, one booker process, Python driver |
| scale | 200 organizations, 40 ships, 400 voyages, 5,000 cargos |
| work | `--bookings 1500 --seed 1 --verify 25`, identical in every configuration |
| isolation | fresh server **and** fresh data file per KDS configuration |

Every configuration below committed exactly 1,500 bookings and wrote exactly
8,425 charge rows — 5.617 fees per booking — and every one passed
`--verify 25`: 100 invariant checks, 0 failures, on both engines.

**The machine must be quiet.** An earlier attempt at this matrix ran while
four `cc1plus` processes held the load average at 5.3, and it reported 91.7
and 122.9 TPS for two runs of the *same* configuration — a 34% spread and a
3× error against the quiet-machine number. Nothing in the driver's output
hinted at it. `uptime` before the run is not optional.

### The noise floor is ±3.2%

Established from inside the matrix rather than assumed.
`--isolation repeatable-read` changes *when* a read view is taken; on a
single connection with no concurrent writer it cannot change what any
statement reads or writes, so its true effect is zero. It measured **+3.2%**.
A second baseline run measured +1.6%. Nothing smaller than ±3.2% is reported
below as a result.

## The unit: what a booking is

One cargo placed on one voyage, inside one transaction:

```
BEGIN
  SELECT ... FROM cargos        WHERE id = <cargo>      pk lookup
  SELECT ... FROM organizations WHERE id = <org>        pk lookup
  SELECT booked_cbm FROM operations WHERE id = <op>     pk lookup   ) --capacity-mode
     or SELECT SUM(cbm) FROM freights WHERE operation_id = <op>     )
  SELECT ... FROM recipes WHERE cargo_type = <t>        non-pk equality
  -- two client-side checks: voyage capacity, customer credit
  INSERT INTO freights ...                              1 row
  INSERT INTO charges  ...                              5.617 rows on average
  UPDATE operations    SET booked_cbm, revenue          btree pk overwrite
  UPDATE organizations SET outstanding                  btree pk overwrite
COMMIT
```

There is no server-side expression that could make those two checks — KDS has
no arithmetic in a select list and no `CHECK` constraint — so they run in the
client, between the reads and the writes. What the engine supplies is that
the read the check was made against and the write the check authorised are
one atomic unit, and `--no-txn` is what prices that guarantee.

## Where the time goes: the wait breakdown

Every wait below is measured client-side as a statement's round trip, so each
figure includes the socket and the Python driver. That overhead is the same
on both engines and cannot be subtracted, only acknowledged. KDS exposes no
server-side wait-event instrumentation, so *within* a statement the split
between page I/O, latch and CPU is not visible from here; what is visible is
which statement class the booking waits in, which is the question that
matters at this level.

| wait type | KDS µs | share | PostgreSQL µs | share |
|---|---:|---:|---:|---:|
| **durability wait** (`COMMIT`, one fsync) | **1,836** | **50.1%** | **1,135** | **27.9%** |
| write-statement wait (1 + 5.617 inserts, 2 updates) | 967 | 26.4% | 1,717 | 42.2% |
| read wait (4 statements) | 598 | 16.3% | 967 | 23.7% |
| client, framing and `BEGIN` (residual) | 262 | 7.1% | 253 | 6.2% |
| **whole booking** | **3,663** | 100% | **4,072** | 100% |

Two engines, two different shapes. On KDS the durability point is the single
largest component of a business transaction and the nine statements inside it
together cost less than it does. On PostgreSQL the writes dominate and the
commit is a quarter. Neither is a defect — they are different placements of
the same total — but they say where each engine's next win is: for KDS, in
the WAL flush path; for PostgreSQL, in the writes.

**Conflict wait is zero and structurally so.** One booker means no second
writer, so first-updater-wins never fires. Lock wait does not exist in this
engine at all — there is no lock manager and no waiting, by design
(`docs/txn.md`): a write conflict is an immediate retryable error, not a
queue. Both are exercised by `S2-03`, not here.

## Per-statement distributions

KDS, baseline configuration. `charge-insert` has 8,425 operations because a
booking writes 5.617 of them; every other row is once per booking.

| statement | ops | mean | p0 | p25 | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|---:|---:|
| cargo-lookup | 1,503 | 149 | 83 | 128 | 136 | 181 | 230 |
| credit-lookup | 1,503 | 134 | 77 | 122 | 127 | 168 | 224 |
| capacity-read | 1,503 | 129 | 74 | 114 | 124 | 167 | 219 |
| recipe-read | 1,503 | 186 | 95 | 130 | 145 | 188 | 245 |
| freight-insert | 1,500 | 124 | 68 | 91 | 115 | 158 | 230 |
| charge-insert | 8,425 | 109 | 58 | 84 | 109 | 150 | 184 |
| operation-update | 1,500 | 114 | 71 | 87 | 117 | 158 | 189 |
| org-update | 1,500 | 117 | 69 | 86 | 116 | 160 | 206 |
| **commit** | 1,500 | **1,836** | **918** | **1,046** | **2,098** | 2,361 | **3,935** |
| whole booking | 1,503 | 3,663 | 865 | 2,880 | 3,727 | 4,568 | 7,896 |

*(µs, one connection, latencies include the client's socket cost)*

Three things in that table are worth more than the means.

**The commit is bimodal.** p25 is 1,046 µs and p50 is 2,098 µs — the
distribution has two populations roughly a millisecond apart, and the p99 at
3,935 µs is nearly 4× the p0. A fast commit costs about what one EBS fsync
costs; a slow one costs about two. Every other statement in the table is
tight — p99 within 2× of p0 — so this variance belongs to the durability path
specifically and not to the machine. Since half a booking *is* this number,
its variance is most of the booking's variance too (p99 7,896 µs against a
p50 of 3,727).

**The reads and the writes cost the same.** A pk lookup is 129–186 µs and an
insert or an update is 109–124 µs, which on a client-measured round trip
means both are dominated by the round trip. The engine's own work is below
this driver's resolution for every statement except the commit. That is a
statement about the measurement, not about the engine, and it is the reason
the wait table above is the honest unit of analysis rather than these rows.

**p0 of the whole booking, 865 µs, is not a fast booking.** It is a
*rejected* one: three of the 1,503 attempts failed their capacity or credit
check, and a rejection pays the four reads and a rollback but none of the
writes and no durability point. So a refusal costs roughly a quarter of an
acceptance — which is what makes the rejection path cheap enough that a run
with a high refusal rate is not automatically a slow one.

## Versus PostgreSQL

Same booking, same seed, same work target, same machine, same device.

| statement | KDS mean | PG mean | KDS p50 | PG p50 | KDS p99 | PG p99 |
|---|---:|---:|---:|---:|---:|---:|
| cargo-lookup | **149** | 230 | 136 | 223 | 230 | 396 |
| credit-lookup | **134** | 218 | 127 | 209 | 224 | 344 |
| capacity-read | **129** | 214 | 124 | 204 | 219 | 411 |
| recipe-read | **186** | 305 | 145 | 294 | 245 | 501 |
| freight-insert | **124** | 242 | 115 | 231 | 230 | 385 |
| charge-insert | **109** | 186 | 109 | 176 | 184 | 305 |
| operation-update | **114** | 219 | 117 | 210 | 189 | 346 |
| org-update | **117** | 211 | 116 | 200 | 206 | 326 |
| commit | 1,836 | **1,135** | 2,098 | **1,122** | 3,935 | **1,390** |
| whole booking | **3,663** | 4,072 | 3,727 | 4,034 | 7,896 | 5,132 |
| **TPS** | **271.9** | 244.6 | | | | |

KDS is **1.5× to 1.9× faster on every one of the eight statements** and
**1.6× slower on the commit**, and nets **+11.2% on throughput**. Much of the
per-statement gap is protocol rather than engine — KDS's newline text
protocol is a lighter round trip than PostgreSQL's v3 wire — which is exactly
why the per-statement rows should not be read as an engine comparison. The
commit row can be: both engines fsync a write-ahead log to the same device
under the same durability promise, and PostgreSQL's flush path is both faster
in the middle and dramatically tighter at the tail (p99 1,390 µs against
3,935 µs, a 2.8× difference in worst case).

**The tail is the sharper result.** At p99 PostgreSQL finishes a booking in
5,132 µs against KDS's 7,896 µs — the one place in this comparison where
PostgreSQL wins the whole unit, and it wins it entirely on commit variance.
An engine that is faster at everything and slower at the tail of its
durability path has a well-localised problem.

## The options matrix

One knob at a time against the baseline. Equal work in every row: 1,500
committed bookings, not equal time.

| # | configuration | KDS TPS | vs base | PG TPS | vs base |
|---|---|---:|---:|---:|---:|
| 1 | **baseline** — `BEGIN`/`COMMIT`, `--capacity-mode cached` | **271.9** | — | **244.6** | — |
| 2 | baseline, repeated (fresh file) | 276.4 | +1.6% | — | |
| 3 | `--capacity-mode scan` | 265.6 | −2.3% | 244.4 | −0.1% |
| 4 | `--fk` — three foreign keys declared | 272.3 | +0.1% | — | |
| 5 | `--cabin` — Cabin on `recipes.cargo_type` | 279.5 | +2.8% | — | |
| 6 | `--isolation repeatable-read` *(control)* | 280.5 | +3.2% | — | |
| 7 | `waystone_recording = off` | 279.7 | +2.9% | — | |
| 8 | **`--no-txn`** — eight autocommitted statements | **83.5** | **−69.3%** | **88.2** | **−63.9%** |

Rows 4 through 7 are all inside the ±3.2% floor that row 6 establishes. The
foreign keys, the Cabin and Waystone recording each cost or save **nothing
this workload can resolve** — that is the honest reading, and reporting +2.8%
for the Cabin as a Cabin result would be reporting the noise.

Row 8 is the one that is not close.

### Autocommit costs 3.3×, and the per-statement table says why

| statement | baseline | `--no-txn` | ratio |
|---|---:|---:|---:|
| cargo-lookup | 149 | 172 | 1.2× |
| credit-lookup | 134 | 142 | 1.1× |
| capacity-read | 129 | 140 | 1.1× |
| recipe-read | 186 | 158 | 0.8× |
| **freight-insert** | **124** | **1,380** | **11.1×** |
| **charge-insert** | **109** | **1,265** | **11.6×** |
| **operation-update** | **114** | **1,304** | **11.4×** |
| **org-update** | **117** | **1,390** | **11.9×** |
| whole booking | 3,663 | 11,948 | 3.3× |

The four reads are unchanged. Every write is ~11.5× slower, because under
autocommit each one is its own transaction and therefore its own fsync. The
wait profile inverts completely: writes go from 26.4% of a booking to
**93.6%**, and every write statement's p0 lands near 900 µs — the floor of a
durability point, now paid nine times instead of once.

PostgreSQL behaves the same way for the same reason (88.2 TPS, −64%), which
is the useful part: this is not a KDS artifact but the cost of the guarantee,
and both engines amortise it identically when given a transaction to amortise
it into.

The consequence for `docs/scenario2-freight.md`'s decision S2-2 is worth
stating plainly. Explicit transactions were chosen there for **correctness**
— eight statements that must be one unit. They are also, on this workload, a
**3.3× throughput win**. The correctness argument never needed the speed, and
nothing trades against it.

## What the derived column is actually substituting for

`operations.booked_cbm` is a running total maintained by every booking so the
capacity check can be a pk lookup instead of an aggregate over the freight
ledger. It is the one place this schema deliberately stores a derived value.

| | KDS `cached` | KDS `scan` | PG `cached` | PG `scan` |
|---|---:|---:|---:|---:|
| capacity-read mean | 129 µs | **239 µs** (+85%) | 214 µs | 234 µs (+9%) |
| capacity-read p99 | 219 µs | 428 µs | 411 µs | 352 µs |
| whole booking | 3,663 µs | 3,750 µs | 4,072 µs | 4,075 µs |
| TPS | 271.9 | 265.6 (−2.3%) | 244.6 | 244.4 (−0.1%) |

**On PostgreSQL the derived column buys nothing, because PostgreSQL has an
index on `freights(operation_id)` and the `SUM` is an index scan. On KDS it
buys 85% of that statement, because the alternative is a `FilterScan` — a
walk of the whole relation.** The derived column is not really compensating
for the absence of an aggregate; it is compensating for the absence of a
secondary index.

That reframes what the number means. −2.3% is small here only because the
ledger holds 1,500 rows and the walk is a few pages; the `scan` side grows
with the relation while the `cached` side stays one descent. Read +85% as a
floor measured on a nearly empty ledger. It also says where the engine-side
fix lies: a secondary index on a non-pk column, or a Cabin doing that job,
removes the reason to denormalise at all — and a denormalised counter is
precisely the thing that makes the transaction have two updates instead of
one, and therefore two conflict axes instead of one.

## The data file is mostly Waystone

Pages persisted at clean shutdown. Identical writes in every row: 1,500
freights, 8,425 charges, 3,000 updates.

| configuration | pages | file | data pages | trail pages |
|---|---:|---:|---:|---:|
| baseline (`cached`) | 792 | 7.86 MB | 230 | **562** |
| `--capacity-mode scan` | 434 | 4.72 MB | 230 | **204** |
| `waystone_recording = off` | **230** | 1.84 MB | 230 | 0 |

Deterministic — every count above reproduces exactly across repeated runs.
Switching recording off collapses both modes to the same 230 pages, so all
562 and 204 extra pages are Waystone state: **4.6 MB of trail against 1.8 MB
of data, a 3.4× multiplier on the data file**, for a run of 1,500
transactions. Recording cost no measurable throughput (row 7 above, +2.9%,
inside the noise floor), so on this workload Waystone is free in time and
expensive in space.

The mechanism is the step-kind trust table doing exactly what it is specified
to do, and the three relations demonstrate all three cases:

- `operations` — probed by `WHERE id = <n>` over **400 voyages drawn
  repeatedly**. A pk equality is lookup-class, therefore trail-replayable,
  therefore recorded once the same instance is seen twice. 562 pages.
- `cargos` — probed by pk over **5,000 cargos drawn without replacement**. No
  cargo id is ever seen twice, `n = 2` never fires, and **nothing is recorded
  at all**.
- `freights` in `scan` mode — `WHERE operation_id = <n>` is search-class and
  is never recorded, which is why that configuration carries 204 pages
  instead of 562.

So the cost axis is **distinct arguments per pattern**, not statements
executed. 5,000 lookups recorded nothing; 400 repeated ones recorded 562
pages.

This is the first measured input to an open decision that has been open since
the feature shipped — `docs/waystone-concpets.md` §9's retention and
eviction, and workplan items P15–P17. Nothing bounds instances per pattern
today. Two things follow for whoever settles it. The bound wants to be on
instance cardinality rather than on traffic. And **a physical-design choice
moves trail volume by 2.75× without mentioning Waystone**: adding
`booked_cbm` changed the capacity check from search-class to lookup-class,
and tripled the file. Nothing in that schema decision looks like a Waystone
decision.

## Concurrency: throughput rises, and READ COMMITTED loses updates

Everything above runs one booker. With several, two results appear that a
single connection cannot produce — one about speed, one about correctness,
and the second is the more important.

| | |
|---|---|
| executed | **2026-08-07 02:04:08 → 02:06:32 UTC** |
| branch / HEAD | `feat-additional-types` / `738fe6e` |
| binary | unchanged — the same 23:47:41 build measured throughout |
| work | 1,500 committed bookings per run, `--seed 1`, fresh server and file each |
| noise | the two one-booker rows differ by 12%, so single-connection rows here are indicative only; the multi-booker rows were taken back to back |

`--contend` shares the two rows a booking updates — the voyage and the
customer. `--no-contend` gives each booker a disjoint slice of **both**
voyages and customers, so no conflict is possible. Cargos are split in either
mode: a cargo ships once, and two bookers holding the same one would be a
driver defect rather than contention.

| bookers | mode | isolation | TPS | conflicts | ops / orgs | **invariant failures** |
|---:|---|---|---:|---:|---|---:|
| 1 | partitioned | RC | 234.1 | 0 | — | 0 |
| 1 | contended | RC | 263.2 | 0 | — | 0 |
| 2 | partitioned | RC | 341.3 | 0 | — | 0 |
| 2 | contended | RC | 359.9 | 1 | 0 / 1 | **1** |
| 4 | partitioned | RC | 404.5 | 0 | — | 0 |
| 4 | contended | RC | 404.4 | 3 | 2 / 1 | **2** |
| 8 | partitioned | RC | 437.1 | 0 | — | 0 |
| 8 | contended | RC | 430.5 | 4 | 3 / 1 | **3** |
| 4 | contended | **RR** | 397.7 | 36 | 17 / 19 | **0** |
| 8 | contended | **RR** | 421.3 | 80 | 25 / 55 | **0** |

*(100 invariant checks per run; RC = READ COMMITTED, RR = REPEATABLE READ)*

### Group commit is real, and it is worth 1.9×

Throughput rises from 234 to 437 TPS as bookers go 1 to 8 — on a server that
dispatches every statement on **one thread**, where no statement executes in
parallel with any other. The engine is not doing more work per second; it is
doing fewer fsyncs per booking. Durability class `group` batches the commits
of concurrently open transactions into one flush, and `CLAUDE.md` records the
consequence of never having tested it with more than one connection: "802
inserts/s strict, 798 group (a batch of one is a batch)". With eight
connections a batch is finally a batch.

The latency table shows the trade exactly:

| bookers | booking p0 | p25 | p50 | p95 | p99 | commit p50 | commit p99 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 878 | 2,971 | 3,840 | 7,376 | 11,575 | 2,122 | 6,258 |
| 2 | 2,533 | 4,990 | 5,346 | 9,061 | 12,406 | 2,131 | 4,570 |
| 4 | 1,302 | 8,402 | 9,553 | 13,286 | 15,978 | 2,218 | 4,489 |
| 8 | 4,031 | 15,760 | 17,805 | 23,627 | 29,360 | 2,608 | 5,232 |

*(µs, partitioned, RC)*

A booking's p50 grows 4.6× while throughput grows 1.9× — the queue in front
of a single-threaded dispatcher, priced. **The commit itself barely moves**:
2,122 µs at one booker and 2,608 µs at eight, +23% while eight times the work
flows through it. That is the whole mechanism in one row — the flush is being
shared, so per-booking durability cost falls even as each booking waits
longer to be served.

### READ COMMITTED loses updates, silently, and the checker catches it

This is the first time `--verify` has failed a run in this scenario, and it
fails for a correct reason:

```
I1 operation 192: booked_cbm=993000, SUM(freights.cbm)=1362000
I3 organization 7: outstanding=14360787, recomputed=14652399
```

Voyage 192 is carrying 1,362,000 milli-m³ of freight and its capacity counter
says 993,000. The rows are all there; the counter that the capacity check
reads is short by 369,000. Two bookers read the same `booked_cbm`, each added
its own cargo, and each wrote back — the second overwrote the first. The
capacity limit is now being enforced against a number that under-reports the
ship's load, which is exactly the failure this workload was built to be able
to detect.

**The engine is not misbehaving.** `docs/txn.md` specifies first-updater-wins
with no waiting: an `UPDATE` is refused when its target was written by a
*concurrent uncommitted* transaction. Two read-modify-write transactions that
overlap in time but commit in sequence are not that case, and under READ
COMMITTED each statement takes a fresh read view, so the second transaction's
read simply happened before the first one's write. This is the classic
lost-update hazard of RC, and PostgreSQL at RC has it too.

What is specific to KDS is **how little the workload can do about it**. There
is no `SELECT ... FOR UPDATE`, and `UPDATE ... SET c = c + n` is not
expressible — the grammar takes a literal on the right-hand side
(`docs/client-manual.md`). So a running total cannot be incremented
atomically at any isolation level; it can only be read, computed client-side,
and written back. That leaves exactly one remedy available today, and it
works:

| 4 bookers, contended | RC | RR |
|---|---:|---:|
| conflicts raised | 3 | **36** |
| conflicts on `operations` / `organizations` | 2 / 1 | 17 / 19 |
| invariant failures | **2** | **0** |
| TPS | 404.4 | 397.7 (**−1.7%**) |

REPEATABLE READ fixes the transaction's read view at `BEGIN`, so a row
written by anyone after that point makes the update conflict rather than
overwrite. The losses become 36 retryable errors, every one retried and
committed, and the invariants hold — for 1.7%. At eight bookers the same
holds at −2.1% (80 conflicts, 0 failures).

Note what the conflict counts say about the two axes. At RC the few conflicts
that surface skew to `operations` (3 of 4 at eight bookers); under RR, where
they are actually being detected, the balance inverts to `organizations`
(55 of 80). 400 voyages against 200 customers means a customer is twice as
likely to be shared, and only RR is sensitive enough to show it. **A conflict
count that is not detecting everything is not just smaller — it is skewed**,
and a per-axis split taken at RC would have pointed capacity work at the
wrong relation.

### What this means for the engine

Three things, in descending order of how much they should change someone's
plans.

1. **A read-modify-write of one column has no correct spelling in this
   engine at the default isolation level.** RR is a remedy, but it is a
   heavier promise than the workload needs and it converts a data-loss bug
   into a retry rate that grows with contention. The narrower fix — an atomic
   `SET c = c + n`, or row locking — is a `docs/txn.md` decision, and this
   run is the argument for having it.
2. **`group` durability has never been exercised until now**, and it is worth
   1.9× at eight connections. Any benchmark of this engine taken on one
   connection understates commit-bound throughput by that factor, including
   every other number in this document.
3. **Cross-core is not what this workload needs next.** Eight bookers already
   saturate a single-threaded dispatcher at 437 TPS with the commit shared
   eight ways; the queue, not the core count, is what p50 is made of.

## A second core buys nothing, and costs an extent and a WAL stream

`cores` is pinned into the superblock when a database is created and
validated at every mount, so each value needs its own data file. This
comparison is six runs interleaved `1, 2, 1, 2` in one two-minute window, on
the same binary as every table above.

| | |
|---|---|
| executed | **2026-08-07 01:18:48 → 01:20:48 UTC** |
| branch / HEAD | `feat-additional-types` / `738fe6e` |
| binary | unchanged — the same 23:47:41 build measured throughout this document |
| machine | 2 physical cores (`nproc = 2`), so `cores = 2` is the ceiling the server will accept |
| noise floor | **±0.5%** — the two `cores = 1` runs differ by 0.18%, the two `cores = 2` runs by 0.5% |

| configuration | TPS | vs `cores = 1` | server CPU | pages |
|---|---:|---:|---:|---:|
| `cores = 1`, run A | 281.1 | — | 2.47 s | 792 |
| `cores = 1`, run B | 280.6 | — | 2.46 s | 792 |
| `cores = 2`, run A | 280.9 | −0.1% | 2.66 s | 856 |
| `cores = 2`, run B | 279.5 | −0.4% | 2.65 s | 856 |
| `cores = 1`, `--no-txn` | 85.0 | — | 3.03 s | 792 |
| `cores = 2`, `--no-txn` | 84.9 | −0.1% | 3.40 s | 856 |

Throughput is unchanged inside the floor, in both the transactional and the
autocommit configuration. The per-statement distributions say the same thing
— nothing moves outside its own run-to-run variation:

| statement | `cores = 1` p50 | `cores = 2` p50 | `cores = 1` p99 | `cores = 2` p99 |
|---|---:|---:|---:|---:|
| cargo-lookup | 136 | 136 | 212 | 231 |
| recipe-read | 145 | 148 | 230 | 270 |
| freight-insert | 117 | 119 | 181 | 197 |
| org-update | 116 | 119 | 198 | 195 |
| commit | 2,087 | 2,081 | 2,677 | 2,483 |
| whole booking | 3,785 | 3,786 | 4,701 | 4,874 |

*(µs, 1,500 bookings each, 8,425 charge rows each)*

The wait breakdown is likewise flat — durability 49.6% of a booking at one
core and 48.6% at two — which is the expected result and worth stating as
such: `kds.conf.sample` says core 0 still serves every statement, that a peer
cannot resolve a relation until the per-core catalog work of `P6` lands, and
that raising `cores` today "buys parallel WAL streams and nothing else". The
measurement agrees with the documentation.

What it does cost is visible in the two right-hand columns:

- **+0.19 s of server CPU** for the same 1,500 bookings, +7.7%. Over a ~5.3 s
  run that is about 3.6% of one core, which tells you the idle peer reactor
  **parks rather than busy-loops** — a pinned never-blocking reactor spinning
  flat out would have shown ~5 s. Under `--no-txn` the gap is +12.2%, on a
  longer run, in the same proportion.
- **+64 pages of data file**, exactly `storage::kDefaultExtentPages`. That is
  the peer core's page-id lease: a leased store takes a run of ids up front
  and never touches the free map, so a second core costs one extent whether
  or not it ever writes to it.
- **+64 MiB of WAL.** Each core preallocates its own segment
  (`wal-<core>-0.log`) with `posix_fallocate` at startup, so `cores = N`
  reserves N × 64 MiB before serving anything.

### The WAL preallocation is a hard startup dependency

That last cost is not a footnote. A first attempt at this comparison ran with
130 MB free on the volume, and `cores = 2` **failed to start**:

```
server stopped: FileLogDevice: posix_fallocate on
  /home/ec2-user/s2cores.db.wal/wal-1-0.log: No space left on device
```

Core 0's 64 MiB segment succeeded and core 1's did not, so the server bound
its port, accepted nothing, and exited. The paired run that did start, on a
volume at 99%, delivered **29.7 TPS against 281** — a 9× collapse, with
server CPU 9× higher, produced entirely by allocating under a nearly full
filesystem. Both numbers were discarded and the comparison re-run with 640 MB
free; they are recorded here only because the failure mode is worth knowing.
Two operational consequences follow: raising `cores` raises the instance's
**startup** disk requirement linearly, and a near-full volume degrades this
engine's write path by an order of magnitude before it fails outright.

## Read shapes versus PostgreSQL: the analytic reads invert the table

Everything in "Versus PostgreSQL" above compares the eight statements a
booking issues — point lookups, one filter scan, inserts, updates, a commit.
What it could not compare is the reads the *reporter* issues, because the
PostgreSQL twin had no reporter. `S2-04`/`S2-05` built both halves: the ckdbs
driver runs the three §6 reads in a second process contending with the
bookers, the twin interleaves the same three statements between bookings on
its one connection, and `tools/compare_scenario2.py` diffs the two `--json`
files after refusing any pair that did not run identical work.

| | |
|---|---|
| executed | **2026-08-07 02:35:41 → 02:36:40 UTC**, four runs interleaved KDS/PG/KDS/PG |
| branch / HEAD | `feat-additional-types` / `d99edcf`, driver changes uncommitted |
| binary | unchanged — the same 23:47:41 build measured throughout this document |
| work | 1,500 committed bookings per run, `--seed 1 --verify 25`, fresh KDS server and data file per round; scale as the head of this document |
| reporter | on, 1 s interval, 20 voyages + 10 customers per pass — 6 passes on each engine |
| repeatability | KDS 275.6 / 271.6 TPS across rounds (1.5%), PG 235.6 / 240.9 (2.2%); every per-statement number below reproduced within those spreads except where noted |
| invariants | 100 checks, 0 failures, both engines, both rounds |

One measured pair, every statement both engines ran, grouped by access case
(round A; round B agrees except the one row flagged):

| case | statement | KDS mean | PG mean | KDS p99 | PG p99 |
|---|---|---:|---:|---:|---:|
| point lookup (pk) | cargo-lookup | **143** | 235 | **270** | 420 |
| | credit-lookup | **134** | 241 | **279** | 402 |
| capacity read (derived column) | capacity-read | **132** | 215 | **270** | 334 |
| non-pk equality, small relation | recipe-read | **152** | 307 | **283** | 512 |
| non-pk equality, growing ledger | manifest-scan | 395 | **238** | 2,419 | **339** |
| grouped aggregate over it | voyage-rollup | 342 | **258** | 2,312 | **377** |
| join + aggregate | customer-statement | 871† | 967 | 4,139 | **1,235** |
| insert | freight-insert | **125** | 242 | **265** | 404 |
| | charge-insert | **114** | 185 | **248** | 294 |
| update (pk) | operation-update | **120** | 221 | **232** | 339 |
| | org-update | **120** | 232 | **268** | 358 |
| durability | commit | 1,761 | **1,146** | 3,410 | **1,420** |
| whole booking | booking | **3,598** | 4,147 | 6,478 | **5,546** |

*(µs; † the join's KDS mean is the one unstable row — 871 in round A, 1,449
in round B, on 60 operations contending with the writers, while PG's held at
967/1,075. Its p99 gap, 3–4× in PostgreSQL's favour, is stable.)*

**The booking statements say what the earlier section said** — KDS 1.6–1.9×
faster on every one, protocol-loaded, commit 1.5× the other way. The new
result is the three reporter rows, where the table inverts: on the two reads
over the growing freight ledger PostgreSQL is 1.7–2× faster in the mean and
**7× tighter at p99**, and it wins the join's tail 3×.

The per-statement rows carry two wire protocols, so the honest form of that
finding is each engine's costs as multiples of *its own* point lookup — the
protocol divides out:

| statement | KDS | PG |
|---|---:|---:|
| recipe-read (93-row relation) | 1.07× | 1.31× |
| manifest-scan (growing ledger) | **2.77×** | **1.01×** |
| voyage-rollup | 2.40× | 1.10× |
| customer-statement | 6.1×† | 4.1× |
| commit | 12.4× | 4.9× |

The full distributions of the three reporter reads carry the mechanism that
the means only hint at:

| statement | engine | ops | mean | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| manifest-scan | KDS | 120 | 395 | **81** | 171 | 257 | 1,295 | **2,419** | 4,910 |
| | PG | 120 | 238 | 181 | 210 | 231 | 312 | **339** | 350 |
| voyage-rollup | KDS | 120 | 342 | 86 | 183 | 259 | 1,040 | 2,312 | 2,609 |
| | PG | 120 | 258 | 211 | 236 | 248 | 313 | 377 | 556 |
| customer-statement | KDS | 60 | 871 | 97 | 406 | 871 | 1,528 | 4,139 | 4,139 |
| | PG | 60 | 967 | 787 | 910 | 950 | 1,122 | 1,235 | 1,235 |

*(µs, round A)*

KDS's p0 is *lower* than PostgreSQL's on every one of the three — 81 µs
against 181 for the scan — because an early pass walks a nearly empty ledger
and the walk of nothing is nearly free. Its p99 is then 30× its p0, because a
late pass walks everything the run has written. PostgreSQL's entire
distribution sits in a 1.9× band from p0 to max: an index probe costs the
same at row 0 and row 1,500. These are not two engines with different speeds;
they are a cost that grows with the relation against one that does not.

PostgreSQL serves `WHERE operation_id = <n>` over the ledger at the price of
a point lookup, flat for the whole run, because it has the
`freights(operation_id)` index. KDS walks the relation — 2.77× its point
lookup *on average over a run in which the ledger grew from 0 to 1,500 rows*,
which is why its p99 is 7× its p50: the last pass costs what the mean of a
longer run would. **This is the same verdict the derived-column section
reached from the write side, measured from the read side**: the gap is not
the fold and not the join machinery — `voyage-rollup` walks the same pages as
`manifest-scan` and comes back cheaper (342 µs against 395), because folding
a voyage's freights to one group row costs less than decoding and returning
them — it is the absence of any secondary access path to a non-pk column on a
relation that keeps growing. `recipe-read`, the same statement shape over a
relation that never grows past 93 rows, stays at 1.07× — a FilterScan is only
expensive when there is something to scan.

Two smaller things the pair settles. The reporter costs the bookers nothing
this driver can resolve — 275.6/271.6 TPS with it on, against 271.9/276.4 in
the baseline rows above, all four inside the ±3.2% floor — so the earlier
sections' numbers stand unrevised beside it. And the outcome accounting is
engine-independent at this scale: both engines committed 1,500, rejected 2
for capacity and 1 for credit, from the same seed — the business logic lives
in the driver, and the identity check that demands it is what makes the rest
of the table a comparison of engines rather than of workloads.

## The commit's second millisecond, found and removed

The headline of this document — half a booking waits in one fsync, bimodal,
1.6× slower than PostgreSQL's — is now historical. Tracing the live server
(`strace -T` around 810 commits) found that a commit issues **exactly one
fsync**, but that fsync had two populations: ~950 µs for the load phase's
small appends and **~2,082 µs for every booking commit**. The difference was
never the WAL design; it was two file-level choices at the durability point:

- **`posix_fallocate` reserves *unwritten* extents**, so every commit's
  write landed in space whose extents the following fsync had to convert —
  a journal transaction inside the exact syscall a commit waits on.
- **`fsync` rather than `fdatasync`** flushed timestamp metadata through the
  xfs journal on every durability point, for nothing recovery could read.

The fix is what PostgreSQL has always done: zero-fill a segment at creation
(one sequential 64 MiB write, off every commit path) and sync data only
(`FileLogDevice::Sync()` → `fdatasync`). Measured, interleaved
baseline/patched/baseline/patched, fresh server and file per run, 1,500
bookings each, 100 invariant checks passing in all four:

| run | TPS | commit mean | p25 | p50 | p99 | booking p50 | p99 |
|---|---:|---:|---:|---:|---:|---:|---:|
| baseline 1 | 270.9 | 1,814 | 1,051 | 2,100 | 3,624 | 3,814 | 6,171 |
| **patched 1** | **333.2** | **1,128** | 1,034 | **1,070** | 2,357 | **2,883** | 5,848 |
| baseline 2 | 270.2 | 1,788 | 1,048 | 2,088 | 3,443 | 3,831 | 6,600 |
| **patched 2** | **344.5** | **1,076** | 1,030 | **1,064** | **1,335** | **2,859** | **3,690** |

*(µs; 2026-08-07 04:50 UTC, both binaries built from the same tree, differing
only in `src/wal/file_log_device.cpp`)*

**Throughput +23–27%, commit p50 −49%, commit p99 −61%, and the bimodality
is gone** — p25 to p50 is now 1,030 → 1,064 µs, one population sitting on
the device's flush floor. Against PostgreSQL's commit (mean 1,146, p50
1,122, p99 1,420 in the read-shape section above), KDS's durability point is
now at parity in the middle and equal at the tail, while keeping its 1.6–1.9×
statement advantage — the configuration this comparison always implied but
never showed. The commit's share of a booking falls from ~50% to ~37%, so
the wait-breakdown tables above should be read with that correction.

What it costs: one segment-sized sequential write and fsync per
`CreateSegment` — at startup and at each 64 MiB roll — instead of a bare
reservation, and the no-ENOSPC-at-append promise is unchanged (the prewrite
allocates for real even where `posix_fallocate` is unsupported).

## What these runs do not answer

- **What `--no-txn` costs in correctness.** Not measured: the concurrency
  section above runs every configuration with transactions on, and the
  lost-update result it found is *inside* `--txn`. Autocommit under
  contention would be strictly worse and has not been quantified.
- **Whether the lost updates reproduce on PostgreSQL.** They should — this is
  RC's documented behaviour on both engines — but the twin has no
  `--bookers`, so it was not run. Building that is the honest way to show the
  finding is about the isolation level and not about KDS.
- **The Cabin and the foreign keys.** Both inside the noise floor. The Cabin
  served 1,496 probes against 8 misses over 8 observed values — the structure
  works; the recipe read is 4% of a booking, so serving it perfectly cannot
  show up in throughput. Pricing it needs a workload that reads recipes far
  more often than it commits.
- **How any of this scales.** One ledger size, one connection. The cores
  comparison above covers `cores = 1` and `cores = 2`, which is this
  machine's ceiling, and the cross-core pipeline does not exist — so no
  configuration here executes a statement anywhere but core 0.
- ~~**Where the commit's second millisecond goes.**~~ Answered and removed —
  see "The commit's second millisecond, found and removed" above. It was
  unwritten-extent conversion plus timestamp metadata inside the commit-path
  fsync, and it did not need `docs/observability.md`'s instrumentation to
  find, only `strace -T` on the live server.

---

# 2026-08-11: a 20× cargo book, on a machine that was not quiet

A second run of the same freight workload with the cargo relation grown from
5,000 rows to **100,000**, asking one question the 2026-08-07 run above could
not: does a booking get slower when the book it searches gets bigger?

**The answer is no, and that is the only strong finding here.** Everything
else in this section is weaker than the section above it, for two reasons
stated before any number is quoted, because both decide how the numbers may
be read.

## Two limits on everything below

**1. Another benchmark shared the machine.** A second worktree
(`feat-order-by`) compiled and ran its own driver on this 2-core box during
the matrix. Its windows, from its artefacts' mtimes: compiles at 07:14-07:22,
08:00-08:03 and 08:09-08:10, and driver runs at 07:32:29-07:41:28 and
08:31:31-08:35:16. Four of the twelve cells overlap one of those windows and
are marked `*` throughout this section. This is the same interference the
2026-08-07 run documents as producing a 34% spread, and it is why no row here
is called a regression.

**2. There is no PostgreSQL comparison, and none can be taken on this host.**
PostgreSQL is not installed, there is no container runtime, and `sudo`
requires a password that is not available — so `tools/pg_setup.sh` cannot run.
Every twin cell is **not executed**. The 2026-08-07 PG numbers are *not*
carried down into this section: they were measured on different hardware and
would not be a comparison for this run.

For the same reason, **no delta against the 5,000-cargo run above is a clean
engine delta**. That run was Amazon EBS gp3 / amzn2023 / gcc 11.5.0; this one
is Azure / Ubuntu 24.04 / gcc 13.3.0 on 2 cores. Different device, different
compiler. Only within-section comparisons mean anything.

## The run

| | |
|---|---|
| executed | **2026-08-11 07:36:08 → 08:42:49 UTC** |
| worktree / branch | `fix-undo-record-asserts` / `worktree-fix-undo-record-asserts` |
| **commit measured** | **`c500d4a`** |
| **why not `main`** | `main` at `393b5a4` **does not compile.** `c09353e` (RC06/RV10) was committed unbuilt and left five compile errors; `a00c727`, `ee2250a` and `393b5a4` all inherited that tree. `c500d4a` repairs exactly those five, none behaviour-changing. There is no way to measure `main` itself |
| device | `/dev/root`, 243G free — a real block device, not tmpfs |
| build | `-DCMAKE_BUILD_TYPE=Release` (`-O3 -DNDEBUG`), gcc 13.3.0 |
| host | Azure, **2 cores**, Ubuntu 24.04 |
| KDS server | `cores = 1`, durability `group`, all other keys default |
| PostgreSQL | **not installed — every twin cell not executed** |
| client | one connection, one booker, Python driver |
| scale | 2,000 organizations, 200 ships, 2,000 voyages, **100,000 cargos** |
| work | `--bookings 1500 --seed 1 --verify 25`, identical in every cell |
| isolation | fresh server **and** fresh data file per configuration |

Every cell committed exactly 1,500 bookings and 8,430 charge rows — 5.620
fees per booking — with **0 failed, 0 conflicted and 0 retries** in all
twelve. The workload itself is not in question; only the timings are.

## The options matrix

`*` marks a cell that overlapped the other benchmark. "vs base" is against
the mean of the two **uncontaminated** baseline replicates, 568.8 TPS.

| # | configuration | KDS TPS | vs base | reading |
|---|---|---:|---:|---|
| 1a | **baseline** (`c100k`) | 555.2 | −2.4% | clean |
| 1b | **baseline**, repeated (`base3`) | **582.4** | +2.4% | clean |
| 1c | baseline, repeated `*` | 547.1 | −3.8% | contended |
| 1d | baseline, repeated `*` | 536.3 | −5.7% | contended |
| 2 | `waystone_recording = off` `*` | 550.6 | −3.2% | contended — unreadable |
| 3 | `--cabin` | 526.5 | −7.4% | clean, but see below |
| 4 | `--fk` | 518.7 | −8.8% | clean, but see below |
| 5 | `--capacity-mode scan` | 513.8 | −9.7% | clean, but see below |
| 6 | `--isolation repeatable-read` | 492.7 | −13.4% | clean, but see below |
| 7 | **`--no-txn`** | **95.7** | **−83.2%** | **real: 5.9×** |
| — | `--isolation repeatable-read`, 2nd run | — | — | **not executed** (aborted, 0-byte file) |

**Only row 7 can be called.** The two clean baseline replicates are 555.2 and
582.4 — themselves **4.8% apart** — so with n=2 there is no usable noise
floor, and rows 3 through 6 sit between 7% and 13% below base against a floor
that could plausibly be 5% or more. They are all *below* base and in a
suggestive order, but this run cannot separate them from the machine. Calling
`repeatable-read` a 13% regression on this evidence would be reporting the
box, not the engine.

Row 7 is 5.9× and needs no floor to be believed: under `--no-txn` each of the
nine statements becomes its own transaction and therefore its own fsync,
exactly as the 2026-08-07 run found (there, 3.3×). Booking mean goes 1,799 µs
→ 10,419 µs.

## The baseline distribution

Cell 1a, the clean 100K baseline. Microseconds.

| statement | ops | mean | p0 | p25 | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|---:|---:|
| cargo-lookup | 1,500 | 55.3 | 41.8 | 47.9 | 49.0 | 60.6 | 115.5 |
| credit-lookup | 1,500 | 49.1 | 30.8 | 39.9 | 41.2 | 52.6 | 103.0 |
| capacity-read | 1,500 | 42.9 | 28.4 | 38.4 | 39.6 | 52.9 | 87.1 |
| recipe-read | 1,500 | 50.1 | 38.2 | 47.4 | 48.4 | 59.1 | 77.1 |
| freight-insert | 1,500 | 46.8 | 30.5 | 38.9 | 39.7 | 49.4 | 129.4 |
| charge-insert | 8,430 | 37.2 | 23.2 | 32.8 | 33.6 | 42.2 | 78.1 |
| operation-update | 1,500 | 41.8 | 29.1 | 37.5 | 38.3 | 46.6 | 69.4 |
| org-update | 1,500 | 41.4 | 26.5 | 36.0 | 36.8 | 47.5 | 75.4 |
| **commit** | 1,500 | **1,167.5** | **945.6** | **1,045.4** | **1,089.1** | 1,332.7 | **2,282.1** |
| whole booking | 1,500 | 1,799.0 | 1,419.0 | 1,609.4 | 1,671.2 | 2,462.7 | 3,875.5 |

## Where a booking spends its time

| wait type | KDS µs | share |
|---|---:|---:|
| **durability wait** (`COMMIT`, one fsync) | **1,167.5** | **64.9%** |
| write-statement wait (1 + 5.620 inserts, 2 updates) | 339.1 | 18.8% |
| read wait (4 statements) | 197.4 | 11.0% |
| client, framing and `BEGIN` (residual) | 95.0 | 5.3% |
| **whole booking** | **1,799.0** | 100% |

The commit's share is **64.9%** here against 50.1% in the 5,000-cargo run —
but that is a different device, and the honest reading is not "the commit got
worse". It is that every *other* part got cheaper (eight statements totalling
536 µs against 1,565 µs) while the fsync did not, so the same fixed cost
occupies more of a smaller booking. The finding the 2026-08-07 section
states — half a booking waits for one fsync — is *more* true here, not less.

## What a 50× larger cargo book changes: nothing a statement can see

The ladder, all at `--bookings 1500`, everything but `--cargos` held equal.
p50 microseconds.

| statement | 2,000 cargos `*` | 10,000 | 100,000 |
|---|---:|---:|---:|
| cargo-lookup | 47.0 | 47.5 | **47.9** |
| credit-lookup | 40.5 | 40.3 | 40.4 |
| capacity-read | 39.4 | 39.0 | 39.2 |
| recipe-read | 48.0 | 48.0 | 48.2 |
| freight-insert | 39.8 | 39.5 | 39.6 |
| charge-insert | 33.7 | 33.6 | 33.9 |
| operation-update | 38.3 | 38.2 | 38.3 |
| org-update | 36.7 | 36.8 | 36.7 |
| commit | 1,174.6 | 1,212.9 | 1,062.6 |

**`cargo-lookup` moves 47.0 → 47.9 µs while the relation it descends grows
50×.** That is 1.9% for a 50-fold size increase, and every other statement is
flat to within its own noise. This is the clustered btree doing exactly what
it exists for: the descent is O(log n) over a tree whose fanout is large
enough that 2,000 and 100,000 rows differ by a fraction of a level, and the
0.9 µs is consistent with that fraction.

**The contamination does not threaten this conclusion, and the direction of
the error is why.** The `2,000` column is the contended one, so contention
can only have made it *slower*; it is nonetheless the **fastest** column. A
clean 2,000-cargo run would be at or below 47.0, which widens the already
tiny gap in the same direction rather than reversing it. Interference adds
noise — it does not manufacture a flat line across nine statements and three
sizes.

The consequence for this workload is that **size is not where its time goes,
and growing the book will not change that**. A booking at 100,000 cargos
spends 64.9% of itself waiting for one fsync and 1.9% more than a 2,000-cargo
booking on the lookup that got 50× more data to search. Anything that wants
this workload faster has to attack the durability point; nothing about the
relation sizes is on the table.

## What this section does not answer

- **Whether the Cabin, the foreign keys, `scan` mode or `repeatable-read`
  cost anything.** Rows 3-6 are all clean cells and all below base, but the
  two clean baseline replicates disagree by 4.8% and n=2 supports no floor.
  This needs a re-run with three or more baseline replicates **on an idle
  machine** before any of the four can be quoted.
- **What `waystone_recording = off` costs.** Its cell was contended; it is
  not reported as a number at all.
- **Anything about PostgreSQL.** Not installed, not installable here.
- **Whether the commit's 1,167 µs has the same composition** as the second
  millisecond the 2026-08-07 section found and removed. Not investigated;
  `strace -T` on the live server is the tool that answered it before.
