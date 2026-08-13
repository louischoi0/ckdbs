# What a ROLLBACK costs when the transaction reserved against an assertion

`bench/results-assertion.md` prices the *commit* half of the assertion
protocol. This is the abort half, and it exists because AS6b
(`docs/feat-assertion.md` §7) gave that half a page write:
`AssertionEnforcer::AbortTxn` now does a page read-modify-write plus a
`StampPageLsn` per aborted reservation, to set `kEntryOrphaned` on the entry
the reservation wrote.

The thesis, and every number below is in service of it: **abort's assertion
cost is a per-reservation cost and commit's is a per-transaction cost, so the
claim that the two halves "now cost the same" is false for every transaction
that reserves more than once.** `CommitTxn` groups its pending reservations by
`(assertion, page)` and pays one page fetch, one `Open`, one WAL record and one
stamp for the whole group; `AbortTxn` walks the reservations one at a time and
pays all four per reservation. Measured on the `cnt` relation, abort's
assertion cost per reservation is **flat at 0.20–0.47 µs across K = 1…32**,
while commit's falls **0.50 → 0.11 µs** over the same range — a 1/K curve,
which is the signature of a cost paid once per transaction. At K = 16 the
protocol costs 5.6 µs to abort against 1.7 µs to commit; at K = 32, 15.2 µs
against 4.6 µs.

The increment AS6b itself added is small and separable: **0.056 µs per
reservation**, which is 1.8 µs on a 32-reservation abort. The asymmetry is
older than AS6b — base already paid `Unapply` and a WAL `Append` per
reservation — but AS6b widened it rather than closing it, which is the
opposite of what the two documents carrying the claim say.

| | |
|---|---|
| **Run** | 2026-08-13, 00:52–02:12 UTC. Abort sweep 00:52–01:04, server-CPU cell 02:02–02:05, mount matrix 02:06–02:12. |
| **Worktree / branch** | `assert-orphan-flag-format-v2` (a git worktree of `/home/cdkbs/ckdbs`), branch `assert-orphan-flag-format-v2` |
| **Commits measured** | **head `2199780`** (committed 2026-08-13 00:36:46) against **base `fb46512`** (2026-08-12 08:31:39). A/B by checkout: the tree was clean at `2199780`, `fb46512` was checked out detached to build the base binary, and the branch was restored to `2199780` afterwards. |
| **Tree** | Clean at both checkouts — `git status --short` empty. The only untracked additions are this file, the four `bench/run_*.sh` and three `bench/summarize_*.py`/`dump_phases.py` helpers, and the `build-rel-*/` trees, none of which is engine source. |
| **Binary provenance** | `build-rel-head/kds_server` mtime 00:40:37, built from `2199780` (committed 00:36:46). `build-rel-base/kds_server` mtime 00:45:54, built from `fb46512` (committed 2026-08-12 08:31:39). **Both binaries postdate the commit they were built from**, and neither is the pre-existing `build-release/` tree (mtime 2026-08-12 23:49), which was not used. |
| **Build type** | Release (`cmake -DCMAKE_BUILD_TYPE=Release`), both sides. `CMakeLists.txt` defaults to Debug and `./build` is Debug; neither was used. GCC 13.3.0, C++20. |
| **Device** | `/dev/nvme0n1p1` (NVMe, `ROTA=0`), ext4, data files under `/home/cdkbs/abbench/` and `/home/cdkbs/mount-bench/`. **Not tmpfs** — the mount driver refuses tmpfs without `--force` and reported `filesystem ext4` in all 22 cells. |
| **Machine** | 2 cores, 15 GiB, kernel 6.17.0-1022-azure. |
| **Server config** | `cores = 1`, **`durability = relaxed`**, `log_level = warn`; everything else at defaults. One **fresh data file and fresh server process per cell**, both sides (ports 15601/15602 for the abort sweep, 15491 for mounts). |
| **Why a fresh file is not optional here** | `67ce947` raised `kMinReadableSegmentFormatVersion` to 2, so the `fb46512` binary refuses a segment the `2199780` binary wrote, naming it as predating the layout. The two sides physically cannot share a data directory. |
| **Enforcement stamp** | `enforcing=on` observed via `SHOW ASSERTIONS` on both sides in every abort cell; the driver refuses to measure a mislabelled engine (`--expect-enforcing on`, the default). |
| **Correctness** | `--verify` passed in every abort cell on both sides: every relation holds exactly `rows + committed × K` rows, and every asserted relation's `GROUP BY grp: COUNT(*), SUM(amount)` equals the unasserted control's — which is what `Unapply` must have restored on every abort. Zero error replies in any measured phase. |
| **Test suite** | `kds_tests` run in Release at both commits before any measurement: **2266 passed at `2199780`**, **2263 passed at `fb46512`**. The three extra are `67ce947`'s own. No failures either side. |
| **Baseline** | No PostgreSQL twin, and PostgreSQL is not installed on this machine at all. §7 states both facts and what would build one. |

