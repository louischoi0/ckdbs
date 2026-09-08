# Scenario 2 (freight) — the BTREE re-baseline of `f6ed10c`

**The comparator every later scenario2 number reads against.** AS-Q6's
mark of 2026-09-08 (`instructions/v3.0.0/raft-marks-2026-09-08-as-q6.md`)
switched `tools/scenario2_freight.py`'s `freights` and `charges` from
`HEAP` to `BTREE` — SUS-1 refuses the word and measurement is BTREE only
for now — and ordered `f6ed10c` re-measured with the changed driver.
`results-scenario2-freight-v2.7.0-157-gf6ed10c.md` keeps its numbers and
is history; §7 is the one bounded reading of the two together, and it is
not reusable.

The scenario0 half of this baseline is
`results-scenario0-stockmarket-btree-v2.7.0-157-gf6ed10c.md`; its stamp
carries the binary provenance and the host, which are this run's.

## 1. Stamp

| Field | Value |
|---|---|
| Date/time | 2026-09-08, 04:17:07–04:19:09 UTC (per-cell times in §2) |
| Worktree | **none** — the main checkout on `main` at `cd5c60e`; the write-up is on `asq6-btree-baseline` |
| Engine commit measured | `f6ed10c`, `git describe --tags` = `v2.7.0-157-gf6ed10c` |
| Driver commit | `cd5c60e`. `git diff f6ed10c..cd5c60e -- tools/scenario2_freight.py` is 18 lines: the two `SCHEMA` storage words, the schema comment and one report-footer sentence. **No change to the workload, its arguments or its statement mix** |
| Binary provenance | The same hashed copy as the scenario0 half: `sha256 2ab1960b…`, matching AL-S8's own stamp; started from the copy in every cell |
| Device | `/home/cdkbs`, `ext4`, `/dev/root`, 52% used. Not tmpfs |
| Host | 8 logical CPUs, AMD EPYC 9V74, 1 socket × 4 cores × 2 threads/core — the AL-S8 host |
| Server config (common) | `log_level = warn`, `placement = namespace`, defaults otherwise. Varied: `cores`, `durability`, `peer_listeners` (`on` only at `cores = 8`) |

## 2. What was run, and in what order

AL-S8's interleave, shared with the scenario0 half: `s0-c1-g`, `s2-c8-s`,
`s2-c1-s`, `s2-c1-g`, `s0-c8-g`, `s0-c1-s`, `s2-c8-g`, `s0-c8-s`. Each
cell: fresh data file, fresh server, then `--schema-only`, `--load-only`
and the measured run against the same `--suffix`, which is this driver's
documented shape.

Arguments, fixed across all four cells and **identical to AL-S8's**:
`--organizations 300 --ships 30 --operations 300 --cargos 4000
--bookers 8 --bookings 3000 --verify 100 --seed 1 --sync`, with `--txn`,
`--contend` and `--manifest` at their defaults (on). A target of 3,000
committed bookings, 375 per booker, contended.

| Cell | Port | Precheck (UTC) | `/proc/loadavg` (1/5/15) | Build check |
|---|---|---|---|---|
| `s2-c8-s` (cores=8, strict) | 15607 | 04:17:07 | 0.27 / 0.13 / 0.22 | `pgrep -a -f 'cc1plus\|cmake --build\|ctest'`: none |
| `s2-c1-s` (cores=1, strict) | 15606 | 04:17:33 | 1.05 / 0.33 / 0.29 | none |
| `s2-c1-g` (cores=1, group) | 15604 | 04:17:51 | 1.04 / 0.36 / 0.30 | none |
| `s2-c8-g` (cores=8, group) | 15605 | 04:18:43 | 1.12 / 0.49 / 0.35 | none |

## 3. TPS

