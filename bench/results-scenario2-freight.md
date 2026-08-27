# Where a freight booking spends its time

**Two-thirds of a booking is one fsync, and almost nothing else in this
matrix can be resolved against it.** A booking on KDS is eight statements
inside one transaction, and it runs at 512 bookings a second: the eight
statements together account for 506 µs of a booking and the `COMMIT` that
follows them for 1,332 µs. Every knob this workload can turn — the derived
capacity column, three foreign keys, a Cabin, Waystone recording, the
isolation level, a second core — moves the booking by less than the fsync's
own run-to-run drift. Only one configuration is outside that floor, and it is
the one that removes the transaction.

That the fsync dominates is also what the cross-engine comparison turns on.
**ckdbs commits 22.5% more bookings a second than PostgreSQL 16.14** on the
same host and device and serves 1.5× to 2.0× more of every one of the eight
statements — while its commit rate is within 8% of PostgreSQL's, because
there the two engines are asking the same filesystem for the same thing. §8
sharpens that into the cleanest statement in the file: change one statement
from a pk lookup to an aggregate over a non-pk column and ckdbs's entire
advantage on it vanishes, the two engines landing 0.3% apart.

The remaining finding is not about speed. **Under READ COMMITTED, concurrent
bookers silently lose updates**, the workload's invariant checker catches it,
and REPEATABLE READ removes every one of them for no throughput that this
machine can measure.

The workload is `docs/inflight/in-progress/scenario2-freight.md`'s freight and cargo book, driven
by `tools/scenario2_freight.py`. How to run it: `bench/docs/README.md`.

## 1. The run

| | |
|---|---|
| executed | **2026-08-18 01:15:15 → 07:29 UTC** — the matrix and the contention cells to 02:41:10, §13's six cells to 03:07:57, §14's interleaved PostgreSQL comparison 03:59:21–04:14:54, and §6's two twinned knobs 06:37–07:29 |
| branch / worktree | `worktree-bench-scenario2-refresh`, in the worktree `bench-scenario2-refresh` |
| commit measured | **`92c76dd`** — "feat: DROP TABLE is atomic inside a transaction (DT5, option b)" — the tip of `origin/main` when the run started; two commits (`a8b3114`, `7a38ff5`, transactional `CREATE INDEX`) landed upstream while it ran and are **not** in the measured binary. The tree carried two edits, both documentation (`.claude/agents/ck-tester.md`, `bench/docs/README.md`); **nothing under `src/` or `include/` was modified**, so the binary is the engine at `92c76dd` |
| **binary measured** | a **copy**, `sha256 13907114b4d6c597…`, taken from `build-release/kds_server` (linked 2026-08-18 01:08:21 UTC) before the first cell and never rewritten. Every server below started from that copy. The build tree is shared with other sessions; measuring it directly would let a rebuild land between two cells of one matrix |
| build | `-DCMAKE_BUILD_TYPE=Release` (`-O3 -DNDEBUG`), gcc 13.3.0, `KDS_WITH_TLS=ON` (OpenSSL 3.0.13) |
| test suite | **2,379 of 2,379 passing** at this commit, run before the first cell |
| device | `/dev/root` — Azure, ext4, 247 GB with 223 GB free. **Not tmpfs**; every data file under `$HOME/bench-s2-*/` |
| kernel / host | 6.17.0-1022-azure, Ubuntu 24.04, AMD EPYC 9V74, **2 vCPUs** |
| KDS server | `cores = 1`, `durability = group`, `placement = creating`, everything else default — including `buffer_pool_frames = 0`, so the eviction sweep armed in MG03–MG06 is present and never fires |
| ports | 15501. Not the documented 15432, which another process on this box binds intermittently; every cell refuses to start if its port is already bound |
| client | one connection per booker, plus the driver's analytic reporter process, Python driver |
| scale | 2,000 organizations, 200 ships, 2,000 voyages, **100,000 cargos** — except the ladder (§9) and the contention cells (§11), which say their own |
| work | `--bookings 1500 --seed 1 --verify 25` in every cell. Equal work, not equal time |
| isolation | fresh server **and** fresh data file per cell |
| machine quiet | every cell gates on `bench/wait_quiet.sh` — no `cc1plus`, `ld`, `dpkg` or test binary running and 1-minute load below 0.70 — and samples the load every 5 s for its own life. No cell was discarded |
| PostgreSQL | **16.14**, on the same host and device, defaults, `synchronous_commit = on` — §14 |

Every cell committed exactly 1,500 bookings and passed `--verify 25` at 100
invariant checks — except the four contended READ COMMITTED cells of §11,
whose failures are the finding that section exists to report.

## 2. The noise floor is ±8.2%, and it is the fsync

Three runs of the identical baseline configuration, fresh file each:

| run | bookings/s | commits/s | the eight statements, as bookings/s |
|---|---:|---:|---:|
| base 1 | 473.1 | 687 | 1,864 |
| base 2 | **556.4** | **812** | 2,278 |
| base 3 | 505.8 | 763 | 1,847 |
| **mean** | **511.7** | **750** | **1,978** |

*(the third column is the rate a booking would run at if the commit were
free — the eight statements' summed cost inverted, which is what makes it
comparable with the other two)*

They span **16.3% peak to peak**, so the floor is **±8.2% about the mean**.
Add the control — `--isolation repeatable-read`, which on a single connection
with no concurrent writer changes *when* a read view is taken and can change
nothing about what it reads — and it lands at +7.5%, inside. Nothing smaller
than ±8.2% is reported below as a result.

**The floor has one mechanism, and sorting the matrix by throughput names
it.** Every 100,000-cargo cell of §6's matrix except `--no-txn`, which is a
different regime, ordered by TPS against its commit and against the
engine-side work of its eight statements:

| cell | bookings/s | commits/s | eight statements, as bookings/s |
|---|---:|---:|---:|
| `waystone_recording = off` | 560.6 | 863 | 1,990 |
| `cores = 2` | 559.5 | 901 | 1,791 |
| base 2 | 556.4 | 812 | 2,278 |
| `--isolation repeatable-read` | 549.9 | 808 | 2,224 |
| `--cabin` | 534.9 | 808 | 1,951 |
| `--capacity-mode scan` | 523.1 | 807 | 1,800 |
| `--fk` | 518.2 | 775 | 1,927 |
| base 3 | 505.8 | 763 | 1,847 |
| base 1 | 473.1 | 687 | 1,864 |

Throughput tracks the commit rate almost perfectly and has no relationship at
all to the engine-side column, which wanders between 1,791 and 2,278 without
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
from here — that gap is `docs/inflight/in-progress/observability.md`'s, and it is unbuilt. Lock
wait does not exist in this engine at all: there is no lock manager and no
waiting, by design (`docs/spec/txn.md`), so a write conflict is an immediate
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
statement-side change in this engine would not show up in this table or in
the TPS column at all: resolving one needs two builds interleaved against
each other, which is a different measurement from this one and is not in this
file.

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