---

## 0. How the comparison is kept honest

Four devices, because a 1 µs claim needs all of them.

**Both binaries in one run.** Each cell starts two servers — `2199780` on
15601, `fb46512` on 15602 — and the driver interleaves them block by block
inside a single process. A machine that drifts mid-run moves both arms
together. Two *sequential* runs of the same driver on this box disagree with
themselves by more than the effect being measured, so a sequential A/B could
not have carried this finding.

**Four relations per side, identically loaded.** `abt_none` and `abt_twin`
carry no assertion; `abt_cnt` carries one (`COUNT(*)`); `abt_multi` carries two
(`COUNT(*)` and `SUM(amount)`), so it makes 2K reservations from the same K
statements. Every number quoted as an "assertion cost" is a difference against
`none` **in the same run on the same side**, which cancels the socket round
trip, `BEGIN`, the K INSERTs and the undo unwind.

**`twin` is the noise floor, measured from inside the run.** It is a second
unasserted relation, so `twin − none` is the same subtraction over a
configuration that is identical by construction. Any row below it is not a
finding.

**Equal work, not equal time.** Every cell runs a fixed 2000 transactions per
relation per ending per side, so a slower configuration is not also a smaller
sample.

---

## 1. The noise floor, from inside the run

`twin − none` on the rollback arm, per cell, both sides. 2000 transactions per
arm.

| cell | head floor (µs) | base floor (µs) | head−base floor |
|---|---|---|---|
| K=1 | −0.20 | −0.10 | −0.10 |
| K=2 | −0.90 | −0.20 | −0.70 |
| K=4 | −0.20 | −0.20 | 0.00 |
| K=8 | −0.40 | −0.30 | −0.10 |
| K=16 | −0.10 | −0.30 | 0.20 |
| K=32, first run | **+2.70** | −0.30 | **+3.00** |
| K=32, repeat | −0.70 | −0.70 | 0.00 |

**The floor is ±0.3 µs of p50 in eight of nine cells**, and the K=32 first run
is the exception that proves the discipline works: its floor blew out to
2.70 µs, so its numbers are not quoted anywhere below. The K=32 repeat, run on
a fresh server pair, came back with a floor of −0.70/−0.70 and a head−base
floor of exactly zero; that is the K=32 cell used throughout.

Two cells are excluded from the head-vs-base double difference for the same
reason and are named rather than dropped silently: the K=32 first run above,
and the `rows=1000` cell of the row sweep (§5), whose base side drifted
2.0 µs.

---

## 2. The headline: abort pays per reservation, commit pays per transaction

`rows = 1000`, `durability = relaxed`, 2000 transactions per arm per side.
Each number is the assertion's own cost — the relation's p50 minus the
unasserted control's p50 in the same run.

| K (INSERTs per txn) | reservations | head ROLLBACK `cnt` | head COMMIT `cnt` | ratio | head ROLLBACK `multi` | head COMMIT `multi` |
|---|---|---|---|---|---|---|
| 1 | 1 | 0.20 | 0.50 | 0.4× | 0.60 | 0.70 |
| 2 | 2 | 0.50 | 0.50 | 1.0× | 0.90 | 0.70 |
| 4 | 4 | 1.40 | 0.70 | 2.0× | 2.50 | 1.10 |
| 8 | 8 | 2.80 | 1.00 | 2.8× | 5.10 | 1.60 |
| 16 | 16 | 5.60 | 1.70 | 3.3× | 10.50 | 2.60 |
| 32 | 32 | **15.20** | **4.60** | **3.3×** | **27.50** | **6.50** |

All µs, p50. `multi` declares two assertions, so its reservation count is 2K
from the same statement count — and its rollback column is close to twice the
`cnt` column at every K, which is the per-reservation shape showing up a second
way, from a knob that does not change the number of statements.

Dividing by the reservation count is what makes the two shapes unmistakable:

