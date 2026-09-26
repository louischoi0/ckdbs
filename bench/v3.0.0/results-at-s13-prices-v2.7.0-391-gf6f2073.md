# AT-S13 — the prices, at `v2.7.0-391-gf6f2073`

`instructions/v3.0.0/workorder-at-m3-uniformity.md`, row AT-S13, reopened on
the operator's word 2026-09-26 with its three cells marked before any ran.
Run 2026-09-26 on `at-s13-prices`.

**The short answer, cell by cell.**

- **Cell 1, E7.** On one `cores = 8` server, 8 sessions spread by the kernel
  pay **+29 to +42 µs an update** against the same sessions pinned to one
  core (~360 µs) - and the controls pay a penalty of the same order (a read
  +41 and +45 µs, `SHOW META` +14 and +31 µs), so it is mostly the
  arrangement and not the write path. Spread is also where two
  cores meet on one row: **3.9–5.7% of hot-row updates are refused
  `TXN_CONFLICT retryable=1`**, against **zero** pinned and zero in AO-S7 -
  some share of them a holder still in flight on a peer, which the
  row-level wait cannot see (below). At 32 sessions the Python driver is the
  bottleneck on both arms and nothing but the refusals resolves clearly
  above an 80 µs floor.
- **Cell 2, the `cores = 1` A/B.** What AT added at one core is **not
  resolvable end to end** (every arm inside ±10 µs of ~360 µs, the controls
  moving as much), and in the engine's own time per statement it is
  **+0.1 and −0.3 µs of ~16.5 µs in two runs out of three**, inside the
  ~0.5 µs `df8cc5f` moves between its own runs; the third read +3.6 µs, all
  of it one ~0.37 s stall - a stall head shows in three of its seven runs
  and `df8cc5f` in none of its three (below).
- **Cell 3, D20.** Omitted-pk inserts spread against pinned: **+9.9 and
  +12.3 µs**, below the same runs' `ping` delta (+14.3, +31.3 µs). The one
  mark and the tail leaf, together, cost less than this cell resolves -
  at most ~12 µs on a ~360 µs insert, ~3%.

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
files at start, `log_level = warn`, otherwise defaults. The head `cores = 1`
server (15591) served the proof before cell 2, so in cell 2 its data file
carries the proof's dropped relation and `df8cc5f`'s does not. At `warn`
every refused statement writes one log line, which only cell 1's spread
`update-hot` arm pays.

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
| `ping` (control) | −11.8 µs | |
| `select-hot` (control) | +1.5 µs | |

The write arms are inside the floor. The driver is measuring the server -
but `ping` moved −11.8 µs between two arrangements that are one, which is
what a single arm's p50 can swing on this host and bounds every delta
below that is smaller than it.

## Cell 1 — E7: spread against pinned, one `cores = 8` server

| run | sessions | `update-hot` | `update-disjoint` | `insert-omitted` | `select-hot` | `ping` | hot refusals (spread) |
|---|---|---|---|---|---|---|---|
| A | 8 | +33.6 µs | +34.4 µs | +9.9 µs | +40.8 µs | +14.3 µs | 627 / 16,000 |
| B | 8 | +41.9 µs | +28.8 µs | +12.3 µs | +45.3 µs | +31.3 µs | 913 / 16,000 |
| C | 32 | +51.5 µs | −26.7 µs | −34.5 µs | +89.2 µs | +49.8 µs | 1,889 / 32,000 |

spread − pinned p50, µs. Pinned refused nothing in any run.

**The update arms cost about what the controls cost.** `select-hot` takes
no borrow and `ping` resolves no relation, and they pay +14 to +45 µs at 8
sessions; the updates' +29 to +42 µs sits inside that range (above `ping`
in run A, either side of it in run B, below `select-hot` in both). So most of the
spread penalty is not the lock family, the page latch or the mark; it is
what one statement costs on a core that serves one or two sessions. The
breakdown says part of what that is:

