# AT-S13 — the prices, at `v2.7.0-391-gf6f2073`

`instructions/v3.0.0/workorder-at-m3-uniformity.md`, row AT-S13, reopened on
the operator's word 2026-09-26 with its three cells marked before any ran.
Run 2026-09-26 on `at-s13-prices`.

**The short answer, cell by cell.**

- **Cell 1, E7.** On one `cores = 8` server, 8 sessions spread by the kernel
  pay **+30 to +45 µs a statement** against the same sessions pinned to one
  core (~360 µs) - and the controls, a read and `SHOW META`, pay it too, so
  it is the arrangement and not the write path. Spread is also where two
  cores meet on one row: **3.9–5.7% of hot-row updates are refused
  `TXN_CONFLICT retryable=1`** (first-updater-wins), against **zero** pinned
  and zero in AO-S7. At 32 sessions the Python driver is the bottleneck on
  both arms and nothing resolves above an 80 µs floor except the refusals.
- **Cell 2, the `cores = 1` A/B.** What AT added at one core is **not
  resolvable end to end** (every arm inside ±10 µs of ~360 µs, the controls
  moving as much), and in the engine's own time per statement it is
  **+0.1 and −0.3 µs of ~16.5 µs in two runs out of three**; the third read
  +3.6 µs with a stall inside head's arms (below).
- **Cell 3, D20.** Omitted-pk inserts spread against pinned: **+9.9 and
  +12.3 µs**, below the same runs' `ping` delta (+14.3, +31.3 µs). The one
  mark and the tail leaf, together, cost less than this cell resolves -
  under ~10 µs on a ~360 µs insert, under 3%.

What none of it is: a throughput price. The driver saturates before the
engine does (18–20k qps whatever the arm), so what spreading *buys* - eight
reactors rather than one - is not in these numbers.

---

## Rules (`bench/README.md`)

1. **Release, rebuilt at the measured commit.** `build-release` configured
   and built at `f6f2073` in the worktree; the A arm of cell 2 built from
   `git archive df8cc5f` in a scratch tree. The driver change that lands with
   this stage does not enter either binary.
2. **Block device.** `/home/cdkbs/at-s13` on `/dev/root`, ext4 (`df -T`).
3. **A copy of the binary, hashed.** Every server started from the copy:
   - `kds_server-head` (`f6f2073`) `2e166abd46df27e7d289efb67fc58d504ddc6d7b152fbd230de5e842e5fcb464`
   - `kds_server-preat` (`df8cc5f`) `75d5724072f331b08d1638900938f6dc740f00d9b89bc5ee4ad9997f512de93e`