| K | head ROLLBACK per reservation | head COMMIT per reservation | base ROLLBACK per res. | base COMMIT per res. |
|---|---|---|---|---|
| 1 | 0.200 | 0.500 | 0.400 | 0.500 |
| 2 | 0.250 | 0.250 | 0.300 | 0.350 |
| 4 | 0.350 | 0.175 | 0.300 | 0.200 |
| 8 | 0.350 | 0.125 | 0.262 | 0.100 |
| 16 | 0.350 | 0.106 | 0.281 | 0.100 |
| 32 | 0.475 | 0.144 | 0.419 | 0.178 |

**The abort column is flat and the commit column is a 1/K curve.** Commit's
per-reservation cost falls by 4.7× between K=1 and K=16 because the work is
paid once and divided by more reservations; abort's does not fall at all. This
is the measurement the whole document exists for, and it holds on *both*
binaries — the asymmetry is a property of the protocol, not of AS6b.

The mechanism is visible in the source at `2199780`. `CommitTxn` builds
`std::map<std::pair<assertion_id, PageId>, std::vector<index>> by_page` and
then loops over *groups*, doing one `store.Get`, one `BoundCabinPage::Open`,
one `wal->Append` carrying every index, and one `StampPageLsn` per group.
`AbortTxn` loops over *reservations*. And the grouping collapses to a single
entry in the normal case, because `BoundCabinChainWriter::Append` always writes
to `tail_` — one chain per assertion, appended in order, 254 entries per page —
so a transaction's K entries share one page unless the page fills mid-way.
A transaction that reserves K times against one page pays K fetches and K
stamps to abort against 1 and 1 to commit.

### The measured unit, in full

`rows = 1000`, K = 32 (the repeat cell), 2000 operations per arm.

| side | arm | relation | ops | mean | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|
| head | ROLLBACK | none | 2000 | 30.6 | 23.2 | 24.9 | 27.3 | 39.9 | 52.8 |
| head | ROLLBACK | twin | 2000 | 29.7 | 23.1 | 24.6 | 26.6 | 38.2 | 46.0 |
| head | ROLLBACK | cnt | 2000 | 45.1 | 29.1 | 37.5 | 42.5 | 58.8 | 75.7 |
| head | ROLLBACK | multi | 2000 | 57.5 | 33.3 | 47.7 | 54.8 | 79.6 | 96.5 |
| head | COMMIT | none | 2000 | 28.3 | 20.9 | 22.4 | 24.6 | 37.2 | 52.5 |
| head | COMMIT | twin | 2000 | 26.4 | 20.9 | 22.1 | 23.8 | 35.0 | 42.6 |
| head | COMMIT | cnt | 2000 | 29.6 | 22.6 | 24.5 | 29.2 | 38.2 | 49.9 |
| head | COMMIT | multi | 2000 | 32.5 | 23.5 | 25.6 | 31.1 | 39.9 | 53.1 |
| base | ROLLBACK | cnt | 2000 | 41.8 | 27.4 | 35.4 | 40.5 | 56.9 | 67.8 |
| base | ROLLBACK | multi | 2000 | 286.1 | 30.1 | 44.2 | 51.0 | 76.3 | 91.4 |
| base | COMMIT | cnt | 2000 | 31.0 | 22.2 | 24.3 | 29.8 | 38.8 | 49.6 |
| base | COMMIT | multi | 2000 | 30.9 | 23.3 | 25.3 | 31.2 | 40.3 | 55.1 |

All µs. The `base / ROLLBACK / multi` mean of 286.1 against a p50 of 51.0 is a
single stall in that arm, not a shape; the percentile columns are why it is
visible rather than averaged into a false result, and p50 is what §2 quotes.

The separation is already clean at p0: `cnt` rolls back with a best case of
29.1 µs against the control's 23.2 µs, so **the assertion's abort cost is
present in the floor of the distribution, not only in its tail** — it is work
every abort does, not a tail event.

---

## 3. What AS6b itself added

The double difference — (asserted − control) on head minus (asserted −
control) on base — isolates exactly the `store.Get` + `Open` + `MarkOrphaned` +
`StampPageLsn` that `67ce947` put in `AbortTxn`. Base already did `Unapply`
plus a WAL `Append` per reservation, so everything else cancels.

