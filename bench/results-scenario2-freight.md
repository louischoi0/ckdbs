# Where a freight booking spends its time

**Two-thirds of a booking is one fsync, and almost nothing else in this
matrix can be resolved against it.** A booking on KDS is eight statements
inside one transaction; the eight together cost 506 µs and the `COMMIT` that
follows them costs 1,332 µs. Every knob this workload can turn — the derived
capacity column, three foreign keys, a Cabin, Waystone recording, the
isolation level, a second core — moves the booking by less than the fsync's
own run-to-run drift. Only one configuration is outside that floor, and it is
the one that removes the transaction.

The second finding is not about speed. **Under READ COMMITTED, concurrent
bookers silently lose updates**, the workload's invariant checker catches it,
and REPEATABLE READ removes every one of them for no throughput that this
machine can measure.

The workload is `docs/scenario2-freight.md`'s freight and cargo book, driven
by `tools/scenario2_freight.py`. How to run it: `bench/docs/README.md`.

## 1. The run

| | |
|---|---|
| executed | **2026-08-18 01:15:15 → 03:07:57 UTC** (the matrix and the contention cells to 02:41:10, §13's six cells after) |
| branch / worktree | `worktree-bench-scenario2-refresh`, in the worktree `bench-scenario2-refresh` |
| commit measured | **`92c76dd`** — "feat: DROP TABLE is atomic inside a transaction (DT5, option b)" — the tip of `origin/main` when the run started; two commits (`a8b3114`, `7a38ff5`, transactional `CREATE INDEX`) landed upstream while it ran and are **not** in the measured binary. The tree carried two edits, both documentation (`.claude/agents/ck-tester.md`, `bench/docs/README.md`); **nothing under `src/` or `include/` was modified**, so the binary is the engine at `92c76dd` |
| **binary measured** | a **copy**, `sha256 13907114b4d6c597…`, taken from `build-release/kds_server` (linked 2026-08-18 01:08:21 UTC) before the first cell and never rewritten. Every server below started from that copy. The build tree is shared with other sessions; measuring it directly would let a rebuild land between two cells of one matrix |
| build | `-DCMAKE_BUILD_TYPE=Release` (`-O3 -DNDEBUG`), gcc 13.3.0, `KDS_WITH_TLS=ON` (OpenSSL 3.0.13) |
| test suite | **2,379 of 2,379 passing** at this commit, run before the first cell |
| device | `/dev/root` — Azure, ext4, 247 GB with 223 GB free. **Not tmpfs**; every data file under `$HOME/bench-s2-*/` |
| kernel / host | 6.17.0-1022-azure, Ubuntu 24.04, AMD EPYC 9V74, **2 vCPUs** |
| KDS server | `cores = 1`, `durability = group`, `placement = creating`, everything else default — including `buffer_pool_frames = 0`, so the eviction sweep armed in MG03–MG06 is present and never fires |
| ports | 15501 (this engine), 15502/15503 (the A/B engines of `results-scenario2-engine-ab.md`). Not the documented 15432, which another process on this box binds intermittently; every cell refuses to start if its port is already bound |
| client | one connection per booker, plus the driver's analytic reporter process, Python driver |
| scale | 2,000 organizations, 200 ships, 2,000 voyages, **100,000 cargos** — except the ladder (§9) and the contention cells (§11), which say their own |
| work | `--bookings 1500 --seed 1 --verify 25` in every cell. Equal work, not equal time |
| isolation | fresh server **and** fresh data file per cell |
| machine quiet | every cell gates on `bench/wait_quiet.sh` — no `cc1plus`, `ld`, `dpkg` or test binary running and 1-minute load below 0.70 — and samples the load every 5 s for its own life. No cell was discarded |
| PostgreSQL | **no cluster on this host** — see §14 |

Every cell committed exactly 1,500 bookings and passed `--verify 25` at 100
invariant checks — except the four contended READ COMMITTED cells of §11,
whose failures are the finding that section exists to report.

## 2. The noise floor is ±8.2%, and it is the fsync

Three runs of the identical baseline configuration, fresh file each:

| run | TPS | commit mean µs | the eight statements, summed µs |
|---|---:|---:|---:|
| base 1 | 473.1 | 1,454.7 | 536.5 |
| base 2 | **556.4** | **1,231.1** | 439.0 |
| base 3 | 505.8 | 1,311.3 | 541.4 |
| **mean** | **511.7** | **1,332.4** | **505.6** |

They span **16.3% peak to peak**, so the floor is **±8.2% about the mean**.
Add the control — `--isolation repeatable-read`, which on a single connection
with no concurrent writer changes *when* a read view is taken and can change
nothing about what it reads — and it lands at +7.5%, inside. Nothing smaller
than ±8.2% is reported below as a result.

**The floor has one mechanism, and sorting the matrix by throughput names
it.** Every 100,000-cargo cell of §6's matrix except `--no-txn`, which is a
different regime, ordered by TPS against its commit and against the
engine-side work of its eight statements:

| cell | TPS | commit mean µs | eight statements, summed µs |
|---|---:|---:|---:|
| `waystone_recording = off` | 560.6 | 1,158.2 | 502.4 |
| `cores = 2` | 559.5 | 1,109.6 | 558.2 |
| base 2 | 556.4 | 1,231.1 | 439.0 |
| `--isolation repeatable-read` | 549.9 | 1,237.7 | 449.7 |
| `--cabin` | 534.9 | 1,238.1 | 512.5 |
| `--capacity-mode scan` | 523.1 | 1,238.7 | 555.5 |
| `--fk` | 518.2 | 1,290.9 | 518.9 |
| base 3 | 505.8 | 1,311.3 | 541.4 |
| base 1 | 473.1 | 1,454.7 | 536.5 |

Throughput tracks the commit almost perfectly and has no relationship at all
to the engine-side column, which wanders between 439 and 558 µs without
regard to the ordering. The fastest cell in this table is *not* the one that
did the least work; it is the one whose fsyncs came back soonest.

## 3. The unit: what a booking is

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
  INSERT INTO charges  ...                              5.62 rows on average
  UPDATE operations    SET booked_cbm, revenue          btree pk overwrite
  UPDATE organizations SET outstanding                  btree pk overwrite
COMMIT
```

There is no server-side expression that could make those two checks — KDS has
no arithmetic in a select list and no `CHECK` constraint — so they run in the
client, between the reads and the writes. What the engine supplies is that
the read the check was made against and the write the check authorised are
one atomic unit. `--no-txn` (§7) is what prices that guarantee, and §11 is
what happens when the guarantee is weaker than the workload assumed.

## 4. Where the time goes: the wait breakdown

Every wait is measured client-side as a statement's round trip, so each
carries the socket and the Python driver. That overhead cannot be subtracted,
only acknowledged. KDS exposes no server-side wait-event instrumentation, so
*within* a statement the split between page I/O, latch and CPU is not visible
from here — that gap is `docs/observability.md`'s, and it is unbuilt. Lock
wait does not exist in this engine at all: there is no lock manager and no
waiting, by design (`docs/txn.md`), so a write conflict is an immediate
retryable error rather than a queue. Conflict wait is therefore structurally
zero here and non-zero only in §11.

Baseline, run 1:

| wait type | µs | share |
|---|---:|---:|
| **durability wait** (`COMMIT`, one fsync) | **1,454.7** | **69.7%** |
| write-statement wait (1 freight + 5.62 charges, 2 updates) | 335.1 | 16.1% |
| read wait (4 statements) | 201.4 | 9.7% |
| client, framing and `BEGIN` (residual) | 94.8 | 4.5% |
| **whole booking** | **2,086.0** | 100% |

## 5. Per-statement distributions

Baseline, run 1. `charge-insert` has 8,430 operations because a booking
writes 5.62 of them; every other row is once per booking.

| statement | ops | mean | p0 | p25 | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|---:|---:|
| cargo-lookup | 1,500 | 55.9 | 45.5 | 52.1 | 53.6 | 64.5 | 72.0 |
| credit-lookup | 1,500 | 47.2 | 32.1 | 42.3 | 43.8 | 56.1 | 80.8 |
| capacity-read | 1,500 | 45.8 | 29.4 | 40.3 | 41.8 | 57.3 | 81.1 |
| recipe-read | 1,500 | 52.5 | 38.9 | 48.7 | 49.9 | 62.3 | 72.2 |
| freight-insert | 1,500 | 44.1 | 31.4 | 40.3 | 41.4 | 51.0 | 64.9 |
| charge-insert | 8,430 | 37.2 | 23.3 | 33.1 | 34.1 | 45.1 | 59.1 |
| operation-update | 1,500 | 41.1 | 29.4 | 38.6 | 39.6 | 50.7 | 63.3 |
| org-update | 1,500 | 40.8 | 26.7 | 36.4 | 37.6 | 48.6 | 60.6 |
| **commit** | 1,500 | **1,454.7** | **1,057.0** | 1,190.4 | 1,268.5 | 2,302.8 | **4,313.3** |
| whole booking | 1,500 | 2,086.0 | 1,526.9 | 1,788.0 | 1,880.8 | 3,083.2 | 5,523.3 |

*(µs, one connection, latencies include the client's socket cost)*

Three readings.

**A read and a write cost the same.** A pk lookup is 46–56 µs and an insert
or an update is 37–44 µs. On a client-measured round trip both are dominated
by the round trip; the engine's own work is below this driver's resolution
for every statement except the commit. That is a statement about the
measurement, not about the engine, and it is why the wait table above is the
honest unit of analysis rather than these rows. It is also why a real
statement-side change in this engine does not show up in this table or in the
TPS column at all — `bench/results-scenario2-engine-ab.md` resolves one, by
interleaving two builds instead of reading one run's rows.

**The eight statements are tight and the commit is not.** Every statement's
p99 sits within 1.6× of its own p0 and its p95 tracks its p50; the commit's
p99 is 4,313 µs against a p0 of 1,057 µs, **4.1×**. The whole booking
inherits it — p99 5,523 µs against a p50 of 1,881 µs — so essentially all of
this workload's latency variance, and all of the noise floor in §2, is one
statement's tail.

**The three baselines agree on the body and disagree on the tail.** Runs 1
and 3 match to within a microsecond on every statement's p50 (53.6/53.2,
43.8/43.7, 41.8/41.9, …) while their throughput differs by 6.9%, because the
difference is entirely in the commit. Run 2's body is 15% faster than either
and its commit is the middle of the three — which is what an 8.2% floor looks
like from the inside.

## 6. The options matrix

One knob at a time. Equal work in every row: 1,500 committed bookings, 8,430
charge rows, `--seed 1`, 100 invariant checks. "vs base" is against the
**mean of the three baseline runs, 511.7 TPS**.

| # | configuration | TPS | vs base | outside the ±8.2% floor? | verify |
|---|---|---:|---:|---|---|
| 1 | **baseline** — `BEGIN`/`COMMIT`, `--capacity-mode cached` | 473.1 | −7.6% | no — it *is* the floor | 100/0 |
| 2 | baseline, repeated (fresh file) | 556.4 | +8.7% | no — it *is* the floor | 100/0 |
| 3 | baseline, repeated (fresh file) | 505.8 | −1.2% | no — it *is* the floor | 100/0 |
| 4 | `--capacity-mode scan` | 523.1 | +2.2% | **no** | 100/0 |
| 5 | `--fk` — three foreign keys declared | 518.2 | +1.3% | **no** | 100/0 |
| 6 | `--cabin` — Cabin on `recipes.cargo_type` | 534.9 | +4.5% | **no** | 100/0 |
| 7 | `--isolation repeatable-read` *(control)* | 549.9 | +7.5% | **no** — it *defines* the floor | 100/0 |
| 8 | `waystone_recording = off` | 560.6 | +9.5% | **no** — see below | 100/0 |
| 9 | `cores = 2` | 559.5 | +9.3% | **no** — see §12 | 100/0 |
| 10 | **`--no-txn`** — eight autocommitted statements | **91.7** | **−82.1%** | **yes** | 100/0 |

Rows 4 through 7 are inside the floor outright. **Rows 8 and 9 clear it by a
point and still are not findings**, and the reason is the mechanism table in
§2: both carry the matrix's two lowest commit means (1,158 and 1,110 µs)
while their engine-side work is 502 and 558 µs — row 8 is level with the
baseline mean and row 9 is the *highest* in the matrix. A configuration that
does more per-statement work and finishes sooner has not saved anything; its
fsyncs came back faster. Reporting +9.5% as a Waystone saving would be
reporting the device.

Row 10 is the one that is not close.

## 7. Autocommit costs 5.6×, and the per-statement table says why

The baseline column is the mean of the three baseline runs, so no single
run's commit drift sets the ratio.

| statement | baseline | `--no-txn` | ratio | `--no-txn` p50 |
|---|---:|---:|---:|---:|
| cargo-lookup | 54.3 | 72.4 | 1.3× | 64.7 |
| credit-lookup | 45.1 | 55.7 | 1.2× | 45.8 |
| capacity-read | 42.9 | 50.4 | 1.2× | 42.5 |
| recipe-read | 50.9 | 59.4 | 1.2× | 51.0 |
| **freight-insert** | **42.5** | **1,234.0** | **29.1×** | 1,136.9 |
| **charge-insert** | **34.3** | **1,223.2** | **35.7×** | 1,117.9 |
| **operation-update** | **39.0** | **1,224.7** | **31.4×** | 1,126.4 |
| **org-update** | **38.4** | **1,239.1** | **32.3×** | 1,128.0 |
| whole booking | 1,933.7 | 10,875.2 | **5.6×** | |

*(µs)*

The four reads are unchanged. Every write is 29–36× slower, because under
autocommit each is its own transaction and therefore its own fsync — 9.6 per
booking instead of one. Each write's p50 lands within 30 µs of the baseline
*commit's* p50 of 1,268 µs, which is the whole explanation in one comparison:
**a write under autocommit is a commit.** The wait profile inverts, writes
going from 16.1% of a booking to **97.2%**.

`docs/scenario2-freight.md`'s decision S2-2 chose explicit transactions for
**correctness** — eight statements that must be one unit. On this machine
they are also a 5.6× throughput win, and nothing trades against it.

## 8. What the derived column buys, and what sets its value

`operations.booked_cbm` is a running total maintained by every booking so the
capacity check can be a pk lookup instead of an aggregate over the freight
ledger. It is the one place this schema deliberately stores a derived value.

`cached` is the mean of the three baseline runs; `scan` is its own cell.

| capacity-read | `cached` | `scan` | |
|---|---:|---:|---:|
| mean | 42.9 | **83.3** | **+94%** |
| p0 | 29.3 | 32.3 | +10% |
| p25 | 37.1 | 61.7 | +66% |
| p50 | 38.8 | 81.8 | +111% |
| p95 | 54.8 | 116.5 | +113% |
| p99 | 94.1 | 128.6 | +37% |
| whole booking, mean | 1,933.7 | 1,884.7 | −2.5% |
| TPS | 511.7 | 523.1 | +2.2% (inside the floor) |

The statement itself more than doubles at the median, and that part is real —
`scan`'s p25 exceeds `cached`'s p95. The p0 row says why the cost is not
fixed: at best case the two are 3 µs apart, because a walk that finds its row
early is a lookup that got lucky. What the doubling does not do is change
throughput, because 40 µs on a 1,900 µs booking is 2%.

**The value of the derived column is set by `--bookings`, not by `--cargos`.**
What `scan` walks is `freights`, a HEAP relation with no pk index, which
grows to exactly 1,500 rows in every cell of this matrix however large the
cargo book is. So the derived column is not compensating for the absence of
an aggregate; it is compensating for **the absence of a secondary access path
to a non-pk column**, and its value grows with the ledger, not with the
reference data. A run booking 100,000 freights would find `scan`
proportionally worse while `cached` stayed one descent.

## 9. The row-set ladder: 2,000 / 10,000 / 100,000 cargos

`--organizations`, `--ships`, `--operations` and the work are held at
2,000 / 200 / 2,000 and `--bookings 1500`; only `--cargos` moves, and
`--cargos N` is the row count of `cargos` exactly.

**Why the ladder starts at 2,000.** A cargo ships once, so 1,500 bookings
need at least 1,500 cargos in the pool; 200 or 1,000 cannot supply the run
and would measure a different workload rather than a smaller one.

| cargos | TPS | cargo-lookup mean | eight statements, summed | commit mean | booking mean | data file |
|---:|---:|---:|---:|---:|---:|---:|
| 2,000 | 520.1 | 54.0 | 511.2 | 1,309.8 | 1,879.2 | 9.5 MB |
| 10,000 | 518.9 | 53.6 | 515.1 | 1,314.2 | 1,912.8 | 10.0 MB |
| 100,000 *(mean of 3)* | 511.7 | 54.3 | 505.6 | 1,332.4 | 1,933.7 | 21.0 MB |

*(the two smaller rungs are one cell each; the top rung is the mean of the
three baseline runs, whose own cargo-lookup means span 50.8–56.1 µs)*

**A fiftyfold larger cargo book changes nothing this workload can measure,
and the reason is that it never reads the cargo book.** Every read a booking
issues is either a primary-key descent — whose cost is a page count, not a
row count — or a scan of a relation whose size is set by `--bookings`. The pk
lookup into `cargos` reads 54.0, 53.6 and 54.3 µs across a fiftyfold range of
that relation's size, a spread smaller than the spread between two runs of
the same configuration. The data file grows with the rows because the rows
are in it; the *booking* does not.

That is the size answer this workload can give, and it is a fixed-cost
answer. The per-row axis of this schema is `freights`, and §8 is where it
shows.

## 10. What the data file holds, and what Waystone costs

Pages persisted at clean shutdown, at 100,000 cargos with identical writes in
every row — 1,500 freights, 8,430 charges, 3,000 updates:

| configuration | data file | pages | vs recording off |
|---|---:|---:|---:|
| baseline (`cached`) | 22,020,096 B | 2,688 | **+640** |
| `--capacity-mode scan` | 19,398,656 B | 2,368 | **+320** |
| `waystone_recording = off` | 16,777,216 B | 2,048 | — |

*(the file is allocated in extents, so these are file sizes rather than live
page counts — the differences are exact multiples of the 8,192-byte page and
reproduce across repeats, which is what makes them comparable)*

Recording cost no throughput this workload can resolve (§6 row 8) and 5.2 MB
of file. The mechanism is the step-kind trust table doing exactly what
`docs/waystone-concpets.md` specifies, and `SHOW PATTERNS` and `SHOW ACCESS`
on the baseline's file name the three cases exactly:

- `operations` and `organizations` — probed by `WHERE id = <n>` over 2,000
  voyages and 2,000 customers **drawn repeatedly**. Lookup-class, therefore
  trail-replayable, therefore recorded once an instance is seen twice.
  `SHOW PATTERNS` on the baseline's file reports **exactly two** auto-origin
  patterns, `class=1`, with 441 and 475 uses and a trail root apiece.
  (`SHOW PATTERNS` prints the pattern's own oid, not the relation's, so the
  identification is by elimination: `SHOW ACCESS` reports three lookup-class
  shapes in the run — `operations`, `organizations` and `cargos` — and the
  third cannot be recorded, for the reason in the next bullet.)
- `cargos` — 1,500 pk lookups, **no cargo id ever seen twice**. Lookup-class
  and never recorded: `n = 2` never fires, and no pattern exists for it.
- `freights` in `scan` mode — `WHERE operation_id = <n>` is search-class,
  never recorded, which is why that configuration carries half the extra
  pages.

So the cost axis is **repetition, not traffic**, and the three lookup shapes
make the cleanest possible case for it: `SHOW ACCESS` reports 1,526 uses on
`operations`, 1,525 on `organizations` and 1,500 on `cargos` — the same
traffic to within 2% — and the first two cost 640 pages between them while
the third costs nothing at all. Nothing bounds instances per pattern today
(`docs/waystone-concpets.md` §9's retention and eviction, workplan items
P15–P17), and the bound this measurement argues for is one on instance
cardinality rather than on statements executed.

## 11. Concurrency: group commit is real, and READ COMMITTED loses updates

Everything above runs one booker. With several, two results appear that a
single connection cannot produce, and the second is the more important.

`--contend` shares the two rows a booking updates — the voyage and the
customer. `--no-contend` gives each booker a disjoint slice of both. Cargos
are split either way: a cargo ships once, and two bookers holding the same
one would be a driver defect rather than contention.

### Group commit, at 100,000 cargos

| bookers | mode | TPS | booking p0 | p25 | p50 | p95 | p99 | commit p50 | commit p99 |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | partitioned | 531.1 | 1,509 | 1,704 | 1,758 | 2,245 | 3,948 | 1,160 | 3,160 |
| 2 | partitioned | 604.8 | 1,612 | 2,800 | 2,918 | 5,118 | 8,808 | 1,153 | 4,583 |
| 2 | contended | 615.6 | 1,522 | 2,782 | 2,911 | 4,970 | 8,027 | 1,151 | 4,365 |
| 4 | partitioned | 674.1 | 1,668 | 5,126 | 5,536 | 8,264 | 13,650 | 1,193 | 4,289 |
| 4 | contended | 714.6 | 1,774 | 4,688 | 5,053 | 8,258 | 13,448 | 1,078 | 3,508 |

*(µs; READ COMMITTED)*

Throughput rises 1.35× from one booker to four on a server that dispatches
every statement on **one thread**, where no statement executes in parallel
with any other. The engine is not doing more work per second; it is doing
fewer fsyncs per booking, because durability class `group` batches the
commits of concurrently open transactions into one flush. The latency table
prices the trade exactly: a booking's p50 grows from 1,758 to 5,536 µs,
**3.1×**, while throughput grows 1.35× — the queue in front of a
single-threaded dispatcher, priced —
and **the commit itself does not move at all** (1,160 µs at one booker,
1,078–1,193 µs at four) while four times the work flows through it.

### READ COMMITTED loses updates, and the checker catches it

Contention on the two updated rows is set by how many distinct voyages and
customers there are to collide on, not by the cargo book, so these cells use
the small reference set — **200 organizations, 40 ships, 400 voyages, 5,000
cargos** — where a collision is likely rather than rare. Everything else is
unchanged: 1,500 bookings, `--seed 1`, `--verify 25`.

| bookers | mode | isolation | TPS | conflicts | ops / orgs | **invariant failures** |
|---:|---|---|---:|---:|---|---:|
| 1 | partitioned | RC | 521.0 | 0 | — | 0 |
| 2 | partitioned | RC | 618.3 | 0 | — | 0 |
| 2 | contended | RC | 627.6 | 0 | — | **2** |
| 4 | partitioned | RC | 637.3 | 0 | — | 0 |
| 4 | contended | RC | 660.1 | 0 | — | **4** |
| 4 | contended | **RR** | 693.2 | 20 | 3 / 17 | **0** |
| 8 | contended | RC | 742.8 | 6 | 4 / 2 | **6** |
| 8 | contended | **RR** | 725.0 | 71 | 31 / 40 | **0** |

*(100 invariant checks per run; RC = READ COMMITTED, RR = REPEATABLE READ)*

A failure reads like this:

```
I3 organization 45: outstanding=8570533, recomputed from its freights and charges=9806458
```

The charge rows are all there; the running total that the credit check reads
is short by 1,235,925. Two bookers read the same `outstanding`, each added
its own charges, and each wrote back — the second overwrote the first. The
credit limit is now being enforced against a number that under-reports what
the customer owes, which is exactly the failure this workload was built to be
able to detect.

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
(`docs/client-manual.md`). A running total cannot be incremented atomically
at any isolation level; it can only be read, computed client-side, and
written back. That leaves exactly one remedy available today, and it works:
REPEATABLE READ fixes the read view at `BEGIN`, so a row written by anyone
after that point makes the update conflict rather than overwrite. The losses
become 20 and 71 retryable errors, every one retried and committed, and the
invariants hold — for **+5.0% at four bookers and −2.4% at eight**, both
inside this document's floor.

**A conflict count that is not detecting everything is not merely smaller —
it is skewed.** At eight contended bookers RC surfaces 6 conflicts weighted
toward `operations` (4 of 6); RR, which is actually detecting them, reports
71 weighted the other way (40 of 71 on `organizations`). 400 voyages against
200 customers means a customer is twice as likely to be shared, and only RR
is sensitive enough to show it. A per-axis split taken at RC would have
pointed capacity work at the wrong relation.

## 12. A second core buys nothing measurable, and costs a WAL stream

`cores = 2` on a two-vCPU box, everything else baseline: **559.5 TPS against
the baseline mean of 511.7, +9.3%** — a point outside the floor, with the
matrix's *highest* engine-side statement cost (558.2 µs) and its lowest
commit mean (1,109.6 µs). By §2's mechanism that is a device result, not a
core result, and one cell cannot be more than indicative either way.

What is not indicative is the space, which is deterministic:

| | `cores = 1` | `cores = 2` |
|---|---:|---:|
| data file | 22,020,096 B | 22,544,384 B (+1 extent) |
| WAL | one stream, `wal-0-0.log`, 64 MB preallocated | **two streams**, 128 MB preallocated |

A core is a WAL stream, and a stream is 64 MB of preallocated file before the
first statement runs. On this workload the second one is pure cost.

## 13. A quarter of the commit's tail is the checkpointer

§5 leaves one thing unexplained: the commit's p99 is 4.1× its p0 while every
statement around it is tight. Two candidates are testable from the outside —
the checkpointer, which runs every `checkpoint_interval_ms` (5,000 by
default), and the driver's analytic reporter process, whose scans queue in
front of the booker on a single-threaded dispatcher. Two cells each, at
100,000 cargos, everything else baseline:

| | TPS | commit p50 | commit p99 | booking p99 |
|---|---|---:|---:|---:|
| baseline | 521.6, 509.5 | 1,183, 1,183 | 3,375, 3,440 | 4,082, 4,582 |
| `checkpoint_interval_ms = 600000` | 543.9, 533.1 | 1,142, 1,154 | **2,287, 2,816** | **3,048, 3,780** |
| `--no-manifest` (reporter off) | 519.2, 521.0 | 1,157, 1,188 | 3,709, 3,366 | 4,348, 4,012 |

*(µs; both cells of each configuration shown, not averaged)*

**The reporter is innocent and the checkpointer is not.** Removing the
reporter entirely changes nothing anywhere — its two commit p99s straddle the
baseline's. Pushing the checkpoint interval past the length of the run, so
that no checkpoint happens inside it, moves the commit's p99 to 2,287 and
2,816 µs: **both cells below both baseline cells and both reporter-off
cells**, the four unchanged ones spanning 3,366–3,709 µs. Two cells against
four is a small sample and is reported as one, but the separation is clean:
no unchanged cell reaches down to a checkpoint-free one, and the effect is a
quarter of the commit's tail.

**It does not move the commit's median**, which stays at 1,142–1,188 µs
against the baseline's 1,183 µs. The checkpointer is not a steady-state tax
on the durability path; it is a periodic stall that lands on whichever commit
is in flight. The throughput difference that follows from it (+4.5%) is
inside §2's floor and is not claimed here as a result — the tail is the
result.

**This is not a recommendation to lengthen the interval.** A checkpoint is
what bounds the next crash's recovery (`docs/workplan-wal-recovery.md` RC08),
and a 600-second interval on a run that lasts three seconds simply removes
checkpointing from the measurement rather than tuning it. What the cell
establishes is where a quarter of the tail lives, which is the input a real
decision about checkpoint cadence would need — and that cadence is an open
decision, `docs/wal.md` §15's.

## 14. Versus PostgreSQL

**Not measured: this host has no PostgreSQL.** There is no `psql`, no
`initdb`, and no server package installed, so `tools/pg_setup.sh init` cannot
create the scratch cluster on port 15433 that `tools/pg_scenario2_freight.py`
needs. The twin exists and is current — it imports its schema and its
business logic from the ckdbs driver, so the two cannot drift into measuring
different questions — and the comparison is one command once a cluster is
available:

```bash
./tools/pg_setup.sh init
./tools/pg_scenario2_freight.py --port 15433 --database bench \
    --organizations 2000 --ships 200 --operations 2000 --cargos 100000 \
    --bookings 1500 --seed 1 --verify 25 --json pg.json
```

Until that runs, **no cross-engine claim in this document exists**, and the
per-statement figures above should not be read as an engine comparison: much
of what a cross-engine table would show at this level is protocol, since
KDS's newline text protocol is a lighter round trip than PostgreSQL's v3
wire. The finding that would survive such a table is the commit row, because
both engines fsync a write-ahead log to the same device under the same
durability promise — and that row is exactly the one this document is about.

## 15. What this run does not answer

- **Whether KDS's commit is fast.** 1,311 µs is two-thirds of a booking, but
  with no second engine on this host there is nothing to say it is slow. The
  §14 command is the whole gap.
- **The other three quarters of the commit's tail.** §13 places a quarter of
  it in the checkpointer; what remains is 2,300–2,800 µs at p99 against a p0
  of 1,057 µs with the checkpointer removed, and the engine exposes no
  wait-event instrumentation that could split that into device, WAL writer
  and reactor. `docs/observability.md` owns that, unbuilt.
- **What contention does past eight bookers**, on a box with two vCPUs. The
  eight-booker rows are already oversubscribed 4:1, so their absolute
  throughput is a scheduling result as much as an engine one.
- **Whether a Cabin or an index removes the `scan` cost.** §8 says the
  derived column stands in for a secondary access path; measuring the
  replacement belongs to `bench/results-index.md`, not here.
- **Anything about recovery.** Every cell shuts down cleanly, and a booking
  workload that crashed mid-matrix is `bench/results-wal-recovery.md`'s
  subject.