| # | configuration | TPS | vs base | outside the ±8.2% floor? | PostgreSQL TPS | vs its base | verify |
|---|---|---:|---:|---|---:|---:|---|
| 1 | **baseline** — `BEGIN`/`COMMIT`, `--capacity-mode cached` | 473.1 | −7.6% | no — it *is* the floor | 449.9 | — | 100/0 |
| 2 | baseline, repeated (fresh file) | 556.4 | +8.7% | no — it *is* the floor | | | 100/0 |
| 3 | baseline, repeated (fresh file) | 505.8 | −1.2% | no — it *is* the floor | | | 100/0 |
| 4 | `--capacity-mode scan` | 523.1 | +2.2% | **no** | **415.0** | **−7.8%** | 100/0 |
| 5 | `--fk` — three foreign keys declared | 518.2 | +1.3% | **no** | *no twin flag* | | 100/0 |
| 6 | `--cabin` — Cabin on `recipes.cargo_type` | 534.9 | +4.5% | **no** | *no PostgreSQL meaning* | | 100/0 |
| 7 | `--isolation repeatable-read` *(control)* | 549.9 | +7.5% | **no** — it *defines* the floor | *no twin flag* | | 100/0 |
| 8 | `waystone_recording = off` | 560.6 | +9.5% | **no** — see below | *no PostgreSQL meaning* | | 100/0 |
| 9 | `cores = 2` | 559.5 | +9.3% | **no** — see §12 | *no PostgreSQL meaning* | | 100/0 |
| 10 | **`--no-txn`** — eight autocommitted statements | **91.7** | **−82.1%** | **yes** | **85.7** | **−80.9%** | 100/0 |

**Two of the nine knobs have a PostgreSQL twin, and both were measured** — in
their own interleaved window on 2026-08-18 06:37–07:29 UTC, two ckdbs and two
PostgreSQL cells per knob plus a baseline pair inside the same window, so the
"vs its base" column is a ratio against a baseline taken beside it rather than
five hours earlier. Those cells put ckdbs at **530.4 TPS against PostgreSQL's
449.9** at the baseline, `--no-manifest` on both sides, which is §14's
protocol.

**The two columns come from different windows and the rows say so.** The ckdbs
column is this matrix's own cell; the PostgreSQL column and its ratio are the
later window's. What makes them safe to read side by side is that the later
window's ckdbs cells reproduce this matrix's: **521.7 TPS for `scan` against
523.1 here, and 92.8 for `--no-txn` against 91.7** — 0.3% and 1.2% apart,
well inside a floor of 8.2%. A row's two ratios are still computed against
their own engine's own baseline, never across the pair. The other seven rows have no twin: `--fk` and `--isolation` have
PostgreSQL meanings but no flag on `tools/pg_scenario2_freight.py`, and
`--cabin`, `waystone_recording` and `cores` have no PostgreSQL equivalent at
all. Building the first two is the task that would fill those cells.

Rows 4 through 7 are inside the floor outright — and row 4 is inside
PostgreSQL's own noise too, at −7.8%. **Rows 8 and 9 clear it by a point and
still are not findings**, and the reason is the mechanism table in
§2: both carry the matrix's two fastest commits (863 and 901 a second) while
their engine-side rate is 1,990 and 1,791 — row 8 is level with the baseline
mean and row 9 is the *slowest* in the matrix. A configuration that
does more per-statement work and finishes sooner has not saved anything; its
fsyncs came back faster. Reporting +9.5% as a Waystone saving would be
reporting the device.

Row 10 is the one that is not close.

## 7. Autocommit costs 5.7×, on both engines

The baseline column is the mean of the three baseline runs, so no single
run's commit drift sets the ratio. **Statements a second**, derived from each
phase's mean:

| statement | baseline | `--no-txn` | ratio |
|---|---:|---:|---:|
| cargo-lookup | 18,416 | 13,812 | 0.75× |
| credit-lookup | 22,173 | 17,953 | 0.81× |
| capacity-read | 23,310 | 19,841 | 0.85× |
| recipe-read | 19,646 | 16,835 | 0.86× |
| **freight-insert** | **23,529** | **810** | **0.034×** |
| **charge-insert** | **29,155** | **818** | **0.028×** |
| **operation-update** | **25,641** | **817** | **0.032×** |
| **org-update** | **26,042** | **807** | **0.031×** |
| whole booking | 517 | 92 | **0.178×** |

The four reads are unchanged. Every write falls to **1/29th to 1/36th** of
its rate, because under autocommit each is its own transaction and therefore
its own fsync — 9.6 per booking instead of one. Each write's rate lands within
5% of the baseline *commit's* own 789/s, which is the whole explanation in one
comparison: **a write under autocommit is a commit.** The wait profile
inverts, writes going from 16.1% of a booking to **97.2%**.

**PostgreSQL pays the same price for the same reason**: 449.9 → 85.7 TPS,
0.191× against ckdbs's 0.175×, both measured in §6's interleaved window. The
two engines are within 9% of each other on the *ratio*, which is what makes
this the cost of the durability guarantee rather than an artefact of either
implementation.

`docs/inflight/in-progress/scenario2-freight.md`'s decision S2-2 chose explicit transactions for
**correctness** — eight statements that must be one unit. On this machine
they are also a 5.7× throughput win on ckdbs and a 5.2× win on PostgreSQL,
and nothing trades against it.

## 8. What the derived column buys, measured on both engines

`operations.booked_cbm` is a running total maintained by every booking so the
capacity check can be a pk lookup instead of an aggregate over the freight
ledger. It is the one place this schema deliberately stores a derived value.
Statements a second on `capacity-read`, from §6's interleaved cells:

| capacity-read | `cached` | `scan` | what the derived column buys |
|---|---:|---:|---:|
| **ckdbs** | **22,625** | 12,315 | **1.84×** |
| **PostgreSQL** | 14,455 | 12,279 | **1.18×** |
| ckdbs ÷ PostgreSQL | **1.57×** | **1.00×** | |

**The derived column is worth 1.84× to ckdbs and 1.18× to PostgreSQL**, and
the last row is the reason: in `cached` mode ckdbs answers that statement
1.57× faster, and in `scan` mode the two engines are **dead level — 12,315
against 12,279, a 0.3% difference**. ckdbs's whole advantage on this
statement disappears the moment the statement becomes an aggregate over a
non-pk column.

That is the claim this section used to argue and now measures. PostgreSQL has
a btree index on `freights(operation_id)`, so its `SUM` is an index scan and
losing the derived column costs it 15%; ckdbs has no secondary access path
here, so the same `SUM` is a `FilterScan` over the whole ledger and losing
the derived column costs it 46%. **The derived column is not compensating for
the absence of an aggregate — both engines have `SUM`. It is compensating for
the absence of a secondary access path**, and the size of the compensation is
exactly the size of the gap between an index scan and a walk.

The distribution behind the ckdbs row, from the matrix cell (`cached` is the
mean of the three baseline runs; a latency table, so it keeps its
percentiles):

| capacity-read, ckdbs | mean | p0 | p25 | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|---:|
| `cached` | 42.9 | 29.3 | 37.1 | 38.8 | 54.8 | 94.1 |
| `scan` | 83.3 | 32.3 | 61.7 | 81.8 | 116.5 | 128.6 |