4. **Host load, per cell.** `/proc/loadavg` and the competing-build `pgrep`
   are in every table's header and every JSON under
   `archive/at-s13-prices-v2.7.0-391-gf6f2073/`. **No competing build in any
   cell**; load 1.2–2.9 (the driver's own threads). One false positive:
   `c2-ab-third.json`'s `competing` lists the invoking shell, whose command
   line carried the `pgrep` pattern as text.
5. **Ports chosen.** 15590 (`cores = 8`, head), 15591 (`cores = 1`, head),
   15592 (`cores = 1`, `df8cc5f`). Not 15432.

Host: 8 CPUs. All three servers `durability = relaxed` (AO-S7: `group`'s
commit is 82–85% of an update and hides what is measured), BTREE, fresh data
files, `log_level = warn`, otherwise defaults.

## The driver, and the proof it ran first

`tools/lock_contention_benchmark.py`, AO-S7's C3 driver, extended at
`f6f2073` with the three things the marks need: `--pin-core C` (a second
pool on the same server whose sessions are all held on core `C`, opened
until each lands there, interleaved block by block with the spread pool),
an `insert-omitted` arm, and a `SHOW META` snapshot per core before and
after the run (the wait breakdown). A driver changed inside a measurement
stage is a driver being measured, so it was proved before it priced
anything (AO-0 item 23's rule): **on the `cores = 1` server spread and pinned
are one arrangement**, and any delta between them is the driver's.

| write arm | spread − pinned p50 (proof) | noise floor |
|---|---|---|
| `update-hot` | −3.0 µs | 3.5 / 2.7 µs |
| `update-disjoint` | +2.0 µs | |
| `insert-omitted` | −1.6 µs | |

Inside the floor. The driver is measuring the server.

## Cell 1 — E7: spread against pinned, one `cores = 8` server

| run | sessions | `update-hot` | `update-disjoint` | `insert-omitted` | `select-hot` | `ping` | hot refusals (spread) |
|---|---|---|---|---|---|---|---|
| A | 8 | +33.6 | +34.4 | +9.9 | +40.8 | +14.3 | 627 / 16,000 |
| B | 8 | +41.9 | +28.8 | +12.3 | +45.3 | +31.3 | 913 / 16,000 |
| C | 32 | +51.5 | −26.7 | −34.5 | +89.2 | +49.8 | 1,889 / 32,000 |

spread − pinned p50, µs. Pinned refused nothing in any run.

**The write arms cost no more than the controls.** `select-hot` takes no
borrow and `ping` resolves no relation, and they pay +14 to +45 µs at 8
sessions - the same range as the writes. So the spread penalty is not the
lock family, the page latch or the mark; it is what one statement costs on
a core that serves one or two sessions. The breakdown says what that is:

| core | statements | engine µs / statement | idle blocks / statement | idle µs / statement |
|---|---|---|---|---|
| run A, core 0 (pinned + spread) | 120,002 | 18.2 | 0.98 | 60.5 |
| run A, cores 1, 4, 7 (one spread session each) | 12,001 each | 24.4–24.8 | 1.43 | 858–862 |
| run B, core 0 | 108,002 | 21.7 | 0.98 | 72.0 |
| run B, cores 1, 2, 4, 6, 7 | 12,001 each | 53.5–57.6 | 1.43–1.44 | 862–870 |
| proof, `cores = 1` | 192,002 | 16.4 | 0.00 | 26.8 |

**A core with one session sleeps before nearly every statement** (1.43 idle
blocks each) and runs the statement **1.3–2.7× slower** than the busy core
(24–58 µs against 18–22) - the cold path of an idle CPU, not a lock. No
kick was involved: `wakes_received` is 0 on every core, the socket's own
readiness ends each block. A busy reactor never blocks (`cores = 1`, 0.00).

**The refusals are the price that is the write's own.** Two cores now reach
one row at once, which AO-S7 could not produce, and an autocommit `UPDATE`
whose snapshot predates the other core's commit is refused retryable by
first-updater-wins. Pinned, the reactor serialises the same sessions and
nothing is refused. 3.9% (A), 5.7% (B) and 5.9% (C) of hot-row updates.

**hot − disjoint**, the tuple lock under real contention: spread +3.6 and
+16.3 µs at 8 sessions against floors of 8.8 and 7.6 µs - one run inside its
floor, one run twice it. **Not a consistent finding.** Pinned: +4.4, +3.2 µs
(floors 1.5, 2.3).

**At 32 sessions the driver is the bottleneck**: ~1.3 ms p50 and ~20k qps on
every arm of both pools, the spread floor 80.3 µs. Only the refusals
resolve.

## Cell 2 — the `cores = 1` A/B: `df8cc5f` against `f6f2073`

End to end, head − pre-AT p50, µs:

| run | `update-hot` | `update-disjoint` | `insert-omitted` | `select-hot` | `ping` |
|---|---|---|---|---|---|
| 1 | +2.5 | +4.4 | −2.2 | +4.8 | −9.9 |
| 2 | +2.3 | +0.2 | −3.8 | +2.1 | +11.8 |
| 3 | +5.6 | −0.1 | −0.6 | −0.9 | +6.5 |

Every arm inside the controls' own swing. The engine's time per statement,
from `SHOW META` (`sched_foreground_polled_us / polls`, 96,001 statements
per server per run):

| run | head | pre-AT | head − pre-AT |
|---|---|---|---|
| 1 | 16.4 µs | 16.3 µs | +0.1 µs |
| 2 | 20.2 µs | 16.6 µs | **+3.6 µs** |
| 3 | 16.5 µs | 16.8 µs | −0.3 µs |

**Run 2 is an outlier and is kept, not dropped**: head's `update-disjoint`
arm in that run fell to 13,051 qps against ~18,900 in every other arm of
every run, which is a stall inside head's share of the run rather than a
per-statement cost - and the other two runs, bracketing it, agree to 0.3 µs.
**The reading: what AT added at one core is under ~0.3 µs of 16.5 µs (2%) in
the engine and unresolvable end to end.** G2 holds to what this host can
see. It includes the Cabin store's partition `std::mutex`, which is taken
at `cores = 1` (`known-gaps.md`): whatever it costs is inside this bound.

## Cell 3 — D20: omitted-pk inserts, spread against pinned

From cell 1's runs: `insert-omitted` spread − pinned **+9.9 µs (A), +12.3 µs
(B)**, where `ping` moved +14.3 and +31.3. The arm prices the one row-id mark
(`sys.tables`, bumped in place under its page latch by every core since
AT-S10b) and the btree's tail leaf together - a named pk takes the same row
(`AdmitExplicitRowId`), so no arm in this engine separates them, and
`SHOW META` attributes no page-latch wait. **Less than this cell resolves:
under ~10 µs on a ~360 µs insert**, and no refusal: inserts do not conflict.

## Against this engine's previous number

AO-S7's C3 at `v2.7.0-304-g5e94dc8`, same driver, `relaxed`, 8 sessions on
`cores = 8`: `update-hot` 440.0 / 428.9 µs p50, `update-disjoint` 436.7 /
427.9, zero refusals. Every write then ran on its relation's owner core, and
a session elsewhere shipped it there - **that engine's spread arm was the
routed arm E7 asks about.** Now:

| p50 µs, 8 sessions | AO-S7 (routed by ownership) | AT-S13 spread | AT-S13 pinned |
|---|---|---|---|
| `update-hot` | 440.0 / 428.9 | 399.4 / 409.8 | 365.8 / 367.9 |
| `update-disjoint` | 436.7 / 427.9 | 395.8 / 393.5 | 361.4 / 364.7 |
| refusals (hot) | 0 | 3.9% / 5.7% | 0 |

Running where the session landed is **~30–40 µs faster** than the shipped
path was, and pinning every session to one core is faster again - at the
price, spread, of hot-row refusals the routed engine never produced.

## What this decides, and what it does not

**E7 is not decided here** - AR2 §7 says it is read off the cell, not
decided by it. What the cell says for the operator to read: *local* costs
~10% of a statement on this host when sessions are thin per core (a
lonely-reactor cost, paid by reads too), and 4–6% hot-row refusals under
real contention; *routed-to-one-core* serialises and avoids both, at the
price of one reactor's capacity, which this driver cannot load. No routing
code exists since AT-S9, so a *routed* default would be a build, not a
setting.

**D20's row-id half stands** as the operator ruled it (invariant 11 over a
per-core cache): its price is under what this host resolves.

**G2 holds to ~0.3 µs** at one core.

Nothing here is an overhead measurement of any AT stage: those rows say
"overhead not measured" and stay saying it.

---

## Raw tables

Every cell, every arm, both pools, p0 / p25 / p50 / p99 in µs, throughput
and errors. The JSON beside them carries each pool's cores and the per-core
`SHOW META` before and after; `breakdown.py` and `tables.py` reproduce
the two derived tables.

**`prove.json`** - sessions 8, 2000 ops/arm/session, 4 blocks; load before `2.90 1.48 0.56 1/301 477643`, after `2.91 1.53 0.58 2/304 478042`
- `s1`: cores 0
- `s1-pinned`: cores 0, pinned to 0 (8 connections opened)

| arm | s1 p0 / p25 / p50 / p99 µs, qps, err | s1-pinned p0 / p25 / p50 / p99 µs, qps, err |
|---|---|---|
| `update-hot` | 41.2 / 243.9 / 360.8 / 1190.5, 18829, 0 | 45.6 / 247.0 / 363.8 / 1168.6, 18854, 0 |
| `update-disjoint` | 47.6 / 249.0 / 364.1 / 1160.7, 18636, 0 | 38.7 / 249.2 / 362.1 / 1168.8, 18735, 0 |
| `update-disjoint-again` | 44.7 / 251.2 / 360.6 / 1141.4, 18953, 0 | 46.3 / 247.6 / 359.4 / 1133.7, 18943, 0 |
| `insert-omitted` | 36.1 / 246.5 / 357.3 / 1153.3, 18777, 0 | 38.7 / 245.5 / 358.9 / 1124.1, 19025, 0 |
| `select-hot` | 57.6 / 322.0 / 459.1 / 1379.9, 15256, 0 | 60.2 / 322.4 / 457.6 / 1376.3, 15379, 0 |
| `ping` | 36.3 / 217.9 / 343.1 / 1192.4, 19614, 0 | 39.8 / 239.3 / 354.9 / 1123.9, 19288, 0 |

**`c1-s8-n8.json`** - sessions 8, 2000 ops/arm/session, 4 blocks; load before `2.68 1.51 0.58 1/297 478050`, after `2.73 1.56 0.60 2/297 478442`
- `s8`: cores 0, 1, 3, 4, 7
- `s8-pinned`: cores 0, pinned to 0 (103 connections opened)

| arm | s8 p0 / p25 / p50 / p99 µs, qps, err | s8-pinned p0 / p25 / p50 / p99 µs, qps, err |
|---|---|---|
| `update-hot` | 52.3 / 277.9 / 399.4 / 1261.1, 17230, 627 | 36.8 / 248.4 / 365.8 / 1174.8, 18669, 0 |
| `update-disjoint` | 48.3 / 272.8 / 395.8 / 1194.5, 17540, 0 | 38.6 / 247.2 / 361.4 / 1148.1, 18927, 0 |
| `update-disjoint-again` | 47.4 / 266.9 / 387.0 / 1182.6, 17673, 0 | 41.0 / 246.5 / 362.9 / 1158.6, 18673, 0 |
| `insert-omitted` | 36.7 / 252.5 / 367.0 / 1192.5, 18180, 0 | 44.7 / 244.0 / 357.1 / 1197.9, 18700, 0 |
| `select-hot` | 62.8 / 349.9 / 496.3 / 1479.3, 14107, 0 | 55.2 / 321.8 / 455.5 / 1379.2, 15418, 0 |
| `ping` | 39.4 / 240.4 / 367.3 / 1231.5, 18225, 0 | 39.8 / 238.6 / 353.0 / 1111.7, 19391, 0 |

**`c1-s8-n8-repeat.json`** - sessions 8, 2000 ops/arm/session, 4 blocks; load before `2.14 1.56 0.65 1/309 480034`, after `2.26 1.60 0.68 1/297 480425`
- `s8`: cores 0, 1, 2, 4, 5, 6, 7
- `s8-pinned`: cores 0, pinned to 0 (68 connections opened)

| arm | s8 p0 / p25 / p50 / p99 µs, qps, err | s8-pinned p0 / p25 / p50 / p99 µs, qps, err |
|---|---|---|
| `update-hot` | 45.9 / 279.3 / 409.8 / 1260.4, 16662, 913 | 38.2 / 249.5 / 367.9 / 1182.8, 18463, 0 |
| `update-disjoint` | 45.5 / 270.1 / 393.5 / 1225.5, 12183, 0 | 38.1 / 248.9 / 364.7 / 1172.7, 18733, 0 |
| `update-disjoint-again` | 47.1 / 275.3 / 401.1 / 1213.4, 17234, 0 | 49.6 / 250.3 / 367.0 / 1153.3, 18595, 0 |
| `insert-omitted` | 35.9 / 254.0 / 371.3 / 1170.7, 17913, 0 | 43.6 / 245.4 / 359.0 / 1200.2, 18847, 0 |
| `select-hot` | 59.5 / 357.6 / 505.7 / 1482.1, 13918, 0 | 62.5 / 324.6 / 460.4 / 1346.1, 15359, 0 |
| `ping` | 44.7 / 258.7 / 385.3 / 1210.7, 17821, 0 | 39.5 / 240.0 / 354.0 / 1140.0, 19262, 0 |

**`c1-s8-n32.json`** - sessions 32, 1000 ops/arm/session, 4 blocks; load before `2.12 1.48 0.59 1/313 478474`, after `2.53 1.61 0.66 1/302 480020`
- `s8`: cores 0, 1, 2, 3, 4, 5, 6, 7
- `s8-pinned`: cores 0, pinned to 0 (218 connections opened)

| arm | s8 p0 / p25 / p50 / p99 µs, qps, err | s8-pinned p0 / p25 / p50 / p99 µs, qps, err |
|---|---|---|
| `update-hot` | 49.7 / 825.0 / 1331.2 / 4543.2, 19991, 1889 | 37.2 / 753.4 / 1279.7 / 4796.1, 20691, 0 |
| `update-disjoint` | 48.3 / 799.3 / 1255.2 / 4480.2, 20360, 0 | 48.3 / 750.2 / 1281.9 / 4896.4, 16891, 0 |
| `update-disjoint-again` | 45.2 / 813.2 / 1335.5 / 4347.3, 20418, 0 | 46.2 / 756.5 / 1279.4 / 4726.9, 21622, 0 |
| `insert-omitted` | 52.3 / 784.1 / 1253.1 / 4299.8, 20687, 0 | 47.7 / 753.7 / 1287.6 / 6046.1, 16861, 0 |
| `select-hot` | 61.7 / 1067.3 / 1711.5 / 5691.0, 15693, 0 | 61.3 / 995.5 / 1622.3 / 5158.2, 17140, 0 |
| `ping` | 45.9 / 771.6 / 1228.0 / 4376.3, 21210, 0 | 38.5 / 725.9 / 1178.2 / 4033.5, 22595, 0 |

**`c2-ab.json`** - sessions 8, 2000 ops/arm/session, 4 blocks; load before `1.26 1.43 0.65 1/301 480471`, after `1.37 1.45 0.66 2/309 480868`
- `head`: cores 0
- `preat`: cores 0

| arm | head p0 / p25 / p50 / p99 µs, qps, err | preat p0 / p25 / p50 / p99 µs, qps, err |
|---|---|---|
| `update-hot` | 44.5 / 246.6 / 362.8 / 1154.3, 18867, 0 | 46.2 / 246.5 / 360.3 / 1142.4, 19074, 0 |
| `update-disjoint` | 34.9 / 249.4 / 365.6 / 1110.5, 19054, 0 | 44.8 / 248.2 / 361.2 / 1165.8, 18550, 0 |
| `update-disjoint-again` | 50.3 / 248.0 / 362.6 / 1152.4, 18810, 0 | 38.0 / 246.7 / 360.7 / 1173.7, 18622, 0 |
| `insert-omitted` | 43.4 / 244.5 / 359.3 / 1153.2, 18956, 0 | 43.6 / 245.5 / 361.5 / 1153.5, 18956, 0 |
| `select-hot` | 58.8 / 324.6 / 463.8 / 1400.3, 15118, 0 | 57.1 / 320.7 / 459.0 / 1352.6, 15382, 0 |
| `ping` | 40.0 / 236.5 / 354.1 / 1156.9, 19215, 0 | 39.3 / 247.2 / 364.0 / 1151.2, 19064, 0 |

**`c2-ab-repeat.json`** - sessions 8, 2000 ops/arm/session, 4 blocks; load before `1.16 1.40 0.65 1/296 480881`, after `1.36 1.43 0.67 1/296 481278`
- `head`: cores 0
- `preat`: cores 0

| arm | head p0 / p25 / p50 / p99 µs, qps, err | preat p0 / p25 / p50 / p99 µs, qps, err |
|---|---|---|
| `update-hot` | 43.5 / 248.9 / 364.0 / 1178.5, 18679, 0 | 45.8 / 248.5 / 361.7 / 1166.2, 18735, 0 |
| `update-disjoint` | 41.5 / 249.6 / 364.8 / 1184.2, 13051, 0 | 45.4 / 249.5 / 364.6 / 1151.1, 18894, 0 |
| `update-disjoint-again` | 37.1 / 249.5 / 363.9 / 1161.9, 18653, 0 | 49.4 / 250.2 / 363.7 / 1162.5, 18500, 0 |
| `insert-omitted` | 35.6 / 244.5 / 357.3 / 1149.8, 18967, 0 | 43.0 / 245.9 / 361.1 / 1141.1, 18773, 0 |
| `select-hot` | 62.1 / 324.1 / 459.4 / 1358.9, 15346, 0 | 59.8 / 318.5 / 457.3 / 1393.8, 15354, 0 |
| `ping` | 36.2 / 246.4 / 364.5 / 1118.9, 19045, 0 | 40.9 / 232.1 / 352.7 / 1150.1, 19320, 0 |

**`c2-ab-third.json`** - sessions 8, 2000 ops/arm/session, 4 blocks; load before `1.15 1.39 0.67 3/305 481307`, after `1.58 1.47 0.70 1/305 481704`
- `head`: cores 0
- `preat`: cores 0

| arm | head p0 / p25 / p50 / p99 µs, qps, err | preat p0 / p25 / p50 / p99 µs, qps, err |
|---|---|---|
| `update-hot` | 44.8 / 251.7 / 365.3 / 1135.9, 18732, 0 | 42.9 / 246.9 / 359.7 / 1183.3, 18710, 0 |
| `update-disjoint` | 47.6 / 248.3 / 359.8 / 1136.8, 18934, 0 | 40.7 / 247.9 / 359.9 / 1197.6, 18730, 0 |
| `update-disjoint-again` | 44.2 / 247.7 / 363.6 / 1138.9, 18821, 0 | 44.7 / 244.9 / 362.2 / 1160.4, 18850, 0 |
| `insert-omitted` | 46.1 / 247.9 / 359.8 / 1155.6, 18854, 0 | 39.9 / 245.3 / 360.4 / 1165.4, 18557, 0 |
| `select-hot` | 53.5 / 327.4 / 460.9 / 1379.1, 15125, 0 | 50.6 / 324.9 / 461.8 / 1352.9, 15303, 0 |
| `ping` | 41.2 / 237.7 / 356.4 / 1168.5, 19017, 0 | 39.9 / 223.4 / 349.9 / 1178.4, 19506, 0 |
