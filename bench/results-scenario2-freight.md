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

## What this run does not answer

- **What a conflict costs.** Zero occurred, by construction: one booker.
  `S2-03` adds `--bookers` and `--contend`.
- **Whether `--txn` and `--no-txn` differ in correctness.** They do not here
  and cannot: with no second writer, `--no-txn` passed all 100 invariant
  checks. The contrast `docs/scenario2-freight.md` §4 is written for needs
  concurrency.
- **The Cabin and the foreign keys.** Both inside the noise floor. The Cabin
  served 1,496 probes against 8 misses over 8 observed values — the structure
  works; the recipe read is 4% of a booking, so serving it perfectly cannot
  show up in throughput. Pricing it needs a workload that reads recipes far
  more often than it commits.
- **How any of this scales.** One ledger size, one connection. The cores
  comparison above covers `cores = 1` and `cores = 2`, which is this
  machine's ceiling, and the cross-core pipeline does not exist — so no
  configuration here executes a statement anywhere but core 0.
- **Where the commit's second millisecond goes.** The bimodal fsync is the
  largest single cost in this workload and this driver cannot see inside it.
  That needs server-side instrumentation, which `docs/observability.md`
  proposes and nothing implements.