*(µs)* The p0 row says why the cost is not fixed: at best case the two are
3 µs apart, because a walk that finds its row early is a lookup that got
lucky. What the doubling does not do is change throughput — `--capacity-mode
scan` is 0.98× the baseline on ckdbs and 0.92× on PostgreSQL, both inside
their own floors — because one statement of a nine-statement booking behind
a 1,300 µs fsync cannot move the total.

**The value of the derived column is set by `--bookings`, not by `--cargos`.**
What `scan` walks is `freights`, a HEAP relation with no pk index, which
grows to exactly 1,500 rows in every cell of this matrix however large the
cargo book is. A run booking 100,000 freights would find `scan`
proportionally worse while `cached` stayed one descent — and PostgreSQL,
whose index scan grows logarithmically, would not.

## 9. The row-set ladder: 2,000 / 10,000 / 100,000 cargos

`--organizations`, `--ships`, `--operations` and the work are held at
2,000 / 200 / 2,000 and `--bookings 1500`; only `--cargos` moves, and
`--cargos N` is the row count of `cargos` exactly.

**Why the ladder starts at 2,000.** A cargo ships once, so 1,500 bookings
need at least 1,500 cargos in the pool; 200 or 1,000 cannot supply the run
and would measure a different workload rather than a smaller one.

| cargos | bookings/s (TPS) | cargo-lookup /s | commit /s | data file |
|---:|---:|---:|---:|---:|
| 2,000 | 520.1 | 18,519 | 763 | 9.5 MB |
| 10,000 | 518.9 | 18,657 | 761 | 10.0 MB |
| 100,000 *(mean of 3)* | 511.7 | 18,416 | 750 | 21.0 MB |

*(the two smaller rungs are one cell each; the top rung is the mean of the
three baseline runs, whose own cargo-lookup rates span 17,825–19,685/s)*