| Cell | cores | durability | TPS | committed | op-update conflicts | org-update conflicts |
|---|---|---|---|---|---|---|
| `s2-c1-g` | 1 | group | **589.0** | 3,000 | 9 | 2 |
| `s2-c8-g` | 8 | group | **308.5** | 3,000 | 7 | 4 |
| `s2-c1-s` | 1 | strict | **553.0** | 3,000 | 5 | 1 |
| `s2-c8-s` | 8 | strict | **294.0** | 3,000 | 9 | 5 |

Every cell committed its full 3,000 bookings, `retries/booking 0.00`.

**Cores cost, durability barely does — the inversion of scenario0, and it
survives the storage change.** Going from one core to eight halves
throughput (589.0 → 308.5 group, −48%; 553.0 → 294.0 strict, −47%), while
group over strict buys 6.5% at one core and 4.9% at eight. §5 puts the
number behind it.

## 4. Percentiles

### `booking` — the whole eight-statement transaction, client-perceived

| Cell | ops | p0 | p25 | p50 | p95 | p99 | max (µs) |
|---|---|---|---|---|---|---|---|
| `s2-c1-g` | 3,356 | 592.4 | 10,293.4 | 11,912.9 | 16,728.4 | 33,370.8 | 81,566.2 |
| `s2-c8-g` | 3,322 | 949.5 | 20,517.6 | 23,565.1 | 34,029.2 | 55,423.6 | 84,389.0 |
| `s2-c1-s` | 3,340 | 717.5 | 10,601.0 | 12,937.0 | 18,635.5 | 26,512.3 | 75,840.7 |
| `s2-c8-s` | 3,306 | 766.6 | 18,689.7 | 23,142.7 | 34,239.2 | 56,167.8 | 154,562.7 |

`ops` exceeds 3,000 because a rejected or conflicted attempt still opens
and measures a span before `finish()` closes it; the committed count is
3,000 in every cell (§3).

### `commit` and `operation-update` — p50/p99, µs

| Cell | commit p50 | commit p99 | operation-update p50 | operation-update p99 |
|---|---|---|---|---|
| `s2-c1-g` | 1,623.5 | 4,387.1 | 209.6 | 2,073.2 |
| `s2-c8-g` | 5,976.1 | 16,180.4 | 471.7 | 5,366.3 |
| `s2-c1-s` | 1,451.6 | 4,811.0 | 273.9 | 3,186.9 |
| `s2-c8-s` | 6,443.0 | 14,823.7 | 254.1 | 5,567.8 |

### The two relations that changed class — `freight-insert` and `charge-insert`, p50/p99, µs

| Cell | freight-insert p50 | freight-insert p99 | charge-insert p50 | charge-insert p99 |
|---|---|---|---|---|
| `s2-c1-g` | 1,235.2 | 2,975.7 | 263.4 | 2,297.7 |
| `s2-c8-g` | 1,389.5 | 6,486.6 | 1,092.5 | 5,378.1 |
| `s2-c1-s` | 1,318.8 | 4,018.4 | 324.6 | 3,840.1 |
| `s2-c8-s` | 1,387.4 | 6,246.4 | 292.4 | 5,827.4 |

`charge-insert` runs ~5.65× per booking (16,949–17,010 ops against 3,000
bookings) and is the cheapest statement in the transaction at one core —
263 µs at p50 against `freight-insert`'s 1,235 µs, for the same kind of
clustered-btree append into a relation that was `HEAP` five days ago.
Both are far below `commit`, which is the point of §5.

## 5. Wait breakdown

| Wait | Estimate | How |
|---|---|---|
| **`COMMIT` itself** | dominant, and it is what the core count buys: 1,623 µs → 5,976 µs group and 1,452 µs → 6,443 µs strict going one core to eight, **3.7× and 4.4×** | the `commit` phase's own p50, cell by cell — the same measurement AL-S8 used to explain its throughput collapse, and the same conclusion |
| **Durability** | small here, unlike scenario0: group over strict is 6.5% and 4.9% of throughput, and `commit` p50 is *lower* under strict at both core counts | §3 and the `commit` row above |
| **The two changed relations' appends** | ~1,235–1,389 µs (`freight`) and ~263–1,092 µs (`charge`) at p50 — together under a third of a booking's p50 at one core | the phase table in §4 |
| **Contention** | present and small: 9/7/5/9 `operation-update` and 2/4/1/5 `org-update` conflicts per 3,000 bookings, `retries/booking 0.00` | the driver's own error columns |

