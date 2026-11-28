# EB close: the four built arms cost what their proxies cost, and a point read pays 0.1 to 0.25 us for it

**Thesis.** Interleaved against the commit EB opened from (`9e7597d5d`, `v3.4.17-224-g9e7597d5d`), EB's closing engine (`a610d007a`, `v3.4.17-231-ga610d007a`) answers the four shapes EB built **31x to 2,989x cheaper in server CPU** (R1 31x, O1 252x, J1 1,283x, J2 2,989x, at 100,000 rows), each now doing work proportional to the rows it returns rather than to the relation (`ANALYZE` row counts below). The price is on the fixed cost of a point **read**: server CPU per statement is **+1.5 % to +3.2 % higher on all four key forms** (+0.10 to +0.25 us of 6.5 to 7.9 us), outside a same-binary control whose own spread is under 1 % on reads, and reproduced by a second, independent pass (+1.0 % to +3.1 %). Client p50 moves by 0.1 to 0.2 us of 16.8 to 18.3 us (about 1 %). Point **writes**, the `ON CONFLICT DO NOTHING` that conflicts and the issued-key `INSERT` show **no delta above their noise floor** (point estimates -0.6 % to +1.4 %, intervals 1 to 2.6 % wide); a 0.1 to 0.2 us cost on a write is inside those intervals and cannot be excluded. The 0.1 to 0.25 us is **not located**: nothing in this run attributes it to a function.

