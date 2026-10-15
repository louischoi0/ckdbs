# BW-S6 INSERT guard: the positional single-row INSERT and the point SELECT read B - A inside the A-against-A' spread; a column list costs about 1.3 us more

**Thesis.** On `worktree-bw-instant-add-drop-column`, with the engine at `33519b28` (`v3.2.0-32-g33519b28`, **B**, BW-S6 and its review fixes) against `045dcc68` (**A**, BW closed, before S6), the existing positional single-row `INSERT` did not slow beyond the spread: server CPU per statement **-0.05 us literal (-0.2 %) and +0.38 us prepared (+1.8 %)**, paired median of 8 runs over both start orders, against an A' - A largest gap of 1.09 / 0.84 us. The point `SELECT` by key reads +0.27 us literal and +1.18 us prepared (+3.9 %), also inside, but the prepared gap is wide (A' - A largest 3.05 us) because the host was shared, so that cell is "not resolved", not "zero". The new column-listed `INSERT INTO i (w, id, v) VALUES (?, ?, ?)` costs **+1.26 us literal and +1.37 us prepared of server CPU (about +5-6 %)** over the positional one on B, paired by run (4 runs, one arm). **The host was not quiet** (below): another session was building and testing throughout, and one group (u1) shows elevated absolute CPU on two cells in all three arms alike. The A/A'/B interleave pairs by run, so the deltas survive that, the absolute levels do not.

Server CPU is the sum of `/proc/<pid>/task/*/schedstat`; wall is the client's. Driver flags are in `tools/bn_census.py`'s docstring; the exact invocations are `archive/bw-s6-insert-v3.2.0-32-g33519b28/scripts/run.sh` (`bench/README.md`: `bench/docs/` stays closed).

