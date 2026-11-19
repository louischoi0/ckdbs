# DC close, measured again at `7e18598d`: the remedy moves nothing the floor can see on the SELECT

Measured again at `7e18598d` (`v3.4.13-16-g7e18598d`, C) after a remedy to the
first close's lean (`results-dc-close-v3.4.13-14-gcd80d31d.md`, B =
`cd80d31d`, `v3.4.13-14-gcd80d31d`). C removes one vector allocation a SELECT
compile and two moves of the `DispatchOutcome` a write, and changes no
behaviour. Same drivers, cells and method as the first file (its "Method"
section; `archive/dc-close-v3.4.13-16-g7e18598d/point_arms.py`, `an_c.py`,
`run_c.sh`). **The descriptive cell was not rerun**: the remedy does not change
what it measures. No PostgreSQL floor; the only prior number for this shape is
the first file's.

Findings. **Bar, C against A: the SELECT is met inside the floor
(+0.13, +0.10, +0.10 µs at 200 / 1K / 10K rows, about 0.5 % of 20.4 µs); the
UPDATE is not resolved from the floor (+0.19, -0.35, -1.12 µs of about 75).
No cell is slower outside its spread.** **The remedy (C - B) moved nothing the
floor can see on the SELECT (-0.00, +0.05, -0.01 µs)**; on the UPDATE it reads
-1.59, -0.08 and -1.12 µs, one interval (200 rows) excluding zero: suggestive,
not established.

## Run stamp

| Item | Value |
|---|---|
| Executed | 2026-11-19: C build 07:17 to 07:19 UTC (85 s); lifetimes 07:20:54 to 07:45:50 |
| Branch / worktree | `worktree-dc-select-for-update`, worktree `dc-select-for-update`; uncommitted document edits only; every arm built from an exported tree of its commit |
| Arms | A `8e7c820f` (`v3.4.13-10-g8e7c820f`), sha256 `ab5b9ee7cc7875bbe199ee402214b8e12176c8a00f909eb6e0ca1aae4cba9437`; A2 a second server of A's copy (control); B `cd80d31d` (`v3.4.13-14-gcd80d31d`), `460473efaa9aee2ac5e8ac7c46736d01b6187a4f3f6666ad73fd560e6ab2faec`; C `7e18598d` (`v3.4.13-16-g7e18598d`), `78b3aede97192d165094e5051a80958b026d36e2bb92d7a13a394412399d3fb3`. A and B are the first file's binaries; C was built Release, tests and sim off, GNU 14.2.0, from an exported tree, its mtime after the commit |
| Provenance | every server started from a copy in `/home/louis/ck-tmp/dc-close/run/` (`A.bin`, `A2.bin`, `B.bin`, `C.bin`), hashes equal to the above |
| Device | `/dev/nvme0n1p1`, ext4, data files and WAL under the run directory; not tmpfs |
| Server config | `cores = 1`, `durability = group`, `buffer_pool_frames = 65536`, `log_level = warn`, others default; server pinned to CPU 2, client CPU 4; ports 28010 to 28073 |
| Design | four arms (A, C, A2, B) interleaved, 7 rounds a lifetime; `sel` blocks 10,000 statements, `upd` blocks 3,000; tables of 200 / 1K / 10K rows; fresh server and data file per arm and lifetime |
| Host load | in every block's record (`point-life*.json`). Lifetimes 1 to 4: load 0.6 to 2.0, 0 competing blocks of 168. **Lifetimes 5 and 6 are excluded**: another session's build ran through them (27 and 40 competing blocks of 168, load up to 9.5). **Lifetime 7 is partly clean**: 15 competing blocks, the rounds containing them dropped, 4 to 5 rounds kept a cell. The pool is lifetimes 1 to 4 and 7: **four fully clean lifetimes and one partial, not five clean** |

## Result: server CPU a statement, µs