| field | value |
|---|---|
| date executed | 2026-11-27, pass 1 (all 14 cells) 23:12-23:15 UTC; pass 2 (the ten `OH-` cells, fresh servers) 23:15-23:17 UTC; one smoke run at one tenth of the statement counts before pass 1 (not reported) |
| branch / tree | `worktree-eb-key-forms-in-the-executor`, worktree `eb-key-forms-in-the-executor`, HEAD `a610d007a`; tracked tree dirty only in `CLAUDE.md` (a modification this run did not touch); `tools/eb_close.py` and this file's archive untracked |
| **base** (A) | `build-release/eb-close/kds_server.base-9e7597d5d`, sha256 `c4ad5c210a12130abd264a5e7efa8750056be9a207a595377a60f76d48a34c84`, `git describe --tags` of its commit `v3.4.17-224-g9e7597d5d` (commit time 2026-11-27 15:12:08 -05:00); copy mtime 2026-11-27 18:07:09 -05:00, **after its commit time**. EB's opening commit, and the engine EB-S0 called "today" |
| **close** (B) | `kds_server.close-a610d007a`, sha256 `a84ab15646cf426ec84a2da8c24115839f749577f7f71ee308abde0cc1dbca58`, `v3.4.17-231-ga610d007a` (commit time 2026-11-27 18:06:38 -05:00); mtime 18:07:09 -05:00, **after its commit time** by 31 s |
| **ctl** | the base binary started as a third server: the in-run noise floor (same sha256 as base) |
| binaries run from | `cp` of each into `/home/ukjin/bench-runs/eb-close-run/` (pass 2: `eb-close-run2/`), hashes re-taken from the copies by the driver and equal to the above (`archive/.../eb-close.json`, `arms`) |
| build | Release (`CMAKE_BUILD_TYPE=Release` in `build-release/CMakeCache.txt`); built by the main session, not rebuilt here |
| device | data files and WAL under `/home/ukjin/bench-runs/`, `/dev/mapper/fedora-root` (xfs on LVM, `df -T` at run time); not tmpfs |
| server | `cores = 1`, `durability = relaxed`, `buffer_pool_frames` 131,072, `log_level = warn`, `join_order = planned` (as EB-S0's driver set it), ports 31960-31962 (pass 2: 31970-31972), never 15432; a fresh data file and a fresh server per arm and per pass |
| host | Intel Core Ultra 7 270K Plus, 24 CPUs, P-cores 0-7, E-cores 8-23, shared with other sessions; **no build or ctest process in any of 756 blocks (pass 1) or 540 (pass 2)** (`/proc/*/comm` scan for `cc1plus`, `cc1`, `ld`, `ctest` before each block); load average (1 min) 1.21-1.75 (mean 1.50) in pass 1, 1.06-1.31 (mean 1.21) in pass 2, per-block values in the JSON |
| pinning | driver on CPU 1; an idle server's threads on CPU 0; the server being measured moved onto one measuring CPU for its block. **The measuring CPU is the same for all three arms within a repeat and rotates over CPUs 2-7 between repeats**, so a slow slot cannot sit under one arm (a fixed arm-to-CPU map cost an earlier DK close a 6 us artefact) |
| driver | `tools/eb_close.py` (new, untracked; `bench/docs/` has no entry for it yet, outside what this run was allowed to write). Archive: `bench/v3.5.0/archive/eb-close-v3.4.17-231-ga610d007a/` (`eb-close.json`, `eb-close-pass2-oh.json`, rendered `tables.md`, `tables-pass2.md`, driver logs, status files) |
| PostgreSQL floor | not measured for these shapes (no server installed on this host) |
| prior number for this shape | none for the overhead cells: this file is their baseline. For the four built arms, EB-S0 (`results-eb-s0-premise-v3.4.17-225-gb761306e0.md`) measured the same shapes at the base engine on 100,000 rows: R1 502 us, J1 240,430 us, J2 223,136 us, O1 3,887 us server CPU; this run's base arm reads 538, 260,138, 243,476 and 4,107 us, within 4-9 % of those on a different day and load |

## 1. Method, and what was cut

Three servers (base, close, ctl) are up together on fresh data files with the same seven relations, **100,000 rows each** (`bt` btree tuple key; `hp` heap with a hidden Keystone read by `keyidentity`; `e` embedded key `(a int32, b int16)`; `w` hashed key `(k varchar(40), n int64)`; `bi` an insert target; `c` with index `c_ab (a, b)`, `a = id % 10`; `o` 100 outer rows). One KWP connection an arm, one statement a round trip. Each cell is **18 repeats**: in each, the three arms run the same statements from one seed (equal work: 12,000 reads, 9,000 writes or conflicts, 6,000 inserts a block for the `OH-` cells; 200, 6, 8 and 100 statements for R1, J1, J2 and O1) in one of the six arm orders, so every order occurs three times. **The row count (reads) or the reply tag (writes) of every statement is compared across arms, and the run stops if one differs**; none did. The unit is **server CPU per statement**: on-CPU ns summed over the server's threads (`/proc/<pid>/task/*/schedstat`) over the block, divided by its statements. The delta is **paired by repeat** (close / base - 1 within one repeat), reported as a mean with a 95 % t-interval over 18 repeats; the control row is ctl / base the same way, so **its spread is the noise floor of that cell**. A delta is a finding only where its interval clears the control's.

Scenarios are in the driver's header; the key literals are drawn uniformly per statement.

| cell | statement | key form |
|---|---|---|
| `OH-bt-read` / `OH-bt-write` | `SELECT v FROM bt WHERE id = K` / `UPDATE bt SET v = v + 1 WHERE id = K` | btree, int64 tuple key |
| `OH-hp-read` / `OH-hp-write` | the same by `keyidentity = K` on `hp` | heap, hidden Keystone |
| `OH-e-read` / `OH-e-write` | `... WHERE a = A AND b = B` on `e` | embedded primary key |
| `OH-w-read` / `OH-w-write` | `... WHERE k = 'key-......' AND n = N` on `w` | hashed primary key on a btree (a unique index `$pkey` beside a hidden Keystone) |
| `OH-bt-conflict` | `INSERT INTO bt VALUES (K, 0) ON CONFLICT DO NOTHING`, K present: writes nothing | S4's neighbour on the tuple key |
| `OH-bi-insert` | `INSERT INTO bi (v) VALUES (x)`: issued key, rightmost leaf | |
| `G-R1` | `SELECT id, v FROM bt WHERE id >= X ORDER BY id LIMIT 100` | S1 |
| `G-J1` | `SELECT o.id, c.v FROM o JOIN c ON c.a = o.a WHERE c.b = o.b` | S2, index `(a, b)` |
| `G-J2` | `SELECT o.id, e.v FROM o JOIN e ON e.a = o.ea WHERE e.b = o.eb` | S2, embedded key |
| `G-O1` | `SELECT a, b, v FROM e ORDER BY a, b LIMIT 100` | S3 |

**Cut, plainly.** (1) **Row counts 200 and 10,000** (rule 9): the order named 100,000; a point statement does not scale with rows, but this run does not show it, and the four built arms' independence of N is read from `ANALYZE`'s examined counts at 100,000 rows only (EB-S0 swept 1,000,000). (2) **`strict` and `group` durability**: every cell is `relaxed`, so there is no fsync in any number here and the commit wait is absent by construction; the claim is a compile path's, not a commit's. (3) **More than one core, more than one connection**: no saturation cell; the limit that bounds each cell is one client round trip (below), not the engine. (4) **S4's column-list and embedded-key `ON CONFLICT`** (a shape the base engine refuses) and J3 (the order's J3, built with S2, was not among the cells asked). (5) **Pass 2 re-ran the `OH-` cells only**; the `G-` cells were measured once. (6) **No profile**: the source of the read overhead is not located. **Time:** about 10 minutes for the driver, one smoke run and both passes (loading 1.4-5.4 s an arm, pass 1 4 minutes, pass 2 2.5 minutes); nothing was built and no quiet-gate hold was needed. **Watchdog:** pass 1 was polled every 60 s by a status-file watcher (`status_age` 0-1 s at each poll); that watcher read the driver's pid once, so it reported the finished pass 1 as an orphan when pass 2 replaced the pid file, a false alarm; **pass 2 was watched by a blocking wait on its status file, not by the 60 s poll**, and finished with `done`.

