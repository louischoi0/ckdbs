> The same cells with the remedy commit `9f588fa2` as a third binary: `bench/v3.4.13/results-df-close-remedy-v3.4.13-15-g9f588fa2.md`.
> DF's close, final (the code as it stands, with the cached index fold, `c16b4fc2`, and the four files tied together): `bench/v3.4.13/results-df-close-final-v3.4.13-17-gc16b4fc2.md`.
> The same cells attributed with knob binaries (what each group of S2 to S4's additions removes): `bench/v3.4.13/results-df-close-knobs-v3.4.13-14-gec5ddfe8.md`.

# DF close: stages S2 to S4 add +0.16 to +0.20 µs to a point SELECT and nothing resolvable to a write; with DF-S1 the bar is not resolved, and its central estimate is over

On worktree `da-q8-memory-bound-key` (branch `worktree-df-statistics-coverage`)
at `ec5ddfe8` (`v3.4.13-14-gec5ddfe8`), the three stages of work order DF that
landed after DF-S1 - DF-S2 `c2925b6e` (transaction counts, the read horizon,
a view's mint stamp, two session words), DF-S3 `83aad532` (index and lock
statistics) and DF-S4 `29810582` (leaf division counts) - cost a
point statement, at `cores = 1` and `durability = relaxed`, **+0.17 µs of
server CPU on a tuple key `SELECT` (+0.20 literal, +0.16 prepared), +0.35 and
+0.32 µs on an index-served `SELECT`, and between -0.06 and +0.13 µs - inside
the noise - on an `UPDATE` and an `INSERT`**, against the parent `8e7c820f`
(`v3.4.13-10-g8e7c820f`, `main` before DF-S2; DF-S1 is in it). The same
cells at 200 and at 10,000 rows give the same figures (below), so the cost
is a per-statement one and does not scale with the row count.

**Verdict against DF-Q10 (a)'s bar** (all of DF adds no more than 0.5 µs of
server CPU a point statement at `cores = 1`): **not resolved, and the central
estimate for a point read is over it.** DF-S1's own measured cost on a tuple
key read is +0.44 µs
(`bench/v3.4.13/results-cx-close-bisect-v3.4.13-1-g11f72ce9.md`, pooled
over four lifetimes, interval +0.27 to +0.54); set beside this file's +0.17
to +0.20 µs the sum for all of DF is about +0.61 µs with a naive interval of
+0.40 to +0.76 µs, which straddles the bar. For a write DF-S2 to DF-S4 are at
zero and DF-S1's write share was never measured, so the bar is not decided
there either. **A cell at `cores = 8` was attempted and did not resolve**:
its one lifetime is bimodal (two servers of one binary 9.5 µs apart) and all
four of its cells were unclean (below). Nothing in this file is rounded
into a pass.

Driver and raw output: `archive/df-close-v3.4.13-14-gec5ddfe8/`. The driver
is `tools/bn_census.py` unmodified (`--cores` and the `select-index` shape
landed in `ec5ddfe8`, outside any measurement stage, DF-Q14 (a));
`life.sh` is the shell wrapper that starts one server lifetime of it,
`an.py` the analysis every table below is generated from (`an_*.txt` are its
outputs), `build.py` the build. No PostgreSQL twin: the question is a delta
inside this engine (`bench/README.md`, rule 4). No number here is set beside
another host's.

## Run stamp