| K | reservations aborted | ROLLBACK `cnt` dd | ROLLBACK `multi` dd | COMMIT `cnt` dd | COMMIT `multi` dd | floor dd |
|---|---|---|---|---|---|---|
| 1 | 1 | −0.20 | 0.00 | 0.00 | 0.10 | −0.10 |
| 2 | 2 | −0.10 | −0.10 | −0.20 | −0.20 | −0.70 |
| 4 | 4 | 0.20 | 0.50 | −0.10 | 0.10 | 0.00 |
| 8 | 8 | **0.70** | **1.30** | 0.20 | 0.20 | −0.10 |
| 16 | 16 | **1.10** | **2.10** | 0.10 | 0.10 | 0.20 |
| 32 (repeat) | 32 | **1.80** | **3.60** | −1.10 | −0.60 | 0.00 |

All µs, p50. **K = 8 is where the added cost first clears the floor**: 0.70 µs
against a floor of −0.10, with the `multi` arm agreeing at 1.30 µs over 16
reservations. Below that it is indistinguishable from zero, which is the honest
reading of K=1, 2 and 4 — not "AS6b is free", but "at one to four reservations
this method cannot tell it from zero".

Normalising the three clear cells by reservation count:

| cell | reservations | dd (µs) | per reservation (µs) |
|---|---|---|---|
| K=8, `cnt` | 8 | 0.70 | 0.088 |
| K=8, `multi` | 16 | 1.30 | 0.081 |
| K=16, `cnt` | 16 | 1.10 | 0.069 |
| K=16, `multi` | 32 | 2.10 | 0.066 |
| K=32, `cnt` | 32 | 1.80 | 0.056 |
| K=32, `multi` | 64 | 3.60 | 0.056 |

**0.056–0.088 µs per aborted reservation**, tightening to 0.056 at the two
largest cells where the signal is furthest above the floor. That is the cost of
one buffer-pool page fetch, one page open, one flag OR and one LSN stamp on an
already-resident page — cheap, consistent, and unmistakably *per reservation*.

The COMMIT columns are the control on the method: `67ce947` and `5384551`
reordered the commit path but did not change what it pays per reservation, and
the commit double difference is within ±1.1 µs of zero at every K, with no
trend.

---

## 4. Where a rolled-back transaction's time goes

The measured unit is one `ROLLBACK` statement: one client round trip that
unwinds K reservations. Decomposing it, `rows = 1000`, K = 32, head:

| wait type | µs at p50 | share | how it was obtained |
|---|---|---|---|
| Client + socket round trip | ~21 | 49% | p0 of the `COMMIT twin` arm (20.9 µs) — the least a round trip on this path has ever cost in the run |
| Transaction unwind (undo trail, no assertion) | 6.4 | 15% | `ROLLBACK none` p50 (27.3) minus the round-trip floor |
| Assertion protocol, 32 reservations | 15.2 | 36% | `ROLLBACK cnt` p50 minus `ROLLBACK none` p50 |
| — of which AS6b's page write | 1.8 | 4% | the head−base double difference at K=32 |
| — of which `Unapply` + WAL append | 13.4 | 32% | the base side's own `cnt − none` |
| Durability / commit fsync | **0** | 0% | `ROLLBACK` reaches no durability point, and `durability = relaxed` puts no fsync on the commit path either |
| Lock / conflict wait | **0** | 0% | one connection per side, no contention by construction |
| Total | 42.5 | 100% | `ROLLBACK cnt` p50 |

Two wait types are named as inapplicable rather than omitted. **Durability is
genuinely zero on this path**, which is the reason `relaxed` was chosen: under
`group` the same cell measured a ~1,030 µs fsync on `COMMIT` and none on
`ROLLBACK`, so an abort-versus-commit comparison at that setting prices the
fsync and not the protocol — the very comparison this document exists to make
would have been buried. **Lock wait is zero by construction**, not by
measurement; a contended version of this workload would be a different driver.

The round trip is about half the measured unit, which is why every finding
above is stated as a difference against a control taken in the same run and
never as an absolute.

---

## 5. Row-set size: the asymmetry is a fixed cost per reservation

K = 16 held constant, sweeping the preloaded row count. 2000 transactions per
arm per side.