## 2. A point read pays a fixed 0.1 to 0.25 us; a point write shows nothing above noise

Server CPU per statement, mean of 18 repeats with the 95 % half-interval. "Throughput" is `1,000,000 / CPU us`, **derived** by this document from the CPU column, not reported by the driver: the rate a serial connection would reach if the client cost nothing, an upper bound that no client reaches (the client's own round trip is about 10 us and is the actual limit, below). Pass 1; pass 2's deltas follow.

| cell | base | close | ctl (base again) | close vs base | ctl vs base (floor) | derived throughput base / close / ctl |
|---|---|---|---|---|---|---|
| `OH-bt-read` | 6.51 +-0.03 us | 6.61 +-0.04 us | 6.50 +-0.03 us | **+1.5 % +-0.5** | -0.3 % +-0.6 | 153,496 / 151,283 / 153,911 /s |
| `OH-hp-read` | 7.17 +-0.03 us | 7.32 +-0.03 us | 7.18 +-0.03 us | **+2.0 % +-0.4** | +0.0 % +-0.2 | 139,406 / 136,663 / 139,357 /s |
| `OH-e-read` | 7.34 +-0.05 us | 7.52 +-0.03 us | 7.32 +-0.05 us | **+2.4 % +-0.6** | -0.3 % +-0.8 | 136,216 / 132,978 / 136,687 /s |
| `OH-w-read` | 7.87 +-0.03 us | 8.12 +-0.05 us | 7.86 +-0.03 us | **+3.2 % +-0.7** | -0.1 % +-0.4 | 127,112 / 123,208 / 127,205 /s |
| `OH-bt-write` | 7.41 +-0.10 us | 7.38 +-0.05 us | 7.36 +-0.03 us | -0.3 % +-1.3 | -0.6 % +-1.4 | 134,989 / 135,445 / 135,879 /s |
| `OH-hp-write` | 8.14 +-0.16 us | 8.13 +-0.14 us | 8.19 +-0.17 us | -0.1 % +-1.5 | +0.7 % +-2.0 | 122,900 / 123,063 / 122,137 /s |
| `OH-e-write` | 8.31 +-0.10 us | 8.35 +-0.13 us | 8.35 +-0.16 us | +0.6 % +-2.1 | +0.5 % +-1.8 | 120,385 / 119,719 / 119,804 /s |
| `OH-w-write` | 9.55 +-0.17 us | 9.48 +-0.08 us | 9.51 +-0.12 us | -0.6 % +-2.0 | -0.3 % +-1.7 | 104,690 / 105,479 / 105,140 /s |
| `OH-bt-conflict` | 5.90 +-0.05 us | 5.96 +-0.08 us | 5.91 +-0.03 us | +1.0 % +-1.3 | +0.1 % +-0.5 | 169,397 / 167,684 / 169,275 /s |
| `OH-bi-insert` | 6.20 +-0.03 us | 6.24 +-0.03 us | 6.20 +-0.03 us | +0.6 % +-0.5 | -0.0 % +-0.4 | 161,244 / 160,322 / 161,328 /s |

The read rows are the only ones that clear their control, and they clear it on every form. **Pass 2** (fresh servers, same commands) read close vs base at **+2.8 % +-2.8** (`OH-bt-read`), **+1.0 % +-0.5** (`hp`), **+3.1 % +-0.6** (`e`), **+2.8 % +-0.8** (`w`), against a control of -0.5 % to +0.3 %; its writes read -0.2 % to +1.4 % (intervals 1.1 to 2.6) and its conflict and insert +0.2 % each. The two passes agree in sign on every read row and on the order of the effect (`e` and `w` about 3 %, the tuple-key forms 1 to 2.8 %); the `bt` read's pass-2 interval is wide (one repeat was slow in close) and its pass-1 interval is the tighter statement. In absolute terms the read cost is **+0.10 us (`bt`), +0.15 us (`hp`), +0.18 us (`e`), +0.25 us (`w`)** in pass 1 and +0.18, +0.07, +0.23, +0.22 us in pass 2.

**What the writes can and cannot say.** Their point estimates are -0.07 to +0.04 us, but their intervals are +-1.3 to +-2.1 % (0.1 to 0.17 us) because a write's CPU is noisier than a read's (the control's own spread is as wide). A read-sized 0.1 to 0.25 us is inside that interval for every write row. So the honest statement is "no write delta resolved", not "writes are free": a regression of the reads' size on writes would not have been seen. The conflict row (+1.0 % +-1.3, then +0.2 % +-0.7) and the insert row (+0.6 % +-0.5, then +0.2 % +-1.3) are likewise inside what a floor of 0.5 to 1.4 % allows.

**Where the microseconds are.** A point read is 6.5 to 7.9 us of server CPU inside a 16.8 to 18.3 us client round trip (p50 below): about 10 us of each statement is the socket, the framing and the Python client, which no engine change touches. The read overhead is therefore about 0.6 to 1.4 % of what the client sees and moves client p50 by 0.1 to 0.2 us. A durability wait is not a component here (`relaxed`: no fsync, `strict` not measured), and no lock or conflict wait occurs on one connection; the decomposition of a latency in this file is *server CPU + socket and client*, with the second term the difference between the CPU column and the latency table.

**Insight.** The cost lands on reads on all four forms alike, including the plain btree tuple key (+0.10 us) whose arm EB did not change, and it grows with the number of columns in the key (`bt` 1, `hp` 1, `e` 2, `w` 2: the two-column forms are the larger pair, +0.18 and +0.25 us against +0.10 and +0.15). That pattern fits the work order's stated expectation that the compile path (`step_compiler.cpp`'s ladder) gained "a few checks per step" and not a cost in any one arm's executor; it does not prove it. **Not verified:** which check, and whether it is in the compile or in the per-statement execution; the method that would settle it is a `perf` or callgrind comparison of the two binaries on `OH-w-read`, the largest delta. A statement-compile cache would remove the term for repeated shapes, but CX-S6 records that none is built, so each of these 12,000-statement blocks re-compiled every statement.

## 3. The built arms: work now proportional to the rows returned

Server CPU per statement over 18 repeats; the ratio is the base mean over the close mean. Throughput is derived (`1,000,000 / CPU us`) as above. The control rows are base against base: R1, J1 and O1 sit within 0.4 %; **J2's control reads -2.5 % +-0.1, a real 2.5 % difference between two servers running one binary** (a property of a server instance's memory placement or heap state, not of the engine's code), which is the floor for the slow cells and changes no conclusion on a 2,989x cell.

| cell | base | close | ctl | gain (base / close) | ctl vs base | derived throughput base / close |
|---|---|---|---|---|---|---|
| `G-R1` | 537.55 +-11.68 us | 17.11 +-0.47 us | 539.43 +-11.85 us | **31.4x** | +0.3 % +-0.2 | 1,860 / 58,433 /s |
| `G-J1` | 260,138 +-1,064 us | 202.7 +-9.8 us | 261,181 +-954 us | **1,283x** | +0.4 % +-0.1 | 4 / 4,933 /s |
| `G-J2` | 243,476 +-809 us | 81.5 +-5.8 us | 237,477 +-774 us | **2,989x** | -2.5 % +-0.1 | 4 / 12,277 /s |
| `G-O1` | 4,106.8 +-12.9 us | 16.29 +-0.43 us | 4,098.5 +-12.4 us | **252x** | -0.2 % +-0.1 | 243 / 61,376 /s |

Every gain is the access arm changing, and `ANALYZE` of the first statement of each cell (one per arm, in the JSON) says how:

| cell | base `ANALYZE` | close `ANALYZE` |
|---|---|---|
| `G-R1` | `Scan bt`, filter `id >= x`: examined 17,711, 90 pages for `x = 17,612` (a walk from the head to `x`, so its cost grows with `x`) | `Range bt range=[x, 2^48-1]`: examined 287, 2 pages |
| `G-J1` | `IndexProbe c key=a` only, `b` filtered per entry: examined 1,000,000, 1,000,100 pages, 100 matched | `IndexProbe c key=a,b`: examined 100, 201 pages |
| `G-J2` | `Scan e` per outer row: examined 10,000,000, 57,900 pages, 100 matched | `Probe e key=embedded(a,b)`: examined 200, 101 pages |
| `G-O1` | `Scan e` of 100,000 rows then `sort a asc, b asc` and a top-100 | `Scan e` stopping at 100: examined 100, no sort node |

Latency distributions, pass 1, all statements pooled, with counts:

| cell | arm | n | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `G-R1` | base | 3,600 | 54.2 us | 377.1 us | 659.0 us | 1,149.5 us | 1,236.1 us |
| `G-R1` | close | 3,600 | 46.5 us | 48.9 us | 49.8 us | 53.4 us | 110.2 us |
| `G-J1` | base | 108 | 255,420 us | 258,397 us | 261,390 us | 263,603 us | 263,950 us |
| `G-J1` | close | 108 | 155.1 us | 180.6 us | 238.1 us | 589.8 us | 640.0 us |
| `G-J2` | base | 144 | 240,506 us | 242,071 us | 244,549 us | 245,668 us | 246,071 us |
| `G-J2` | close | 144 | 79.0 us | 82.9 us | 100.7 us | 380.3 us | 441.2 us |
| `G-O1` | base | 1,800 | 4,099 us | 4,235 us | 4,281 us | 4,476 us | 4,711 us |
| `G-O1` | close | 1,800 | 54.5 us | 56.3 us | 57.0 us | 63.9 us | 269.2 us |

R1's base distribution is wide (p0 54 us to p99 1,236 us) because its cost is the distance from the head to the random `x`; the close arm is a 3-leaf range whose p25-p95 span is 4.5 us. J1 and J2 close have a long right tail (p95 590 and 380 us against p50 238 and 101) that the means hide: a minority of blocks ran slower, consistent with 6 and 8 statements a block and the p95 of 108 and 144 samples being one or two values. These are at the same order as the proxies EB-S0 priced (S0: R1 16.3 us, J1 187 us, J2 49 us over a tuple-key probe, O1 14.8 us over an elided sort; this run's close arm: 17.1, 202.7, 81.5 and 16.3 us), so **the built arms reach their proxies' cost**, J2 being the one above its proxy (a probe computed per outer row against a tuple-key probe of 100 rows). The independence of N is **not** measured here; at 100,000 rows the base's `ANALYZE` shows examined counts of 17,711 (R1, x-dependent), 1,000,000 (J1), 10,000,000 (J2) and 100,000 (O1) against 287, 100, 200 and 100.