| core | statements | engine µs / statement | idle blocks / statement | idle µs / statement |
|---|---|---|---|---|
| run A, core 0 (pinned + spread) | 120,002 | 18.2 µs | 0.98 | 60.5 µs |
| run A, cores 1, 4, 7 (one spread session each) | 12,001 each | 24.4–24.8 µs | 1.43 | 858–862 µs |
| run B, core 0 | 108,002 | 21.7 µs | 0.98 | 72.0 µs |
| run B, cores 1, 2, 4, 6, 7 | 12,001 each | 53.5–57.6 µs | 1.43–1.44 | 862–870 µs |
| proof, `cores = 1` | 192,002 | 16.4 µs | not counted | 26.8 µs |

**Every core of the `cores = 8` server blocks before nearly every
statement** - a lonely core 1.43 times a statement, the busy core 0 0.98
times - because this driver keeps too few statements in flight to leave a
reactor anything queued. The block count does not separate the
arrangements. `cores = 1` shows none because the count is taken only where
a reactor has a waker (`scheduler.cpp`, `may_sleep`) and a single-core
server has none; its idle µs says it blocks too. The idle-µs column is the
whole run's sleep over the core's own statements - most of it the other
pool's turn - and is no per-statement wait.

What does separate them is engine time. In run A a lonely core runs a
statement **1.3× slower** than the busy core (24.4–24.8 µs against 18.2):
+6 µs of a ~34 µs penalty, the rest outside every counter here (the wake of
a sleeping reactor thread is the candidate, not shown). **Run B's
53.5–57.6 µs is not a per-statement cost**: one ~0.4 s stall landed on
every core at once during the spread pool's `update-disjoint` (max latency
413 ms; 350–400 ms of extra polled time on each core against run A's rate,
core 0 included) and is smeared across 12,001 polls. No kick was involved:
`wakes_received` is 0 on every core.

**The refusals are the price that is the write's own - part of it.** Two
cores now reach one row at once, which AO-S7 could not produce. The write
check refuses a row whose header names a writer the statement's snapshot
cannot see: one that committed after it, or one still in flight (`txn.md`
§5; `READ COMMITTED` is the default, and KDS aborts retryably rather than
re-reading). An in-flight holder on the *same* core is waited for (AO-S3);
one on a *peer* is not, because the row-level wait's predicate is
`IsInFlight`, one core's live set (`NoteBlockingWriter`,
`command_dispatcher.cpp:8076`) - the gap AT-S5e and AT-S5f closed for DDL
and for fk parents and not for a row. The `cores = 8` server's `kds.log`
(archived as `s8-kds.log`) holds 3,422 `was written by` refusals, which do not say
which of the two they are, and 7 `is held by`, which are that gap by
construction. How much of the 4–6% a cross-core wait would convert, these
runs do not say; `docs/inflight/known-gaps.md` (Locks) carries the gap. Pinned, the reactor serialises the same sessions and
nothing is refused. 3.9% (A), 5.7% (B) and 5.9% (C) of hot-row updates; the
refused statements' latencies are in the arm's percentiles.

**hot − disjoint**, the tuple lock under real contention: spread +3.6 and
+16.3 µs at 8 sessions against floors of 8.8 and 7.6 µs - one run inside its
floor, one run twice it. **Not a consistent finding.** Pinned: +4.4, +3.2 µs
(floors 1.5, 2.3).

**At 32 sessions the driver is the bottleneck**: 1.2–1.3 ms p50 and
17–23k qps on the write arms and `ping` of both pools (`select-hot`
1.6–1.7 ms), the spread floor 80.3 µs. Only the refusals resolve. The
pinned pool's `update-disjoint` carries the same ~0.4 s stall (max 391 ms).

## Cell 2 — the `cores = 1` A/B: `df8cc5f` against `f6f2073`

End to end, head − pre-AT p50, µs:

| run | `update-hot` | `update-disjoint` | `insert-omitted` | `select-hot` | `ping` |
|---|---|---|---|---|---|
| 1 | +2.5 µs | +4.4 µs | −2.2 µs | +4.8 µs | −9.9 µs |
| 2 | +2.3 µs | +0.2 µs | −3.8 µs | +2.1 µs | +11.8 µs |
| 3 | +5.6 µs | −0.1 µs | −0.6 µs | −0.9 µs | +6.5 µs |

Every arm inside the controls' own swing. The engine's time per statement,
from `SHOW META` (`sched_foreground_polled_us / polls`, 96,001 statements
per server per run):

| run | head | pre-AT | head − pre-AT |
|---|---|---|---|
| 1 | 16.4 µs | 16.3 µs | +0.1 µs |
| 2 | 20.2 µs | 16.6 µs | **+3.6 µs** |
| 3 | 16.5 µs | 16.8 µs | −0.3 µs |

**Run 2 is kept, not dropped, and its +3.6 µs is not a per-statement
cost**: head's `update-disjoint` arm in that run took 1.23 s against
~0.85 s for every other write arm of cell 2 (13,051 qps against ~18,900),
its maximum latency 371 ms, and head's extra polled time over the run
(+3.8 µs × 96,001 ≈ 365 ms) is that one stall. **It is not a one-off**: the
same ~0.4 s stall, in the same arm, is in cell 1's run B (spread, 413 ms)
and run C (pinned, 391 ms) - three of head's seven runs, none of
`df8cc5f`'s three. Its cause is not identified here. It sits inside
foreground polled time and, in run B, on every core at once, which an
instance-wide hold would produce and so would a WAL write held up by device
writeback under the log's latch; three against zero is too few to call it
AT's, and too many to call it noise.
**The reading, per statement: runs 1 and 3 put what AT added at one core
at +0.1 and −0.3 µs of 16.5 µs, inside the ~0.5 µs `df8cc5f` moves between
its own runs (16.3–16.8 µs) - under ~3% in the engine, unresolvable end to
end.** G2 holds per statement to what this host can see, and the bound includes the Cabin store's partition `std::mutex`, which is taken
at `cores = 1` (`known-gaps.md`). The stall is open, and `known-gaps.md` carries it.

## Cell 3 — D20: omitted-pk inserts, spread against pinned

From cell 1's runs: `insert-omitted` spread − pinned **+9.9 µs (A), +12.3 µs
(B)**, where `ping` moved +14.3 and +31.3. The arm prices the one row-id mark
(`sys.tables`, bumped in place under its page latch by every core since
AT-S10b) and the btree's tail leaf together - a named pk takes the same row
(`AdmitExplicitRowId`), so no arm in this engine separates them, and
`SHOW META` attributes no page-latch wait. **Less than this cell resolves:
at most ~12 µs on a ~360 µs insert (~3%)**, which is also the proof run's
own `ping` swing (−11.8 µs), and no refusal: inserts do not conflict.

## Against this engine's previous number

AO-S7's C3 at `v2.7.0-304-g5e94dc8`, the same driver before this stage's
additions (its update arms run first in both), `relaxed`, 8 sessions on
`cores = 8`: `update-hot` 440.0 / 428.9 µs p50, `update-disjoint` 436.7 /
427.9, zero refusals. Every write then ran on its relation's owner core, and
a session elsewhere shipped it there - **that engine's spread arm was the
routed arm E7 asks about.** Now:

| p50 µs, 8 sessions | AO-S7 (routed by ownership) | AT-S13 spread | AT-S13 pinned |
|---|---|---|---|
| `update-hot` | 440.0 µs / 428.9 µs | 399.4 µs / 409.8 µs | 365.8 µs / 367.9 µs |
| `update-disjoint` | 436.7 µs / 427.9 µs | 395.8 µs / 393.5 µs | 361.4 µs / 364.7 µs |
| refusals (hot) | 0 | 3.9% / 5.7% | 0 |