Round-paired median over rounds; the last column is the pooled median with a
bootstrap 95 % interval (2,000 resamples) over lifetimes 1, 2, 3, 4 and 7
(32 to 33 rounds a cell). Columns for lifetimes 5 and 6 show only the rounds
that had no competing process (0 to 3) and are **not** in the pool.

**C - A, the bar:**

| Pair | Cell | life 1 | life 2 | life 3 | life 4 | life 5 | life 6 | life 7 | pooled median [95% CI] (rounds) |
|---|---|---|---|---|---|---|---|---|---|
| C - A | sel-200 | -0.01 (n=7) | +0.19 (n=7) | -0.09 (n=7) | +0.32 (n=7) | +0.15 (n=3) | +0.02 (n=1) | +0.24 (n=5) | +0.13 [-0.09, +0.30] (33) |
| C - A | sel-1k | +0.09 (n=7) | +0.11 (n=7) | +0.03 (n=7) | +0.12 (n=7) | +1.76 (n=3) | - | +0.03 (n=4) | +0.10 [+0.03, +0.17] (32) |
| C - A | sel-10k | +0.10 (n=7) | +0.43 (n=7) | +0.20 (n=7) | +0.09 (n=7) | +0.11 (n=2) | - | -0.13 (n=5) | +0.10 [+0.01, +0.26] (33) |
| C - A | upd-200 | -1.10 (n=7) | +0.04 (n=7) | +0.75 (n=7) | +0.33 (n=7) | -0.41 (n=3) | - | -0.48 (n=4) | +0.19 [-1.22, +1.38] (32) |
| C - A | upd-1k | +2.97 (n=7) | +0.36 (n=7) | -0.72 (n=7) | -1.31 (n=7) | -0.72 (n=2) | - | +0.73 (n=4) | -0.35 [-1.08, +0.36] (32) |
| C - A | upd-10k | +1.35 (n=7) | -2.50 (n=7) | -1.65 (n=7) | -1.63 (n=7) | -1.81 (n=2) | +0.00 (n=1) | -1.04 (n=5) | -1.12 [-2.39, +0.50] (33) |

**C - B, the remedy's own effect:**

| Pair | Cell | life 1 | life 2 | life 3 | life 4 | life 5 | life 6 | life 7 | pooled median [95% CI] (rounds) |
|---|---|---|---|---|---|---|---|---|---|
| C - B | sel-200 | -0.06 (n=7) | -0.00 (n=7) | +0.06 (n=7) | -0.16 (n=7) | -0.21 (n=3) | -0.53 (n=1) | -0.21 (n=5) | -0.00 [-0.21, +0.06] (33) |
| C - B | sel-1k | +0.09 (n=7) | -0.04 (n=7) | +0.23 (n=7) | -0.09 (n=7) | +1.27 (n=3) | - | -0.05 (n=4) | +0.05 [-0.11, +0.15] (32) |
| C - B | sel-10k | -0.12 (n=7) | -0.18 (n=7) | +0.27 (n=7) | -0.01 (n=7) | +0.22 (n=2) | - | -0.50 (n=5) | -0.01 [-0.21, +0.19] (33) |
| C - B | upd-200 | -1.85 (n=7) | -0.68 (n=7) | -1.27 (n=7) | -3.43 (n=7) | +1.30 (n=3) | - | -3.22 (n=4) | -1.59 [-3.43, -0.68] (32) |
| C - B | upd-1k | +1.68 (n=7) | -0.18 (n=7) | -2.53 (n=7) | +0.07 (n=7) | +3.41 (n=2) | - | +1.09 (n=4) | -0.08 [-1.20, +0.66] (32) |
| C - B | upd-10k | +0.61 (n=7) | -3.99 (n=7) | -0.74 (n=7) | -1.34 (n=7) | -2.00 (n=2) | -4.60 (n=1) | +0.03 (n=5) | -1.12 [-2.56, +0.52] (33) |