## 4. Latency: the client round trip is the floor under every point statement

Pass 1, all statements pooled, n a row's count over 18 repeats. Close against base differs by 0.1 to 0.3 us at p0 to p99 on the reads and by 0.0 to 0.1 us on the writes; no percentile of a write moves visibly. Tails (max up to 335 ms, in `OH-bt-write` base) are single events on a shared host and appear in base, close and control alike (`max` columns of the JSON); the p99 column is the stable one.

| cell | arm | n | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|
| `OH-bt-read` | base | 216,000 | 15.9 us | 16.6 us | 16.8 us | 17.8 us | 18.6 us |
| `OH-bt-read` | close | 216,000 | 16.0 us | 16.7 us | 16.9 us | 17.9 us | 18.7 us |
| `OH-bt-read` | ctl | 216,000 | 15.8 us | 16.6 us | 16.8 us | 17.8 us | 18.6 us |
| `OH-bt-write` | base | 162,000 | 14.8 us | 15.6 us | 15.8 us | 16.9 us | 19.3 us |
| `OH-bt-write` | close | 162,000 | 14.9 us | 15.6 us | 15.8 us | 16.9 us | 19.5 us |
| `OH-bt-write` | ctl | 162,000 | 14.8 us | 15.6 us | 15.8 us | 16.9 us | 18.6 us |
| `OH-hp-read` | base | 216,000 | 16.3 us | 17.3 us | 17.5 us | 18.5 us | 19.3 us |
| `OH-hp-read` | close | 216,000 | 16.4 us | 17.4 us | 17.7 us | 18.7 us | 19.4 us |
| `OH-hp-read` | ctl | 216,000 | 16.3 us | 17.3 us | 17.5 us | 18.5 us | 19.2 us |
| `OH-hp-write` | base | 162,000 | 15.3 us | 16.2 us | 16.5 us | 17.8 us | 20.5 us |
| `OH-hp-write` | close | 162,000 | 15.4 us | 16.2 us | 16.5 us | 17.7 us | 20.1 us |
| `OH-hp-write` | ctl | 162,000 | 15.3 us | 16.3 us | 16.5 us | 17.8 us | 20.7 us |
| `OH-e-read` | base | 216,000 | 16.7 us | 17.5 us | 17.7 us | 18.7 us | 19.5 us |
| `OH-e-read` | close | 216,000 | 16.9 us | 17.7 us | 17.9 us | 18.8 us | 19.5 us |
| `OH-e-read` | ctl | 216,000 | 16.7 us | 17.4 us | 17.7 us | 18.6 us | 19.3 us |
| `OH-e-write` | base | 162,000 | 15.7 us | 16.5 us | 16.7 us | 17.9 us | 20.7 us |
| `OH-e-write` | close | 162,000 | 15.5 us | 16.5 us | 16.7 us | 17.9 us | 20.2 us |
| `OH-e-write` | ctl | 162,000 | 15.7 us | 16.5 us | 16.7 us | 18.0 us | 20.6 us |
| `OH-w-read` | base | 216,000 | 17.3 us | 18.0 us | 18.3 us | 19.2 us | 20.0 us |
| `OH-w-read` | close | 216,000 | 17.4 us | 18.3 us | 18.5 us | 19.5 us | 20.3 us |
| `OH-w-read` | ctl | 216,000 | 17.2 us | 18.0 us | 18.3 us | 19.3 us | 20.1 us |
| `OH-w-write` | base | 162,000 | 16.6 us | 17.6 us | 17.9 us | 19.6 us | 21.3 us |
| `OH-w-write` | close | 162,000 | 16.7 us | 17.7 us | 17.9 us | 19.3 us | 21.0 us |
| `OH-w-write` | ctl | 162,000 | 16.7 us | 17.7 us | 17.9 us | 19.5 us | 21.3 us |
| `OH-bt-conflict` | base | 162,000 | 12.9 us | 14.2 us | 14.4 us | 15.3 us | 16.3 us |
| `OH-bt-conflict` | close | 162,000 | 13.3 us | 14.2 us | 14.4 us | 15.3 us | 16.3 us |
| `OH-bt-conflict` | ctl | 162,000 | 12.9 us | 14.2 us | 14.4 us | 15.3 us | 16.2 us |
| `OH-bi-insert` | base | 108,000 | 13.6 us | 14.3 us | 14.5 us | 15.4 us | 18.8 us |
| `OH-bi-insert` | close | 108,000 | 13.8 us | 14.4 us | 14.5 us | 15.5 us | 18.7 us |
| `OH-bi-insert` | ctl | 108,000 | 13.8 us | 14.3 us | 14.5 us | 15.4 us | 18.5 us |