Running where the session landed is **19–41 µs faster** than the shipped
path was, raw. `ping`, which neither engine routes, moved too (AO-S7
389.6 / 385.0 µs, AT-S13 spread 367.3 / 385.3 µs), so the host alone
accounts for up to ~22 µs of it; net of that, run for run, the update arms
are ~18–35 µs faster. Pinning every session to one core is faster again - at the price,
spread, of hot-row refusals the routed engine never produced. This is an
observation about two engines, not a price of the route: 87 commits and 17
days apart, a different interleave partner (AO-S7's second server, AT-S13's
pinned pool), and AO-S7's JSON does not record which cores its sessions
landed on.

## What this decides, and what it does not

**E7 is not decided here** - AR2 §7 says it is read off the cell, not
decided by it. What the cell says for the operator to read: *local* costs
~30–40 µs a statement on this host when sessions are thin per core (a
lonely-reactor cost, paid by reads too) - ~10% of this Python driver's
~360 µs round trip, and more than the engine's own 16–22 µs of the
statement, so a faster client would see a larger share - and 4–6% hot-row
refusals under this driver's contention, part of which is the per-core
row-level wait above rather than anything spreading requires; *affinity*
(every session on one core, which is what the pinned pool emulates)
serialises and avoids both, at the price of one reactor's capacity, which
this driver cannot load. The pinned pool is not a route: a statement
shipped from its session's core, as AO-S7's engine did, keeps that core's
wake and adds a hop, and measured slower than today's spread (above). No
owner core or route exists since AT-S9, so either default would be a
build, not a setting. One host, `relaxed` durability, one client process.

**D20's row-id half stands** as the operator ruled it (invariant 11 over a
per-core cache): its price is under what this host resolves.

**G2 holds per statement to within ~0.5 µs (~3%)** at one core; the ~0.4 s
stall head shows in three runs of seven is open.

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

| arm | s1: p0 / p25 / p50 / p99, throughput, errors | s1-pinned: p0 / p25 / p50 / p99, throughput, errors |
|---|---|---|
| `update-hot` | 41.2 µs / 243.9 µs / 360.8 µs / 1190.5 µs, 18829 qps, 0 errors | 45.6 µs / 247.0 µs / 363.8 µs / 1168.6 µs, 18854 qps, 0 errors |
| `update-disjoint` | 47.6 µs / 249.0 µs / 364.1 µs / 1160.7 µs, 18636 qps, 0 errors | 38.7 µs / 249.2 µs / 362.1 µs / 1168.8 µs, 18735 qps, 0 errors |
| `update-disjoint-again` | 44.7 µs / 251.2 µs / 360.6 µs / 1141.4 µs, 18953 qps, 0 errors | 46.3 µs / 247.6 µs / 359.4 µs / 1133.7 µs, 18943 qps, 0 errors |
| `insert-omitted` | 36.1 µs / 246.5 µs / 357.3 µs / 1153.3 µs, 18777 qps, 0 errors | 38.7 µs / 245.5 µs / 358.9 µs / 1124.1 µs, 19025 qps, 0 errors |
| `select-hot` | 57.6 µs / 322.0 µs / 459.1 µs / 1379.9 µs, 15256 qps, 0 errors | 60.2 µs / 322.4 µs / 457.6 µs / 1376.3 µs, 15379 qps, 0 errors |
| `ping` | 36.3 µs / 217.9 µs / 343.1 µs / 1192.4 µs, 19614 qps, 0 errors | 39.8 µs / 239.3 µs / 354.9 µs / 1123.9 µs, 19288 qps, 0 errors |

**`c1-s8-n8.json`** - sessions 8, 2000 ops/arm/session, 4 blocks; load before `2.68 1.51 0.58 1/297 478050`, after `2.73 1.56 0.60 2/297 478442`
- `s8`: cores 0, 1, 3, 4, 7
- `s8-pinned`: cores 0, pinned to 0 (103 connections opened)

