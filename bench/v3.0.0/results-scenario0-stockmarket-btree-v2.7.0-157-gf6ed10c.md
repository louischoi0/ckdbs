# Scenario 0 (stock market) — the BTREE re-baseline of `f6ed10c`

**This is the comparator every later scenario0 number reads against**, and
it exists because the AL-S8 file beside it can no longer be one. AS-Q6's
mark of 2026-09-08 (`instructions/v3.0.0/raft-marks-2026-09-08-as-q6.md`)
switched `tools/scenario0_stockmarket.py`'s `trades` and
`user_periodic_profit` from `HEAP` to `BTREE`, because SUS-1 refuses
`CREATE TABLE … HEAP` and measurement is BTREE only for now; the same
mark ordered `f6ed10c` re-measured with the changed driver so a baseline
of the new shape exists. That is this run.

`results-scenario0-stockmarket-v2.7.0-157-gf6ed10c.md` — the AL-S8 file —
keeps its numbers and is **history**. A number here is never a delta
against it (`bench/README.md`); §7 states the one bounded reading of the
two side by side and forbids its reuse.

**The engine is byte-identical to AL-S8's.** Same commit, same binary,
same `sha256`. Only the driver moved.

## 1. Stamp

| Field | Value |
|---|---|
| Date/time | 2026-09-08, 04:16:59–04:19:35 UTC (per-cell times in §2) |
| Worktree | **none** — run from the main checkout on `main` at `cd5c60e` (`v2.7.0-285-gcd5c60e`), which is where the changed drivers are. The write-up is on `asq6-btree-baseline` |
| Engine commit measured | `f6ed10c`, `git describe --tags` = `v2.7.0-157-gf6ed10c` |
| Driver commit | `cd5c60e`. `git diff f6ed10c..cd5c60e -- tools/scenario0_stockmarket.py` is 49 lines: the two `SCHEMA` storage words, the phase-detail string, the docstring's storage paragraph, two `--help`/footer sentences. **No change to the workload's shape, its arguments or its statement mix** |
| Tree cleanliness | Clean; nothing was built during the run |
| Binary provenance | `/home/cdkbs/bench-runs/asq6-btree-f6ed10c/kds_server`, `sha256 2ab1960bc056e7cc5c59be4946a2cf1250b4e65b941934c96ebf736e80435af3` — **the same hash AL-S8's own stamp records**, copied from `/home/cdkbs/bench-runs/al-s8-f6ed10c/kds_server` into this run's directory and started from the copy in every cell (rule 3). Not rebuilt: a rebuild of `f6ed10c` would be a different binary with the same source, and the archived one is provably what AL-S8 measured |
| Device | `/home/cdkbs`, `ext4`, `/dev/root` (`df -T`), 52% used. Not tmpfs |
| Build type | Release, built at `f6ed10c` on 2026-09-03 (source mtime `01:17:56 UTC`, after that commit) |
| Host | 8 logical CPUs, AMD EPYC 9V74, 1 socket × 4 cores × 2 threads/core — the AL-S8 host, same shape |
| Server config (common) | `log_level = warn`, `placement = namespace` (shipped default), `checkpoint_interval_ms` default, auth and TLS off. Varied: `cores`, `durability`, `peer_listeners` (`on` only at `cores = 8`) |

## 2. What was run, and in what order

**AL-S8's own interleave**, so ordering is not a variable between the two
runs: `s0-c1-g`, `s2-c8-s`, `s2-c1-s`, `s2-c1-g`, `s0-c8-g`, `s0-c1-s`,
`s2-c8-g`, `s0-c8-s`. The scenario2 half is
`results-scenario2-freight-btree-v2.7.0-157-gf6ed10c.md`. Every cell got
a fresh data file and a fresh server process from the hashed copy, and
the data file was deleted after it.

Arguments, fixed across all four cells and **identical to AL-S8's**:
`--users 100 --accounts-per-user 3 --assets 30 --traders 8
--txn-per-user 50 --verify 200 --seed 1 --sync` — a work target of 5,000
committed business transactions, 2 `INSERT trades` + 2 `UPDATE accounts`
each, 625 per trader. `--profit` at its default, on.