**A fiftyfold larger cargo book changes nothing this workload can measure,
and the reason is that it never reads the cargo book.** Every read a booking
issues is either a primary-key descent — whose cost is a page count, not a
row count — or a scan of a relation whose size is set by `--bookings`. The pk
lookup into `cargos` runs at 18,519, 18,657 and 18,416 statements a second
across a fiftyfold range of that relation's size, a spread smaller than the
spread between two runs of the same configuration. The data file grows with the rows because the rows
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
`docs/spec/waystone-concpets.md` specifies, and `SHOW PATTERNS` and `SHOW ACCESS`
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
(`docs/spec/waystone-concpets.md` §9's retention and eviction, workplan items
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

**The engine is not misbehaving.** `docs/spec/txn.md` specifies first-updater-wins
with no waiting: an `UPDATE` is refused when its target was written by a
*concurrent uncommitted* transaction. Two read-modify-write transactions that
overlap in time but commit in sequence are not that case, and under READ
COMMITTED each statement takes a fresh read view, so the second transaction's
read simply happened before the first one's write. This is the classic
lost-update hazard of RC, and PostgreSQL at RC has it too.

What is specific to KDS is **how little the workload can do about it**. There
is no `SELECT ... FOR UPDATE`, and `UPDATE ... SET c = c + n` is not
expressible — the grammar takes a literal on the right-hand side
(`docs/spec/client-manual.md`). A running total cannot be incremented atomically
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
matrix's *lowest* engine-side rate (1,791 bookings a second) and its fastest
commit (901 a second). By §2's mechanism that is a device result, not a
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
decision, `docs/spec/wal.md` §15's.

## 14. Versus PostgreSQL

Three cells a side, **interleaved** — ckdbs, PostgreSQL, ckdbs, PostgreSQL,
ckdbs, PostgreSQL — 2026-08-18 03:59:21 → 04:14:54 UTC, after the matrix
above and on the same host, the same device and the same quiet-machine gate.
Same booking, same `--seed 1`, same 1,500 committed target, same 100
invariant checks; fresh data file per ckdbs cell and a **fresh database** per
PostgreSQL cell, because dropping and recreating relations leaves the
cluster's bloat behind and a fresh data file does not.

| | |
|---|---|
| PostgreSQL | **16.14** (Ubuntu 16.14-0ubuntu0.24.04.1), extracted rootless into `$HOME/pg16` — this host has no `postgresql` package and `sudo` needs a password, so the archive `.deb`s were unpacked with `dpkg -x` and put on `PATH`. `tools/pg_setup.sh init` then ran unmodified |
| cluster | `$HOME/pg-bench/data`, port 15433, database per cell, **ext4 on `/dev/root`, not tmpfs** |
| tuning | **PostgreSQL's own defaults** — a baseline tuned by hand is not a baseline. `synchronous_commit = on` and `fsync = on`, so both engines fsync per commit to the same device |
| ckdbs | the same `92c76dd` binary copy as the rest of this document, `sha256 13907114…` |
| both | `--no-manifest`. The two drivers place the analytic reporter differently — ckdbs runs it in a second process that contends, the twin runs it inline on one connection where it displaces bookings — which is a difference between drivers, not engines. §13 measured the reporter as costing ckdbs nothing, so removing it costs this comparison no information |

**ckdbs commits 22.5% more bookings a second**, and the three cells a side
barely move: 531.2, 532.7, 527.6 against 440.8, 432.7, 433.6 — a 1.0% and a
1.9% spread, far tighter than the ±8.2% §2 measured across the matrix, which
is what an interleaved window buys.

| | ckdbs | PostgreSQL |
|---|---:|---:|
| TPS (median of three) | **531.2** | 433.6 |
| whole booking, mean | **1,856.6** | 2,303.7 |
| committed / charge rows | 1,500 / 8,430 | 1,500 / 8,446 |
| invariant checks | 100, 0 failures | 100, 0 failures |

**The work is equal to within 0.2%, not exactly equal.** The twin applied
5.63 fees per booking against ckdbs's 5.62 — 16 charge rows more over 1,500
bookings, from a different draw order in the two drivers rather than from a
different rule. It is stated rather than corrected because it is smaller than
any difference below and because it runs *against* ckdbs, which wrote fewer
rows.

### Every statement, and the one row that is an engine comparison

| statement | ops | ckdbs /s | PostgreSQL /s | ratio |
|---|---:|---:|---:|---:|
| cargo-lookup | 1,500 | **18,116** | 11,834 | 1.53× |
| credit-lookup | 1,500 | **22,222** | 13,699 | 1.62× |
| capacity-read | 1,500 | **23,202** | 14,430 | 1.61× |
| recipe-read | 1,500 | **19,841** | 10,661 | 1.86× |
| freight-insert | 1,500 | **24,155** | 12,077 | 2.00× |
| charge-insert | 8,430 / 8,446 | **28,902** | 17,794 | 1.62× |
| operation-update | 1,500 | **25,189** | 13,228 | 1.90× |
| org-update | 1,500 | **26,110** | 13,889 | 1.88× |
| **commit** | 1,500 | **795** | 735 | **1.08×** |
| whole booking | 1,500 | **539** | 434 | 1.24× |

*(statements a second, medians across the three cells a side; the latency
distributions those rates come from are below)*

| percentiles, whole booking | p0 | p25 | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|
| ckdbs | 1,524.6 | 1,715.5 | 1,775.7 | 2,226.1 | 3,489.8 |
| PostgreSQL | 1,828.7 | 2,090.2 | 2,169.0 | 2,993.8 | 4,913.5 |

| percentiles, commit | p0 | p25 | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|
| ckdbs | 1,016.8 | 1,130.3 | 1,171.7 | 1,568.4 | 2,759.4 |
| PostgreSQL | 1,036.5 | 1,158.7 | 1,210.8 | 2,037.8 | 4,032.7 |

**Do not read the eight statement rows as an engine comparison.** Much of a
1.5×–2.0× gap at this level is protocol: ckdbs's newline text protocol is a
lighter round trip than PostgreSQL's v3 wire, and §5 already established that
every one of these statements is dominated by the round trip rather than by
the engine's own work. §8's `scan` row is the sharpest demonstration — change
one statement from a pk lookup to an aggregate and the same two engines land
within 0.3% of each other. What the rows do establish is that the *shape* is the
same on both engines — a read and a write cost about the same, because on
both the round trip is what is being measured.

**The commit row is the comparison.** Both engines fsync a write-ahead log to
the same ext4 filesystem on the same device under the same promise, so the
protocol argument does not apply to it. They land within 8% of each other on
throughput (795 commits a second against 735) and within 3% at the median
(1,171.7 µs against 1,210.8) — **on the durability path these two engines are
the same engine**, which is what should be expected when the fsync is the work and
both are asking the same filesystem for it.

Where they differ is the tail: at p99 the ckdbs commit is 2,759 µs against
PostgreSQL's 4,033, and the whole booking 3,490 against 4,914. §13 places a
quarter of ckdbs's own commit tail in its checkpointer; PostgreSQL's
equivalents — its checkpointer and its background writer — are untuned here
by the deliberate choice above, and a tuned cluster is the obvious next
measurement rather than a claim this run can make.

### Space

| | bytes | of which |
|---|---:|---|
| ckdbs data file | 22,020,096 | 16,777,216 data pages + 5,242,880 of Waystone trail (§10) |
| PostgreSQL database | 21,380,119 | 7,586,319 is an empty database on this cluster, so **13,793,800** is the workload |

PostgreSQL stores the same rows in about **18% fewer bytes than ckdbs's data
pages alone**, and it does so while carrying btree indexes ckdbs does not
have — the two the twin's own header names, on `freights` and `charges`,
which is what makes its `--capacity-mode scan` cheap and ckdbs's expensive
(§8). Counting Waystone, ckdbs's file is 1.60× the bytes PostgreSQL spends
on the same workload.
Neither number is tuned: ckdbs allocates in extents and PostgreSQL has not
been vacuumed, so both carry slack this run did not try to remove.

## 15. What this run does not answer

- **Whether either engine's commit is fast in absolute terms.** §14 shows the
  two agree to within 3% at the median, which says they are asking the same
  filesystem the same question — not that ~1,200 µs is a good answer. Pricing
  the fsync itself against the raw device is a measurement no driver here
  makes.
- **The other three quarters of the commit's tail.** §13 places a quarter of
  it in the checkpointer; what remains is 2,300–2,800 µs at p99 against a p0
  of 1,057 µs with the checkpointer removed, and the engine exposes no
  wait-event instrumentation that could split that into device, WAL writer
  and reactor. `docs/inflight/in-progress/observability.md` owns that, unbuilt.
- **What contention does past eight bookers**, on a box with two vCPUs. The
  eight-booker rows are already oversubscribed 4:1, so their absolute
  throughput is a scheduling result as much as an engine one.
- **Whether a Cabin or an index removes the `scan` cost.** §8 says the
  derived column stands in for a secondary access path. Measuring the
  replacement is `tools/index_benchmark.py`'s question and this workload does
  not ask it.
- **Anything about recovery.** Every cell shuts down cleanly. What a booking
  workload costs when it crashes mid-matrix is `tools/mount_cost_benchmark.py`'s
  question, against `docs/workplan-wal-recovery.md`.

## 16. Addendum, 2026-08-20 — one WAL segment roll is worth the whole cross-engine result, and the undo purge is visible in the file

Re-measured at **`2755045`**, the commit that carries the statement-local
inner build (JB1–JB5) and NULL storage (NU1–NU8). Twenty-two ckdbs cells and
five PostgreSQL cells, three of the ckdbs cells being probes built to
discriminate between causes rather than to fill a matrix row.

**Neither landing moves a single cell of this workload, and the reason is
structural rather than statistical: the booking issues no shape either one
can reach.** What did move is two things neither of them owns.

**A booking workload on a 100,000-cargo file crosses a 64 MiB WAL segment
boundary exactly once, and crossing it stalls the whole server for
0.47–0.79 s.** `WalStream::Append` seals and rolls inline, and
`FileLogDevice::CreateSegment` then `posix_fallocate`s 64 MiB, zero-fills all
of it in 64 × 1 MiB `pwrite`s and `fsync`s — on the appending statement's
own path, which on a single-threaded dispatcher is every connection's path.
In 16 of this run's 20 hundred-thousand-cargo cells that crossing landed
*inside* the measured booking phase, where it costs a fifth of the run's
throughput; in the other four it landed in the load and the booking phase is
clean. **That one
event is the entire difference between ckdbs measuring 0.94× PostgreSQL's
booking rate on this workload and measuring 1.18×.**

**The data file is smaller than the rows in it would suggest, and
`SHOW META` names why:** after a full load and 1,500 bookings the undo chain
holds **2 live pages against 1,256 recycled**. `docs/inflight/in-progress/workplan-undo-purge.md`'s
UP1–UP3 plateau claim is met on a real workload here for the first time, and
the consequence is that ckdbs's data pages now hold this workload in **19%
fewer bytes than PostgreSQL spends on it**.

### 16.1 The run

| | |
|---|---|
| executed | **2026-08-20 08:38:47 → 10:15:24 UTC** — the matrix and the ladder to 09:25:11, the checkpointer probe and the interleaved cross-engine window 09:25:25–09:58:02, the load/measure split probe 09:58:29–10:08:59, the `SHOW META` probe to 10:15:24 |
| branch / worktree | **no worktree** — the primary checkout `/home/cdkbs/ckdbs` on branch `main` |
| commit measured | **`2755045`**, recorded by every cell. Tree clean; the only untracked path is `.claude/worktrees/`, which contains no source |
| **binary measured** | a **copy**, `sha256 5afe5373dde5366c3cea3054c7a4bcf4e15f88021f3485f410e6096a469907be`, taken from `build-release/kds_server` before the first cell and never rewritten. Every server below started from that copy |
| binary provenance | `build-release/kds_server` was **relinked at 2026-08-20 08:35:06 UTC by this session's own `cmake --build`**, which was *not* a no-op: the tree's previous binary was linked 00:12 and predated `dcb2ce8` (a `src/exec/step_vm.cpp` change committed 07:19). Its sha256 was `fee06830…`; measuring it would have measured an engine two commits behind HEAD. The copy's mtime is therefore *after* every commit in `2755045` |
| build | `-DCMAKE_BUILD_TYPE=Release` (`-O3 -DNDEBUG`), gcc 13.3.0, `KDS_WITH_TLS=ON` (OpenSSL 3.0.13 from a rootless `dpkg -x` tree) |
| test suite | **2,513 of 2,513 passing** at this commit, run from the same build immediately before the first cell |
| device | `/dev/root` — Azure, **ext4** (`df -T`, checked in this session), 247 GB with 201 GB free. **Not tmpfs**; every data file under `$HOME/bench-s2-jb8/cells/<cell>/`, every WAL under `<cell>/s2.db.wal/` |
| kernel / host | 6.17.0-1022-azure, Ubuntu 24.04, AMD EPYC 9V74, **2 vCPUs** |
| KDS server | `cores = 1`, `durability = group`, `placement = creating`, `log_level = info`, everything else default — including `buffer_pool_frames = 0` and `join_build_max_rows = 65536` |
| ports | 15501, and 15502 for the `SHOW META` probe. Not 15432: an unrelated resident `kds_server` from another project binds it, and was left alone |
| scale | 2,000 organizations, 200 ships, 2,000 voyages, **100,000 cargos**, except the ladder (§16.6) |
| work | `--bookings 1500 --seed 1 --verify 25` in every cell. Equal work, not equal time |
| isolation | fresh server **and** fresh data file per cell |
| machine quiet | every cell gates on `bench/wait_quiet.sh` and samples the load for its own life; `pgrep cc1plus` was 0 before and after every cell in the run. No cell was discarded |
| PostgreSQL | **16.14** (Ubuntu 16.14-0ubuntu0.24.04.1), the standing scratch cluster on port 15433, `$HOME/pg-bench/data` on the same ext4 device, **PostgreSQL's own defaults** — `synchronous_commit = on`, `fsync = on`, `shared_buffers = 128 MB`, nothing tuned |

Every cell committed exactly 1,500 bookings and passed `--verify 25` at 100
invariant checks with **0 failures** — ckdbs and PostgreSQL alike. Drivers,
flags and invocations: `bench/docs/README.md`, entry `scenario2_freight.py`.

### 16.2 The floor has one mechanism and it is not the fsync: it is the segment roll

Three baseline cells, fresh file each, and then three cells of the same
configuration in which the segment crossing fell outside the booking phase:

| cell group | bookings/s | spread | where the segment roll landed |
|---|---:|---:|---|
| `base1` / `base2` / `base3` | 464.8 / 424.2 / **407.4** | **14.1%** | inside the booking phase, all three |
| `metaprobe` / `settle0` / `settle90` | 510.5 / **523.2** / 517.3 | **2.5%** | inside the load phase, all three |

*(the second group runs `--load-only` first and the measured invocation
after it, so the file has absorbed two full loads by the time the bookings
start and the 64 MiB boundary is behind them. The measured statements, the
scale, the seed, the isolation and the 1,500-commit target are identical;
these are not faster cells, they are cells whose stall fell somewhere the
booking phase does not measure)*

**The floor this run adopts is therefore two numbers.** On the raw basis the
widest same-configuration disagreement is **14.1%**; with the roll's single
booking excluded — the cell's elapsed time reduced by
`booking_max − booking_p50` — the widest is **5.4%** (`ckptoff2` against
`ckptoff1`). Every verdict below is given on both bases and nothing is
claimed on a difference smaller than 14.1% raw or 5.4% roll-excluded.

The mechanism is not inference. Four controls were run to eliminate the
alternatives, and the code names itself:

| probe | result | what it eliminates |
|---|---|---|
| `checkpoint_interval_ms = 600000`, two cells | booking max **779.6 ms / 687.5 ms** — unchanged | not the checkpointer (which §13 showed owns a quarter of the *commit tail*) |
| `--no-manifest`, three cells | booking max **668.6 / 764.2 / 610.6 ms** — unchanged | not the analytic reporter contending on the dispatcher |
| the 2,000- and 10,000-cargo rungs | booking max **43.0 ms / 8.9 ms**, and `ls` shows **one** WAL segment ever created | the workload that never crosses 64 MiB never stalls |
| PostgreSQL, three interleaved cells | booking max **9.5 / 11.5 / 16.6 ms** | not the host, the device or the page cache |

And the log agrees: every 100,000-cargo cell's WAL directory holds
`wal-0-0.log` **and** `wal-0-1.log`, both 67,108,864 bytes; the two ladder
rungs hold only `wal-0-0.log`. In `src/wal/file_log_device.cpp`, `Prewrite`
writes 64 MiB of zeros and `fsync`s, and its own comment says why — the
extent conversion is being paid once here instead of inside a commit's
fsync, "what PostgreSQL's `wal_init_zero` does and for the same reason". The
trade is sound and this file's §14 is where the commit-side benefit shows.
**What this addendum adds is the other half of the price: it is paid
synchronously, by whatever statement is in flight, and this engine creates
each segment where PostgreSQL recycles one.** The scratch cluster beside it
runs `wal_init_zero = on`, `wal_recycle = on` and `wal_segment_size = 16 MB`
— it zero-fills for the same reason, over a quarter of the file, and then
renames spent segments instead of creating new ones, which is why its
`pg_wal` directory holds a stable set of 16 MiB files and its booking maximum
never exceeds 17 ms. ckdbs already zero-fills; what it does not do is
recycle, and `docs/spec/wal.md` owns whether it should.

Where it landed, per cell, is deterministic rather than random. `base1`'s
own checkpoint anchors put the LSN at **60,762,680** five seconds before the
run ended and **67,608,272** at the end, so 67,108,864 was crossed inside the
window that contains the booking phase: the load leaves the log just short of
the boundary and the bookings step over it. A configuration that writes more
WAL per row crosses earlier, which is exactly why `--fk` is the one
100,000-cargo cell of the matrix whose booking phase is clean — its crossing
fell in `load-cargos`, at 670.0 ms.

### 16.3 The options matrix

One knob at a time, 1,500 committed bookings and 8,430 charge rows in every
row. **TPS raw** is what the driver measured; **TPS roll-excluded** removes
the single booking that absorbed the segment crossing, and is the basis on
which knobs are compared, because the crossing belongs to the file rather
than to the knob. "vs base" is against the roll-excluded baseline mean of
**529.6 TPS**.

| # | configuration | TPS raw | TPS roll-excl. | vs base | outside the 5.4% floor? | verify |
|---|---|---:|---:|---:|---|---|
| 1 | **baseline** — `BEGIN`/`COMMIT`, `--capacity-mode cached` | 464.8 | 543.6 | +2.6% | no — it *is* the floor | 100/0 |
| 2 | baseline, repeated (fresh file) | 424.2 | 526.5 | −0.6% | no — it *is* the floor | 100/0 |
| 3 | baseline, repeated (fresh file) | 407.4 | 518.6 | −2.1% | no — it *is* the floor | 100/0 |
| 4 | `--capacity-mode scan` | 399.5 | 495.3 | −6.5% | **marginal** — see below | 100/0 |
| 5 | `--fk` — three foreign keys declared | 495.6 | 500.5 | −5.5% | **marginal** | 100/0 |
| 6 | `--cabin` — Cabin on `recipes.cargo_type` | 461.9 | 544.4 | +2.8% | **no** | 100/0 |
| 7 | `--isolation repeatable-read` *(control)* | 474.2 | 560.8 | +5.9% | **no** — it *defines* the floor | 100/0 |
| 8 | `waystone_recording = off` | 468.1 | 554.6 | +4.7% | **no** | 100/0 |
| 9 | `cores = 2` | 421.2 | 509.7 | −3.8% | **no** | 100/0 |
| 10 | `checkpoint_interval_ms = 600000` | 401.8 / 429.9 | 507.6 / 535.1 | −4.2% / +1.0% | **no** | 100/0 |
| 11 | **`--no-txn`** — eight autocommitted statements | **90.0** | **94.3** | **−82.2%** | **yes** | 100/0 |

Rows 4 and 5 are the two that land within a point of the floor, and both are
resolved by the twinned window of §16.8, where they were re-run beside a
baseline taken minutes earlier: `--capacity-mode scan` measures 504.2
roll-excluded against that window's baseline median of 526.4, **−4.2%,
inside**. `--fk` has no PostgreSQL flag and no second cell, so it stays
"marginal and not claimed". Row 7's control behaves as a control should.

**Row 11 is the only result in the matrix, and it reproduces §7 exactly.**
Autocommit costs 5.6× on ckdbs and 4.9× on PostgreSQL in the same window,
for the same reason: every write becomes its own fsync.

| statement | ckdbs base /s | ckdbs `--no-txn` /s | ratio | PostgreSQL ratio |
|---|---:|---:|---:|---:|
| cargo-lookup | 18,797 | 15,552 | 0.83× | 0.90× |
| credit-lookup | 22,989 | 21,834 | 0.95× | 0.93× |
| capacity-read | 24,038 | 23,419 | 0.97× | 0.93× |
| recipe-read | 19,841 | 19,268 | 0.97× | 0.95× |
| **freight-insert** | **24,272** | **908** | **0.037×** | 0.070× |
| **charge-insert** | **29,326** | **928** | **0.032×** | 0.048× |
| **operation-update** | **25,381** | **923** | **0.036×** | 0.065× |
| **org-update** | **26,810** | **919** | **0.034×** | 0.063× |
| whole booking | 560 | 100 | **0.179×** | 0.205× |

*(statements a second, derived as `1,000,000 / p50 µs` — the driver is a
serial single-connection loop per booker, so that is its `ops / elapsed`;
p50 rather than mean because the segment roll contaminates one mean per cell)*

Every ckdbs write under autocommit lands within 3% of that cell's own
commit rate. A write under autocommit is a commit, on both engines.

### 16.4 Per-statement distributions

`settle0`, whose booking phase carries no segment roll and whose means are
therefore readable. `charge-insert` runs 5.62 times per booking; every other
row is once.

| statement | ops | mean | p0 | p25 | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|---:|---:|
| cargo-lookup | 1,500 | 54.8 | 45.5 | 52.4 | 53.7 | 63.2 | 68.9 |
| credit-lookup | 1,500 | 45.6 | 32.4 | 42.5 | 43.8 | 54.9 | 63.9 |
| capacity-read | 1,500 | 44.4 | 30.6 | 40.5 | 41.9 | 53.5 | 66.3 |
| recipe-read | 1,500 | 59.9 | 47.6 | 57.7 | 58.9 | 70.4 | 76.3 |
| freight-insert | 1,500 | 42.0 | 31.7 | 40.3 | 41.2 | 49.3 | 54.6 |
| charge-insert | 8,430 | 35.0 | 23.2 | 32.9 | 33.8 | 42.8 | 49.5 |
| operation-update | 1,500 | 40.0 | 29.9 | 38.5 | 39.3 | 49.3 | 53.5 |
| org-update | 1,500 | 37.8 | 27.1 | 36.2 | 37.2 | 45.7 | 50.3 |
| **commit** | 1,500 | **1,262.5** | **1,057.2** | 1,162.4 | 1,198.1 | 1,502.9 | **2,517.9** |
| whole booking | 1,500 | 1,886.1 | 1,571.0 | 1,767.5 | 1,816.4 | 2,154.1 | 3,232.4 |

*(µs, one connection, latencies include the client's socket cost)*

One row of that table is not comparable with §5's and says so here rather
than being quietly read as one: `recipe-read` at 58.9 µs against §5's 49.9.
It is elevated in all three of the roll-free cells (58.9 / 59.3 / 58.9) and
in none of the sixteen single-invocation ones (44.2–51.0), because the
two-invocation arrangement drops and re-creates the eight relations, and a
`FilterScan` over the 93-row `recipes` then runs against a relation placed
after a whole orphaned generation of pages. It is an artefact of the probe,
not of the engine at this commit; §16.9 uses a single-invocation cell for the
comparison against §5 for exactly that reason.

And the same table for a cell that *does* carry the roll (`base1`), which is
what a p99-only reading would miss entirely:

| base1 | ops | mean | p0 | p25 | p50 | p95 | p99 | **max** |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| credit-lookup | 1,500 | **357.8** | 32.3 | 42.1 | 43.7 | 56.0 | 89.7 | **466,401** |
| whole booking | 1,500 | 2,123.8 | 1,425.9 | 1,582.9 | 1,636.7 | 2,261.3 | 4,182.7 | **469,222** |

One sample in 1,500 is 10,600× the p50 of its own phase. It moves the mean by
7.8× and does not touch p99 — which is why this addendum tables the maximum
and the file's earlier sections, which do not, could not have seen it.

### 16.5 Where the time goes: the wait breakdown

`settle0`, means, one booking. Every wait is a client-measured round trip, so
each carries the socket and the Python driver; that cannot be subtracted, only
acknowledged. **Lock wait does not exist in this engine** — no lock manager,
no waiting, a write conflict is an immediate retryable error
(`docs/spec/txn.md`) — so conflict wait is structurally zero on a single booker
and non-zero only in §11's contended cells. Within a statement, the split
between page I/O, latch and CPU is **not measurable**: there is no
server-side wait-event instrumentation. `docs/inflight/in-progress/observability.md` owns that gap
and it is unbuilt.

| wait type | µs | share |
|---|---:|---:|
| **durability wait** (`COMMIT`, one fsync) | **1,262.5** | **66.9%** |
| write-statement wait (1 freight + 5.62 charges, 2 updates) | 316.5 | 16.8% |
| read wait (4 statements) | 204.7 | 10.9% |
| client, framing and `BEGIN` (residual) | 102.4 | 5.4% |
| **whole booking** | **1,886.1** | 100% |

*(the read line carries the ~9 µs of `recipe-read` artefact §16.4 describes —
0.5% of the booking, and it inflates the read share rather than deflating it)*

There is a fifth wait in 16 of this run's 20 hundred-thousand-cargo cells and
it belongs to the run rather than to the booking: **the segment roll, 0.47 s
to 0.79 s, once**. Spread over 1,500 bookings it is 310–530 µs each — the
size of the entire write-statement wait — which is why it is reported as its
own line rather than folded into an average nobody experiences.

### 16.6 The row-set ladder: 2,000 / 10,000 / 100,000 cargos

`--organizations`, `--ships`, `--operations` and the work are held at
2,000 / 200 / 2,000 and `--bookings 1500`; only `--cargos` moves, and
`--cargos N` is the row count of `cargos` exactly. The ladder starts at 2,000
because a cargo ships once and 1,500 bookings need at least 1,500 cargos.

| cargos | TPS raw | TPS roll-excl. | cargo-lookup /s | commit p50 µs | WAL segments | data file |
|---:|---:|---:|---:|---:|---:|---:|
| 2,000 | 542.0 | 550.2 | 19,493 | 1,143.4 | **1** | 8,388,608 B |
| 10,000 | 530.1 | 531.4 | 19,157 | 1,156.3 | **1** | 8,912,896 B |
| 100,000 *(mean of 3)* | 432.1 | 529.6 | 18,823 | 1,121.9 | **2** | 16,252,928 B |

**A fiftyfold larger cargo book still changes nothing the booking does** —
the pk lookup into `cargos` serves 19,493, 19,157 and 18,823 statements a
second across the range, a spread of 3.6%, and the commit's median moves by
3.1%. Both are inside the 5.4% floor. What it changes is whether the run crosses a WAL segment boundary, and
that is worth 18% of raw throughput. The size axis of this workload is not
the cargo book; it is the WAL.

### 16.7 What the file holds, and the undo purge's first workload datum

Pages persisted at clean shutdown and file bytes, at 100,000 cargos with
identical writes in every row — 1,500 freights, 8,430 charges, 3,000 updates:

| configuration | data file | pages persisted | vs recording off |
|---|---:|---:|---:|
| baseline (`cached`) | 16,252,928 B | 1,871 | **+671** |
| `--capacity-mode scan` | 13,631,488 B | 1,513 | **+313** |
| `waystone_recording = off` | 11,010,048 B | 1,200 | — |

Waystone's cost is **671 pages** on the baseline and **313** in `scan` mode —
5.2 MB and 2.4 MB, reproducing §10's mechanism exactly (`scan` mode's
`WHERE operation_id = ?` is search-class and never recorded, so it carries
half). `SHOW PATTERNS` on a live baseline file reports **exactly two**
auto-origin patterns, `class=1`, with **441 and 475 uses** — the same two
counts §10 records — and `SHOW ACCESS` reports 1,526 uses on `operations`,
1,525 on `organizations` and 1,500 on `cargos`, the third recorded not at all
because no cargo id is ever drawn twice. The cost axis is repetition, not
traffic, and it has not moved.

**What has moved is the rest of the file, and `SHOW META` says why.** Taken
on a live server at the two points of one 100,000-cargo cell:

| | `undo_pages_live` | `undo_pages_recycled` |
|---|---:|---:|
| empty file, at mount | 0 | 0 |
| after the load (104,305 rows, autocommit) | **2** | 566 |
| after 1,500 bookings | **2** | **1,256** |

The chain **plateaus at two live pages** while 1,256 pages are recycled into
the log's next growth — 10.3 MB of undo that a non-purging engine would be
carrying. This is the first measurement of `docs/inflight/in-progress/workplan-undo-purge.md`'s
UP1–UP3 on a real workload rather than a unit test, and it is what makes the
space comparison in §16.8 come out the way it does.

### 16.8 Versus PostgreSQL

Three cells a side, **interleaved** — ckdbs, PostgreSQL, ckdbs, PostgreSQL,
ckdbs, PostgreSQL — 2026-08-20 09:30:40 → 09:46:57 UTC, same host, same
device, same quiet gate, `--no-manifest` on both sides. Fresh data file per
ckdbs cell and a **fresh database** per PostgreSQL cell.

The two engines wrote 8,430 and 8,446 charge rows for 1,500 bookings — 0.2%
apart, from a different draw order in the two drivers, and it runs against
ckdbs.

| | ckdbs raw | ckdbs roll-excluded | PostgreSQL |
|---|---:|---:|---:|
| TPS (median of three) | 416.9 | **526.4** | **444.1** |
| the three cells | 413.5 / 416.9 / 433.7 | 506.6 / 529.0 / 526.4 | 444.1 / 439.1 / 453.8 |
| ckdbs ÷ PostgreSQL | **0.94×** | **1.18×** | — |
| committed / charge rows | 1,500 / 8,430 | | 1,500 / 8,446 |
| invariant checks | 100, 0 failures | | 100, 0 failures |

**Both rows are true and the second is the engine comparison.** PostgreSQL
never pays a segment roll in these cells — its own equivalent, `wal_init_zero`
over a 16 MB segment, is four times smaller and its cluster recycles
segments rather than creating them — so a raw-TPS comparison is comparing
one engine's file-lifetime event against another's absence of one. The
per-statement medians, which no single event can move, say the same thing as
the roll-excluded column:

| statement | ops | ckdbs /s | PostgreSQL /s | ratio |
|---|---:|---:|---:|---:|
| cargo-lookup | 1,500 | **18,797** | 12,165 | 1.55× |
| credit-lookup | 1,500 | **22,989** | 14,085 | 1.63× |
| capacity-read | 1,500 | **24,038** | 14,706 | 1.63× |
| recipe-read | 1,500 | **19,841** | 10,823 | 1.83× |
| freight-insert | 1,500 | **24,272** | 12,531 | 1.94× |
| charge-insert | 8,430 / 8,446 | **29,326** | 18,832 | 1.56× |
| operation-update | 1,500 | **25,381** | 13,699 | 1.85× |
| org-update | 1,500 | **26,810** | 14,205 | 1.89× |
| **commit** | 1,500 | **849** | 850 | **1.00×** |
| whole booking | 1,500 | **560** | 468 | 1.20× |

*(statements a second, derived from the median p50 across the three cells a
side)*

**The commit row is now exact.** 1,177.9 µs against 1,177.1 µs at the
median — a 0.07% difference, where §14 measured 3%. Two engines fsyncing a
write-ahead log to the same ext4 filesystem under the same promise are, on
the durability path, one engine. Read the other eight rows with §14's
caution: at 34–92 µs a round trip they are substantially protocol, and
§16.9's `scan` row is the demonstration.

| percentiles, whole booking | p0 | p25 | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|
| ckdbs | 1,541.8 | 1,726.6 | 1,784.7 | 2,191.2 | 3,764.7 |
| PostgreSQL | 1,819.2 | 2,059.6 | 2,136.2 | 2,732.2 | 4,823.9 |

| percentiles, commit | p0 | p25 | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|
| ckdbs | 1,021.0 | 1,135.3 | 1,177.9 | 1,541.0 | 2,706.6 |
| PostgreSQL | 1,009.5 | 1,122.2 | 1,177.1 | 1,778.3 | 3,778.9 |

*(µs, medians of the three cells a side. ckdbs's advantage over PostgreSQL is
in the commit's tail — 2,707 µs against 3,779 at p99 — and §13 places a
quarter of ckdbs's own tail in its checkpointer)*

**The derived column, both engines.** `operations.booked_cbm` is a running
total so the capacity check can be a pk lookup instead of a `SUM` over the
freight ledger:

| capacity-read, statements/s | `cached` | `scan` | what the derived column buys |
|---|---:|---:|---:|
| **ckdbs** | **24,038** | 11,614 | **2.07×** |
| **PostgreSQL** | 14,706 | 12,706 | **1.16×** |
| ckdbs ÷ PostgreSQL | **1.63×** | **0.91×** | |

| capacity-read distributions | mean | p0 | p25 | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|---:|
| ckdbs `cached` | 44.7 | 30.6 | 40.8 | 42.3 | 54.9 | 64.8 |
| ckdbs `scan` | 100.2 | 34.5 | 64.2 | 86.1 | 126.5 | 135.6 |
| PostgreSQL `cached` | 70.1 | 57.1 | 66.6 | 68.0 | 81.6 | 99.4 |
| PostgreSQL `scan` | 81.3 | 69.4 | 76.8 | 78.7 | 95.5 | 113.2 |

*(µs)* §8's finding holds in its strong form and slightly harder: change one
statement from a pk lookup to an aggregate over a non-pk column and ckdbs's
1.63× advantage becomes **0.91×** — it goes from ahead to marginally behind,
because PostgreSQL's `SUM` is an index scan on `freights(operation_id)` and
ckdbs's is a `FilterScan` over the whole ledger. The derived column is
compensating for the absence of a secondary access path, not of an aggregate,
and the compensation is worth 2.07× here. (The 0.91× against §8's 1.00× is
one cell a side and is inside neither engine's floor; the *direction* is what
reproduces, not the point.)

**Space.**

| | bytes | of which |
|---|---:|---|
| ckdbs data file | 16,252,928 | 11,010,048 data pages + 5,242,880 Waystone trail (§16.7) |
| PostgreSQL database | 21,380,119 | 7,748,631 is an empty database on this cluster, so **13,631,488** is the workload |

**ckdbs's data pages hold this workload in 19% fewer bytes than
PostgreSQL spends on it**, and even counting the Waystone trail the whole
file is 1.19× PostgreSQL's — while PostgreSQL carries two btree indexes
ckdbs does not have. §16.7's `undo_pages_recycled = 1,256` is the mechanism.
Neither number is tuned: ckdbs allocates in extents and PostgreSQL has not
been vacuumed.

### 16.9 What the two landings at this commit did to this workload: nothing, and why

The task this addendum was run for was to find which of `2755045`'s two
landings — the statement-local inner build (JB1–JB5) and NULL storage
(NU1–NU8) — moves a cell of this workload. **Neither does, and in both cases
the reason is structural, so it is a statement about the workload rather
than a measurement that came out flat.**

**The inner build cannot fire here.** The booking is eight statements and
none of them is a join. The workload issues exactly one join shape, in the
analytic reporter and in `--verify`'s I3 check, and `ANALYZE` on a live
server shows what it compiles to:

```
SELECT c.org_id, SUM(f.cbm) FROM freights AS f
  JOIN cargos AS c ON f.cargo_id = c.id WHERE c.org_id = 7 GROUP BY c.org_id

step 0 Scan  freights AS f
step 1 Probe cargos   AS c key=0:0.3
```

The inner side binds to `cargos.id`, which is the Keystone primary key, so
the structure ladder stops at its **first** arm — the pk probe. The build is
the ladder's *last* arm (`docs/spec/join-inner-build.md` §5), reached only
when the pk, both index and both Cabin arms decline. Nothing in this schema
can reach it. `bench/results-scenario3-library.md` is where that arm is
measured.

**The NULL bitmap is zero-width in every relation here.** `NU1`–`NU8` size
the tail bitmap to the *nullable* columns, and `tools/scenario2_freight.py`
declares none — all eight relations are entirely `NOT NULL`. The row layout
is therefore byte-identical to a pre-NU engine's by construction, and the
matrix agrees. `base1`, the first cell of this run, against §5's baseline,
p50 µs, all eight statements — the widest disagreement is **1.6%**, which is
the round trip's own drift:

| | cargo | credit | capacity | recipe | freight-ins | charge-ins | op-upd | org-upd |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| §5, at `92c76dd` | 53.6 | 43.8 | 41.8 | 49.9 | 41.4 | 34.1 | 39.6 | 37.6 |
| §16, at `2755045` | 53.6 | 43.7 | 41.9 | 50.7 | 41.2 | 34.2 | 39.7 | 37.7 |
| Δ | 0.0% | −0.2% | +0.2% | **+1.6%** | −0.5% | +0.3% | +0.3% | +0.3% |

Encode and decode both run in every one of these statements, and neither
moved.

**What did move from the file's standing sections, and why:**

| standing section | standing number | this run | why |
|---|---:|---:|---|
| §2, baseline TPS | 511.7 (mean of 3) | 432.1 raw, **529.6 roll-excluded** | the segment roll fell inside the booking phase in all three cells here, and §2 does not table a maximum, so where it fell in that run is unrecorded |
| §14, ckdbs ÷ PostgreSQL | 1.22× | 0.94× raw, **1.18× roll-excluded** | the same one event |
| §14, commit median | 1,171.7 vs 1,210.8 µs (3% apart) | **1,177.9 vs 1,177.1 µs (0.07%)** | tighter interleaving; the finding is unchanged and sharper |
| §14, space | ckdbs data pages 18% *more* than PostgreSQL's workload | ckdbs data pages **19% fewer** | `undo_pages_recycled = 1,256`, `undo_pages_live = 2` — the undo purge (UP1–UP3, `docs/inflight/in-progress/workplan-undo-purge.md`) |
| §10, Waystone's cost | +640 / +320 pages | +671 / +313 pages | unchanged |
| §8, derived column | 1.84× ckdbs, 1.18× PostgreSQL | 2.07× / 1.16× | one cell a side both times; direction reproduces |

No cell moved for either of the commit's two features.

### 16.10 What this addendum does not answer

- **Whether recycling a WAL segment would remove the stall.** §16.2 locates
  it and names the code, and points out that PostgreSQL recycles where this
  engine creates. It does not measure a recycling implementation, because
  none exists — that is `docs/spec/wal.md`'s decision to take.
- **Where inside the 0.47–0.79 s the time goes.** `Prewrite` is 64 `pwrite`s
  and an `fsync`; splitting those needs instrumentation the engine does not
  have (`docs/inflight/in-progress/observability.md`).
- **Whether the undo purge costs anything.** §16.7 measures what it
  *recovers*. The purge's own CPU is inside statements whose medians are
  unchanged, which bounds it below this driver's resolution but does not
  price it.
- **The contention cells.** §11's finding — READ COMMITTED loses updates and
  REPEATABLE READ removes them — was not re-run here; this addendum is one
  booker throughout, and §11's cells stand at their own commit.