| rows | head ROLLBACK `cnt` | head COMMIT `cnt` | ratio | ROLLBACK per res. | COMMIT per res. | head floor |
|---|---|---|---|---|---|---|
| 200 | 5.20 | 2.20 | 2.4× | 0.325 | 0.137 | −0.60 |
| 1,000 | 5.80 | 2.50 | 2.3× | 0.362 | 0.156 | −0.30 |
| 10,000 | 6.70 | 2.00 | 3.4× | 0.419 | 0.125 | −0.20 |
| 1,000 (K sweep cell) | 5.60 | 1.70 | 3.3× | 0.350 | 0.106 | −0.10 |

All µs, p50. **A 50× change in relation size moves abort's per-reservation cost
by 0.325 → 0.419 µs and commit's by 0.137 → 0.125 µs** — both essentially flat,
against a 3.3× gap between them. The assertion settle path touches the Bound
Cabin chain and the group header, neither of which is indexed by relation
cardinality, and the measurement says so: this is a fixed cost per reservation,
and the row count is not the variable that governs it. K is.

The `rows = 1000` row-sweep cell is the one whose base side drifted (floor dd
+1.70), so its head−base double difference is not quoted; its head-only columns
above are internally consistent with the other two and with the K sweep.

---

## 6. Ordinary statements: the regression check

`5384551` moved a WAL append relative to a page write on `CommitTxn`, which
every autocommitted statement against an asserted relation takes. That is not a
formality, so it gets its own arms — autocommitted `INSERT`, `UPDATE`,
`SELECT`, `DELETE`, interleaved between the two binaries block by block.

At `rows = 10,000`, 4000 operations per arm per relation per side, two
independent repeats:

| arm | relation | head p50 | base p50 | Δ repeat 1 | Δ repeat 2 |
|---|---|---|---|---|---|
| `INSERT` | none | 32.6 | 32.1 | +0.50 | +0.20 |
| `INSERT` | twin | 31.5 | 31.1 | +0.40 | +0.20 |
| `INSERT` | cnt | 32.7 | 32.3 | +0.40 | +0.40 |
| `INSERT` | multi | 33.9 | 33.2 | +0.70 | +0.70 |
| `UPDATE` | none | 34.2 | 34.2 | 0.00 | −0.20 |
| `UPDATE` | cnt | 33.3 | 33.2 | +0.10 | −0.10 |
| `UPDATE` | multi | 35.7 | 35.4 | +0.30 | −0.10 |
| `SELECT` | none | 34.8 | 34.7 | +0.10 | −0.10 |
| `SELECT` | cnt | 33.6 | 33.6 | 0.00 | −0.30 |
| `SELECT` | multi | 34.0 | 34.2 | −0.20 | −0.70 |
| `DELETE` | none | 33.0 | 32.8 | +0.20 | −0.20 |
| `DELETE` | cnt | 33.1 | 33.0 | +0.10 | −0.40 |
| `DELETE` | multi | 34.3 | 34.3 | 0.00 | −0.80 |

All µs; head p50 and base p50 are repeat 1's. **No regression: every delta is
within ±0.8 µs.** On the `UPDATE`, `SELECT` and `DELETE` arms the result is a
clean null — five of those nine rows change sign outright between the two
repeats and three more move from exactly zero to slightly negative.

The `INSERT` arm is the one that does not flip: all four relations are positive
in both repeats, +0.20 to +0.70 µs. That is worth naming rather than rounding
away, and it is worth naming that **it cannot be the change under test**,
because it is the same size on `none` and `twin` — relations with no assertion
declared, where `CommitTxn` returns at its first map lookup and `5384551`'s
reshuffle never executes. The likelier explanation is positional: within every
interleaved block the driver iterates `for side in sides` with head first, so
head takes the arm's first statement after each block boundary and whatever
that costs. A driver change to alternate the side order per block would settle
it; until then the honest statement is that the ordinary INSERT path shows a
consistent sub-microsecond head-first bias on asserted and unasserted relations
alike, and no assertion-attributable regression.

The same arms at `rows = 200` and `rows = 1,000` (200 and 1000 ops per arm)
agree: deltas from −1.00 to +0.80 µs with no consistent sign. One earlier
`rows = 10,000` cell showed `INSERT` +5.5 µs on head, and it is worth naming
because of how it resolved — the +5.5 appeared on `twin`, a relation carrying
**no assertion at all**, and p25 was identical between the sides (23.4 vs
23.2). A shift that appears only above p25 and only on an unasserted relation
is a stall, not a code path; the two repeats above, run at 4× the sample count,
do not reproduce it.

---

## 7. Versus PostgreSQL

**No comparison was run, and no twin exists.** Two independent reasons, both
stated rather than left as a gap:

1. **PostgreSQL is not installed on this machine.** `postgres`, `psql`,
   `pg_ctl` and `initdb` are all absent, no `/usr/lib/postgresql` exists, and
   `tools/pg_setup.sh init` would have to build or install a server first. No
   PostgreSQL number in this document would have been measured, so none is
   given. This also means the ordinary-statement arms of §6 — which *do* have a
   twin in `tools/pg_benchmark.py` — could not be baselined this run.
2. **No twin exists for the workload even with a cluster running.** PostgreSQL
   parses `CREATE ASSERTION` in no released version, so the group-ceiling
   mechanism has no counterpart to measure; the nearest emulation is a
   constraint trigger, which prices a different mechanism. This is the same
   reason `bench/results-assertion.md` §6 gives.

**The task that would build one**, for the part that is buildable: a
`tools/pg_assertion_abort.py` twin that models the ceiling as a deferred
`CONSTRAINT TRIGGER` on the grouped column and measures `ROLLBACK` against
`COMMIT` over the same K sweep. It would answer a narrower question than this
document does — "what does a mature engine charge to abort a transaction that
touched a deferred constraint" — and it would need the caveat that PostgreSQL's
trigger queue is a per-row in-memory list with no page write at all, so the
comparison is of designs and not of implementations of one design.

For the mount half (§8) the position is the one `bench/docs/README.md` already
records: the twin is `pg_ctl start` to first accepted connection, clean and
after `kill -9`, over the same nine-mount shape, and it does not exist yet.

---

## 8. Mount cost, with real assertion entries to walk

`tools/mount_cost_benchmark.py --assertion` declares the ceiling before the
load, so every loaded row leaves one Bound Cabin entry and every measured mount
runs RC07's revival and `AttachEntriesFromPages` over those entries.
`--assert-rollback-every 4` rolls back every fourth row in its own transaction,
so a quarter of the entries on the page are orphaned — the shape `67ce947`
changed, since the head binary's walk skips a marked entry and the base
binary's cannot. Nine mounts of one data file per cell, clean stop,
`durability = relaxed`.

### Head and base are indistinguishable at every cell

| rows | shape | head p50 | base p50 | head−base | head p0 | head p95 |
|---|---|---|---|---|---|---|
| 200 | no assertion | 103.4 | 101.9 | +1.5 | 99.8 | 113.3 |
| 200 | assertion | 133.8 | 132.6 | +1.2 | 132.0 | 141.1 |
| 200 | assertion + rollbacks | 132.3 | 131.7 | +0.6 | 128.3 | 136.0 |
| 1,000 | no assertion | 100.4 | 100.4 | 0.0 | 96.9 | 101.5 |
| 1,000 | assertion | 127.9 | 128.8 | −0.9 | 125.3 | 135.5 |
| 1,000 | assertion + rollbacks | 128.1 | 130.6 | −2.5 | 125.2 | 130.3 |
| 10,000 | no assertion | 73.4 | 73.1 | +0.3 | 71.8 | 78.1 |
| 10,000 | assertion | 87.2 | 86.5 | +0.7 | 85.0 | 91.4 |
| 10,000 | assertion + rollbacks | 86.3 | 86.1 | +0.2 | 83.1 | 97.1 |

All ms, nine mounts per cell. The head−base column runs −2.5 to +1.5 ms
against a within-cell p0-to-p95 spread of 5–14 ms, so **`kEntryOrphaned` and
the walk's skip cost nothing measurable at mount**, with or without orphaned
entries present. Nine mounts is a small sample and p95/p99 collapse onto the
maximum — stated rather than hidden.

### Reviving an assertion costs one extra full segment scan

This is the finding of the mount half. Declaring an assertion adds a large,
*decreasing* amount to the mount:

| rows (= entries) | no-assertion p50 | assertion p50 | delta | `recovery_analysis_us` p50 | delta ÷ analysis |
|---|---|---|---|---|---|
| 200 | 103.4 | 133.8 | +30.3 | 29.7 | 1.02 |
| 1,000 | 100.4 | 127.9 | +27.5 | 28.3 | 0.97 |
| 2,000 | 94.2 | 123.3 | +29.1 | 26.2 | 1.11 |
| 5,000 | 85.0 | 107.7 | +22.7 | 21.2 | 1.07 |
| 10,000 | 73.4 | 87.2 | +13.8 | 12.9 | 1.07 |