| Cell | Port | Precheck (UTC) | `/proc/loadavg` (1/5/15) | Build check |
|---|---|---|---|---|
| `s0-c1-g` (cores=1, group) | 15600 | 04:16:59 | 0.20 / 0.12 / 0.22 | `pgrep -a -f 'cc1plus\|cmake --build\|ctest'`: none |
| `s0-c8-g` (cores=8, group) | 15601 | 04:18:10 | 1.03 / 0.41 / 0.32 | none |
| `s0-c1-s` (cores=1, strict) | 15602 | 04:18:18 | 0.94 / 0.41 / 0.32 | none |
| `s0-c8-s` (cores=8, strict) | 15603 | 04:19:09 | 1.43 / 0.61 / 0.39 | none |

**The one-minute load rises through the run** — 0.20 to 1.43 — because
each cell's own server is what the previous cell left in the average, not
because anything else ran. The build check is empty at every cell. AL-S8's
own loads were 0.12–0.73 by the same measure; §7 says why that matters
and what it does not license.

`placement = namespace` with no `CREATE NAMESPACE` in the driver means
every relation is core 0's, at `cores = 1` and `cores = 8` alike. The
`cores = 8` cells therefore measure eight reactors over one owner, not
eight owners — AL-S8's own choice, kept so the pair is comparable.

## 3. TPS

Committed business transactions over the wall time they took.

| Cell | cores | durability | TPS | committed | torn |
|---|---|---|---|---|---|
| `s0-c1-g` | 1 | group | **750.6** | 5,000 | 0 |
| `s0-c8-g` | 8 | group | **794.3** | 5,000 | 0 |
| `s0-c1-s` | 1 | strict | **212.1** | 5,000 | 0 |
| `s0-c8-s` | 8 | strict | **213.0** | 5,000 | 0 |

Every cell reached its full 5,000-transaction target with zero torn
statements and zero error replies on every phase.

**Durability is the axis that matters and it is unchanged in kind**:
group is 3.5× strict at one core (750.6 / 212.1) and 3.7× at eight
(794.3 / 213.0). Going from one core to eight buys +5.8% under group and
+0.4% under strict — which at `placement = namespace` is what it should
be, since the extra reactors own nothing.

## 4. Percentiles

### `txn` — the measured unit (2 INSERT + 2 UPDATE, one round trip per statement)

| Cell | ops | p0 | p25 | p50 | p95 | p99 | max (µs) |
|---|---|---|---|---|---|---|---|
| `s0-c1-g` | 5,000 | 8,600.0 | 9,959.1 | 10,304.7 | 12,327.5 | 17,037.9 | 37,255.8 |
| `s0-c8-g` | 5,000 | 8,249.4 | 9,300.7 | 9,522.1 | 11,648.0 | 16,607.7 | 82,284.9 |
| `s0-c1-s` | 5,000 | 4,337.9 | 31,627.0 | 34,019.4 | 66,598.4 | 73,956.8 | 120,724.9 |
| `s0-c8-s` | 5,000 | 18,883.9 | 30,540.2 | 38,212.0 | 45,222.9 | 53,656.8 | 148,745.4 |

### `trade-insert` (the relation that changed class) and `account-update` (the one that did not) — p50/p99, µs

| Cell | trade-insert p50 | trade-insert p99 | account-update p50 | account-update p99 |
|---|---|---|---|---|
| `s0-c1-g` | 2,541.9 | 4,867.0 | 2,550.2 | 4,717.9 |
| `s0-c8-g` | 2,356.9 | 5,338.5 | 2,362.8 | 5,036.3 |
| `s0-c1-s` | 8,439.9 | 18,858.4 | 8,381.3 | 19,123.2 |
| `s0-c8-s` | 9,150.7 | 15,238.7 | 9,096.5 | 15,469.3 |

**The two are still statistically the same statement**, to within 0.4% at
p50 in every cell — which is the finding AL-S8 recorded and which the
storage change did not disturb. A clustered-btree append and an in-place
btree overwrite cost the same here because both are dominated by the
durability wait (§5), not by the page work that differs between them.

`profit-scan` and `profit-insert` ran in every cell (250, 249, 350 and
300 ops) and are in the archive; they are the reporting process, not the
measured unit.

## 5. Wait breakdown

Rule 3: name each wait and give it a share, or say it does not apply. No
`--server-log` was used, for AL-S8's stated reason, so the breakdown is
derived from the phases.