**B - A, the first run's lean, seen again:**

| Pair | Cell | life 1 | life 2 | life 3 | life 4 | life 5 | life 6 | life 7 | pooled median [95% CI] (rounds) |
|---|---|---|---|---|---|---|---|---|---|
| B - A | sel-200 | +0.06 (n=7) | +0.21 (n=7) | -0.05 (n=7) | +0.23 (n=7) | +0.55 (n=3) | +0.55 (n=1) | +0.21 (n=5) | +0.18 [+0.07, +0.22] (33) |
| B - A | sel-1k | -0.01 (n=7) | +0.26 (n=7) | -0.08 (n=7) | +0.25 (n=7) | -0.03 (n=3) | - | +0.08 (n=4) | +0.12 [-0.07, +0.25] (32) |
| B - A | sel-10k | +0.17 (n=7) | +0.42 (n=7) | -0.17 (n=7) | +0.37 (n=7) | -0.11 (n=2) | - | +0.37 (n=5) | +0.23 [+0.05, +0.40] (33) |
| B - A | upd-200 | +2.02 (n=7) | +1.97 (n=7) | +1.99 (n=7) | +2.10 (n=7) | -3.88 (n=3) | - | +1.08 (n=4) | +1.98 [+1.37, +3.62] (32) |
| B - A | upd-1k | +2.25 (n=7) | +1.18 (n=7) | +1.14 (n=7) | -1.10 (n=7) | -4.13 (n=2) | - | -0.40 (n=4) | +0.66 [-0.21, +1.30] (32) |
| B - A | upd-10k | +1.62 (n=7) | +1.25 (n=7) | -0.11 (n=7) | +0.27 (n=7) | +0.19 (n=2) | +4.60 (n=1) | +2.45 (n=5) | +0.84 [-0.11, +1.72] (33) |

**A2 - A, the floor** (two servers of one binary):

| Pair | Cell | life 1 | life 2 | life 3 | life 4 | life 5 | life 6 | life 7 | pooled median [95% CI] (rounds) |
|---|---|---|---|---|---|---|---|---|---|
| A2 - A | sel-200 | -0.05 (n=7) | +0.19 (n=7) | -0.27 (n=7) | +0.11 (n=7) | +0.24 (n=3) | +0.25 (n=1) | +0.06 (n=5) | +0.03 [-0.13, +0.11] (33) |
| A2 - A | sel-1k | -0.20 (n=7) | -0.04 (n=7) | -0.30 (n=7) | +0.12 (n=7) | +0.20 (n=3) | - | +0.00 (n=4) | -0.03 [-0.16, +0.07] (32) |
| A2 - A | sel-10k | -0.09 (n=7) | +0.24 (n=7) | -0.22 (n=7) | +0.25 (n=7) | -0.18 (n=2) | - | +0.48 (n=5) | +0.04 [-0.12, +0.24] (33) |
| A2 - A | upd-200 | +0.11 (n=7) | +0.17 (n=7) | +0.89 (n=7) | -0.89 (n=7) | -1.99 (n=3) | - | -0.56 (n=4) | +0.06 [-0.88, +0.89] (32) |
| A2 - A | upd-1k | +0.97 (n=7) | +0.53 (n=7) | -0.68 (n=7) | -1.42 (n=7) | -3.00 (n=2) | - | -2.55 (n=4) | -0.71 [-1.57, +0.53] (32) |
| A2 - A | upd-10k | +2.68 (n=7) | -1.37 (n=7) | +1.22 (n=7) | -3.16 (n=7) | -1.56 (n=2) | +15.04 (n=1) | +1.29 (n=5) | -0.12 [-1.37, +1.22] (33) |

**Verdict on the bar "no cell slower outside its own spread", C against A:**