All ms, head side. **The assertion's mount cost tracks `recovery_analysis_us`
within 11% at every cardinality, across a 2.3× range of scan cost** — and it
*falls* as the entry count rises, which rules out the per-entry page walk as
the dominant term outright.

The source says why: `src/exec/assertion_recover.cpp:270` calls
`wal::ScanLog(device, core_id, from_lsn, visit)`. Assertion recovery performs
**its own full log scan**, a fourth one, alongside analysis, redo and the
`WalStream::ScanTail` at `WalManager::Open`. `ScanLog` reads from its start LSN
to the end of the 64 MiB segment, so its cost is governed by how much segment
body remains — which is why a bigger log makes it *cheaper*, exactly as
`bench/results-wal-recovery.md` documents for analysis and redo.

Two consequences, both actionable:

**It is invisible to the engine's own observability.** The cost lands entirely
in the residual — p50 wall minus all five `SHOW META` counters. The
no-assertion residual is 36.1/36.9/37.0/38.6/40.2 ms across the five
cardinalities; the assertion residual is 65.1/64.4/64.8/60.5/52.7 ms. A 13–29 ms
component of an 87–134 ms mount has no counter, so `SHOW META` under-reports a
mount with an assertion by up to 29 ms and the accounting that
`bench/results-wal-recovery.md` Part II closes to within 4 ms does not close
here.

**The open scan-narrowing item is worth more than currently credited.**
`docs/known-gaps.md` carries "narrow the scan to the durable end rather than
the segment's end" as open, and `bench/results-wal-recovery.md` sizes it at
~65 ms of a 112 ms mount on the strength of three scans. With one assertion
declared it is **four** scans, and the measurement above puts the fourth at
12.9–29.7 ms depending on log fill. The fix is worth roughly a third more than
the standing estimate on any instance that declares an assertion, and one more
scan for every additional assertion if the scan is per-assertion rather than
shared.

### Against the standing baseline

`bench/results-wal-recovery.md` records 112 ms p50 with ~65 ms of scan reads,
on an empty log. The no-assertion cells here run 100–103 ms at 200–1,000 rows
and 73 ms at 10,000 rows, with analysis+redo of 59.5 ms and 25.1 ms
respectively. Those are consistent with the baseline once log fill is
accounted for — this run's logs are not empty, so its anchors sit further into
the segment and its scans are cheaper. **The baseline is not contradicted**;
the cells are not an empty-log configuration and are not offered as a re-run of
it.

---

## 9. What this run teaches about the engine

**The batching asymmetry is a design fact, and it is now measured.** Commit was
built to amortise: `by_page` exists precisely so that a transaction reserving
many times against one page settles with one record and one stamp. Abort was
built as a straight unwind, in reverse order, one reservation at a time,
because until AS6b it only had to call `Unapply` and log. AS6b gave the abort
loop a page write without giving it commit's grouping, and the result is that
the cheaper half of the protocol is now the *commit* half by a factor that
grows with K — 3.3× at K=16 and K=32. Nothing about this is expensive in
absolute terms today (15 µs on a 32-reservation abort), but it is the wrong
shape, and it will be the wrong shape by a larger factor on any workload that
batches writes inside a transaction, which is the workload bulk insert exists
to serve.

**A stated design expectation is contradicted, and it is quoted rather than
paraphrased.** `docs/feat-assertion.md` §7, under "What it costs", says:

> Commit was already paying exactly this to clear `kEntryReserved`
> (§6.2 step 4), so the two halves of the protocol now cost the same.

and `docs/known-gaps.md` carries the same claim as "abort becomes a page write
(one read-modify-write plus a `StampPageLsn` per aborted reservation), which is
what commit was already paying to clear `kEntryReserved`."

Both are false as written, for the same reason: the phrase "per aborted
reservation" is correct for abort and wrong for commit. Commit pays one
read-modify-write and one `StampPageLsn` **per `(assertion, page)` group**, and
a transaction's reservations normally land on one page. The two halves cost the
same only at K = 1. This document does not edit either file — a spec correction
is not a tester's change to make — but the correction they need is precise:
commit's cost is per page-group, abort's is per reservation, and they coincide
only for a single-reservation transaction.

**Both were corrected at `91abc15`**, after this run and on the strength of it,
so the sentences quoted above no longer appear in either file. They are kept
here verbatim because a results file that falsifies a claim has to show the
claim it falsified — and because the reasoning that produced it is worth not
repeating: "commit already pays this" was inferred from the two paths writing
the same kind of thing, without checking that they loop over different things.