| Wait | Estimate | How |
|---|---|---|
| **Durability** | dominant: ~5,900 µs of `trade-insert`'s 8,440 µs strict p50 at `cores = 1`, falling to ~0 of its 2,542 µs group p50 | the group-vs-strict delta at fixed cores, cleanest at `cores = 1` where no peer-attach path is also in the number |
| **Page work — the descent and its splits** | not separable from the number, and **smaller than this run can resolve**: `trade-insert` (btree append) and `account-update` (btree overwrite by pk) differ by under 0.4% at p50 in all four cells, so whatever the append's split share is, it is inside that | the two phases' p50s, cell by cell |
| **Peer attach / cross-core** | ~0 by construction | `placement = namespace` puts every relation on core 0; no statement crosses an owner, and the `cores = 8` cells confirm it by moving throughput 5.8% and 0.4% rather than by a factor |
| **Client and driver** | present but not isolated | eight trader processes over loopback; the same in both runs and in every cell |

## 6. Correctness

`--verify 200` read back 200 account balances in every cell: **all
match**, all four cells. `torn = 0` and `rolled_back = 0` everywhere.
Zero error replies on every phase of every cell — no lease-refill
`TXN_CONFLICT`, which is what `placement = namespace` predicts, since no
relation is peer-owned.

## 7. What this run says — and the one comparison it permits once

**As a baseline it says what AL-S8's said, with the storage class
changed**: durability dominates, cores buy little at
`placement = namespace`, and insert and update cost the same.

**The comparison against AL-S8, stated once and not to be reused.** Same
binary, same host, same arguments, same cell order; the driver's storage
word is the only deliberate variable. So the two runs price *the driver
change*, and nothing else deliberate:

| Cell | AL-S8 (HEAP trades) | this run (BTREE trades) | difference |
|---|---|---|---|
| `s0-c1-g` | 700.9 | 750.6 | +7.1% |
| `s0-c8-g` | 754.7 | 794.3 | +5.2% |
| `s0-c1-s` | 192.6 | 212.1 | +10.1% |
| `s0-c8-s` | 207.2 | 213.0 | +2.8% |

**Every cell is faster with a btree `trades`, which is the opposite of
what the driver's own schema comment predicted** — it said a heap tail
append is "exactly right" for an insert-only relation and a tree "would
buy a descent and a split per row".

**And the attribution is not available from this pair.**
`account-update` — whose relation was `BTREE` in *both* runs and whose
statement did not change at all — also moved, from 2,703.6 µs to
2,550.2 µs at p50 in `s0-c1-g`, −5.7%. A statement that did not change
cannot have been sped up by the change, so at least part of every number
in the table above is the five days between the runs: a different kernel
page cache state, a different data file, a different process history.
**This table therefore prices the driver change and the interval
together, and separates neither.** No later document may cite it as the
cost of a btree append, and none may use it as a comparator: the
comparator is this file's §3, against which the next scenario0 run is
read.

What would separate them is an interleaved A/B of the two drivers against
this one binary in one session — which is a measurement nobody has
ordered and which prices a driver rather than the engine.

## 8. Noise floor

Not established for this run: each cell ran once, and the eight-cell
matrix is 2.5 minutes end to end, so a repeat was affordable and was not
taken because the mark asked for a baseline rather than for a
distribution. The rising one-minute load across the cells (§2) is the
one visible source of drift, and the strict pair — 212.1 at `cores = 1`
under load 0.94 and 213.0 at `cores = 8` under load 1.43 — is the closest
thing to a floor this run contains: two cells whose configurations differ
and whose throughput does not.

**A repeat is the first thing to add** if any later delta against this
file lands inside a few percent.

**Amended the same day by AM-S6's four-sample A/B**
(`results-am-s6-m1-baseline-v2.7.0-286-g1e7148f.md` §4, §9), which ran
this arm four times per cell: the `cores = 1` cells are stable to 2–7%,
so §3's two one-core numbers are sound to about that. **The `cores = 8`
cells are bimodal on this host** — `s0-c8-g` on this very binary ran
469, 781, 551 and 554 TPS across four runs — so **§3's `s0-c8-g` of
794.3 is a single draw from the high mode, not a central value**, and
`s0-c8-s`'s 213.0 is one draw of a distribution whose spread was 4.1%
there and reached 66.6% on the sibling cell. No eight-core delta against
this file is meaningful below roughly 50%. Nothing above is re-measured
or edited; this note is what a later reader needs to not misuse it.

Archive: `bench/v3.0.0/archive/scenario0-btree-v2.7.0-157-gf6ed10c/` —
per-cell JSON, driver stdout, prechecks, the run script and the timeline.