| Cell | C - A pooled | Floor (A2 - A pooled; per lifetime) | Verdict |
|---|---|---|---|
| sel-200 | +0.13 µs [-0.09, +0.30] | +0.03; -0.27 to +0.24 | **Met, inside the floor** |
| sel-1k | +0.10 µs [+0.03, +0.17] | -0.03; -0.30 to +0.20 | **Met, inside the floor** (interval above zero by 0.03 µs; a lean of 0.1 is not excluded) |
| sel-10k | +0.10 µs [+0.01, +0.26] | +0.04; -0.22 to +0.48 | **Met, inside the floor** (same lean) |
| upd-200 | +0.19 µs [-1.22, +1.38] | +0.06; -0.89 to +0.89 | **Not resolved from the floor** |
| upd-1k | -0.35 µs [-1.08, +0.36] | -0.71; -2.55 to +0.97 | **Not resolved from the floor** |
| upd-10k | -1.12 µs [-2.39, +0.50] | -0.12; -3.16 to +2.68 | **Not resolved from the floor** |

**Did the remedy move anything?** On the SELECT, no: C - B is -0.00, +0.05 and
-0.01 µs with intervals of about +/- 0.2, and C - A (+0.10 to +0.13) is the
same lean B - A shows here (+0.18, +0.12, +0.23) and in the first file
(+0.10 to +0.23). On the UPDATE, B - A at 200 rows reads +1.98 µs
[+1.37, +3.62] in this run (+1.97 to +2.10 in each of lifetimes 1 to 4; the
first file pooled +0.17) and C - A reads +0.19 µs, so C - B is -1.59 µs
[-3.43, -0.68], the one interval of six that excludes zero. At 1K and 10K rows
B - A and C - B contain zero. One cell in one direction, against a first run
that disagrees on B - A, suggests the two `DispatchOutcome` moves cost on the
order of 1 to 2 µs of an UPDATE; it is not a finding.

Throughput and latency (median over blocks of each block's statistic; a
`sel` block is 10,000 statements, an `upd` block 3,000; blocks a cell in the
column):

| Cell | Arm | Blocks | QPS | p0 µs | p25 µs | p50 µs | p95 µs | p99 µs | Server CPU a statement µs |
|---|---|---|---|---|---|---|---|---|---|
| sel-200 | A | 33 | 25,364 | 34.2 | 37.1 | 37.6 | 46.1 | 55.2 | 20.26 |
| sel-200 | A2 | 33 | 25,474 | 34.2 | 37.1 | 37.6 | 46.1 | 55.1 | 20.31 |
| sel-200 | B | 33 | 25,358 | 35.5 | 37.3 | 37.7 | 46.3 | 55.0 | 20.38 |
| sel-200 | C | 33 | 25,302 | 35.0 | 37.2 | 37.7 | 46.3 | 55.5 | 20.40 |
| sel-1k | A | 32 | 25,344 | 33.1 | 37.1 | 37.6 | 46.3 | 54.9 | 20.35 |
| sel-1k | A2 | 32 | 25,351 | 35.4 | 37.2 | 37.7 | 45.9 | 55.2 | 20.27 |
| sel-1k | B | 32 | 25,301 | 33.5 | 37.2 | 37.7 | 46.3 | 55.5 | 20.47 |
| sel-1k | C | 32 | 25,329 | 34.1 | 37.2 | 37.7 | 46.3 | 56.3 | 20.40 |
| sel-10k | A | 33 | 25,246 | 34.7 | 37.3 | 37.8 | 46.7 | 55.5 | 20.46 |
| sel-10k | A2 | 33 | 25,262 | 34.5 | 37.4 | 37.9 | 46.4 | 56.2 | 20.40 |
| sel-10k | B | 33 | 25,175 | 35.7 | 37.5 | 37.9 | 46.3 | 55.4 | 20.60 |
| sel-10k | C | 33 | 25,135 | 34.4 | 37.4 | 37.9 | 46.6 | 55.8 | 20.48 |
| upd-200 | A | 32 | 1,946 | 312.9 | 441.3 | 495.2 | 693.1 | 820.7 | 74.37 |
| upd-200 | A2 | 32 | 1,948 | 312.7 | 441.2 | 495.4 | 685.3 | 821.7 | 74.46 |
| upd-200 | B | 32 | 1,938 | 310.5 | 442.9 | 498.4 | 692.1 | 826.6 | 76.16 |
| upd-200 | C | 32 | 1,948 | 307.7 | 441.0 | 497.6 | 686.5 | 823.9 | 74.67 |
| upd-1k | A | 32 | 1,943 | 311.1 | 441.6 | 494.8 | 691.4 | 832.8 | 74.72 |
| upd-1k | A2 | 32 | 1,938 | 314.7 | 441.2 | 495.9 | 687.7 | 823.0 | 74.44 |
| upd-1k | B | 32 | 1,944 | 311.4 | 438.5 | 495.1 | 689.5 | 835.9 | 75.14 |
| upd-1k | C | 32 | 1,933 | 311.8 | 441.2 | 498.9 | 698.6 | 839.0 | 74.79 |
| upd-10k | A | 33 | 1,939 | 314.0 | 441.1 | 497.0 | 690.2 | 822.1 | 75.08 |
| upd-10k | A2 | 33 | 1,936 | 311.9 | 442.2 | 497.0 | 692.8 | 833.9 | 74.26 |
| upd-10k | B | 33 | 1,947 | 309.5 | 439.6 | 495.7 | 688.8 | 822.3 | 74.84 |
| upd-10k | C | 33 | 1,950 | 310.0 | 440.4 | 494.9 | 686.4 | 838.6 | 74.26 |