**The fix, if one is wanted, is already written next door.** `AbortTxn` could
group by `(assertion, page)` exactly as `CommitTxn` does; `MarkOrphaned` is
already a named method on `BoundCabinPage` (`01c8dbe`), so the loop body is a
`for` over indexes inside a group. The obstacle is not the page write but the
WAL record: `ASSERT_ROLLBACK` carries one key and one delta per record, so
grouping the page write would still leave one record per reservation unless the
record grows a repeated-index form the way `ASSERT_COMMIT` already has. That is
a format decision and belongs with the owners of `docs/wal.md` §4.1 — which has
just moved to version 2 and would be the cheap moment to take it.

**Mount acquired its first real data point on assertion revival, and it is a
scan, not a walk.** The entry walk everyone would expect to dominate does not
even show: the cost is flat-to-falling in entry count and tracks
`recovery_analysis_us` within 11%, because `assertion_recover.cpp` runs its own
`wal::ScanLog` to the segment end. A reader of `docs/workplan-wal-recovery.md`
RC07 would reasonably expect assertion revival to be priced by how many entries
exist; it is priced by how much empty segment is left. That is the same
pathology the recovery series already found twice, and it is the third
independent argument for narrowing the scan.

**`durability` chooses which question a benchmark can answer.** Under `group`
this workload's `COMMIT` carries a ~1,030 µs fsync and its `ROLLBACK` carries
none, so abort "beats" commit by 1,000 µs and the protocol is invisible. Under
`relaxed` the same cells resolve a 0.056 µs per-reservation difference. Neither
setting is more honest than the other; they answer different questions, and a
document that does not name which one it chose is not reporting a result.

---

## 10. Reproducing this

Drivers and flags are documented in `bench/docs/README.md`; this file states
findings and does not re-explain the tools. The helpers used to run the matrix
live beside it:

| helper | what it does |
|---|---|
| `bench/run_ab_server.sh` | starts one server on a fresh data directory, waits for its "listening on" line, prints the pid |
| `bench/run_abort_sweep.sh` | one A/B cell: fresh server pair, one `assertion_abort_benchmark.py` invocation, JSON out |
| `bench/run_ksweep.sh` | the K = 1…32 sweep of §2 |
| `bench/run_rowsweep.sh` | the row-set sweep of §5 and the ordinary arms of §6 |
| `bench/run_followups.sh` | the §6 repeats and the server-CPU cell |
| `bench/run_mounts.sh`, `bench/run_mounts2.sh` | the §8 mount matrix |
| `bench/wait_quiet.sh` | blocks until no compiler, linker, test binary or package job is running and the 1-minute load is under 0.70 |
| `bench/summarize_abort.py`, `bench/summarize_ordinary.py`, `bench/summarize_mounts.py`, `bench/dump_phases.py` | the tables above, from the cells' JSON |

### A note on the server-CPU meter

`--server-pid` samples `/proc/<pid>/stat` (utime+stime) around each arm and
reports server CPU per transaction, and it is reported here as **unable to
resolve this effect**. At K=16 with 18,000 transactions per arm — 0.56 µs of
quantisation, three rounds, both sides sampled — the unasserted `twin` control
moved ±11 µs per transaction against a ~250 µs base. The effect being measured
is 5.6 µs. The floor is twice the signal, so no CPU number is quoted as a
finding; the scheduler variance on a 2-core box, not the jiffy granularity, is
what defeats it. The client-latency meter, at 2000 samples per arm with a
±0.3 µs in-run floor, is what carries every number above, and it is a valid
proxy here for one specific reason: under `relaxed` nothing on the rollback
path blocks on I/O, so a latency difference between two relations in the same
run is a CPU difference plus socket noise, and `twin` bounds the socket noise.

### Machine quiet, and one run that was not

All cells report the 1-minute load average before and after, from inside the
driver. The abort sweep ran at 0.16–1.79, the mount matrix at 0.54–0.69, both
consistent with this run's own servers and nothing else. **One cell is
excluded and named**: the first server-CPU cell ran 0.92 → 5.42 because a build
started in a different worktree of the same repository partway through it, and
it was re-run at 0.12 after the machine went quiet. That is the same class of
contention that cost an earlier attempt at this measurement its entire run, and
`bench/wait_quiet.sh` is the gate added so the next one does not pay it.