| Item | Value |
|---|---|
| Executed | 2026-10-14, UTC. Builds 11:41:29-11:44:29; measurement 11:44:51-12:01:25 (phases u1, u2, l1 in `progress.txt`). Whole task about 20 minutes: the stage budget, with l1 ending at its edge |
| Worktree / branch | `bw-instant-add-drop-column`, branch `worktree-bw-instant-add-drop-column`; HEAD `33519b28`; tree dirty by one file at the start of the measurement: `tools/bn_census.py` gained the `insert-listed` shape (committed with this file) |
| Commits measured | **A** = `045dcc68` (`git archive`; BW closed, S6 absent), **B** = `33519b28` (`v3.2.0-32-g33519b28`, committed 11:40:56 UTC). A' = A's bytes started as a second server (in-run control) |
| Binary provenance | Built from `git archive <commit>` into `/home/cdkbs/bench-runs/bw-s6/src-{A,B}`. **A and A'** sha256 `a53e9b147c4deb57a86806f4b37889a03d5b282af9c3d776db5fd2f4cf053a55` (source mtime 11:44:29.68 UTC); **B** `2b6e0e0ffbb3564f3f3b31507e4ea740ed1be98b1ba28352784ba5b04821bb62` (11:44:28.98 UTC). Both newer than their commits; every server started from the copies in `bin/` (`archive/.../binaries.txt`) |
| Build | `CMAKE_BUILD_TYPE=Release`, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`, `-j4` each, built together (3 min, slowed by the host) and finished before any measurement |
| Host / device | 8 vCPU, Linux 7.0.0-1014-azure. Data files and WAL under `/home/cdkbs/bench-runs/bw-s6/wd-*/` on `/dev/root`, **ext4, not tmpfs** (`df -T`) |
| Host load | **Not quiet.** `uptime` showed a load average of 9.9 at the start and 14.8 at the end of the builds; `cc1plus` processes of other sessions (`fix-create-table-cross-core-visibility` building `kds_tests`) ran through the first minutes of the measurement. Load averages recorded before each run: 10.9, 6.5, 5.5, 4.8 (u1), 5.4, 5.4, 3.2, 10.2 (u2); competing processes 3, 2, 0, 0, 8, 2, 2, 3. `--quiet-load 2` could not be met and gave up after its 20 s wait each time; the per-cell gates (`--cell-quiet-pct 10 --cell-clean-pct 20 --cell-foreign-pct 15`) flagged **0 unclean windows** |
| Pinning | One server process per arm, one busy at a time, on CPU 2; the driver on CPU 4 |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, `scan_kernel` default; fresh server and data file per arm per run |
| Drivers | `tools/bn_census.py` sha256 `35693dc24f17...` (with the `insert-listed` shape), `tools/kwp.py` `99341280c37f...` |
| Suite | Not executed in this task; no engine code was changed by this measurement |

## 1. What was run and what was cut

Three arms (A, A', B), a fresh server and data file per arm per run, the arms interleaved block by block (6 blocks per cell), literal and prepared alternating. `bn_census.py --rows 1000 --ops 2000 --only select-pk,insert`: `--rows` is the table size (the point `SELECT` reads a 1,000-row table; the `INSERT` goes into `i`); 2,000 statements per path per arm per run (1,998 after the block split).

| group | start order | runs | shapes |
|---|---|---|---|
| u1 | B, A', A | 4 | `select-pk`, `insert` (positional, `INSERT INTO i VALUES (?, ?, ?)`) |
| u2 | A, A', B | 4 | same |
| l1 | B alone | 4 | `insert` and `insert-listed` (`INSERT INTO i (w, id, v) VALUES (?, ?, ?)`, keys offset by 20,000,000) |

**Cut for the 20-minute budget:** the every-row `UPDATE` and the scan rows (BW close's moving cell, not this question); row-set sizes 200 and 10,000 (a single-row `INSERT` and a point `SELECT` do not scale with the table, the close showed the same at 200/1k/10k; only 1,000 was re-run here); a multi-row `INSERT` (no driver exists); the column-listed `INSERT` on A and A' (the syntax is new in B). No PostgreSQL twin: the floor was not measured for this shape. The closing 20-minute mark fell inside l1, which finished at 12:01:25.

## 2. Headline: B - A against the A' - A spread

Server CPU per statement in us, pooled over both start orders (8 runs); B - A paired by run. "Inside" means the median B - A is not above the largest |A' - A|.

| cell | path | A | A' | B | B - A | % of A | B below A | typical / largest A' - A | verdict | BW close B - A (`v3.2.0-22-g541ce440`) |
|---|---|---|---|---|---|---|---|---|---|---|
| positional `INSERT` | literal | 23.79 | 23.10 | 23.43 | -0.05 | -0.2 % | 4 of 8 | 0.56 / 1.09 | inside | +0.48 |
| positional `INSERT` | prepared | 21.88 | 22.05 | 22.19 | +0.38 | +1.8 % | 3 of 8 | 0.27 / 0.84 | inside | +0.29 |
| point `SELECT` by key | literal | 30.71 | 30.21 | 30.55 | +0.27 | +0.9 % | 3 of 8 | 1.18 / 4.18 | inside | +0.11 |
| point `SELECT` by key | prepared | 30.11 | 30.81 | 30.80 | +1.18 | +3.9 % | 2 of 8 | 0.84 / 3.05 | inside, wide gap | +0.17 |

By start order, the same cells (B - A, us): u1 (B started first) insert -0.08 / +0.21, select-pk -0.02 / +1.18; u2 (B started last) insert -0.05 / +0.38, select-pk +0.27 / +0.88 (literal / prepared). No sign flips with order that the spread would not explain. Throughput form (derived, 1e6 / mean wall, section 4): positional `INSERT` at B 16,194 / 22,578 statements/s literal / prepared against 20,519 / 22,686 at A; the literal figure's mean is pulled down by a wall tail (p99 81 us) that the A' arm shows as well (19,876), so the literal throughput gap is host noise by the control, not a finding.

Unusual cells. u1 alone read elevated in all three arms alike: positional literal `INSERT` 39.0 / 39.4 / 38.9 us (A / A' / B) and prepared `SELECT` 45.6 / 46.1 / 46.3 us, against 23.1 / 22.7 / 23.2 and 29.8 / 29.4 / 29.9 in u2. The cause was not isolated (the first run's load was 10.9, the later ones 4.8-6.5; no `clean=false` window); the elevated cells carry the same level in A' as in A, so they say the host, not the build. They are left in the pooled medians, which is why the pooled absolute levels (23.8 vs 22.4 in the BW close for the literal `INSERT`) sit slightly above the close's.

## 3. The column-listed INSERT, B alone

Four runs, one server, paired by run (positional and listed alternate inside a run). Server CPU in us, and wall.

| shape | path | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU us |
|---|---|---|---|---|---|---|---|---|---|
| `INSERT INTO i VALUES (?, ?, ?)` | literal | 7,992 | 39.6 | 43.6 | 44.2 | 52.4 | 98.5 | 20,404 | 23.0 |
| `INSERT INTO i (w, id, v) VALUES (?, ?, ?)` | literal | 7,992 | 40.5 | 45.3 | 45.8 | 52.8 | 73.3 | 20,368 | 23.8 |
| `INSERT INTO i VALUES (?, ?, ?)` | prepared | 7,992 | 39.5 | 41.1 | 41.5 | 46.7 | 69.5 | 22,145 | 22.1 |
| `INSERT INTO i (w, id, v) VALUES (?, ?, ?)` | prepared | 7,992 | 40.3 | 42.3 | 42.8 | 48.5 | 68.0 | 21,738 | 23.3 |

Listed - positional server CPU, paired: **+1.26 us literal [0.10, 1.72], +1.37 us prepared [-0.31, 1.90]**, n = 4. The prepared interval crosses zero and 4 runs are few, so this is a direction (the listed statement costs one to two microseconds, about 5-6 % of a 22-23 us statement), with the p25/p50 wall rows agreeing (+1.2 to +1.6 us). The listed shape here also permutes the column order (`w, id, v`), which is the worst case for the mapping, not the typical one.

## 4. Latency of the positional INSERT and the point SELECT, pooled

Wall per statement in us, median over runs of each run's percentile (`bn_census.py` emits p90, not p95); ops = statements over 8 runs.

| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU us | driver CPU us |
|---|---|---|---|---|---|---|---|---|---|---|---|
| insert | literal | A | 15,984 | 41.1 | 43.2 | 43.9 | 54.7 | 80.2 | 20,519 | 23.8 | 24.5 |
| insert | literal | A2 | 15,984 | 41.0 | 43.2 | 43.8 | 55.7 | 88.6 | 19,876 | 23.1 | 24.7 |
| insert | literal | B | 15,984 | 37.5 | 43.3 | 43.9 | 55.2 | 81.2 | 16,194 | 23.4 | 24.5 |
| insert | prepared | A | 15,984 | 35.8 | 40.4 | 40.9 | 49.0 | 71.9 | 22,686 | 21.9 | 22.3 |
| insert | prepared | A2 | 15,984 | 34.7 | 40.5 | 41.0 | 49.1 | 71.4 | 22,471 | 22.1 | 22.2 |
| insert | prepared | B | 15,984 | 35.0 | 40.6 | 41.1 | 49.5 | 71.6 | 22,578 | 22.2 | 22.2 |
| select-pk | literal | A | 15,984 | 54.3 | 56.3 | 62.3 | 74.7 | 102.2 | 15,464 | 30.7 | 32.9 |
| select-pk | literal | A2 | 15,984 | 54.7 | 56.7 | 62.0 | 74.3 | 113.8 | 15,508 | 30.2 | 32.5 |
| select-pk | literal | B | 15,984 | 54.0 | 56.3 | 62.3 | 73.4 | 99.2 | 15,300 | 30.5 | 32.8 |
| select-pk | prepared | A | 15,984 | 51.0 | 53.2 | 57.4 | 69.1 | 140.7 | 16,415 | 30.1 | 31.1 |
| select-pk | prepared | A2 | 15,984 | 50.8 | 53.4 | 58.7 | 67.2 | 103.9 | 16,680 | 30.8 | 30.0 |
| select-pk | prepared | B | 15,984 | 51.3 | 53.4 | 58.7 | 66.9 | 89.7 | 16,718 | 30.8 | 30.0 |

The p25 and p50 columns (the body of the distribution, which a busy neighbour moves least) agree across A, A' and B to within 0.2 us for the `INSERT` (literal 43.2 / 43.2 / 43.3; prepared 40.4 / 40.5 / 40.6). `bad` replies = 0 in every cell. The row set is 1,000 rows (`--rows`), and these shapes are size-independent by construction (a keyed point statement); sizes 200 and 10,000 were not re-run.

## 5. Waits

The measured unit is one statement on one connection, `relaxed`, one core.

| wait type | positional INSERT | point SELECT |
|---|---|---|
| durability / fsync | none (`relaxed`) | none |
| write statement | server CPU 22-24 us of wall p50 41-44 us | n/a |
| read | in the statement | server CPU 30-31 us of wall p50 57-62 us |
| client and socket round trip | the remainder, about 19-20 us per statement (driver CPU 22-25 us) | about 27-32 us |
| lock or conflict wait | none: one connection | none |
| buffer-pool miss | not instrumented in this driver | not instrumented |

## 6. Insight

The positional statement is 22-24 us, dominated by the descent, the leaf write, WAL staging and the socket; the measured prepared +0.38 us is 0.45 of the largest A' - A gap (0.84), in the same direction as the BW close's +0.29 and unresolved at this run count, so this run neither shows nor rules out a small slowdown of that path (the code of S6 was not read for this document). A named list is a different statement: it pays one to two microseconds to map listed positions onto columns and fill the unlisted ones, which at 23 us is visible in a single-row statement and would be amortised in a multi-row one (not measured). If that cost matters, the permuted order is the part to look at, since the in-order list is the shape most clients send; that variant was not run.

The point `SELECT` prepared cell's +1.18 us is the largest inside-the-spread delta here and its spread is the widest; the BW close measured +0.17 us on a quieter host, and nothing in S6 touches a keyed read, so the cell is read as host noise and should be re-run quiet if the question matters.