QPS agrees across arms within 0.5 % on the SELECT and 0.7 % on the UPDATE, and
the percentiles are one distribution at the resolution shown. Wait
decomposition is not repeated: a point SELECT is about 20.4 µs of server CPU in
a 37.7 µs round trip and an UPDATE about 75 µs in about 495 µs, the rest the
group sync and the socket, as in the first file.

## What the run teaches

The two removals are below this host's resolution on a statement that reads and
at most 1 to 2 µs of 75 on one that commits, so the 0.1 to 0.2 µs lean the
first close saw on the plain SELECT is not explained by the temporary vector: C
still sits 0.10 to 0.13 µs above A, as B did. What the lean is, is not known
from this data; the layout offset the CX close bisect measured between builds
of nearly identical source (0.2 to 0.3 µs) is a candidate that cannot be
separated from a real cost without a PMU or a same-source rebuild control. The
practical reading is the first file's: a statement without the clause pays
nothing measurable, and the UPDATE cell bounds a cost only at about 1 to 2 µs
of 75. The wait that dominates an UPDATE's round trip, the group sync, is
untouched by any of this.

## What was cut, and the time

* **Fewer than five clean lifetimes**: four fully clean (1 to 4) and one
  partial (7, competing rounds dropped). Lifetimes 5 and 6 ran under another
  session's build and are excluded; 7 was run to replace them and met that
  build's tail.
* Rounds cut to 7 a lifetime (8 in the first file), with four arms. Statement
  counts a block as in the first file. Descriptive cell not rerun.
* **Time.** Building 85 s (quiet host). Running 07:20:54 to 07:45:50, about
  25 min wall for seven lifetimes, of which about 4 min was the quiet gate
  holding (07:18:38 to 07:20:54 before lifetime 1, 07:37:10 to 07:38:55
  before 6, 07:42:18 to 07:42:33 before 7; `archive/.../held2.log`), so about
  21 min of running against the 20 min target; the overrun is lifetime 7. The
  gate's process pattern also matches other sessions' shell wrappers, so some
  holds were on a wrapper and not on a compiler. A 60 s watchdog wrote
  `archive/.../watch2.txt`. No `ctest`, no engine code touched.