| arm | s8: p0 / p25 / p50 / p99, throughput, errors | s8-pinned: p0 / p25 / p50 / p99, throughput, errors |
|---|---|---|
| `update-hot` | 52.3 µs / 277.9 µs / 399.4 µs / 1261.1 µs, 17230 qps, 627 errors | 36.8 µs / 248.4 µs / 365.8 µs / 1174.8 µs, 18669 qps, 0 errors |
| `update-disjoint` | 48.3 µs / 272.8 µs / 395.8 µs / 1194.5 µs, 17540 qps, 0 errors | 38.6 µs / 247.2 µs / 361.4 µs / 1148.1 µs, 18927 qps, 0 errors |
| `update-disjoint-again` | 47.4 µs / 266.9 µs / 387.0 µs / 1182.6 µs, 17673 qps, 0 errors | 41.0 µs / 246.5 µs / 362.9 µs / 1158.6 µs, 18673 qps, 0 errors |
| `insert-omitted` | 36.7 µs / 252.5 µs / 367.0 µs / 1192.5 µs, 18180 qps, 0 errors | 44.7 µs / 244.0 µs / 357.1 µs / 1197.9 µs, 18700 qps, 0 errors |
| `select-hot` | 62.8 µs / 349.9 µs / 496.3 µs / 1479.3 µs, 14107 qps, 0 errors | 55.2 µs / 321.8 µs / 455.5 µs / 1379.2 µs, 15418 qps, 0 errors |
| `ping` | 39.4 µs / 240.4 µs / 367.3 µs / 1231.5 µs, 18225 qps, 0 errors | 39.8 µs / 238.6 µs / 353.0 µs / 1111.7 µs, 19391 qps, 0 errors |

**`c1-s8-n8-repeat.json`** - sessions 8, 2000 ops/arm/session, 4 blocks; load before `2.14 1.56 0.65 1/309 480034`, after `2.26 1.60 0.68 1/297 480425`
- `s8`: cores 0, 1, 2, 4, 5, 6, 7
- `s8-pinned`: cores 0, pinned to 0 (68 connections opened)

| arm | s8: p0 / p25 / p50 / p99, throughput, errors | s8-pinned: p0 / p25 / p50 / p99, throughput, errors |
|---|---|---|
| `update-hot` | 45.9 µs / 279.3 µs / 409.8 µs / 1260.4 µs, 16662 qps, 913 errors | 38.2 µs / 249.5 µs / 367.9 µs / 1182.8 µs, 18463 qps, 0 errors |
| `update-disjoint` | 45.5 µs / 270.1 µs / 393.5 µs / 1225.5 µs, 12183 qps, 0 errors | 38.1 µs / 248.9 µs / 364.7 µs / 1172.7 µs, 18733 qps, 0 errors |
| `update-disjoint-again` | 47.1 µs / 275.3 µs / 401.1 µs / 1213.4 µs, 17234 qps, 0 errors | 49.6 µs / 250.3 µs / 367.0 µs / 1153.3 µs, 18595 qps, 0 errors |
| `insert-omitted` | 35.9 µs / 254.0 µs / 371.3 µs / 1170.7 µs, 17913 qps, 0 errors | 43.6 µs / 245.4 µs / 359.0 µs / 1200.2 µs, 18847 qps, 0 errors |
| `select-hot` | 59.5 µs / 357.6 µs / 505.7 µs / 1482.1 µs, 13918 qps, 0 errors | 62.5 µs / 324.6 µs / 460.4 µs / 1346.1 µs, 15359 qps, 0 errors |
| `ping` | 44.7 µs / 258.7 µs / 385.3 µs / 1210.7 µs, 17821 qps, 0 errors | 39.5 µs / 240.0 µs / 354.0 µs / 1140.0 µs, 19262 qps, 0 errors |

