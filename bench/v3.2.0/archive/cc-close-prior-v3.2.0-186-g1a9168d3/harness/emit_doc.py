import json
T = json.load(open('/home/cdkbs/bench-runs/cc-prior/out/tables.json'))
doc = f"""# CC prior re-measure: the steward's relation prior is within 0.72-1.16x of the truth on all 28 relations, and the flips still happen

Follows `bench/v3.2.0/results-cc-close-v3.2.0-184-gc547ef14.md` (the baseline for every number here, by that describe string). Raw files,
the harness and the per-block latencies: `bench/v3.2.0/archive/cc-close-prior-v3.2.0-186-g1a9168d3/`. Driver notes: the cc-close harness
(`cc_close_run.py`, archived with the close) plus the scripts in this archive's `harness/`.

**Finding.** On `v3.2.0-186-g1a9168d3` (B, branch `worktree-cc-stats-planner`; its only change from `c547ef14` is
`ReadRelationPrior` descending under each root child) against the opening commit `v3.2.0-178-g65dc45c5` (A):

1. **Prior accuracy gate met.** After the steward seeded, `est_rows` over the 28 scenario 1 / scenario 2 relations is
   **{T['lo']:.2f}x to {T['hi']:.2f}x** of `COUNT(*)` (gate 0.5-2.0x), on a fresh server and again after a restart. The three weak cells of
   `c547ef14` (`daily_bars_x60k` 0.38x, `cargos_df` 0.66x, `cargos_n200` 0.64x) are now 1.08x, 0.93x and 0.91x.
2. **The steward is still unmeasurable by cost.** Over a 20 s idle window after seeding, B did 56 seeds over 258 sampled pages
   (longest step 105 µs) and 8 pool misses; the A control did 0. The `system` tick consumed 907 µs on B and 889 µs on A.
3. **The flips are realised with the new priors.** B's server CPU per statement is 36.9-38.1 % lower on compare-all (2,872 and 10,000 results),
   28.8 % lower on the customer statement at 10,000 cargos and 59.4-60.2 % lower at 1,000 cargos; run beside a server on the `c547ef14` binary in the same
   session, B is within the floor of it on all four cells (-1.2 to +0.5 %).
4. **One caveat carried from the close is not resolved.** At 10,000 cargos this session measures -28.8 % / -29.3 % on clean servers with both priors; the
   close's 44.7-46.7 % clean-server figure was not reproduced (section "The gain cells"). The cause is not diagnosed here.

## Stamp

| Item | Value |
|---|---|
| Date and time | 2026-10-18. Build finished 15:19:18 UTC; runs 15:20:34-15:27:16 UTC (first pass 15:20:34-15:22:33, priors rerun 15:23:28-15:24:05, four-arm gain 15:24:18-15:26:03, warm-order 15:26:33-15:27:16) |
| Worktree / branch | `/home/cdkbs/ckdbs/.claude/worktrees/cc-stats-planner` / `worktree-cc-stats-planner` |
| B: `git describe --tags` | `v3.2.0-186-g1a9168d3` (commit `1a9168d3`, 2026-10-18 15:18:28 UTC) |
| A: `git describe --tags` | `v3.2.0-178-g65dc45c5`, the CC-S0 binary (copy from the close run) |
| BO (control arm in the four-arm run) | `v3.2.0-184-gc547ef14`, the close's binary copy, which carries the old prior |
| Tree | Clean (`git status` empty) at the build and at the start of the run; this worktree's own `build-release`, `cmake --build build-release --target kds_server` (Release; `CMAKE_BUILD_TYPE=Release` verified in the cache) |
| Binary B | `build-release/kds_server` mtime 2026-10-18 15:19:18, after the commit time 15:18:28, so it is the engine at HEAD. Copied to `/home/cdkbs/bench-runs/cc-prior/kds_server_B`, sha256 `78d44e1851dc885f96f489b71e59cd0ac26ed94c6e7443ca9957f99c988f245f`. Every B server started from the copy |
| Binary A | `/home/cdkbs/bench-runs/cc-close/kds_server_A`, sha256 `8ce595c65ad1dcdc003443d96896d4c6f6b346fa262bf05b9a1de7c9d1ce495e` (the close's record; source binary mtime 2026-10-18 11:55:47) |
| Binary BO | `/home/cdkbs/bench-runs/cc-close/kds_server_B`, sha256 `3bc1180ec90c3957a1c5d7066c5bd696eea71dd190d9ed858c7cb6da7ad9d3ac` (the close's record) |
| Device | `/dev/root`, ext4 (`df -T $HOME` checked); data files under `/home/cdkbs/bench-runs/cc-prior/run/`. Not tmpfs. The OS page cache held the freshly copied file, so "cold" below means a cold engine pool, not a cold device |
| Host | 8 CPUs; servers pinned to CPU 2, client to CPU 3. `pgrep cc1plus` empty and no build running during any run (the build ended before the first server started). Loadavg at the first look 6.07 (other sessions), 1.46 at launch; the highest loadavg1 at any block start was 1.16 (gain), 1.16 (four-arm), 0.90 (warm-order) |
| Server config | `cores = 1`, `durability = group`, `buffer_pool_frames = 65536`, `join_order = planned`, `log_level = warn`; fresh data file copied from the CC-S0 pristine file for every server |
| Dataset | The close's: scenario 1 and 3 relations at 200 / 1,000 / 2,872 / 10,000 results and 200 / 1,000 / 10,000 / 60,480 stats rows, scenario 2 at 200 / 1,000 / 10,000 / 200,000 cargos. No overhead tables were created in these servers |
| Not executed | The test suite (the commit changed the steward's read only; this was a measurement); `cores > 1`; `strict`; the PostgreSQL floor (not measured for this shape) |

## Prior accuracy

Servers B and A were started from the pristine file, so the pool held 147 pages after the mount (the 28 relations' leaves were cold). Five repetitions
of every scenario statement shape were run on each (compare-all at four sizes, customer statement at four sizes, the cross-join at four sizes;
literals varied), which is what names the 28 relations to the planner. The harness then polled `SHOW META` only, every 2 s, until the statistics
generation had been unchanged three polls running, and read `SELECT * FROM sys.relation_stats` once, last. All 28 relations were published
{T['wtxt']} after the statements. Then `COUNT(*)` of each. The "restarted" column is B stopped and started again on its own file, with a pool
cold again, one execution of each shape (compare-all with `LIMIT 1`), the same wait; the old column is `c547ef14` from the close file's Result 5.
24 side tables (charges, fees, operations, organizations, recipes, ships) are named by no planned block and carry no prior, as at the close.

By height, est/true:

| Height | Relations | B `1a9168d3` | `c547ef14` (close, Result 5) |
|---|---|---|---|
{T['htab']}

Per relation (the estimate is published as the centre of a half-octave band, `2^(k/2)`, so a single figure is exact to about -16 % / +19 % at best):

| Relation | Height | Leaves | True COUNT(*) | est_rows | est / true | est_rows after restart (est / true) | `c547ef14` est_rows | `c547ef14` est / true |
|---|---|---|---|---|---|---|---|---|
{T['ptab']}

Height 1 stays exact. The height-2 spread moved from 0.64-1.01 to 0.72-1.16 and the height-3 relations from 0.38-1.08 to 0.93-1.08: the descent under each
root child measures the rightmost subtree apart, which is the one an append-built tree leaves not full, and that is what a height-3 estimate
had averaged in (`docs/spec/plan-stats.md` PS1 states the same reason). The restart column reproduces the fresh column on all 28 relations, so
the estimate is a function of the tree, not of what the pool held when the steward sampled it. What the data cannot separate: four relations sit
at 0.72-0.82 with values identical to `c547ef14` (`model_results_r1k` 724, `model_results_r10k` 8,192, `freights_n1k` 724, and the 200-row set at 181),
and the view publishes only the band centre, so a sampling error and a band rounding cannot be told apart from here. Whether it matters to a plan: the join planner's `switch` for the four gain statements below is identical under both priors in two cells (0.190739, 0.190542) and differs in the other two (0.2105 against 0.1908 and 0.1926 against 0.1773), on the same side of the flip each time.

The rule 3 decomposition (waits) does not apply to an estimate.

## Steward cost

The window is the idle time after the statements; B and A ran the same statements from the same cold start, A with no steward. The B-restarted row is a
cold pool again and a lighter statement set (compare-all with `LIMIT 1`), so seeding is likelier to meet leaves no statement read.

| Server and window | Wall in window | Seeds | Seed pages | Publishes | Longest step (`steward_us_max`) | `system` tick: consumed / polls | Pool misses | Pool hits |
|---|---|---|---|---|---|---|---|---|
{T['stab'].replace('| 0.0 s |','| - |').replace('| - µs |','| - |')}

Seeding the first 28 relations took ten seconds at the steward's pace (the wait log: 7, 13, 19, 27 and 28 relations published at 2, 4, 6, 8 and 10 s);
what follows is the 10 s re-seed of the same relations. The cold-leaf concern does not materialise: the steward's reads add 8 pool misses (fresh) and 20
(restarted, where the statements read less) to a pool of 3,500-3,700 pages, presumably because a statement that names a relation has usually just read
its leaves (not separated here), and its longest step is 102-106 µs against the 54-69 µs the close recorded for `c547ef14` (the step is bounded at 16 pages; the
descents now touch more pages per relation, 4.6 per seed here against 2.8 in the close's 60 s mixed run). At one step per 1 s tick that is under 0.011 % of core 0. B's `system` tick
consumed 18 µs more than A's in 20 s, inside what two identical servers differ by.

**The view is not free.** `SELECT * FROM sys.relation_stats` itself read {T['mis_view']:,} cold pages and {T['hit_view']:,} resident ones in one call on B (the first
version of this run polled the view every 2 s and saw about 5,630 page hits per poll; archived in `out/v1-view-polled/`), so anyone polling it to watch the
steward disturbs the pool they are watching. This run read the view once, last. The cause (the `leaves` column is a count of leaf pages) was not verified in the source.

## The gain cells

Fresh A, A2 (A's binary again, the noise floor from inside the run) and B servers with no extra tables, each warmed on the four cells and 8 s idle, then five
interleaved rounds of a fixed statement count per cell (the close's calibration: about 1 s on A). The first pass had three arms; the second added BO, the
`c547ef14` binary, so both priors ran in one session. The floor per cell is the larger of A2's difference and half of A's round spread, at least 2 %.
Server CPU is the sum of every server thread's `schedstat`; QPS is client-observed. Both plans are the same in A and unchanged by the prior:
B reorders every cell (`reordered=1`, `ANALYZE` in the archive's `plans4.json`) and BO executes the same order with the same examined / pages counts in all four. The measured unit is a serial read statement at `group` durability with no commit wait, lock wait or conflict, so rule 3's decomposition reduces to server CPU against client round trip (p50 minus CPU), which is not broken down further.

| Cell | A: CPU / QPS | B: CPU / QPS | B vs A (pass 1) | B vs A (pass 2) | BO (`c547ef14`) vs A | B vs BO | A2 vs A (pass 2 / pass 1) | Floor | `c547ef14` close, full matrix |
|---|---|---|---|---|---|---|---|---|---|
{T['gtab']}

The gate from the flip (15 % lower than A) is met on all four cells, in both passes. B against BO is -1.1 %, -1.2 %, +0.5 % and -0.2 %: inside the 2 % floor in
every cell, so the new prior neither adds to nor takes from what the close measured. Latency, pass 2 (five rounds; p0 is the best statement; the last column is the spread of per-round CPU):

| Cell | Arm | Statements (n) | Server CPU / statement | QPS | p0 µs | p25 µs | p50 µs | p95 µs | p99 µs | Round spread |
|---|---|---|---|---|---|---|---|---|---|---|
{T['ltab']}

The B arm of compare-all 2,872 shows a tail in pass 2 (p95 5,487 µs, p99 6,212 µs, against 3,549 and 3,570 µs in pass 1) with a round spread of 2.7 % in CPU: a
client-side stall in one or two rounds, not seen in the other pass or in BO, not a cost of the prior.

Pass 1 (three arms), same columns:

| Cell | Arm | Statements (n) | Server CPU / statement | QPS | p0 µs | p25 µs | p50 µs | p95 µs | p99 µs | Round spread |
|---|---|---|---|---|---|---|---|---|---|---|
{T['l1tab']}

**The 10,000-cargo cell stayed at -29 %.** The close measured this flip at -29.1 % in its full matrix and at -44.7 to -46.7 % on servers that had never had other tables
created in them, and put the difference on an engine state it did not diagnose. These servers had no tables created and still give -28.8 % (pass 2), -28.8 % (pass 1) and,
for BO, -29.1 %. A third check tests one candidate, the warm-up order (compare-all warmed before the customer statement, or not), on B alone:

| Cell | A CPU | B warmed on customer-statement cells only | B warmed on compare-all first |
|---|---|---|---|
{T['g5tab']}

Warm-up order is not the cause: both give -29.3 %. What differs from the close's clean-server run is not identified here; at this size this session measured -28.8 %, -28.8 % and -29.3 % on B and -29.1 % on BO, matching the close's full-matrix -29.1 %, and the -45 % figure is not reproduced.

Cells cut for the 20-minute budget: 200-row and 1,000-row compare-all, 200-cargo customer statement, the 200,000-cargo cell, the cross-join, and the overhead cells
(the commit changed nothing on those paths). Rule 9 is therefore met for the prior (200 to 200,000 rows) and not for the gain cells (1,000 to 10,000 only).

## What the run says about the engine

* **The prior's error is now dominated by publication, not by sampling.** Every estimate is published as a half-octave band centre, so even a perfect
  sample reads 0.84-1.19x; the new range 0.72-1.16 sits close to that bound, and the one residual structural cost (the four 0.72-0.82 relations) is below the plan's
  sensitivity (`switch` values equal in two of four gain cells and on the same side of the flip in all four).
* **A statement-path-free change can still move the steward's footprint**: the longest step went from 54-69 µs to 102-106 µs, still two orders of magnitude from
  anything a statement sees, but it is the figure to re-read if the step budget (16 pages) or the descent count changes again.
* **`sys.relation_stats` is a diagnostic that perturbs what it observes** (one call: {T['mis_view']:,} misses); the harness must not poll it.
* No open decision in `CLAUDE.md` acquired data from this run.

## Baseline and reproduction

The baseline is `v3.2.0-184-gc547ef14` (the close file above) for the prior and for the gain percentages, and A (`v3.2.0-178-g65dc45c5`) for the gain; there is no
prior number for the steward-cost table in the close beyond `steward_us_max` (54-69 µs), which is cited above. The PostgreSQL floor was not measured for this shape.
Reproduce: place the two binaries as stamped, `python3 harness/prior_run.py priors`, `harness/gain_bold.py`, `harness/gain_order.py` from this archive
(they import the close's `cc_close_run.py` from `/home/cdkbs/bench-runs/cc-close/harness/`, archived with the close), and `harness/mkdoc.py` + `emit_doc.py` regenerate the tables.
`patch1.py` is the edit that moved the harness from polling the view to polling `SHOW META` after the first pass.
"""
open('/home/cdkbs/ckdbs/.claude/worktrees/cc-stats-planner/bench/v3.2.0/results-cc-close-prior-v3.2.0-186-g1a9168d3.md', 'w').write(doc)
