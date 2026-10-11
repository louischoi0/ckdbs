# CREATE ASSERTION target resolution: interleaved A/B, write-path guard

**Thesis.** The change at `76335d6d` rewrites how `CommandDispatcher::HandleAssertion`
resolves a `CREATE ASSERTION`'s target relation (resolve once under a view, take the
relation `X` on that oid, re-resolve once under the settled check view after the grant)
and removes two internal name lookups from `exec::CreateAssertion`. It touches no write
path and not the per-write assertion check. The measurement finds exactly that: an
`INSERT` into an asserted relation and the two read controls are unchanged within the
harness's floor, and `CREATE ASSERTION` itself is about 1 % (6 us of ~650 us) faster on
a 1,000-row relation and indistinguishable on a 10,000-row one. A `CREATE ASSERTION` is
a relation scan (0.65-0.69 us per row); the saved lookups are a fixed few microseconds,
so the saving is visible only where the scan is short.

## Stamp

| | |
|---|---|
| Executed | 2026-10-10 20:35:01 - 20:40:40 UTC (main pass 20:35:01-20:38:29, swap pass 20:38:32-20:40:40) |
| Branch | `worktree-fix-assertion-on-invisible-relation` (worktree `fix-assertion-on-invisible-relation`) |
| B (measured) | `76335d6d`, `git describe --tags` = `v2.7.0-749-g76335d6d`; tree clean when built (`git status` empty; the files this run adds are untracked) |
| A (baseline) | base commit `6b0d8412`, `v2.7.0-747-g6b0d8412` |
| Binary B | `build-release/kds_server` built 2026-10-10 20:34:05 UTC (after HEAD's commit time 20:32:17), release, `CMAKE_BUILD_TYPE=Release`; copy `sha256 94adffb24212ddd4ed69827b52014a06eab7ad9548f7e9d6fa0c759ce527631d` |
| Binary A | `sha256 ee29d277d3b19939f1c93bdd57e789796c3a987dbd421deafc862afa2c7ef574`. **Provenance caveat:** reused from an earlier scratch build, not rebuilt here; that build (`bs/build-A.log`, 2026-10-10 15:14 UTC) was of a base worktree at `1a5176ae` (`v2.7.0-746-g1a5176ae`), the parent of `6b0d8412`. `git diff --name-only 1a5176ae 6b0d8412 -- src include CMakeLists.txt tests` lists 0 files (the commit adds bench and docs only), so the engine source is the same as `6b0d8412`'s. The binary's original mtime was not recorded (the run's copy is dated 20:3x); its first copies in other run directories are dated 15:17 UTC. |
| Copies | both servers started from copies in `/home/cdkbs/bench-runs/at/` (`kds_server-A`, `kds_server-B`), hashes above, never from a build tree |
| Device | data files and logs under `/home/cdkbs/bench-runs/at/work/` on `/dev/root`, ext4, NVMe (`nvme0n1`, MSFT NVMe Accelerator). Not tmpfs (`df -T`). |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 131072`, `log_level = warn`, BTREE relations, no other non-default key; fresh server and fresh data file per run |
| Pinning | both servers of a pair on CPU 6 (one idle while the other is timed), client on CPU 7 |
| Driver | scratch, in the archive's `drivers/` (`at_ab.py`, `at_run.py`, `analyze.py`); `tools/` untouched. Not added to `bench/docs/README.md` (rule 7): it is a one-stage scratch driver, and the commands to reproduce are below. |

Reproduce, from a directory holding `kds_server-A` and `kds_server-B`
(`OUT` in `at_run.py`):

```
python3 at_run.py 8 1000,10000 res                 # main pass: slot A runs binary A, slot B binary B
AT_SWAP=1 python3 at_run.py 5 1000,10000 res-swap  # swap pass: slot A runs binary B, slot B binary A
python3 analyze.py res; python3 analyze.py res-swap
```

Raw per-run JSON, gzip'd per-statement latencies, per-run host files, both passes'
`analysis.json` and the driver sources are archived in
`bench/v3.0.0/archive/assertion-target-overhead-v2.7.0-749-g76335d6d/`.

## Method

One "pair" is one run of a fresh server per binary, same data, with every arm run in 16
blocks that alternate which server goes first (equal statement count per server, 1,000
statements per block, 16,000 per arm). Row counts map to the driver's `--rows N`:
the asserted relation and its control hold `N` rows in `N` distinct groups, and the
`CREATE ASSERTION` relation `cr` holds `N` rows (`id = g = 1..N`, loaded in 250-row
multi-row `INSERT`s). Eight main pairs and five swap pairs at each size.

| Cell | Statement | Rows | Per run |
|---|---|---|---|
| 1 write-check guard | `INSERT INTO as_a VALUES (id, g, 0)`, `g` an existing group; `as_a` carries `CREATE ASSERTION cap ... GROUP BY (g) CHECK COUNT(*) <= 2000000000` (the guard that the write check is untouched) | 1,000 | 16,000 per server |
| 1 control | the same `INSERT` into `as_n`, no assertion; and the arm repeated once more as the in-run noise floor | 1,000 | 16,000 each |
| 2 | `CREATE ASSERTION capcr ON cr GROUP BY (g) CHECK COUNT(*) <= 2000000000`, timed client-side, then `DROP ASSERTION capcr` (timed separately) | 1,000 and 10,000 | 20 creates per server |
| 3 control | `SELECT * FROM <rel> WHERE id = k` (pk point lookup) | 1,000 and 10,000 | 16,000 per server |
| ping | `SHOW META`, the client/socket floor | with each size | 16,000 per server |

Rule 9 (sweep 200 / 1K / 10K) is narrowed by the request: cell 1 runs at 1,000 rows
only, cell 2 at 1,000 and 10,000; 200 rows was not run.

Statistics. Percentiles are computed per run (nearest rank over that run's samples);
the tables give the median over runs. A delta is B - A within a pair; "med [IQR]" is the
median and interquartile range of the per-pair deltas. The **swap pass** gives the
position-free effect: with `d_main = slotB(binary B) - slotA(binary A)` and
`d_swap = slotB(binary A) - slotA(binary B)`, the binary effect is
`x = (d_main - d_swap) / 2` and the slot effect (a property of the server's place in the
pair, not of the engine) is `s = (d_main + d_swap) / 2`.

### Host state

26 runs (16 main + 10 swap), every one started with no `cc1plus`, `cmake --build`,
`ctest` or `kds_tests` process running (`competing = none` in all 26) and no CPU
other than 6 and 7 busier than 0.043 during the run (0.08 in the second before it).
CPU 6 ran 0.52-0.57 busy and CPU 7 0.49-0.59 (the client is synchronous, so the
server and client each idle about half of each round trip). Load average at start was
1.76-2.54 with CPUs 0-5 idle, i.e. load from outside the CPUs measured. **Excluded runs:
0 of 26.** Nothing was built during the measurement; the only build (B, `nice`'d to
CPUs 0-5) finished at 20:34:05, before the first run.

## Result 1: the write check and the controls do not move

`INSERT` into an asserted relation at 1,000 rows is 77.5 us p50 on A and 77.0 us on B.
B - A is -0.40 us in the main pass and +0.70 us in the swap pass, so the position-free
binary effect is -0.55 us (-0.7 %), B marginally faster, not slower. The controls give
the floor: the unasserted `INSERT` (-0.07 us), the repeated unasserted arm (-0.13 us),
`SHOW META` (-0.25 and +0.12 us) and the point `SELECT` (+0.02 us at both sizes) all
sit within 0.25 us of zero at p50, which is what this harness can resolve on a 70-100 us
statement. The engine's own time per statement (the `SHOW META` delta of
`sched_foreground_polled_us` over polls) shows the same: 28.14 vs 28.26 us for the
asserted `INSERT`, 24.87 vs 24.89 for the unasserted one. The guard holds: the change
leaves the write path alone.

QPS here is derived as 1,000,000 / p50 us (a serial single-connection driver). It is
derived from p50 rather than the mean because the means in this run carry sporadic
stalls (below); no cell moves by more than the floor.

| Cell (rows) | A QPS | B QPS | B-A QPS | d p50 us, main med [IQR] | B>A pairs | d p50 us, swap | binary effect x | x as % of A |
|---|---|---|---|---|---|---|---|---|
| INSERT, assertion, existing group (1,000) | 12,903 | 12,987 | +84 | -0.40 [-0.78, +0.03] | 2/8 | +0.70 [+0.40, +0.90] (5/5) | -0.55 us | -0.7 % |
| INSERT, no assertion, control (1,000) | 13,699 | 13,717 | +18 | -0.05 [-0.70, +0.12] | 2/8 | +0.10 [+0.00, +0.70] (3/5) | -0.07 us | -0.1 % |
| INSERT, no assertion, repeated (noise floor) | 13,699 | 13,717 | +18 | -0.15 [-0.60, +0.20] | 3/8 | +0.10 [+0.10, +0.80] (4/5) | -0.13 us | -0.2 % |
| pk point SELECT, control (1,000) | 10,215 | 10,204 | -11 | +0.05 [-0.23, +0.50] | 4/8 | +0.00 [-0.30, +1.00] (2/5) | +0.02 us | +0.0 % |
| pk point SELECT, control (10,000) | 10,571 | 10,560 | -11 | +0.25 [-0.20, +0.43] | 5/8 | +0.20 [-0.40, +0.30] (3/5) | +0.02 us | +0.0 % |
| SHOW META, client/socket floor (1,000-row runs) | 13,928 | 13,889 | -39 | -0.10 [-0.33, +0.23] | 3/8 | +0.40 [+0.10, +0.40] (4/5) | -0.25 us | -0.3 % |
| SHOW META, client/socket floor (10,000-row runs) | 13,986 | 13,947 | -39 | +0.05 [-0.23, +0.43] | 4/8 | -0.20 [-0.20, -0.10] (1/5) | +0.12 us | +0.2 % |

The point `SELECT` does not scale with the row count (pk descent); the 1,000 and 10,000
row cells are shown as evidence, 98.0 vs 94.7 us on B.
A point `SELECT` is slower than an `INSERT` here by ~25 us at p50; that is a property of
the driver's text-protocol reply, not of this change.

Distributions (main pass; per-run percentile, median over 8 runs, 16,000 statements per
run per engine; us):

| Cell | Engine | p0 | p25 | p50 | p75 | p99 | engine us/stmt |
|---|---|---|---|---|---|---|---|
| INSERT, assertion, existing (1,000) | A | 73.5 | 76.5 | 77.5 | 80.8 | 109.2 | 28.14 |
| | B | 70.8 | 76.1 | 77.0 | 80.5 | 109.7 | 28.26 |
| INSERT, no assertion (1,000) | A | 69.3 | 72.2 | 73.0 | 75.9 | 103.3 | 24.87 |
| | B | 69.5 | 71.9 | 72.9 | 75.8 | 103.2 | 24.89 |
| INSERT, no assertion, repeated | A | 69.4 | 72.2 | 73.0 | 75.8 | 102.4 | 24.88 |
| | B | 69.4 | 72.1 | 72.9 | 75.8 | 103.0 | 24.90 |
| pk SELECT (1,000) | A | 91.5 | 96.2 | 97.9 | 104.5 | 131.0 | 32.69 |
| | B | 92.1 | 96.4 | 98.0 | 104.8 | 131.2 | 32.85 |
| pk SELECT (10,000) | A | 89.9 | 93.5 | 94.6 | 98.5 | 120.0 | 30.23 |
| | B | 90.2 | 93.5 | 94.7 | 98.8 | 120.8 | 30.54 |
| SHOW META (1,000-row runs) | A | 68.8 | 71.0 | 71.8 | 73.8 | 93.2 | 30.76 |
| | B | 68.8 | 71.0 | 72.0 | 74.0 | 93.2 | 30.94 |
| SHOW META (10,000-row runs) | A | 68.5 | 70.5 | 71.5 | 73.4 | 93.3 | 30.70 |
| | B | 68.1 | 70.8 | 71.7 | 73.6 | 93.4 | 30.83 |

Where the time goes (rule 3). For the asserted `INSERT`, the 77.5 us p50 is about
28.1 us of engine foreground time plus ~49 us of client, socket and reply round trip
(the unasserted `INSERT`: 24.9 + ~48). Assertion admission is therefore ~3.3 us of the
engine's share, the same on A and B. There is no durability wait on this path
(`durability = relaxed`, the statement does not wait for an fsync), and with one serial
session no lock or conflict wait is possible; neither was measured as a separate share
because there is nothing to separate.

**Means are not a usable estimator in this run.** The mean differs from the median by
a stall-driven tail, and two cells show a mean delta that is the *slot* and not the
binary: the 1,000-row `SELECT` mean is +5.95 us in the main pass and +5.75 us in the swap
pass (binary effect +0.1 us), and the unasserted `INSERT` mean is +1.65 / -1.66 us
(binary effect +1.65 us, but the repeated arm gives -0.60 us and no percentile p0-p99
moves). The floor for a 16,000-statement mean here is therefore about 2 us, against
about 0.3 us for p50.

## Result 2: CREATE ASSERTION is a scan, and the change saves a fixed few microseconds

`CREATE ASSERTION` takes 651.8 us (p50) on A and 643.5 us on B over 1,000 rows, and
6,904.7 us and 6,898.1 us over 10,000 rows. B is faster in 8/8 main pairs at
1,000 rows (median -9.9 us, IQR [-12.6, -6.5]); in the swap pass binary A is slower
than binary B in 4/5 pairs (+2.7 us); the position-free binary effect is -6.3 us,
-1.0 % of the statement. At 10,000 rows the same arithmetic gives -8.3 us, which is
0.12 % of 6,900 us and inside the pairs' IQR (-36 to +17 us), so it is not a finding there.

The unchanged `DROP ASSERTION` is the only create-shaped control in the run; at 1,000
rows its apparent binary effect is -3.3 us (3/8 main pairs B>A, 4/5 swap), at 10,000 rows
+0.25 us. The create effect at 1,000 rows (-6.3 us) is about twice the control's apparent
bias, consistent in sign across both passes and with what the diff removes (name lookups
are a fixed cost), but with 20 samples per run per server and a control that wanders by
3 us it is a small likely saving, not a result of weight. The scan itself costs
0.65 us per row at 1,000 rows and 0.69 us per row at 10,000 rows (p50 / rows); a
create's fixed part is small beside it, which is why the saving shows at 1,000 rows and
disappears into the scan's spread at 10,000.

QPS below is creates per second derived as 1,000,000 / p50 us (a mean-derived figure is
dominated by the stalls described under the table).

| Cell (rows) | A /s | B /s | B-A /s | d p50 us, main med [IQR] | B>A pairs | d p50 us, swap med [IQR] | binary effect x | slot effect s |
|---|---|---|---|---|---|---|---|---|
| CREATE ASSERTION (1,000) | 1,534 | 1,554 | +20 | -9.90 [-12.60, -6.53] | 0/8 | +2.70 [+2.20, +5.70] (4/5 A slower) | -6.30 us (-1.0 %) | -3.60 us |
| CREATE ASSERTION (10,000) | 144.8 | 145.0 | +0.2 | +6.05 [-35.95, +17.33] | 5/8 | +22.60 [+22.00, +28.20] (4/5) | -8.27 us (-0.1 %) | +14.32 us |
| DROP ASSERTION, unchanged code (1,000) | 7,886 | 7,994 | +108 | -1.75 [-2.60, +2.77] | 3/8 | +4.90 [+2.00, +6.80] (4/5) | -3.33 us | +1.58 us |
| DROP ASSERTION, unchanged code (10,000) | 1,183 | 1,164 | -19 | +7.30 [-3.75, +23.37] | 5/8 | +6.80 [-5.70, +8.30] (3/5) | +0.25 us | +7.05 us |

Distributions (main pass; per-run percentile over that run's 20 statements, median over
8 runs; us; p99 of 20 samples is the run's maximum):

| Cell | Engine | p0 | p25 | p50 | p75 | p99 |
|---|---|---|---|---|---|---|
| CREATE ASSERTION (1,000) | A | 612.2 | 630.4 | 651.8 | 709.0 | 1,725.8 |
| | B | 610.8 | 622.2 | 643.5 | 784.0 | 2,333.1 |
| CREATE ASSERTION (10,000) | A | 6,697.1 | 6,772.2 | 6,904.7 | 6,994.9 | 7,953.6 |
| | B | 6,671.6 | 6,741.4 | 6,898.1 | 7,024.6 | 18,248.2 |
| DROP ASSERTION (1,000) | A | 112.8 | 115.8 | 126.8 | 168.2 | 746.6 |
| | B | 112.2 | 114.0 | 125.1 | 159.9 | 2,854.7 |
| DROP ASSERTION (10,000) | A | 792.3 | 814.3 | 845.5 | 894.2 | 1,043.1 |
| | B | 794.0 | 819.5 | 859.1 | 898.9 | 2,844.8 |

**The upper tail belongs to the slot, not to the engine.** Every run's slot-B server
shows one or two creates at 14-19 ms among 6.7-7.0 ms ones (10,000 rows; at 1,000 rows
a 1.7-2.3 ms one), and it does so at the same create indices (17-18) whichever binary
runs there: the main pass's B-slot p99 is +10,169 us and the swap pass's is +9,006 us
over the A slot, the mean delta +829 us and +859 us. Reading either as a regression
would be a mistake of the harness; the cause is not identified (a periodic activity
in the server that shares the position, ruled out as binary-specific only by
the swap). The percentiles used for judgement above (p0-p50) are not reached by it;
p75 and p99 are, so no verdict is drawn from them.

Where the time goes (rule 3). The `SHOW META` delta of engine foreground time over the
create window is 99.5 % of the client-measured sum of create latencies at 1,000 rows and
99.4-99.6 % at 10,000 (each window also contains one `SHOW META` poll of ~31 us, so
the true engine share is ~94-99 %): a create is its engine's scan and publication
work; the client round trip (~41 us beyond the `SHOW META` engine time, from the ping
cell) is ~6 % of a 650 us create and ~0.6 % of a 6,900 us one. The wait types of rule 3:
durability (fsync) is off the path in `relaxed`; the lock waits a create can take
(relation `X` waiting for open writers and readers) are zero here because the session
is alone; the create's own `X` grant is not separable from the scan in this
driver. The per-wait decomposition inside the engine was not measured.

## What this run teaches

- The change is invisible to the statement path. The asserted `INSERT`, the
  unasserted control, the repeated control and both point-read controls all land
  within 0.55 us of zero (and the largest of those, the asserted `INSERT`, is B faster),
  on 73-98 us statements.
- A `CREATE ASSERTION` is ~99 % engine time and ~0.65-0.69 us per row of relation scan,
  so any fixed-cost trimming in the DDL front end (name resolution, relation lock
  order) is a 1 % effect at 1,000 rows and under 0.2 % at 10,000. The change's reason is
  correctness (the oid that was locked is the oid built over), and the data say it costs
  nothing for it.
- This harness cannot separate effects under ~0.3 us on a 70-100 us statement or
  under ~3 us on a 650 us `CREATE`; it also carries a slot-bound upper tail on 20-sample
  create series, so p75/p99 of creates are not evidence for either direction.

## Baseline

The baseline is binary A (`6b0d8412`), interleaved in every pair, so every B figure
above is a delta against this engine's own immediately preceding state. No earlier
file in `bench/v3.0.0/` carries a sampled `CREATE ASSERTION` latency (the AZ-S7
driver times one create per server at setup), so the create cells here become the
baseline for that shape. The PostgreSQL floor was not measured for this shape.

## Tests

The suite was not executed for this run: no engine code was edited by the
measurement. B's own commits carry their tests (`tests/assertion_enforce_test.cpp`,
`tests/assertion_publish_rig_test.cpp` changed in the diff). Each pair ran with
verification: both servers ended with equal row counts in `as_a`, `as_n` and `cr`,
every `CREATE`/`DROP ASSERTION` replied without error, no statement returned an `ERR`
(`verify_ok` is true in all 26 runs).