**`c1-s8-n32.json`** - sessions 32, 1000 ops/arm/session, 4 blocks; load before `2.12 1.48 0.59 1/313 478474`, after `2.53 1.61 0.66 1/302 480020`
- `s8`: cores 0, 1, 2, 3, 4, 5, 6, 7
- `s8-pinned`: cores 0, pinned to 0 (218 connections opened)

| arm | s8: p0 / p25 / p50 / p99, throughput, errors | s8-pinned: p0 / p25 / p50 / p99, throughput, errors |
|---|---|---|
| `update-hot` | 49.7 µs / 825.0 µs / 1331.2 µs / 4543.2 µs, 19991 qps, 1889 errors | 37.2 µs / 753.4 µs / 1279.7 µs / 4796.1 µs, 20691 qps, 0 errors |
| `update-disjoint` | 48.3 µs / 799.3 µs / 1255.2 µs / 4480.2 µs, 20360 qps, 0 errors | 48.3 µs / 750.2 µs / 1281.9 µs / 4896.4 µs, 16891 qps, 0 errors |
| `update-disjoint-again` | 45.2 µs / 813.2 µs / 1335.5 µs / 4347.3 µs, 20418 qps, 0 errors | 46.2 µs / 756.5 µs / 1279.4 µs / 4726.9 µs, 21622 qps, 0 errors |
| `insert-omitted` | 52.3 µs / 784.1 µs / 1253.1 µs / 4299.8 µs, 20687 qps, 0 errors | 47.7 µs / 753.7 µs / 1287.6 µs / 6046.1 µs, 16861 qps, 0 errors |
| `select-hot` | 61.7 µs / 1067.3 µs / 1711.5 µs / 5691.0 µs, 15693 qps, 0 errors | 61.3 µs / 995.5 µs / 1622.3 µs / 5158.2 µs, 17140 qps, 0 errors |
| `ping` | 45.9 µs / 771.6 µs / 1228.0 µs / 4376.3 µs, 21210 qps, 0 errors | 38.5 µs / 725.9 µs / 1178.2 µs / 4033.5 µs, 22595 qps, 0 errors |

**`c2-ab.json`** - sessions 8, 2000 ops/arm/session, 4 blocks; load before `1.26 1.43 0.65 1/301 480471`, after `1.37 1.45 0.66 2/309 480868`
- `head`: cores 0
- `preat`: cores 0

| arm | head: p0 / p25 / p50 / p99, throughput, errors | preat: p0 / p25 / p50 / p99, throughput, errors |
|---|---|---|
| `update-hot` | 44.5 µs / 246.6 µs / 362.8 µs / 1154.3 µs, 18867 qps, 0 errors | 46.2 µs / 246.5 µs / 360.3 µs / 1142.4 µs, 19074 qps, 0 errors |
| `update-disjoint` | 34.9 µs / 249.4 µs / 365.6 µs / 1110.5 µs, 19054 qps, 0 errors | 44.8 µs / 248.2 µs / 361.2 µs / 1165.8 µs, 18550 qps, 0 errors |
| `update-disjoint-again` | 50.3 µs / 248.0 µs / 362.6 µs / 1152.4 µs, 18810 qps, 0 errors | 38.0 µs / 246.7 µs / 360.7 µs / 1173.7 µs, 18622 qps, 0 errors |
| `insert-omitted` | 43.4 µs / 244.5 µs / 359.3 µs / 1153.2 µs, 18956 qps, 0 errors | 43.6 µs / 245.5 µs / 361.5 µs / 1153.5 µs, 18956 qps, 0 errors |
| `select-hot` | 58.8 µs / 324.6 µs / 463.8 µs / 1400.3 µs, 15118 qps, 0 errors | 57.1 µs / 320.7 µs / 459.0 µs / 1352.6 µs, 15382 qps, 0 errors |
| `ping` | 40.0 µs / 236.5 µs / 354.1 µs / 1156.9 µs, 19215 qps, 0 errors | 39.3 µs / 247.2 µs / 364.0 µs / 1151.2 µs, 19064 qps, 0 errors |