The tuple-key read costs 16.8 us at the client and 6.5 us of server CPU; the remaining 10.3 us is outside the engine. That ratio is why a 0.1 to 0.25 us engine cost is 0.6 to 1.4 % of what a client of this kind sees, and why a **saturating** load, where the server's CPU rather than a client round trip is the bound, would show the same increment as the 1.5 to 3.2 % of server throughput (derived column above). This run did not drive such a load (cut 3), so that last sentence is arithmetic on the CPU column and not a measurement.

## 5. What the run teaches, and what it does not

- **The four built arms are real and large**: the base engine ran each shape as a scan or a half-key probe whose cost was the relation (examined 17,711 to 10,000,000 rows), the closing engine as a descent or a window whose cost is the answer (100 to 287 examined). EB-S0's expectation that each family sits above its proxy's 2x threshold holds at 100,000 rows for R1, J1, J2 and O1 (31x to 2,989x) and the close arm lands on the proxy's cost.
- **The compile path got dearer for the statements that did not need EB**: every point read pays 0.10 to 0.25 us, by key form in the order `bt` < `hp` < `e` < `w`. This is the first data point on the work order's overhead question; it is small, it reproduced across two passes, and it is above the control. It is not located.
- **A write's fixed cost cannot resolve it**: at 7 to 10 us with a 1.3 to 2.1 % interval, a write could carry the same increment unseen. A cell that would see it needs more repeats of the write blocks or a lower-variance write (the `bi` insert is the quietest at +-0.5 % but its second pass was wider).
- **The control found one real instance difference** (J2, 2.5 %) that no CPU pinning removes; a cell with a floor that large would have been misread had it been measured without a same-binary arm.
- **Not found**: any cell where close was slower than base on a statement EB claims to speed, and any `OH-` cell where the two arms returned different answers.

## 6. Tests

No engine code, test, or document under `src/`, `include/`, `tests/` or `instructions/` was changed by this run, so the suite was **not executed** and is not part of this file's claim. The built arms' answers were compared across arms per statement by the driver (row counts for reads, reply tags for writes), as the file's equal-answers check; no `--verify`-style durability check applies (`relaxed`, no crash cell).