| Item | Value |
|---|---|
| Executed | 2026-11-19, 06:44 to 07:28 UTC (builds 06:44:54 to 06:47:32; `cores = 1` lifetimes 06:51:42 to 06:59:00; the `cores = 8` lifetime 06:59:12 to 07:14:06; the row-count sweep 07:14:29 to 07:28:15, of which 07:14:29 to 07:20:49 was held, below) |
| Branch / worktree | `worktree-df-statistics-coverage`, worktree `da-q8-memory-bound-key`, tree clean at `ec5ddfe8` |
| A, the baseline | `8e7c820f2db14dc2d0e176fe640c8fe061178f28` (`v3.4.13-10-g8e7c820f`, committed 2026-11-19 05:03:15 UTC): `main` before DF-S2, DF-S1 in it |
| B, the measured | `ec5ddfe8b3ea5c5904a24763991cc5fb72810def` (`v3.4.13-14-gec5ddfe8`, committed 06:43:48 UTC): DF-S2 `c2925b6e`, DF-S3 `83aad532`, DF-S4 `29810582`; `ec5ddfe8` itself changes `tools/bn_census.py` only. `git diff 8e7c820f ec5ddfe8` is 53 files, +2,933 / -178 |
| A' | A's binary again, a second server of it in every lifetime: the same-binary control, the floor each lifetime states for itself |
| Build | Release, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, `kds_server` only, GNU 14.2.0, from a `git archive` of each commit into `/home/louis/dfclose/src<commit>`, built there (`build.py`), 78 s each; never `./build`, never the worktree's `build-release` |
| Binary provenance | A: `ab5b9ee7cc7875bbe199ee402214b8e12176c8a00f909eb6e0ca1aae4cba9437`, built 06:46:12 UTC; B: `35bac562e5bfcc23614cd70aa35e9fda44359b52a5fa3f6eb9b8024fb4021604`, built 06:47:29 UTC. Each binary is newer than its commit; every server started from a copy in `/home/louis/dfclose/run/` (`kds_server_A`, `kds_server_B`), hashed after the copy |
| Device | Data files and WAL under `/home/louis/dfclose/run/wd_*/` on `/dev/nvme0n1p1`, ext4 (`df -T` at run time); not tmpfs |
| Host | Intel Xeon Platinum 8581C @ 2.10 GHz, 8 CPUs (2 threads a core), Linux 6.12.116+deb13-cloud-amd64, no PMU. `cores = 1` cells: server pinned to CPU 3, the driver to CPU 5 (SMT siblings 7 and 1, watched by the cell gate). Another session's measurement was running pinned to CPUs 2 and 4 (the driver's defaults) when this run started; a first attempt on 2 and 4 was killed after one cell and discarded, and this run moved to 3 and 5 |
| Server config | `cores = 1` (and 8 for one lifetime), `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, every other key default; ports 32100 to 32400 |
| Host load | `cores = 1` lifetimes: 1-minute load 1.39 to 2.15 at the 240 arm records of the ten main lifetimes (`host` in every arm of every `out_*.json`), 0.84 to 9.10 in the minute-by-minute watch (`watch.txt`) over the whole run, the high end when another session ran its suite at 07:13 to 07:17; 79 of the 80 cells of the ten lifetimes were clean, the one that was not being `insert literal` of lifetime 1 |

## Method

**What a lifetime is.** One `bn_census.py` invocation with `--runs 1`: three
fresh servers (A, A', B), a fresh data file each, the relations loaded, then
every cell on all three, arms interleaved in blocks and the order of a block
rotating by one server per block; the order of the `--arm` flags rotates with
the lifetime number (`life.sh`). A lifetime ends by stopping its servers, so
every lifetime is three new processes. The bisect of CX's close
(`bench/v3.4.13/results-cx-close-bisect-v3.4.13-1-g11f72ce9.md`) found that
two servers of one binary differ by up to about 0.27 µs, "the floor is the
process"; this run therefore restarts the servers ten times for `cores = 1`
and reports each lifetime, with A' in every one.

| Parameter | Value |
|---|---|
| Cells | `select-pk` (`SELECT * FROM t WHERE id = ?`, a tuple key descent, one row), `select-index` (`SELECT * FROM x WHERE v = ?`, an equality `x_v` serves, one row), `update-lit` (`UPDATE t SET v = ? WHERE id = ?`, autocommit, one row), `insert` (`INSERT INTO i VALUES (?, ?, ?)`, autocommit); each on both driver paths, `literal` (an unnamed `C_PARSE` of the text each time) and `prepared` (one named parse, `BIND`/`EXECUTE`) |
| Statements | 12,000 an arm, cell and lifetime, in 24 blocks of 500; a block is one arm's run of 500 statements, the unit of pairing |
| Lifetimes | 10 at `cores = 1`, 1,000 rows; 6 each at 200 and 10,000 rows (select-pk, select-index and update-lit; no insert); 1 at `cores = 8` |
| Row-count mapping | `--rows N` sets the rows of `t` and `x` (the cells' relations); `--scan-rows` is fixed at 1,000 and no cell here reads it. 200, 1K and 10K are `--rows 200`, `1000`, `10000` |
| Concurrency | one connection, serial, closed loop |
| Server CPU | the sum of `/proc/<pid>/task/*/schedstat`'s first field over every server thread, over a block, per statement |
| A delta | the median over a lifetime's blocks of the block-paired difference (B's block minus A's block, run within seconds of each other), then pooled: the median over every block of every clean lifetime, with a 2,000-resample bootstrap of that median over blocks (seeded); and the median of the lifetimes' medians with its bootstrap over lifetimes |
| Floor | A' - A, the same two quantities: it states what two servers of one binary differ by, per lifetime and pooled |
| Clean cell | the driver's gate (`--cell-quiet-pct 15 --cell-quiet-wait 60 --cell-retries 2 --cell-clean-pct 25`): a cell runs in a window where the pinned CPUs and their SMT siblings are quiet, and is clean when the siblings stayed at or under 25 % and no foreign task took more than 5 % of the server's or the driver's CPU; an unclean cell is dropped from every paired table here and named |
| Correctness | every reply is checked against its shape's tag and a `select-pk` reply for one row: 0 bad replies over every arm, cell and lifetime of this file |

The exact command of lifetime `i` of the `cores = 1` run (`life.sh c1 1 10 1 3 12000 24 select-pk,select-index,update-lit,insert 1000 32100 5`;
`<run>` is `/home/louis/dfclose/run`, the worktree's `tools/bn_census.py`,
git blob `a6aee7135c964c75a01a695627055ef95d54b5b7`; `A2` is the driver's label for A'):

```
python3 tools/bn_census.py --arm A=<run>/kds_server_A --arm A2=<run>/kds_server_A --arm B=<run>/kds_server_B \
  --workdir <run>/wd_c1_i --port 32100 --pin-server 3 --pin-driver 5 --cores 1 --rows 1000 --scan-rows 1000 \
  --ops 12000 --blocks 24 --runs 1 --only select-pk,select-index,update-lit,insert \
  --cell-quiet-pct 15 --cell-quiet-wait 60 --cell-retries 2 --cell-clean-pct 25 --out <run>/out_c1_i.json
```

The order of the three `--arm` flags is A, A2, B for `i` divisible by 3, A2, B, A for `i` = 1 mod 3 and B, A, A2 for `i` = 2 mod 3. The
sweeps are `life.sh r200 1 6 1 3 12000 24 select-pk,select-index,update-lit 200 32300 5`
and `... r10k ... 10000 32400 5` (`sweep.sh`, `sweep2.sh`); the `cores = 8` cell is
`life.sh c8 1 6 8 0,1,2,3,4,5,6,7 12000 24 select-pk,update-lit 1000 32200 5`, stopped after lifetime 1 (below).

## Result: `cores = 1`, 1,000 rows, ten lifetimes

What S2 to S4 cost, pooled over the ten lifetimes (9 for `insert literal`,
whose first lifetime's cell was unclean and is dropped); server CPU a
statement, A being the baseline:

| Cell | A server CPU | B - A pooled median [95 % bootstrap over blocks] | lifetime medians of B - A above 0 | A' - A pooled median (floor) | lifetime medians of A' - A: range | blocks B > A | server CPU delta as share of A |
|---|---|---|---|---|---|---|---|
| select-pk literal | 20.70 us | +0.20 us [+0.11, +0.29] | 7 of 10 | -0.02 us | -0.26..+0.17 us | 149 of 240 | +0.97 % |
| select-pk prepared | 19.97 us | +0.16 us [+0.09, +0.21] | 9 of 10 | -0.03 us | -0.17..+0.27 us | 159 of 240 | +0.80 % |
| select-index literal | 23.71 us | +0.35 us [+0.29, +0.41] | 9 of 10 | -0.04 us | -0.45..+0.19 us | 181 of 240 | +1.48 % |
| select-index prepared | 23.07 us | +0.32 us [+0.27, +0.38] | 10 of 10 | -0.01 us | -0.31..+0.43 us | 188 of 240 | +1.37 % |
| update-lit literal | 20.67 us | +0.02 us [-0.04, +0.08] | 6 of 10 | +0.07 us | -0.15..+0.32 us | 125 of 240 | +0.07 % |
| update-lit prepared | 20.03 us | -0.06 us [-0.12, +0.01] | 4 of 10 | -0.06 us | -0.26..+0.18 us | 106 of 240 | -0.30 % |
| insert literal | 19.52 us | +0.09 us [+0.03, +0.16] | 5 of 9 | +0.01 us | -0.19..+0.19 us | 127 of 216 | +0.46 % |
| insert prepared | 18.81 us | +0.05 us [-0.02, +0.12] | 5 of 10 | -0.01 us | -0.26..+0.22 us | 130 of 240 | +0.27 % |

The tuple key `SELECT` pays +0.16 to +0.20 µs (0.8 to 1.0 % of its server
CPU), the index-served `SELECT` +0.32 to +0.35 µs (1.4 to 1.5 %), and the
writes between -0.06 and +0.09 µs, none of which is above the floor. Each
read cell is above its floor: the pooled A' - A is -0.04 to -0.01 µs there,
and B is above A in 62, 66, 75 and 78 % of the blocks of the four read cells (a floor would give about half).
No write cell is: `update-lit literal` is +0.02 µs (125 of 240 blocks above
zero), `update-lit prepared` -0.06 µs, `insert literal` +0.09 µs and `insert
prepared` +0.05 µs with intervals that reach zero or sit 0.03 µs above it.
**A single lifetime does not resolve any of these**: lifetime medians of B -
A on `select-pk literal` run from -0.09 to +0.59 µs against A' - A of
-0.26 to +0.17 µs, and the floor of one lifetime is as wide as the
effect; it is the ten lifetimes together that resolve the reads.

Both read paths, per shape (the two paths' blocks pooled), and the lifetime
medians with their own bootstrap:

| Shape | A server CPU | B - A pooled median [95 % bootstrap over blocks] | A' - A pooled median | lifetime-median B - A: median [95 % bootstrap over lifetimes], range, lifetimes above 0 | lifetime-median A' - A: median [95 % bootstrap over lifetimes], range, lifetimes above 0 | blocks B > A |
|---|---|---|---|---|---|---|
| select-pk | 20.34 us | +0.17 us [+0.13, +0.22] | -0.02 us | +0.14 us [+0.05, +0.32], -0.08..+0.50, 8 of 10 | -0.02 us [-0.14, +0.08], -0.21..+0.17, 4 of 10 | 308 of 480 |
| select-index | 23.39 us | +0.35 us [+0.30, +0.38] | -0.03 us | +0.31 us [+0.18, +0.57], +0.10..+0.62, 10 of 10 | -0.07 us [-0.19, +0.12], -0.38..+0.31, 4 of 10 | 369 of 480 |
| update-lit | 20.35 us | -0.03 us [-0.07, +0.02] | +0.02 us | +0.03 us [-0.09, +0.07], -0.21..+0.17, 6 of 10 | -0.00 us [-0.09, +0.15], -0.17..+0.24, 4 of 10 | 231 of 480 |
| insert | 19.17 us | +0.07 us [+0.03, +0.12] | +0.00 us | +0.07 us [-0.03, +0.18], -0.08..+0.22, 7 of 9 | +0.05 us [-0.11, +0.08], -0.19..+0.14, 6 of 9 | 257 of 456 |

`select-index` is above zero in all ten lifetimes (+0.31 µs, bootstrap over
lifetimes +0.18 to +0.57) and A' - A is not (-0.07 µs, -0.19 to +0.12): this
is the one cell where the lifetime medians themselves are resolved. `select-pk`
is above zero in 8 of 10 (+0.14 µs, +0.05 to +0.32), A' - A in 4 of 10.

### Every lifetime

#### select-pk / literal

| Lifetime | A | A' | B | B - A (median of blocks) | A' - A (floor) | blocks B > A | clean (attempts) |
|---|---|---|---|---|---|---|---|
| 1 | 20.34 us | 20.47 us | 20.75 us | +0.44 us | +0.12 us | 20 of 24 | yes (1) |
| 2 | 20.79 us | 20.60 us | 20.61 us | -0.05 us | -0.14 us | 10 of 24 | yes (1) |
| 3 | 21.31 us | 21.27 us | 21.72 us | +0.06 us | -0.01 us | 13 of 24 | yes (1) |
| 4 | 20.49 us | 20.27 us | 20.65 us | +0.16 us | -0.18 us | 18 of 24 | yes (1) |
| 5 | 20.69 us | 20.84 us | 20.58 us | -0.00 us | +0.05 us | 11 of 24 | yes (1) |
| 6 | 20.94 us | 21.01 us | 21.37 us | +0.39 us | +0.17 us | 18 of 24 | yes (1) |
| 7 | 20.78 us | 20.73 us | 21.44 us | +0.21 us | -0.26 us | 15 of 24 | yes (1) |
| 8 | 20.43 us | 20.67 us | 20.45 us | -0.09 us | -0.03 us | 11 of 24 | yes (1) |
| 9 | 20.54 us | 20.64 us | 21.14 us | +0.59 us | +0.07 us | 21 of 24 | yes (1) |
| 10 | 20.72 us | 20.66 us | 20.80 us | +0.00 us | -0.04 us | 12 of 24 | yes (2) |
| **pooled, 10 clean lifetimes** | 20.70 us | 20.72 us | 20.95 us | **+0.20 us** [+0.11, +0.29] blocks; lifetime medians median +0.11 [-0.02, +0.39], range -0.09..+0.59 | -0.02 us; lifetime medians -0.02, range -0.26..+0.17 | 149 of 240 | |

#### select-pk / prepared

| Lifetime | A | A' | B | B - A (median of blocks) | A' - A (floor) | blocks B > A | clean (attempts) |
|---|---|---|---|---|---|---|---|
| 1 | 19.69 us | 19.76 us | 20.01 us | +0.30 us | +0.04 us | 21 of 24 | yes (1) |
| 2 | 19.96 us | 20.04 us | 19.85 us | -0.11 us | -0.15 us | 10 of 24 | yes (1) |
| 3 | 20.19 us | 20.79 us | 20.27 us | +0.16 us | +0.27 us | 14 of 24 | yes (1) |
| 4 | 20.01 us | 19.90 us | 20.14 us | +0.12 us | -0.17 us | 16 of 24 | yes (2) |
| 5 | 19.92 us | 19.98 us | 20.18 us | +0.29 us | -0.01 us | 18 of 24 | yes (1) |
| 6 | 20.06 us | 20.30 us | 20.29 us | +0.24 us | +0.16 us | 17 of 24 | yes (1) |
| 7 | 19.87 us | 19.61 us | 19.93 us | +0.06 us | -0.16 us | 14 of 24 | yes (1) |
| 8 | 20.03 us | 19.98 us | 20.12 us | +0.06 us | -0.04 us | 13 of 24 | yes (1) |
| 9 | 19.94 us | 19.89 us | 20.46 us | +0.41 us | -0.10 us | 19 of 24 | yes (2) |
| 10 | 20.01 us | 19.94 us | 20.05 us | +0.09 us | -0.03 us | 17 of 24 | yes (1) |
| **pooled, 10 clean lifetimes** | 19.97 us | 20.02 us | 20.13 us | **+0.16 us** [+0.09, +0.21] blocks; lifetime medians median +0.14 [+0.06, +0.29], range -0.11..+0.41 | -0.03 us; lifetime medians -0.03, range -0.17..+0.27 | 159 of 240 | |

#### select-index / literal

| Lifetime | A | A' | B | B - A (median of blocks) | A' - A (floor) | blocks B > A | clean (attempts) |
|---|---|---|---|---|---|---|---|
| 1 | 23.68 us | 23.97 us | 24.26 us | +0.68 us | +0.19 us | 20 of 24 | yes (1) |
| 2 | 23.53 us | 23.47 us | 23.72 us | +0.22 us | -0.08 us | 18 of 24 | yes (2) |
| 3 | 23.92 us | 23.73 us | 24.12 us | +0.28 us | -0.28 us | 14 of 24 | yes (1) |
| 4 | 23.46 us | 23.57 us | 24.22 us | +0.58 us | +0.08 us | 23 of 24 | yes (1) |
| 5 | 23.71 us | 23.84 us | 24.43 us | +0.41 us | -0.04 us | 18 of 24 | yes (1) |
| 6 | 24.11 us | 23.90 us | 24.68 us | +0.46 us | +0.19 us | 17 of 24 | yes (1) |
| 7 | 23.83 us | 23.49 us | 23.77 us | -0.02 us | -0.45 us | 11 of 24 | yes (1) |
| 8 | 23.81 us | 23.80 us | 24.08 us | +0.29 us | -0.06 us | 19 of 24 | yes (1) |
| 9 | 23.44 us | 23.55 us | 24.06 us | +0.57 us | +0.07 us | 22 of 24 | yes (1) |
| 10 | 23.62 us | 23.51 us | 23.71 us | +0.20 us | -0.17 us | 19 of 24 | yes (1) |
| **pooled, 10 clean lifetimes** | 23.71 us | 23.68 us | 24.10 us | **+0.35 us** [+0.29, +0.41] blocks; lifetime medians median +0.35 [+0.22, +0.57], range -0.02..+0.68 | -0.04 us; lifetime medians -0.05, range -0.45..+0.19 | 181 of 240 | |

#### select-index / prepared

| Lifetime | A | A' | B | B - A (median of blocks) | A' - A (floor) | blocks B > A | clean (attempts) |
|---|---|---|---|---|---|---|---|
| 1 | 24.21 us | 24.38 us | 24.97 us | +0.56 us | +0.43 us | 20 of 24 | yes (1) |
| 2 | 22.86 us | 22.75 us | 22.92 us | +0.12 us | -0.07 us | 18 of 24 | yes (1) |
| 3 | 22.86 us | 22.78 us | 23.11 us | +0.24 us | -0.17 us | 18 of 24 | yes (1) |
| 4 | 22.99 us | 23.03 us | 23.59 us | +0.56 us | +0.01 us | 22 of 24 | yes (1) |
| 5 | 22.79 us | 22.77 us | 23.23 us | +0.33 us | -0.09 us | 21 of 24 | yes (1) |
| 6 | 22.80 us | 23.02 us | 23.20 us | +0.40 us | +0.21 us | 22 of 24 | yes (1) |
| 7 | 23.14 us | 22.93 us | 23.29 us | +0.21 us | -0.31 us | 14 of 24 | yes (1) |
| 8 | 23.18 us | 23.03 us | 23.34 us | +0.23 us | -0.23 us | 16 of 24 | yes (1) |
| 9 | 22.90 us | 22.94 us | 23.47 us | +0.65 us | +0.12 us | 20 of 24 | yes (1) |
| 10 | 22.94 us | 22.81 us | 23.03 us | +0.15 us | -0.12 us | 17 of 24 | yes (1) |
| **pooled, 10 clean lifetimes** | 23.07 us | 23.04 us | 23.42 us | **+0.32 us** [+0.27, +0.38] blocks; lifetime medians median +0.29 [+0.19, +0.56], range +0.12..+0.65 | -0.01 us; lifetime medians -0.08, range -0.31..+0.43 | 188 of 240 | |

#### update-lit / literal

| Lifetime | A | A' | B | B - A (median of blocks) | A' - A (floor) | blocks B > A | clean (attempts) |
|---|---|---|---|---|---|---|---|
| 1 | 20.38 us | 20.80 us | 20.65 us | +0.21 us | +0.29 us | 19 of 24 | yes (1) |
| 2 | 20.44 us | 20.61 us | 20.58 us | +0.12 us | +0.11 us | 15 of 24 | yes (1) |
| 3 | 20.71 us | 20.72 us | 20.58 us | -0.06 us | -0.10 us | 10 of 24 | yes (2) |
| 4 | 20.70 us | 20.64 us | 20.78 us | +0.07 us | -0.02 us | 13 of 24 | yes (1) |
| 5 | 20.92 us | 20.88 us | 21.06 us | +0.08 us | -0.15 us | 13 of 24 | yes (1) |
| 6 | 20.73 us | 21.04 us | 20.82 us | +0.12 us | +0.32 us | 14 of 24 | yes (1) |
| 7 | 20.93 us | 20.78 us | 20.98 us | -0.13 us | -0.07 us | 11 of 24 | yes (1) |
| 8 | 20.53 us | 20.78 us | 20.77 us | -0.04 us | +0.25 us | 11 of 24 | yes (1) |
| 9 | 20.72 us | 20.61 us | 20.86 us | +0.10 us | +0.02 us | 14 of 24 | yes (1) |
| 10 | 20.68 us | 20.69 us | 20.46 us | -0.22 us | +0.11 us | 5 of 24 | yes (1) |
| **pooled, 10 clean lifetimes** | 20.67 us | 20.76 us | 20.76 us | **+0.02 us** [-0.04, +0.08] blocks; lifetime medians median +0.08 [-0.06, +0.12], range -0.22..+0.21 | +0.07 us; lifetime medians +0.06, range -0.15..+0.32 | 125 of 240 | |

#### update-lit / prepared

| Lifetime | A | A' | B | B - A (median of blocks) | A' - A (floor) | blocks B > A | clean (attempts) |
|---|---|---|---|---|---|---|---|
| 1 | 19.74 us | 19.87 us | 19.81 us | +0.14 us | +0.18 us | 16 of 24 | yes (1) |
| 2 | 20.02 us | 20.12 us | 19.93 us | -0.07 us | -0.12 us | 8 of 24 | yes (1) |
| 3 | 19.91 us | 20.16 us | 19.94 us | -0.07 us | +0.10 us | 10 of 24 | yes (1) |
| 4 | 20.17 us | 19.82 us | 20.07 us | +0.07 us | -0.14 us | 15 of 24 | yes (1) |
| 5 | 20.18 us | 20.08 us | 20.09 us | +0.04 us | -0.12 us | 13 of 24 | yes (1) |
| 6 | 20.22 us | 20.22 us | 20.47 us | +0.04 us | +0.05 us | 13 of 24 | yes (1) |
| 7 | 20.12 us | 19.91 us | 20.28 us | -0.05 us | -0.26 us | 10 of 24 | yes (1) |
| 8 | 19.97 us | 20.24 us | 19.71 us | -0.28 us | +0.05 us | 3 of 24 | yes (1) |
| 9 | 19.91 us | 19.79 us | 19.96 us | -0.05 us | -0.14 us | 11 of 24 | yes (1) |
| 10 | 20.03 us | 20.15 us | 19.90 us | -0.21 us | +0.04 us | 7 of 24 | yes (1) |
| **pooled, 10 clean lifetimes** | 20.03 us | 20.04 us | 20.02 us | **-0.06 us** [-0.12, +0.01] blocks; lifetime medians median -0.05 [-0.14, +0.05], range -0.28..+0.14 | -0.06 us; lifetime medians -0.04, range -0.26..+0.18 | 106 of 240 | |

#### insert / literal

| Lifetime | A | A' | B | B - A (median of blocks) | A' - A (floor) | blocks B > A | clean (attempts) |
|---|---|---|---|---|---|---|---|
| 1 | 19.92 us | 19.98 us | 19.88 us | +0.10 us | +0.12 us | 14 of 24 | NO (1) |
| 2 | 19.25 us | 19.38 us | 19.52 us | +0.25 us | +0.19 us | 18 of 24 | yes (1) |
| 3 | 19.41 us | 19.65 us | 19.61 us | +0.15 us | +0.15 us | 18 of 24 | yes (1) |
| 4 | 19.59 us | 19.37 us | 19.55 us | -0.02 us | -0.12 us | 11 of 24 | yes (1) |
| 5 | 19.78 us | 19.74 us | 20.34 us | +0.27 us | -0.01 us | 17 of 24 | yes (1) |
| 6 | 19.34 us | 19.33 us | 19.43 us | +0.13 us | +0.06 us | 16 of 24 | yes (1) |
| 7 | 19.92 us | 19.64 us | 20.00 us | -0.05 us | -0.19 us | 10 of 24 | yes (1) |
| 8 | 19.68 us | 19.47 us | 19.59 us | -0.07 us | -0.12 us | 9 of 24 | yes (1) |
| 9 | 19.29 us | 19.42 us | 19.77 us | +0.26 us | +0.14 us | 18 of 24 | yes (1) |
| 10 | 19.43 us | 19.56 us | 19.49 us | -0.10 us | +0.03 us | 10 of 24 | yes (1) |
| **pooled, 9 clean lifetimes** | 19.52 us | 19.51 us | 19.70 us | **+0.09 us** [+0.03, +0.16] blocks; lifetime medians median +0.13 [-0.07, +0.26], range -0.10..+0.27 | +0.01 us; lifetime medians +0.03, range -0.19..+0.19 | 127 of 216 | |

#### insert / prepared

| Lifetime | A | A' | B | B - A (median of blocks) | A' - A (floor) | blocks B > A | clean (attempts) |
|---|---|---|---|---|---|---|---|
| 1 | 19.05 us | 18.73 us | 18.98 us | +0.18 us | -0.03 us | 15 of 24 | yes (1) |
| 2 | 18.53 us | 18.46 us | 18.51 us | -0.01 us | -0.03 us | 11 of 24 | yes (1) |
| 3 | 18.60 us | 18.71 us | 18.83 us | +0.22 us | +0.22 us | 19 of 24 | yes (1) |
| 4 | 18.98 us | 18.80 us | 19.13 us | +0.09 us | -0.21 us | 15 of 24 | yes (1) |
| 5 | 18.93 us | 19.24 us | 18.97 us | -0.00 us | -0.02 us | 12 of 24 | yes (1) |
| 6 | 18.71 us | 18.72 us | 18.93 us | -0.01 us | +0.00 us | 12 of 24 | yes (1) |
| 7 | 18.91 us | 18.66 us | 18.74 us | -0.10 us | -0.26 us | 7 of 24 | yes (1) |
| 8 | 18.76 us | 19.40 us | 19.04 us | +0.00 us | +0.15 us | 12 of 24 | yes (1) |
| 9 | 18.63 us | 18.76 us | 18.91 us | +0.19 us | +0.07 us | 18 of 24 | yes (1) |
| 10 | 19.04 us | 18.84 us | 18.90 us | -0.27 us | -0.10 us | 9 of 24 | yes (1) |
| **pooled, 10 clean lifetimes** | 18.81 us | 18.83 us | 18.89 us | **+0.05 us** [-0.02, +0.12] blocks; lifetime medians median +0.00 [-0.05, +0.18], range -0.27..+0.22 | -0.01 us; lifetime medians -0.02, range -0.26..+0.22 | 130 of 240 | |

### Latency and throughput (driver-side)

QPS is derived, `1,000,000 / mean µs` of a serial single-connection driver;
the table is the median over lifetimes of each lifetime's per-arm statistic.
The driver emits p0, p25, p50, p90 and p99, **not p95**, and reports no
wait-event decomposition; it is the unmodified `tools/` driver and the
percentiles are what it computes (`bench/README.md`: a driver change inside
a measurement stage measures the driver). The statements an arm column is
10 lifetimes of 12,000.

| Cell | Arm | statements an arm | QPS (1e6 / mean) | p0 | p25 | p50 | p90 | p99 | server CPU | driver CPU |
|---|---|---|---|---|---|---|---|---|---|---|
| select-pk literal | A | 120,000 | 18,617 qps | 47.3 us | 49.6 us | 50.6 us | 57.4 us | 72.3 us | 20.71 us | 31.98 us |
| select-pk literal | A' | 120,000 | 18,431 qps | 47.4 us | 49.5 us | 50.4 us | 58.3 us | 74.6 us | 20.66 us | 32.03 us |
| select-pk literal | B | 120,000 | 18,632 qps | 47.5 us | 49.7 us | 50.7 us | 57.5 us | 71.5 us | 20.78 us | 32.06 us |
| select-pk prepared | A | 120,000 | 20,149 qps | 44.1 us | 45.8 us | 46.5 us | 53.6 us | 68.2 us | 19.99 us | 28.62 us |
| select-pk prepared | A' | 120,000 | 20,178 qps | 44.1 us | 45.7 us | 46.5 us | 54.0 us | 67.3 us | 19.96 us | 28.56 us |
| select-pk prepared | B | 120,000 | 20,236 qps | 44.3 us | 45.8 us | 46.5 us | 52.9 us | 66.5 us | 20.13 us | 28.39 us |
| select-index literal | A | 120,000 | 17,444 qps | 49.4 us | 52.7 us | 53.9 us | 62.0 us | 79.2 us | 23.69 us | 32.17 us |
| select-index literal | A' | 120,000 | 17,436 qps | 49.5 us | 52.7 us | 53.9 us | 61.9 us | 79.6 us | 23.65 us | 32.14 us |
| select-index literal | B | 120,000 | 17,285 qps | 49.7 us | 52.9 us | 54.3 us | 62.3 us | 79.1 us | 24.10 us | 32.27 us |
| select-index prepared | A | 120,000 | 18,967 qps | 46.2 us | 48.7 us | 49.9 us | 56.6 us | 71.5 us | 22.92 us | 28.59 us |
| select-index prepared | A' | 120,000 | 18,959 qps | 46.1 us | 48.7 us | 49.9 us | 56.7 us | 71.1 us | 22.93 us | 28.78 us |
| select-index prepared | B | 120,000 | 18,840 qps | 46.5 us | 49.1 us | 50.2 us | 57.1 us | 71.4 us | 23.26 us | 28.75 us |
| update-lit literal | A | 120,000 | 22,078 qps | 35.6 us | 41.2 us | 42.0 us | 49.0 us | 64.2 us | 20.70 us | 23.77 us |
| update-lit literal | A' | 120,000 | 21,978 qps | 35.6 us | 41.3 us | 42.0 us | 48.8 us | 64.0 us | 20.75 us | 23.66 us |
| update-lit literal | B | 120,000 | 21,934 qps | 35.8 us | 41.1 us | 41.8 us | 49.0 us | 64.9 us | 20.78 us | 23.81 us |
| update-lit prepared | A | 120,000 | 23,874 qps | 34.5 us | 38.1 us | 38.7 us | 45.1 us | 60.7 us | 20.03 us | 20.92 us |
| update-lit prepared | A' | 120,000 | 23,803 qps | 33.4 us | 38.0 us | 38.7 us | 44.9 us | 60.7 us | 20.10 us | 20.90 us |
| update-lit prepared | B | 120,000 | 23,873 qps | 34.7 us | 38.0 us | 38.6 us | 45.0 us | 60.4 us | 19.95 us | 20.90 us |
| insert literal | A | 120,000 | 22,748 qps | 38.5 us | 40.2 us | 40.9 us | 48.0 us | 64.0 us | 19.51 us | 23.65 us |
| insert literal | A' | 120,000 | 22,633 qps | 38.5 us | 40.1 us | 40.8 us | 47.8 us | 65.0 us | 19.52 us | 23.57 us |
| insert literal | B | 120,000 | 22,601 qps | 38.4 us | 40.1 us | 40.9 us | 48.1 us | 66.2 us | 19.60 us | 23.64 us |
| insert prepared | A | 120,000 | 24,509 qps | 34.9 us | 37.2 us | 37.7 us | 44.2 us | 61.5 us | 18.83 us | 21.05 us |
| insert prepared | A' | 120,000 | 24,427 qps | 34.4 us | 37.2 us | 37.7 us | 44.2 us | 60.6 us | 18.75 us | 21.02 us |
| insert prepared | B | 120,000 | 24,373 qps | 35.1 us | 37.1 us | 37.7 us | 44.5 us | 61.7 us | 18.92 us | 21.09 us |

Throughput does not resolve the effect: B against A is -0.9 and -0.7 % on
`select-index`, +0.1 and +0.4 % on `select-pk` and between -0.7 and 0 % on the
write cells, with p50 within 0.4 µs and p0 within 0.5 µs of A in every cell;
the 0.2 to 0.35 µs of server CPU is under 1 % of a 50 µs round trip and sits
among the client's 32 µs and the socket.

**Waits.** This driver breaks a statement into two things only: the server's
on-CPU time and the client's wall time. On `select-pk literal` the mean
client latency is 53.7 µs (1,000,000 / 18,617 qps) of which the server is
on-CPU 20.7 µs (39 %) and the rest, about 33 µs, is the client's own
CPU (31.98 µs) and the socket and scheduling round trip; the two overlap and
are not a partition. `durability = relaxed` has no fsync wait to book, one
connection has no lock or conflict wait, and `sys.wait_stats` was not
sampled by this driver: no per-wait-type share is given and none would
change a delta in server on-CPU time.

## Result: the row-count sweep (200, 1,000 and 10,000 rows)

The same arms at 200 and 10,000 rows, six lifetimes each (`select-pk`,
`select-index`, `update-lit`; the 1,000-row figures are the ten above):

| Cell | 200 rows B - A | 1,000 rows B - A | 10,000 rows B - A |
|---|---|---|---|
| select-pk literal | +0.19 µs [+0.12, +0.29] | +0.20 µs [+0.11, +0.29] | +0.21 µs [+0.12, +0.31] |
| select-pk prepared | +0.16 µs [+0.11, +0.21] | +0.16 µs [+0.09, +0.21] | +0.15 µs [+0.07, +0.22] |
| select-index literal | +0.26 µs [+0.19, +0.33] | +0.35 µs [+0.29, +0.41] | +0.21 µs [+0.12, +0.33] |
| select-index prepared | +0.22 µs [+0.16, +0.29] | +0.32 µs [+0.27, +0.38] | +0.20 µs [+0.12, +0.28] |
| update-lit literal | +0.13 µs [+0.04, +0.21] | +0.02 µs [-0.04, +0.08] | +0.02 µs [-0.07, +0.10] |
| update-lit prepared | +0.01 µs [-0.05, +0.04] | -0.06 µs [-0.12, +0.01] | +0.03 µs [-0.04, +0.15] |
| A server CPU, select-pk literal | 20.52 µs | 20.70 µs | 21.15 µs |
| Largest A' - A lifetime median of the cell group | 0.31 µs | 0.45 µs | 0.38 µs |

The point statement does not scale with the row count (a tuple key descent
is logarithmic and the three sizes are one to two levels): `select-pk`'s
figures are +0.15 to +0.21 µs at every size. The `select-index` figure
moves from +0.20 to +0.35 µs across the sizes, which is inside what 6 versus
10 lifetimes can move when the floor of one lifetime is 0.4 µs; the three
sizes do not show a row-dependent cost. The sweep's per-lifetime and per-shape
tables are `archive/.../an_r200.txt` and `an_r10k.txt`; every one of its 72
cells was clean.

## Result: `cores = 8`, not resolved

DF-Q14 (a) asks for `select-pk` and `update-lit` at `cores = 8`. One lifetime
was run (`--cores 8`, the three servers pinned to every CPU of the host,
`0,1,2,3,4,5,6,7`, the driver on CPU 5 and so sharing it with a reactor,
each server running 8 reactors): **all four cells were unclean after three
attempts**, 582 s of the cell gate were spent waiting for a quiet window,
and the lifetime is bimodal exactly as AM-S6 found at this core count:

#### select-pk / literal

| Lifetime | A | A' | B | B - A (median of blocks) | A' - A (floor) | blocks B > A | clean (attempts) |
|---|---|---|---|---|---|---|---|
| 1 | 35.57 us | 26.15 us | 26.37 us | -9.22 us | -9.46 us | 0 of 24 | NO (3) |

#### select-pk / prepared

| Lifetime | A | A' | B | B - A (median of blocks) | A' - A (floor) | blocks B > A | clean (attempts) |
|---|---|---|---|---|---|---|---|
| 1 | 33.32 us | 25.37 us | 25.24 us | -8.13 us | -7.87 us | 0 of 24 | NO (3) |

#### update-lit / literal

| Lifetime | A | A' | B | B - A (median of blocks) | A' - A (floor) | blocks B > A | clean (attempts) |
|---|---|---|---|---|---|---|---|
| 1 | 44.96 us | 51.64 us | 51.78 us | +8.11 us | +9.04 us | 22 of 24 | NO (3) |

#### update-lit / prepared

| Lifetime | A | A' | B | B - A (median of blocks) | A' - A (floor) | blocks B > A | clean (attempts) |
|---|---|---|---|---|---|---|---|
| 1 | 34.79 us | 40.28 us | 38.59 us | +5.57 us | +4.84 us | 20 of 24 | NO (3) |

In the two `select-pk` cells A is 9 µs a statement above A' - the same binary
- in every block (`select-pk literal` A 35.57 µs, A' 26.15 µs, B 26.37 µs;
0 of 24 blocks have B above A), i.e. one of the three processes landed in a
slower placement of its reactors; in `update-lit` the foreign CPU on the
server's CPUs was 90 to 95 % (another session's suite, load 8 to 9) and
A' is +9.0 and +4.8 µs above A. **B - A is therefore a placement and not a
measurement, and no cores = 8 delta is claimed.** Of the one comparison
that is not a placement, B - A' on the two `select-pk` cells is +0.22 and
-0.13 µs, on a single lifetime. The server's idle burn at 8 reactors is
about 60 ms of CPU a second a server (6 %), against 5.5 to 7 ms at one core.
Further `cores = 8` lifetimes were cut (the host was held by another session's
`ctest -j8` from 07:13, and each lifetime cost about 15 minutes of
quiet-gate waiting here); this cell is owed.

## What the cells can and cannot see

What S2 to S4 put on a statement's path (the work order's §7 entries): at S2 a
mint stamp stored where a read view is registered and one relaxed store
beside each publication of a core's snapshot bound, two words on the session's
wait statistics set and cleared each statement, and a counter bump at every
transaction decide; at S3, for a `SELECT`, a walk of the chain's steps for
index steps and, for an index-served read only, one latch, one hash lookup
and one wall-clock read for each run recorded, and one counter bump an index
seek; at S4 nothing on a point statement.

- **The two session words and the decide counter are not seen.** They are on
  every statement and every autocommit write; the write cells are at -0.06
  to +0.13 µs with a floor as wide as the figure. If they cost anything it is
  below about 0.1 µs, the width of the pooled intervals.
- **What a point `SELECT` pays is about +0.17 µs, and the cells cannot
  say which stage.** A reads' extra against a write's is the view's mint stamp
  and snapshot-bound store (S2: a write mints no read view) plus the chain-step
  walk (S3, a SELECT only); the two are not separated, because there is no
  per-stage binary in this run, only A and B. The attribution is by the shape
  of the statement, not by the stage.
- **An index-served read pays about 0.15 µs more than a tuple key read**
  (+0.32 to +0.35 µs against +0.16 to +0.20 µs at 1,000 rows, +0.20 to
  +0.26 against +0.15 to +0.21 at the other sizes): S3's latch, hash lookup,
  clock read and per-seek counter. That is the one added cost on the
  engine's own account that has a price here, and it is the first measurement
  of the per-index fold DF-S0's spike never ran (`DF-Q14`'s "part C's
  per-index fold, which no cell of the driver reaches"). It is pooled over
  ten lifetimes and over 6 each at the other sizes, positive in 10 of 10
  lifetimes at 1,000 rows and 5 of 6 at each other size; it is the shape DF's bar is least likely to hold.
- **S4 is correctly absent**: the stage claimed nothing on a point statement
  and the update and insert cells show nothing.
- **Not measured**: a scan, a range read, a `DELETE`, a transaction of
  several statements (the counter's per-decide cost is divided by one),
  `strict` durability (where the commit parks and S2's decide count runs on
  another thread's path), `cores > 1` (above), and any contended store.

## Against the bar and against DF-S1

| Stage | Cell | Server CPU a statement | Source |
|---|---|---|---|
| DF-S1 (`58d258a3` to `ee89842e`; the bisect's M - X, pooled over four lifetimes, includes CX-S3b to CY) | tuple key read | +0.44 µs [+0.27, +0.54] | `bench/v3.4.13/results-cx-close-bisect-v3.4.13-1-g11f72ce9.md`, `group` durability, its own arms driver |
| DF-S2 to DF-S4 (`8e7c820f` to `ec5ddfe8`) | `select-pk`, both paths, 1,000 rows | +0.17 µs [+0.13, +0.22] | this file, `relaxed`, `bn_census.py` |
| all of DF, summed | tuple key read | about +0.61 µs, naive interval +0.40 to +0.76 | the two rows above |
| DF-S2 to DF-S4 | `select-index`, both paths | +0.35 µs [+0.30, +0.38] | this file; DF-S1 was not priced on this shape |
| DF-S2 to DF-S4 | `update-lit`, `insert` | -0.03 µs [-0.07, +0.02]; +0.07 µs [+0.03, +0.12] | this file; DF-S1's write share was not measured |
| DF-Q10 (a)'s bar | a point statement, `cores = 1` | 0.5 µs | the work order |

**Not resolved.** The sum's central estimate (+0.61 µs) is 0.11 µs over
the bar, and its naive interval spans it (the two terms come from different
drivers, durabilities and arm sets, so they are not strictly additive, and a
binary's layout moves a result by 0.2 to 0.3 µs in the bisect's finding); a
root-sum-square interval would be +0.47 to +0.75 µs, which touches it. DF-S1
alone has used 0.44 of the 0.5 µs, and what S2 to S4 add on a point read is
more than the 0.06 µs left, by every estimate here whose interval
excludes zero. If the operator reads the bar by the central estimates it is
exceeded on a point read; by the intervals it is not decided; for a write it is
not decided (S2 to S4 at about 0, DF-S1's write share unmeasured); and for an
index-served read the cost of S2 to S4 alone (+0.35 µs) is 70 % of the bar
before DF-S1 is added. This file does not take that decision (`CLAUDE.md`,
Open Decisions; DF-Q10 (a) sends a part over its bar back to the operator
with the column that costs it: the column here is the per-index fold's, on
the index-served shape).

**Against this engine's own previous number.** For `select-pk` the previous
measurement of the same shape is the bisect's `pk-point-read` (M, `11f72ce9`,
21.81 µs of server CPU, `group` durability, a different driver and client
path): that file's absolute figure is not like-for-like with A's 20.70 µs
(literal) here and no delta is taken across the two; its DF-S1 step is the
previous number the deltas above are read against. For `select-index`,
`update-lit` and `insert` at `cores = 1` there is **no previous number**; this
file is their baseline. At `cores = 8` there is none, and this file's is
unresolved.

## Time spent

| What | Time |
|---|---|
| Building A and B | 2 min 38 s (06:44:54 to 06:47:32), 78 s each |
| Running: `cores = 1` and 1,000 rows, ten lifetimes | 7 min 18 s |
| Running: row-count sweep, 12 lifetimes | 7 min 15 s (07:20:49 to 07:23:15, then 07:23:26 to 07:28:15) |
| Running: `cores = 8`, one lifetime | 14 min 54 s, of which the cell gate's quiet waits were 9 min 42 s and the idle waits (the driver waits up to 90 s for each of the three servers to fall to idle, 4 min 30 s at the cap) the rest |
| Held by the process-level gate (`life.sh` waits for another session's `ctest` or build to end) | 07:14:06 to 07:14:22 (the `cores = 8` lifetime 2, never started) and 07:14:29 to 07:20:49 for the sweep: about 6 min 35 s. **3 min 23 s of that was the other session's `ctest -j8` (running from 07:13:34, gone by 07:17:52); the remaining 3 min (07:17:52 to 07:20:49) were this run's own poll commands, whose command lines contained the gate's pattern and held it**, a mistake of this run and not the host's |
| Aborted | a first smoke on CPUs 2 and 4 (06:47:35 to 06:50:40) beside another session's measurement; killed after one cell, discarded; a second smoke (06:50:56 to 06:51:36, 600 statements) that fixed the pin arguments |
| Wall clock, first build to last result | 44 min, inside the one-hour budget; of it about 21 min was gate or idle waiting (6 min 35 s, 9 min 42 s and up to 4 min 30 s) |

## What was cut

- **`cores = 8` after one lifetime** (planned six): the lifetime's cost,
  about 15 minutes, was mostly gate waiting and the 90 s idle wait the
  driver gives each of 24 reactor servers; it did not resolve. Owed.
- **`insert` at 200 and 10,000 rows** and the `insert` cell at `cores = 8`:
  the budget.
- **A per-stage binary** (`c2925b6e`, `83aad532`, `29810582` each built):
  the close asks for the whole of B - A and the attribution by stage would
  have cost three builds and three more arms.
- **`strict` durability, writes of several statements, and a scan**: not
  asked of this close, listed above as unmeasured.
- Percentiles are p0, p25, p50, p90, p99: the driver's own set, not p95.

## What the run says about the engine

The statistics DF adds are not free on the one path an OLTP engine lives on,
and they are *shaped*: the cost lands on reads, not on writes. A tuple key
`SELECT` pays about +0.17 µs of 20 µs for the read-view stamp, the snapshot
bound store and the step walk, and an index-served read about +0.35 µs,
because it additionally pays the per-index fold (a latch, a hash lookup, a
clock read) once for every recorded run. A write pays nothing resolvable for
the decide counter and the session words, although both are on it, which is
consistent with DF-S0's spike (an `UPDATE`'s cell at +0.11 to +0.23 µs and
unresolved) and the spike's read figures (+0.18 and +0.35 µs, unresolved from its noise)
are confirmed in order of magnitude: the real change's tuple key read cost,
+0.16 to +0.20 µs, is at the spike's lower end, and its write cost, about 0,
below the spike's +0.11 to +0.23 µs.

The bar is a sum over stages, and the sum is already past the point where
a stage's own run can pass it: DF-S1 took 0.44 µs, nearly the whole
allowance, in the stage the operator kept as built on 2026-11-19
(`raft-marks-2026-11-19.md` §4). The index-served cost is the one DF-S3 put on a read
that nothing else carries, and nothing in this run prices a cheaper form of
it. The floor is the process, not the repeat: one lifetime's A'
- A is as wide as the effect (-0.45 to +0.43 µs on `select-index` across
lifetimes), and only ten restarts, with the pooled floor at -0.04 µs,
settle the reads. At 8 reactors the floor is 9 µs, and this host cannot
price a 0.5 µs bar there until it is quiet and the reactors' placement is
fixed; that is a fact about the host and the measurement, and none about the
engine's cost at `cores = 8`.