**`c2-ab-repeat.json`** - sessions 8, 2000 ops/arm/session, 4 blocks; load before `1.16 1.40 0.65 1/296 480881`, after `1.36 1.43 0.67 1/296 481278`
- `head`: cores 0
- `preat`: cores 0

| arm | head: p0 / p25 / p50 / p99, throughput, errors | preat: p0 / p25 / p50 / p99, throughput, errors |
|---|---|---|
| `update-hot` | 43.5 µs / 248.9 µs / 364.0 µs / 1178.5 µs, 18679 qps, 0 errors | 45.8 µs / 248.5 µs / 361.7 µs / 1166.2 µs, 18735 qps, 0 errors |
| `update-disjoint` | 41.5 µs / 249.6 µs / 364.8 µs / 1184.2 µs, 13051 qps, 0 errors | 45.4 µs / 249.5 µs / 364.6 µs / 1151.1 µs, 18894 qps, 0 errors |
| `update-disjoint-again` | 37.1 µs / 249.5 µs / 363.9 µs / 1161.9 µs, 18653 qps, 0 errors | 49.4 µs / 250.2 µs / 363.7 µs / 1162.5 µs, 18500 qps, 0 errors |
| `insert-omitted` | 35.6 µs / 244.5 µs / 357.3 µs / 1149.8 µs, 18967 qps, 0 errors | 43.0 µs / 245.9 µs / 361.1 µs / 1141.1 µs, 18773 qps, 0 errors |
| `select-hot` | 62.1 µs / 324.1 µs / 459.4 µs / 1358.9 µs, 15346 qps, 0 errors | 59.8 µs / 318.5 µs / 457.3 µs / 1393.8 µs, 15354 qps, 0 errors |
| `ping` | 36.2 µs / 246.4 µs / 364.5 µs / 1118.9 µs, 19045 qps, 0 errors | 40.9 µs / 232.1 µs / 352.7 µs / 1150.1 µs, 19320 qps, 0 errors |

**`c2-ab-third.json`** - sessions 8, 2000 ops/arm/session, 4 blocks; load before `1.15 1.39 0.67 3/305 481307`, after `1.58 1.47 0.70 1/305 481704`
- `head`: cores 0
- `preat`: cores 0

| arm | head: p0 / p25 / p50 / p99, throughput, errors | preat: p0 / p25 / p50 / p99, throughput, errors |
|---|---|---|
| `update-hot` | 44.8 µs / 251.7 µs / 365.3 µs / 1135.9 µs, 18732 qps, 0 errors | 42.9 µs / 246.9 µs / 359.7 µs / 1183.3 µs, 18710 qps, 0 errors |
| `update-disjoint` | 47.6 µs / 248.3 µs / 359.8 µs / 1136.8 µs, 18934 qps, 0 errors | 40.7 µs / 247.9 µs / 359.9 µs / 1197.6 µs, 18730 qps, 0 errors |
| `update-disjoint-again` | 44.2 µs / 247.7 µs / 363.6 µs / 1138.9 µs, 18821 qps, 0 errors | 44.7 µs / 244.9 µs / 362.2 µs / 1160.4 µs, 18850 qps, 0 errors |
| `insert-omitted` | 46.1 µs / 247.9 µs / 359.8 µs / 1155.6 µs, 18854 qps, 0 errors | 39.9 µs / 245.3 µs / 360.4 µs / 1165.4 µs, 18557 qps, 0 errors |
| `select-hot` | 53.5 µs / 327.4 µs / 460.9 µs / 1379.1 µs, 15125 qps, 0 errors | 50.6 µs / 324.9 µs / 461.8 µs / 1352.9 µs, 15303 qps, 0 errors |
| `ping` | 41.2 µs / 237.7 µs / 356.4 µs / 1168.5 µs, 19017 qps, 0 errors | 39.9 µs / 223.4 µs / 349.9 µs / 1178.4 µs, 19506 qps, 0 errors |