## 6. Correctness — AL-S8's finding reproduces, and it is not this change's

`--verify 100` runs 400 invariant checks per cell (I1 booked capacity
against the sum of freight volumes, I2 within ship capacity, I3
outstanding against recomputed charges, I4 charge rows against rules).

| Cell | failures / 400 |
|---|---|
| `s2-c1-g` | 27 |
| `s2-c8-g` | 32 |
| `s2-c1-s` | 33 |
| `s2-c8-s` | 24 |

**AL-S8 recorded 18–33 per 400 on the same invariant and called it
orthogonal to that stage.** This run's 24–33 is the same band on the same
engine with a different storage class, which is the strongest statement
available that the finding is neither caused by nor cured by the storage
change — and that it is still open. It belongs to the driver's
lost-update shape or to the engine, and this file does not decide which;
AL-S8's §6 is the analysis and nothing here supersedes it.

## 7. What this run says — and the one comparison it permits once

**As a baseline**: cores cost, durability is nearly free, `COMMIT`
carries the core-count penalty, and the invariant failures persist.

**The comparison against AL-S8, stated once and not to be reused.** Same
binary, same host, same arguments, same order; the storage word is the
only deliberate variable:

| Cell | AL-S8 (HEAP ledgers) | this run (BTREE ledgers) | difference |
|---|---|---|---|
| `s2-c1-g` | 578.4 | 589.0 | +1.8% |
| `s2-c8-g` | 312.7 | 308.5 | −1.3% |
| `s2-c1-s` | 543.8 | 553.0 | +1.7% |
| `s2-c8-s` | 284.7 | 294.0 | +3.3% |

**Two of four up, one down, all inside ±3.3%** — which for this workload
says the storage class of the two append-only ledgers is not what the
number is made of. That is consistent with §5: a booking is eight
statements dominated by its `COMMIT`, and the two appends are a third of
it at one core and less at eight.

**The attribution caveat is the scenario0 half's and applies here
unchanged** (that file's §7): a statement whose class did not change also
moved between the runs, so this table prices the driver change and the
five days between the runs together and separates neither. It is not a
comparator. The comparator is §3 above.

## 8. Noise floor

Not established: one run per cell, and the ±3.3% spread in §7 is inside
what a single unrepeated cell can carry on a host whose one-minute load
moved from 0.27 to 1.12 across the four. **A repeat is owed before any
later delta inside a few percent is believed.** The pair that comes
closest to a floor is `s2-c1-g` and `s2-c1-s` — 589.0 and 553.0 under
loads 1.04 and 1.05, differing by their durability class and 6.5%.

**Amended the same day by AM-S6's four-sample A/B**
(`results-am-s6-m1-baseline-v2.7.0-286-g1e7148f.md` §4, §9), which ran
this arm four times per cell: the two `cores = 1` cells are stable to
3.5–4.1%, so §3's 589.0 and 553.0 are sound to about that. **The two
`cores = 8` cells are not** — `s2-c8-g` on this binary ran 325, 194, 349
and 317 TPS across four runs, a 79.9% spread, and `s2-c8-s` 293–322 —
so §3's 308.5 and 294.0 are single draws and no eight-core delta against
them is meaningful below roughly 50%. Nothing above is re-measured or
edited.

Archive: `bench/v3.0.0/archive/scenario2-btree-v2.7.0-157-gf6ed10c/` —
per-cell JSON, driver stdout and prechecks.
