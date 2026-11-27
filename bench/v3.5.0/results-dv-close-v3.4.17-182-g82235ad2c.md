# DV close: the unreachable-page census and the mount's Waystone walk, base against closing commit

## Stamp

| item | value |
|---|---|
| executed | 2026-11-27 about 05:48-05:57 UTC (build about 1.5 min, volume loading about 4 min, measured cells under 2 min each; no quiet-gate hold) |
| branch | `worktree-dv-unreachable-page-reclaim` |
| arm A (base) | `630ecc359`, `v3.4.17-178-g630ecc359`, the commit DV opened from; `git archive` extracted outside the repo and built Release there; sha256 `f2d8a751d587b91b2caa46346c60c4c5e1f68d1a067f47dc5a63be1dbb59bac7`; source mtime 2026-11-27 00:48:54 -0500 |
| arm B (closing) | built from `82235ad2c` (`v3.4.17-182-g82235ad2c`), tree clean of source (only `CLAUDE.md` modified, a doc); sha256 `57cf898bc390a6ead0e9b169c729960de6bf74410e23a389a9a97820006c0019`; source binary mtime 2026-11-27 00:48:55 -0500, i.e. 65 s before the worktree's HEAD moved to `8204fbec2` (`v3.4.17-183-g8204fbec2`) at 00:50:00. `git diff 82235ad2c 8204fbec2 -- src include` is one file, `include/kds/server/page_reclaim.hpp`, 5 insertions and 1 deletion, a comment (per the coordinator's note; the stat confirms one header). **B's binary is `82235ad2c`; the engine source at HEAD equals it but for that header.** The file is named by `82235ad2c`. |
| build | Release for both (`-DCMAKE_BUILD_TYPE=Release`), target `kds_server`; Debug not measured |
| binaries | each copied to `/home/ukjin/.cache/dv-close-run/kds_server.{A,B}` (hashed above); every server started from a copy (cells 1-2 copy again into their workdir) |
| device | `/dev/mapper/fedora-root`, xfs (`df -T /home/ukjin/.cache/dv-close-run`); data files and WAL there, not tmpfs (`/tmp` is tmpfs on this host and was not used for data) |
| server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 131072`, `log_level = warn` (cells 1-2) / `info` (cells 3-4), ports 31990/31991/31995, never 15432; server `taskset -c 0-7` (P cores), driver CPU 8 (cells 1-2) or 9 (cells 3-4); one connection an arm; one server at a time is busy |
| host state | `/proc/loadavg` before cells: cells 1-2 3.22 / 2.71 / 2.37; cell 3 4.67 / 3.78 / 2.84; cell 4 4.38 / 3.73 / 2.83 (other sessions' servers were running on the host: load 4-5 includes them); `pgrep -f "cc1plus|cmake --build|ctest"` found nothing before any cell and `busy` was 0 in all 220 per-block samples of cells 1-2 (raw in the archive) |
| prior number | none for these shapes: see "Prior numbers" |
| PostgreSQL floor | not measured for this shape |
| drivers | the `tools/` drivers do not cover DV's shapes (a keyed INSERT, CREATE/DROP, mount time), so the measurement used scratch drivers modelled on `tools/du_close.py` (same `Arm`, `run_block`, schedstat CPU, rotated arm order); they are archived beside the raw output and are **not** unmodified `tools/` scripts, which `bench/README.md` names as the rule; the cost is that the driver is part of what was measured, equally in both arms |
| raw output | `bench/v3.5.0/archive/dv-close-v3.4.17-182-g82235ad2c/` (`ab.json`, `mounts-c3.json`, `mounts-c4.json`, drivers, `build.out`) |

## Thesis

1. **DV costs nothing a statement can see.** Keyed INSERT, point SELECT and
   point UPDATE by tuple key, and a CREATE TABLE + DROP TABLE pair, a
   BEGIN; CREATE TABLE; ROLLBACK triple, read -3.3% to -0.9% in server CPU
   B against A, every one inside the noise floor (paired per-round sd 2.0-3.5%,
   within-arm range 3.6-14% for the point shapes). They are not gains.
2. **The mount's new walk of each opted Waystone pattern's stale directory is
   not visible at 16 patterns.** Mount to listening on a 1 GiB volume holding
   16 opted shapes: A 79.2 ms mean (median 80.0 ms), B 81.3 ms (median 78.7
   ms), 12 mounts each, ranges 67.7-95.2 ms and 75.4-92.4 ms; the difference
   is a fraction of either spread.
3. **The census is a linear read of the volume, priced at about 0.5 to 0.7 s
   a GiB, and it is off by default.** On B, `volume_census_at_mount = true`
   adds 179 ms at 0.25 GiB, 660 ms at 1 GiB and about 1.9 s at 4 GiB of
   allocated pages. Mount without it reads 64-123 ms at every size
   (medians 75.4, 75.8 and 117.7 ms; the mount reads no data page), so the
   key is the whole cost and the default pays nothing.

## Cell 1-2: server CPU, interleaved A/B

Method: both servers up, a fresh data file each; per shape 11 rounds, the arm
order rotated each round (A B, B A, ...), a fixed count of units per block
(10,000 statements for the three point shapes, 300 pairs/triples for the DDL
shapes) after a warm block; CPU is the server's on-CPU time summed over its
threads (`/proc/<pid>/task/*/schedstat`) per unit. Relations: `s` (1,000 rows
`(id, owner, v)`), `i` (keyed INSERT target, ids ascending). Row-count
mapping: the shapes touch one row by tuple key and do not scale with rows
(a fixed cost); the relation `s` is 1,000 rows only (200/1K/10K sweep not
run: see "What was cut"). QPS is derived, 1,000,000 / p50 µs (serial one
connection).

| shape | A CPU median (µs/unit) | B CPU median (µs/unit) | delta | paired delta mean ± sd | A range over 11 rounds | B range over 11 rounds | A stmts/s (derived) | B stmts/s (derived) |
|---|---|---|---|---|---|---|---|---|
| keyed INSERT | 6.38 µs | 6.32 µs | -0.9 % | -1.1 % ± 2.5 % | 6.21-7.10 µs | 6.12-6.88 µs | 51,020 stmts/s | 51,282 stmts/s |
| point SELECT (control) | 6.59 µs | 6.46 µs | -2.0 % | -2.7 % ± 2.0 % | 6.49-6.90 µs | 6.30-6.59 µs | 42,735 stmts/s | 43,103 stmts/s |
| point UPDATE | 7.51 µs | 7.26 µs | -3.3 % | -2.7 % ± 2.2 % | 7.36-7.63 µs | 7.14-7.63 µs | 47,847 stmts/s | 48,309 stmts/s |
| CREATE TABLE + DROP TABLE, a pair | 562.6 µs | 555.3 µs | -1.3 % | -2.8 % ± 3.4 % | 92.0-1,036.9 µs (grows by round) | 80.6-1,028.5 µs | 1,663 pairs/s | 1,604 pairs/s |
| BEGIN; CREATE TABLE; ROLLBACK | 265.1 µs | 257.7 µs | -2.8 % | +2.8 % ± 3.5 % | 177.1-326.9 µs (grows by round) | 184.5-329.1 µs | 3,449 triples/s | 3,370 triples/s |

The noise floor comes from the point SELECT, which no DV path is on: -2.0 %
(paired -2.7 %), with within-arm ranges of 4.5-6.2 %; the INSERT's own range
is 12-14 %. **The floor is taken as 4 %: no delta in the table reaches it and
none is a finding.** The point SELECT, INSERT and UPDATE rows moving the same
way (all slightly below A) is not read as an engine gain; that it is this
A/B pair's placement (A's server started first) is unverified. Paired deltas are taken at the same
round index in both arms, which also holds the catalog size equal (next
paragraph).

**A DDL pair grows with the catalog, the same in both arms.** CPU of a
CREATE+DROP pair rises by about 94 µs each 300 pairs (92 to 1,037 µs over 11
rounds, 3,300 pairs, in A and in B alike), roughly 0.3 µs per previously
dropped table per pair. The reading, not traced: the tombstoned oids and
their catalog rows are never reclaimed and some path (the one-name-one-row
check, CT7, or the catalog walk) is linear in them. A rollback triple grows more slowly (177 to 327 µs,
about 15 µs a round). This is the engine's behaviour at both commits, not DV's;
it is why only same-round pairs are compared and why the ranges above are wide.
DV's recorded roots add nothing the pair can see: B's pair at round 0 is 80.6
µs against A's 92.0 µs and at round 10 1,028.5 µs against 1,036.9 µs.

### Latency (client round trip, all rounds pooled)

| shape | arm | n | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| keyed INSERT | A | 110,000 | 18.0 µs | 18.9 µs | 19.6 µs | 20.6 µs | 24.4 µs |
| keyed INSERT | B | 110,000 | 17.9 µs | 18.7 µs | 19.5 µs | 20.4 µs | 27.5 µs |
| point SELECT | A | 110,000 | 21.8 µs | 22.6 µs | 23.4 µs | 24.2 µs | 25.3 µs |
| point SELECT | B | 110,000 | 21.5 µs | 22.4 µs | 23.2 µs | 24.0 µs | 24.9 µs |
| point UPDATE | A | 110,000 | 19.2 µs | 20.3 µs | 20.9 µs | 22.0 µs | 23.5 µs |
| point UPDATE | B | 110,000 | 18.7 µs | 20.0 µs | 20.7 µs | 21.7 µs | 23.1 µs |
| CREATE+DROP pair | A | 3,300 | 82.4 µs | 329.4 µs | 601.2 µs | 1,143.9 µs | 1,208.0 µs |
| CREATE+DROP pair | B | 3,300 | 74.4 µs | 333.7 µs | 623.4 µs | 1,119.2 µs | 1,169.9 µs |
| BEGIN; CREATE; ROLLBACK | A | 3,300 | 204.1 µs | 246.2 µs | 289.9 µs | 365.0 µs | 451.0 µs |
| BEGIN; CREATE; ROLLBACK | B | 3,300 | 208.5 µs | 254.2 µs | 296.7 µs | 367.9 µs | 485.0 µs |

(The pair rows' percentiles mix every round, so they carry the catalog growth
above; the INSERT p99 differs by 3.1 µs on a 110,000-row sample against p95s
2% apart and is not read as a delta.)

### Wait breakdown (`sys.wait_stats`, the whole run, both arms' own server)

A latency here is the client's round trip around about 6-7 µs of server CPU,
so the rest is socket and driver: a point statement is 20-23 µs round trip
against 6.5-7.5 µs of server on-CPU time. The server's own waits over the
whole run, cumulative (core 0, 1 core):

| event | A count | A total | B count | B total |
|---|---|---|---|---|
| `cpu.on_cpu` | - | 3,697,281 µs | - | 3,673,292 µs (-0.6 %) |
| `sched.run_queue` | 352,922 | 78,253 µs | 352,926 | 32,545 µs |
| `sysio.wal_write` | 95 | 44,883 µs | 40 | 11,707 µs |
| `sysio.checkpoint` | 8 | 7,422 µs | 8 | 7,343 µs |
| `commit.wal_flush`, `commit.ceiling_wait`, `io.*`, `app.lock_*`, `res.*`, `net.send_backpressure` | 0 | 0 | 0 | 0 |

Durability is `relaxed`, so there is no commit fsync wait; no lock, I/O or
conflict wait occurred. The sum of on-CPU time agrees between arms to 0.6 %
over 3.7 s. The `wal_write` and `run_queue` totals differ between arms
(78 ms against 33 ms and 45 ms against 12 ms) with identical counts of
`run_queue` waits; they are one run each, ordering-dependent (the arms'
WAL writes land on a different device state), and not read.

## Cell 3: mount to listening, A against B, 1 GiB, 16 opted shapes

Volume: built by A (`kds_server.A`): `big (id int64, k int64, pad char(1000))`
BTREE, 905,001 rows, data file 1,073,741,824 bytes (130,300 allocated pages
per B's census), plus 16 relations `w0..w15` of 1,000 rows each with
`WAYSTONE ON SELECT v FROM wN WHERE id = 5` (16 opted shapes; each shape
run 80 times over 40 keys twice after a mount) and a clean shutdown (SIGTERM).
Each mount runs on its own `cp -r` of the same volume directory, same input
for A and B; 12 mounts each, order alternated per pair; time from process
spawn to the first accepted TCP connection (polled every 2 ms), warm page
cache (the volume was just copied, not dropped from cache), `taskset -c 0-7`.
The shapes are on 16 small relations, not on the big one; the big relation
is what the A volume's 1 GiB is.

| arm | mounts | mean | median | min | max | sd |
|---|---|---|---|---|---|---|
| A (`630ecc359`) | 12 | 79.2 ms | 80.0 ms | 67.7 ms | 95.2 ms | 8.4 ms |
| B (`82235ad2c`) | 12 | 81.3 ms | 78.7 ms | 75.4 ms | 92.4 ms | 5.7 ms |
| B - A | - | +2.1 ms (+2.7 %) | -1.3 ms | - | - | inside either sd |

A first batch of 5 mounts each (kept as `mounts-c3-first-batch-of-5.json`, same procedure; A 85.7-106.6 ms, B 90.0-95.6 ms) read B about 6 ms slower; the 12-mount run, which is the table above, does not repeat that, so the sign is not stable and the delta is inside the spread. Mount time is about 80 ms with or without DV's walk: the mount reads the catalog and the anchor, not the volume.

Wait breakdown for a mount: not applicable (no sessions; `sys.wait_stats` is
a per-run view and no statement ran).

## Cell 4: `volume_census_at_mount` on against off (B only)

Three snapshots of one A-built volume at 0.25, 1.0 and 4.0 GiB of data file
(same layout as above, 223,001, 905,001 and 3,649,001 rows). B mounted each 3
times with the key off and 3 with it on, alternating, a fresh copy each,
warm page cache (cold not run). Added ms is the on median minus the off median.

| volume | allocated pages (census) | off mounts | on mounts | added | allocated GiB | added per GiB |
|---|---|---|---|---|---|---|
| 0.25 GiB | 32,079 pages | 75.4 / 113.6 / 63.9 ms | 254.6 / 240.0 / 264.3 ms | +179 ms | 0.245 GiB | 731 ms/GiB |
| 1.0 GiB | 130,300 pages | 74.9 / 75.8 / 79.8 ms | 742.1 / 735.9 / 732.9 ms | +660 ms | 0.994 GiB | 664 ms/GiB |
| 4.0 GiB | 523,533 pages | 72.7 / 117.7 / 123.0 ms | 2,066.3 / 1,995.8 / 1,976.4 ms | +1,878 ms | 3.994 GiB | 470 ms/GiB |

The log line each mount wrote (one example per size, the first on mount):

```
INFO [census] 32079 page(s) allocated, 32072 reached, 0 reached by no root, 7 previous-run undo page(s); the mark is whole
INFO [census] 130300 page(s) allocated, 129646 reached, 640 reached by no root (waystone owner 0: 624, waystone_dir owner 0: 16), 14 previous-run undo page(s); the mark is whole
INFO [census] 523533 page(s) allocated, 522232 reached, 1280 reached by no root (waystone owner 0: 1248, waystone_dir owner 0: 32), 21 previous-run undo page(s); the mark is whole
```

The census is a read of every allocated page: about 0.47-0.73 s per GiB on
a warm cache, the per-GiB figure falling with size because a fixed part
(about 100 ms at 0.25 GiB: the mount's own 75 ms plus the mark's catalog
walk) is amortised; the marginal rate between 1 and 4 GiB is (1,878 - 660)
ms / 3.0 GiB = 406 ms/GiB. On a cold cache the figure is device-bound and
was not measured. The off-mount times of 117-123 ms at 4 GiB are two of three
mounts and are noise on a loaded host (72.7 ms the third).

The census also reports what a mount leaves unreached: 640 pages at 1 GiB (624 trail pages and 16 directory pages) and 1,280 at 4 GiB (1,248 and 32), all Waystone (owner 0); none at 0.25 GiB. No other class is unreached in these three volumes, which matches DV-S0's measurement that Waystone pages are the class over DV-Q1's bar. The counts follow the volume's history, not its size: `vol.py`'s `build()` mounts one file once per snapshot under A, so the 0.25, 1 and 4 GiB snapshots are that file after its first, second and third mount, and 0, 640 and 1,280 is 640 for each earlier A mount (16 shapes, 1 directory and 39 trail pages each); that every such page is an A renewal's stranding rather than another source is the class's reading, not traced per page.

## Prior numbers

No results file in `bench/v3.5.0` measures these shapes under this driver
and pin, so there is no delta against this engine's own history and **this run
becomes the baseline**. For context only, not a delta (different driver, one
CPU pin, 1,000,000-row neighbours): `results-du-close-v3.4.17-155-g0babe36e6.md`
read a point UPDATE by tuple key at 7.46-7.72 µs and a point SELECT at
6.70-6.86 µs of server CPU, against 7.26-7.51 µs and 6.46-6.59 µs here. No
earlier mount-time figure exists in `bench/v3.5.0` (`grep` for mount time
found none), and none for a CREATE/DROP pair.

## What the run says about the engine

* The per-statement paths DV touched (transaction end with an empty DDL
  vector, CREATE TABLE's root record) are not measurable at server-CPU
  resolution (4 % floor), including the DDL pair, where the pair itself
  is hundreds of µs.
* **A DDL pair costs more with every dropped table that came before it**
  (about 0.3 µs a prior pair, linear over 3,300 pairs, both commits). Read,
  not traced, as a growth the catalog's no-reclaim rule (`catalog.md`) pays
  in CPU and not only in pages; DV counts the pages and sweeps nothing (DV-Q7), so
  this is the next cost of a catalog that only grows. Not caused by DV,
  and a follow-up candidate: where in the create/drop path the walk is.
* A mount is constant in volume size with the census off (medians 75.4 and
  75.8 ms at 0.25 and 1 GiB; 117.7 ms at 4 GiB, two of three mounts read as
  host noise beside a 72.7 ms third), because recovery after a clean
  shutdown reads no data pages.
  DV-R5's census is the one thing that makes mount cost scale with the
  volume, which is why it sits behind a boot key defaulting off; 2 s at
  4 GiB warm is the price to turn it on.
* The census at three sizes finds only Waystone pages unreached (640 at 1 GiB,
  1,280 at 4 GiB), matching DV-S0; the count follows the A mounts each snapshot had been through (`vol.py`), not its size.

## What was cut or not executed

* The 200 / 1K / 10K row-count sweep (rule 9): not run. The shapes are fixed-
  cost point statements and DDL that do not touch a relation's rows; the
  point statements ran on a 1,000-row `s` only. The mount cells run at 0.25,
  1 and 4 GiB (cell 3 at 1 GiB only).
* Cold-cache mount: not run (warm only; stated per cell).
* The 16 Waystone shapes in cell 3 are on 16 small relations, not on the
  1 GiB relation.
* `tools/` drivers could not be used unmodified for DV's shapes; scratch
  drivers were used (see Stamp).
* PostgreSQL floor: not measured.
* No `ctest`, no test suite run, per the order. A correctness `--verify` mode
  does not exist for these drivers; each unit's reply was checked (an INSERT
  reply `INSERTED`, an UPDATE touching 1 row, CREATE/DROP/BEGIN/ROLLBACK tags).
