# DF close, attributed with knob binaries: removing all three groups of additions takes out about three quarters of B - A on a read, and no single knob is separated from the binaries' own offsets

On worktree `da-q8-memory-bound-key` (branch `worktree-df-statistics-coverage`)
at `ec5ddfe8` (`v3.4.13-14-gec5ddfe8`, B), whose tree the coordinator reports `35f8cce4` (HEAD after the
remedy `9f588fa2` was reverted) to repeat byte for byte, four patched exports of
B each remove one group of what DF-S2 to DF-S4 put on a read's path, and a
seventh arm in every lifetime is A (`8e7c820f`, B's parent) again. At `cores = 1`,
`relaxed`, 1,000 rows, ten lifetimes, server CPU a statement, both driver paths
pooled:

| | select-pk | select-index |
|---|---|---|
| B - A (all of DF-S2 to S4) | +0.18 µs [+0.12, +0.23] | +0.33 µs [+0.28, +0.41] |
| KA - A (all three knobs: what is left) | +0.07 µs [+0.00, +0.09] | +0.15 µs [+0.11, +0.22] |
| B - KA (what the three knobs remove) | +0.14 µs [+0.07, +0.19] | +0.24 µs [+0.16, +0.28] |
| floor, A' - A | -0.10 µs [-0.15, -0.04] | -0.15 µs [-0.19, -0.10] |

**Reading, stated without rounding.** (1) The three knobs together, KA, remove
+0.14 µs of B's +0.18 µs on a tuple key read and +0.24 µs of its +0.33 µs on an
index-served read, about three quarters of each, both intervals above zero;
what is left, KA - A, is +0.07 µs (reaching zero) and +0.15 µs (above it).
(2) **No single knob's removal is separated from the floor in a way that
attributes the cost**: every one of B - K1, B - K2 and B - K3 is above zero
on the reads, but they do not add up (on a tuple key read +0.08, +0.22 and +0.15
µs sum to +0.45 µs against a B - A of +0.18 and a B - KA of +0.14 µs), the
knob binaries differ from B in layout as well as in the removed code, and
this run's A' sits 0.10 to 0.15 µs *below* A (a floor with its interval off
zero), so a difference of that size is the binaries' and the processes' own
and is not attributed to a line of code. (3) What is left in KA - A (+0.15 µs
on `select-index`) is what no knob removes: the wider `StepStats`, the
index-walk counter bumped at each seek, the larger `Transaction` and
manager, and the code's layout; this run cannot split them.

## Run stamp

| Item | Value |
|---|---|
| Executed | 2026-11-19, patches and builds 07:55:50 to 08:00:58 UTC; seven-arm lifetimes 08:01:03 to 08:11:21 UTC |
| Branch / worktree | `worktree-df-statistics-coverage`, worktree `da-q8-memory-bound-key`; the patches were applied to exports under `/home/louis/dfclose/`, never to the worktree; nothing committed |
| A | `8e7c820f` (`v3.4.13-10-g8e7c820f`), sha256 `ab5b9ee7cc7875bbe199ee402214b8e12176c8a00f909eb6e0ca1aae4cba9437` (the first file's binary) |
| A' | A's binary again, a second server of it in every lifetime: the floor |
| B | `ec5ddfe8` (`v3.4.13-14-gec5ddfe8`), sha256 `35bac562e5bfcc23614cd70aa35e9fda44359b52a5fa3f6eb9b8024fb4021604` (the first file's binary; HEAD `35f8cce4` has the same tree) |
| K1 | B's export with `RecordIndexUse(chain, stats);` removed from `CommandDispatcher::RecordExecution`; sha256 `905b56677f4316c9e86be3aec37349ae901e23756c2b9252bddfd068d6aaaeee` |
| K2 | B's export with the bodies of `Session::NoteViewMinted` and `EndStatementView` empty and `NoteTxnBegan` keeping only `txn_began_ns_ = ns;`; sha256 `fbdc40101d292573b2211161cf78b8f45152561a4ca654ac83390e1cb8a393af` |
| K3 | B's export without the `min_snapshot_minted_ns` store in `PublishSnapshotBound`, the two `view_minted_ns_ = now_ns` assignments and the two `reader_minted_ns_[...] = now_ns` stores in `manager.cpp`; sha256 `63650ca1f655e5f975d462977693e99f2627c0b29f605101d0aac8995507ebf3` |
| KA | K1 + K2 + K3 in one binary; sha256 `569a06d03f09c1564ce20a8d14b8fd571d39f102fd8fbdedb52359cd3f9746d3` |
| Patches | `archive/df-close-knobs-v3.4.13-14-gec5ddfe8/K1.diff`, `K2.diff`, `K3.diff`, `KA.diff`, made by `mk.py` (each assertion-checked: every line read as the task described, so no equivalent change was needed; the removed `RecordIndexUse` call is commented, `now_ns` and `minted_ns` are `(void)`-cast, the `NoteViewMinted` parameter unnamed, so each builds without a new warning) |
| Build | Release, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, `kds_server` only, GNU 14.2.0; 76, 75, 76 and 79 s for KA, K1, K2, K3 in that order (`buildk.py`, `build3.out`); every server started from a copy in `/home/louis/dfclose/run/` |
| Device | data files and WAL under `/home/louis/dfclose/run/wd_k_*/` on `/dev/nvme0n1p1`, ext4; not tmpfs |
| Host | Intel Xeon Platinum 8581C @ 2.10 GHz, 8 CPUs, Linux 6.12.116+deb13-cloud-amd64, no PMU; server on CPU 3, driver on CPU 5 (SMT siblings 7 and 1 watched) |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, every other key default; ports 32600 to 32606 |
| Host load | 1-minute load 0.55 to 5.01 at the cells; 40 of 40 cells clean; the process-level gate held lifetime 4 for about 90 s (another session's command line carrying a build or test word, 08:04:30 to 08:06:03) |

## Method

The first file's design with seven arms a lifetime (A, A', B, K1, K2, K3, KA):
ten lifetimes, every server fresh with a fresh data file, arms interleaved in
blocks of 500 statements (24 blocks, 12,000 statements an arm, cell and
lifetime), the block's order rotating by one server per block and the
`--arm` order by the lifetime number. Cells: `select-pk` and `select-index`,
each `literal` and `prepared`; one connection; `--rows 1000`. The task allowed
six arms; K3 was kept as its own arm because the seven fitted (53 s a
lifetime). A delta is the median over blocks of the block-paired difference,
pooled over the lifetimes with a 2,000-resample bootstrap over blocks; the
floor is A' - A. `B - Kx` is the paired difference of B's block and Kx's
block and does not involve A. 0 bad replies.

```
python3 tools/bn_census.py --arm A=<run>/kds_server_A --arm A2=<run>/kds_server_A --arm B=<run>/kds_server_B \
  --arm K1=<run>/kds_server_K1 --arm K2=<run>/kds_server_K2 --arm K3=<run>/kds_server_K3 --arm KA=<run>/kds_server_KA \
  --workdir <run>/wd_k_i --port 32600 --pin-server 3 --pin-driver 5 --cores 1 --rows 1000 --scan-rows 1000 \
  --ops 12000 --blocks 24 --runs 1 --only select-pk,select-index --cell-quiet-pct 15 --cell-quiet-wait 60 \
  --cell-retries 2 --cell-clean-pct 25 --out <run>/out_k_i.json
```
(`lifek.sh k 1 10 12000 24 select-pk,select-index 1000 32600 5 3 A A2 B K1 K2 K3 KA`;
`A2` is the driver's label for A'; the tool is `tools/bn_census.py`, git blob
`a6aee7135c964c75a01a695627055ef95d54b5b7`, unmodified.)

## Result: per cell

Server CPU a statement; each cell the pooled median of the block-paired delta
[95 % bootstrap over blocks], 240 blocks; the headline matrix, every pair of
every cell (A's server CPU in the second column):

| Cell | A | B - A | K1 - A | K2 - A | K3 - A | KA - A | B - K1 | B - K2 | B - K3 | B - KA | A' - A |
|---|---|---|---|---|---|---|---|---|---|---|---|
| select-pk literal | 21.42 us | +0.25 [+0.14, +0.31] | +0.14 [+0.07, +0.20] | +0.06 [-0.02, +0.13] | +0.12 [+0.00, +0.22] | +0.08 [+0.00, +0.15] | +0.08 [+0.03, +0.19] | +0.23 [+0.14, +0.31] | +0.17 [+0.07, +0.25] | +0.18 [+0.06, +0.26] | -0.09 [-0.17, -0.02] |
| select-pk prepared | 20.56 us | +0.12 [+0.06, +0.19] | +0.10 [+0.05, +0.17] | -0.05 [-0.12, +0.00] | +0.00 [-0.08, +0.07] | +0.03 [-0.07, +0.09] | +0.07 [+0.00, +0.14] | +0.21 [+0.15, +0.27] | +0.15 [+0.10, +0.19] | +0.12 [+0.03, +0.18] | -0.12 [-0.17, -0.03] |
| select-index literal | 24.45 us | +0.33 [+0.21, +0.42] | +0.16 [+0.10, +0.23] | +0.19 [+0.10, +0.26] | +0.28 [+0.22, +0.38] | +0.18 [+0.09, +0.26] | +0.18 [+0.12, +0.25] | +0.14 [+0.07, +0.27] | +0.02 [-0.05, +0.11] | +0.14 [+0.04, +0.26] | -0.15 [-0.23, -0.09] |
| select-index prepared | 23.57 us | +0.33 [+0.24, +0.46] | +0.13 [+0.07, +0.21] | +0.10 [+0.06, +0.16] | +0.21 [+0.12, +0.26] | +0.14 [+0.07, +0.22] | +0.25 [+0.20, +0.28] | +0.29 [+0.18, +0.35] | +0.16 [+0.09, +0.24] | +0.29 [+0.21, +0.36] | -0.14 [-0.21, -0.07] |

By shape, the two paths pooled:

| Shape | B - A | K1 - A | K2 - A | K3 - A | KA - A | B - K1 | B - K2 | B - K3 | B - KA | A' - A |
|---|---|---|---|---|---|---|---|---|---|---|
| select-pk | +0.18 [+0.12, +0.23] | +0.12 [+0.08, +0.16] | +0.00 [-0.06, +0.06] | +0.05 [-0.01, +0.12] | +0.07 [+0.00, +0.09] | +0.08 [+0.03, +0.12] | +0.22 [+0.17, +0.27] | +0.15 [+0.11, +0.19] | +0.14 [+0.07, +0.19] | -0.10 [-0.15, -0.04] |
| select-index | +0.33 [+0.28, +0.41] | +0.15 [+0.11, +0.20] | +0.13 [+0.08, +0.18] | +0.24 [+0.20, +0.29] | +0.15 [+0.11, +0.22] | +0.21 [+0.18, +0.25] | +0.23 [+0.14, +0.29] | +0.10 [+0.04, +0.17] | +0.24 [+0.16, +0.28] | -0.15 [-0.19, -0.10] |

(us; lifetime medians per shape of the same pairs)

| Shape | B - A | K1 - A | K2 - A | K3 - A | KA - A | B - K1 | B - K2 | B - K3 | B - KA | A' - A |
|---|---|---|---|---|---|---|---|---|---|---|
| select-pk | +0.17 [+0.10, +0.29], 9 of 10 | +0.12 [+0.09, +0.13], 10 of 10 | +0.00 [-0.14, +0.09], 5 of 10 | +0.04 [+0.00, +0.16], 8 of 10 | +0.04 [-0.06, +0.20], 6 of 10 | +0.12 [+0.02, +0.19], 8 of 10 | +0.24 [+0.08, +0.33], 10 of 10 | +0.18 [+0.13, +0.22], 9 of 10 | +0.13 [+0.05, +0.21], 8 of 10 | -0.08 [-0.19, +0.06], 4 of 10 |
| select-index | +0.37 [+0.19, +0.46], 10 of 10 | +0.20 [+0.06, +0.28], 8 of 10 | +0.13 [+0.06, +0.20], 10 of 10 | +0.23 [+0.15, +0.34], 10 of 10 | +0.13 [+0.03, +0.29], 8 of 10 | +0.21 [+0.09, +0.31], 9 of 10 | +0.23 [+0.14, +0.33], 9 of 10 | +0.10 [+0.03, +0.23], 9 of 10 | +0.26 [+0.13, +0.31], 9 of 10 | -0.11 [-0.19, -0.04], 1 of 10 |

Per cell, with lifetime medians (range, lifetimes above zero) and blocks above zero:

#### select-pk / literal (A server CPU 21.42 us; arms' mean server CPU: A 21.42, A2 21.26, B 21.63, K1 21.55, K2 21.56, K3 21.52, KA 21.42)

| Pair | Pooled median [95 % bootstrap] | Lifetime medians: median, range, above 0 | Blocks above 0 |
|---|---|---|---|
| B - A | +0.25 us [+0.14, +0.31] | +0.24 us, -0.10..+0.52, 9 of 10 | 166 of 240 |
| K1 - A | +0.14 us [+0.07, +0.20] | +0.15 us, -0.18..+0.25, 9 of 10 | 147 of 240 |
| K2 - A | +0.06 us [-0.02, +0.13] | +0.02 us, -0.28..+0.25, 5 of 10 | 130 of 240 |
| K3 - A | +0.12 us [+0.00, +0.22] | +0.07 us, -0.24..+0.41, 7 of 10 | 134 of 240 |
| KA - A | +0.08 us [+0.00, +0.15] | +0.08 us, -0.23..+0.40, 7 of 10 | 133 of 240 |
| B - K1 | +0.08 us [+0.03, +0.19] | +0.12 us, -0.05..+0.30, 9 of 10 | 143 of 240 |
| B - K2 | +0.23 us [+0.14, +0.31] | +0.20 us, -0.00..+0.61, 9 of 10 | 160 of 240 |
| B - K3 | +0.17 us [+0.07, +0.25] | +0.21 us, -0.15..+0.46, 9 of 10 | 148 of 240 |
| B - KA | +0.18 us [+0.06, +0.26] | +0.13 us, -0.05..+0.57, 8 of 10 | 152 of 240 |
| A' - A | -0.09 us [-0.17, -0.02] | -0.12 us, -0.39..+0.31, 4 of 10 | 101 of 240 |

#### select-pk / prepared (A server CPU 20.56 us; arms' mean server CPU: A 20.56, A2 20.48, B 20.76, K1 20.70, K2 20.49, K3 20.72, KA 20.76)

| Pair | Pooled median [95 % bootstrap] | Lifetime medians: median, range, above 0 | Blocks above 0 |
|---|---|---|---|
| B - A | +0.12 us [+0.06, +0.19] | +0.14 us, -0.17..+0.38, 7 of 10 | 144 of 240 |
| K1 - A | +0.10 us [+0.05, +0.17] | +0.09 us, +0.05..+0.38, 10 of 10 | 144 of 240 |
| K2 - A | -0.05 us [-0.12, +0.00] | -0.06 us, -0.27..+0.21, 3 of 10 | 105 of 240 |
| K3 - A | +0.00 us [-0.08, +0.07] | +0.02 us, -0.31..+0.27, 7 of 10 | 118 of 240 |
| KA - A | +0.03 us [-0.07, +0.09] | -0.01 us, -0.42..+0.35, 4 of 10 | 126 of 240 |
| B - K1 | +0.07 us [+0.00, +0.14] | +0.10 us, -0.19..+0.25, 6 of 10 | 134 of 240 |
| B - K2 | +0.21 us [+0.15, +0.27] | +0.25 us, -0.11..+0.36, 9 of 10 | 169 of 240 |
| B - K3 | +0.15 us [+0.10, +0.19] | +0.14 us, -0.08..+0.41, 8 of 10 | 153 of 240 |
| B - KA | +0.12 us [+0.03, +0.18] | +0.11 us, -0.12..+0.26, 8 of 10 | 141 of 240 |
| A' - A | -0.12 us [-0.17, -0.03] | -0.11 us, -0.42..+0.13, 3 of 10 | 95 of 240 |

#### select-index / literal (A server CPU 24.45 us; arms' mean server CPU: A 24.45, A2 24.40, B 24.91, K1 24.77, K2 24.75, K3 24.85, KA 24.72)

| Pair | Pooled median [95 % bootstrap] | Lifetime medians: median, range, above 0 | Blocks above 0 |
|---|---|---|---|
| B - A | +0.33 us [+0.21, +0.42] | +0.33 us, +0.05..+0.79, 10 of 10 | 175 of 240 |
| K1 - A | +0.16 us [+0.10, +0.23] | +0.18 us, -0.10..+0.45, 8 of 10 | 150 of 240 |
| K2 - A | +0.19 us [+0.10, +0.26] | +0.13 us, +0.03..+0.39, 10 of 10 | 154 of 240 |
| K3 - A | +0.28 us [+0.22, +0.38] | +0.27 us, +0.02..+0.77, 10 of 10 | 165 of 240 |
| KA - A | +0.18 us [+0.09, +0.26] | +0.14 us, -0.05..+0.39, 8 of 10 | 153 of 240 |
| B - K1 | +0.18 us [+0.12, +0.25] | +0.21 us, -0.00..+0.35, 9 of 10 | 159 of 240 |
| B - K2 | +0.14 us [+0.07, +0.27] | +0.20 us, -0.06..+0.42, 7 of 10 | 143 of 240 |
| B - K3 | +0.02 us [-0.05, +0.11] | +0.07 us, -0.21..+0.32, 7 of 10 | 126 of 240 |
| B - KA | +0.14 us [+0.04, +0.26] | +0.24 us, -0.05..+0.45, 9 of 10 | 145 of 240 |
| A' - A | -0.15 us [-0.23, -0.09] | -0.09 us, -0.36..+0.12, 2 of 10 | 85 of 240 |

#### select-index / prepared (A server CPU 23.57 us; arms' mean server CPU: A 23.57, A2 23.48, B 23.97, K1 23.85, K2 23.77, K3 23.78, KA 23.72)

| Pair | Pooled median [95 % bootstrap] | Lifetime medians: median, range, above 0 | Blocks above 0 |
|---|---|---|---|
| B - A | +0.33 us [+0.24, +0.46] | +0.41 us, +0.04..+0.57, 10 of 10 | 181 of 240 |
| K1 - A | +0.13 us [+0.07, +0.21] | +0.19 us, -0.12..+0.32, 8 of 10 | 157 of 240 |
| K2 - A | +0.10 us [+0.06, +0.16] | +0.09 us, -0.02..+0.29, 7 of 10 | 146 of 240 |
| K3 - A | +0.21 us [+0.12, +0.26] | +0.18 us, +0.04..+0.55, 10 of 10 | 162 of 240 |
| KA - A | +0.14 us [+0.07, +0.22] | +0.11 us, -0.07..+0.55, 8 of 10 | 146 of 240 |
| B - K1 | +0.25 us [+0.20, +0.28] | +0.20 us, -0.08..+0.50, 9 of 10 | 175 of 240 |
| B - K2 | +0.29 us [+0.18, +0.35] | +0.29 us, +0.02..+0.55, 10 of 10 | 167 of 240 |
| B - K3 | +0.16 us [+0.09, +0.24] | +0.18 us, -0.14..+0.42, 9 of 10 | 154 of 240 |
| B - KA | +0.29 us [+0.21, +0.36] | +0.26 us, -0.10..+0.46, 9 of 10 | 166 of 240 |
| A' - A | -0.14 us [-0.21, -0.07] | -0.12 us, -0.38..+0.13, 3 of 10 | 86 of 240 |

## What the knobs say

- **How much of B - A KA removes.** B - KA is +0.14 µs of B - A = +0.18 µs on
  `select-pk` (78 %) and +0.24 µs of +0.33 µs on `select-index` (73 %); per
  cell the shares run from 42 % to 100 % (`select-pk literal` +0.18 of +0.25,
  `prepared` +0.12 of +0.12, `select-index literal` +0.14 of +0.33 -
  that cell removes the least - and `prepared` +0.29 of +0.33). Every interval
  of B - KA excludes zero, the lowest lower end being +0.03 µs.
  The honest size of the attribution is "about three quarters, give or take the
  floor", since a binary differing only in layout moves a cell by 0.1 to
  0.3 µs.
- **Is a single knob's removal resolved from the floor?** Each
  `B - Kx` has a pooled interval above zero on most read cells, and
  `B - K2` is the largest on a tuple key read (+0.22 µs [+0.17, +0.27], 10 of
  10 lifetime-shape medians above zero), then `B - K3` (+0.15), then `B - K1`
  (+0.08). On `select-index` it is `B - K2` (+0.23), `B - K1` (+0.21), then
  `B - K3` (+0.10 µs [+0.04, +0.17]). **But these do not compose and are not
  attributed**: the three tuple key removals sum to +0.45 µs against a B - A of
  +0.18, K2 - A is +0.00 µs on `select-pk` and K1 - A +0.12, and the order of
  the knobs by size differs between the two shapes. A knob binary also differs
  from B in layout, a removal of the size of the floor (this run's A' is
  below A by 0.10 to 0.15 µs, intervals off zero, and the bisect found two
  servers of one binary 0.27 µs apart) is not attributed, and every one of
  these single-knob figures is of that size. The data support "no one knob
  carries the cost", not "this knob carries x".
- **A's offset.** A' - A is -0.09 to -0.15 µs in every cell (A' faster than A, A' above A in 2 to 4 of 10 lifetimes): the A
  server of this run is the slower of two of one binary by about 0.1 µs, so the `X - A` columns
  read about 0.1 µs low for every arm and the `B - Kx` columns are untouched
  by it. Reading B - A against A' would give about +0.28 µs on
  `select-pk` and +0.48 µs on `select-index`; this is not substituted, because
  A' is as much a process as A.
- **What is left, KA - A.** +0.07 µs [+0.00, +0.09] on `select-pk` (not
  resolved from the floor) and +0.15 µs [+0.11, +0.22] on `select-index`
  (above it, 8 of 10 lifetime medians). It is made of what no knob removes:
  the wider `StepStats`, the index-walk counter bumped at each seek, the
  larger `Transaction` and manager, and the code's layout; this run does not
  separate them, and a layout offset is the likeliest part of the tuple key
  figure. The index-served remainder is the one above the floor, and the
  per-seek counter and the wider `StepStats` are the pieces of it that belong to
  an index read.
- **Not measured**: `K1` as the step walk alone (the removed call is the
  whole `RecordIndexUse`), writes, `cores > 1`, a per-piece cost inside a knob.
  A and the knob binaries share DF-S1's code, so DF-S1's +0.44 µs
  (`bench/v3.4.13/results-cx-close-bisect-v3.4.13-1-g11f72ce9.md`) is in every
  column's base and is not attributed here.

### Every lifetime

#### select-pk / literal

| Lifetime | A | A' | B | K1 | K2 | K3 | KA | B - A | K1 - A | K2 - A | K3 - A | KA - A | B - K1 | B - K2 | B - K3 | B - KA | A' - A | clean |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 21.25 us | 21.02 us | 21.44 us | 21.34 us | 21.61 us | 21.50 us | 21.40 us | +0.30 us | +0.18 us | +0.07 us | +0.28 us | +0.18 us | +0.23 us | +0.19 us | +0.04 us | +0.26 us | -0.17 us | yes |
| 2 | 21.13 us | 21.25 us | 21.45 us | 22.04 us | 21.20 us | 21.39 us | 21.23 us | +0.11 us | +0.12 us | -0.11 us | +0.21 us | +0.11 us | +0.08 us | +0.32 us | +0.16 us | +0.05 us | +0.04 us | yes |
| 3 | 21.50 us | 21.30 us | 21.46 us | 21.37 us | 21.57 us | 21.31 us | 21.40 us | +0.09 us | +0.11 us | +0.17 us | -0.01 us | -0.05 us | +0.05 us | -0.00 us | +0.24 us | +0.10 us | +0.03 us | yes |
| 4 | 21.78 us | 21.06 us | 21.62 us | 21.43 us | 21.38 us | 21.35 us | 21.26 us | -0.10 us | -0.18 us | -0.28 us | -0.24 us | -0.23 us | +0.09 us | +0.20 us | +0.25 us | +0.17 us | -0.39 us | yes |
| 5 | 21.37 us | 21.40 us | 21.87 us | 21.75 us | 21.42 us | 21.52 us | 21.47 us | +0.36 us | +0.20 us | -0.18 us | +0.02 us | +0.05 us | +0.12 us | +0.50 us | +0.33 us | +0.20 us | -0.13 us | yes |
| 6 | 21.57 us | 21.08 us | 21.61 us | 21.23 us | 21.15 us | 21.07 us | 21.54 us | +0.20 us | +0.06 us | -0.03 us | -0.01 us | +0.22 us | +0.12 us | +0.20 us | +0.31 us | -0.04 us | -0.18 us | yes |
| 7 | 21.76 us | 21.47 us | 21.67 us | 21.63 us | 22.01 us | 21.44 us | 22.05 us | +0.33 us | +0.20 us | +0.25 us | +0.06 us | +0.40 us | +0.22 us | +0.26 us | +0.18 us | -0.05 us | +0.31 us | yes |
| 8 | 21.33 us | 21.45 us | 22.06 us | 21.48 us | 21.18 us | 21.47 us | 21.16 us | +0.52 us | +0.10 us | -0.13 us | +0.07 us | -0.09 us | +0.30 us | +0.61 us | +0.46 us | +0.57 us | -0.11 us | yes |
| 9 | 21.14 us | 21.06 us | 21.37 us | 21.18 us | 22.08 us | 22.10 us | 21.15 us | +0.28 us | +0.17 us | +0.06 us | +0.17 us | +0.09 us | +0.13 us | +0.18 us | +0.01 us | +0.23 us | -0.14 us | yes |
| 10 | 21.40 us | 21.52 us | 21.70 us | 22.03 us | 21.97 us | 22.06 us | 21.53 us | +0.15 us | +0.25 us | +0.18 us | +0.41 us | +0.07 us | -0.05 us | +0.03 us | -0.15 us | +0.07 us | +0.04 us | yes |

#### select-pk / prepared

| Lifetime | A | A' | B | K1 | K2 | K3 | KA | B - A | K1 - A | K2 - A | K3 - A | KA - A | B - K1 | B - K2 | B - K3 | B - KA | A' - A | clean |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 20.56 us | 20.10 us | 20.65 us | 20.44 us | 20.25 us | 20.44 us | 20.75 us | +0.21 us | +0.07 us | -0.05 us | +0.04 us | -0.01 us | +0.20 us | +0.36 us | +0.22 us | +0.26 us | -0.31 us | yes |
| 2 | 20.26 us | 20.31 us | 20.60 us | 20.52 us | 20.43 us | 20.51 us | 20.77 us | +0.25 us | +0.09 us | -0.04 us | +0.20 us | +0.29 us | +0.13 us | +0.25 us | +0.10 us | +0.09 us | +0.12 us | yes |
| 3 | 20.41 us | 20.46 us | 20.47 us | 20.42 us | 20.49 us | 20.48 us | 20.41 us | -0.01 us | +0.07 us | +0.01 us | -0.03 us | -0.12 us | -0.02 us | +0.06 us | -0.05 us | +0.12 us | -0.02 us | yes |
| 4 | 20.88 us | 20.92 us | 20.94 us | 20.96 us | 20.73 us | 20.94 us | 20.76 us | -0.17 us | +0.18 us | -0.27 us | -0.31 us | -0.42 us | -0.19 us | -0.11 us | +0.20 us | +0.13 us | -0.42 us | yes |
| 5 | 20.88 us | 20.65 us | 20.86 us | 21.09 us | 20.58 us | 21.60 us | 21.01 us | -0.05 us | +0.09 us | -0.22 us | +0.05 us | -0.15 us | -0.06 us | +0.28 us | +0.00 us | +0.11 us | -0.21 us | yes |
| 6 | 20.61 us | 20.56 us | 20.92 us | 20.78 us | 20.74 us | 20.82 us | 21.14 us | +0.10 us | +0.10 us | +0.21 us | +0.03 us | +0.12 us | +0.14 us | +0.14 us | +0.16 us | -0.12 us | -0.09 us | yes |
| 7 | 20.30 us | 20.26 us | 20.81 us | 20.38 us | 20.25 us | 20.35 us | 20.54 us | +0.38 us | +0.05 us | -0.07 us | +0.02 us | +0.19 us | +0.25 us | +0.25 us | +0.22 us | +0.15 us | -0.20 us | yes |
| 8 | 20.54 us | 20.50 us | 20.58 us | 20.69 us | 20.35 us | 20.65 us | 20.49 us | +0.11 us | +0.09 us | -0.14 us | +0.01 us | -0.02 us | +0.06 us | +0.31 us | +0.12 us | +0.08 us | +0.08 us | yes |
| 9 | 20.58 us | 20.42 us | 20.71 us | 20.64 us | 20.47 us | 20.37 us | 20.61 us | +0.25 us | +0.10 us | -0.08 us | -0.17 us | -0.08 us | +0.19 us | +0.29 us | +0.41 us | +0.19 us | -0.14 us | yes |
| 10 | 20.56 us | 20.62 us | 21.04 us | 21.09 us | 20.59 us | 21.02 us | 21.13 us | +0.17 us | +0.38 us | +0.18 us | +0.27 us | +0.35 us | -0.09 us | +0.13 us | -0.08 us | -0.07 us | +0.13 us | yes |

#### select-index / literal

| Lifetime | A | A' | B | K1 | K2 | K3 | KA | B - A | K1 - A | K2 - A | K3 - A | KA - A | B - K1 | B - K2 | B - K3 | B - KA | A' - A | clean |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 24.32 us | 24.07 us | 24.56 us | 24.25 us | 24.57 us | 24.57 us | 24.45 us | +0.37 us | +0.03 us | +0.14 us | +0.27 us | +0.07 us | +0.34 us | +0.18 us | +0.13 us | +0.25 us | -0.21 us | yes |
| 2 | 24.63 us | 24.72 us | 24.96 us | 25.23 us | 24.48 us | 25.03 us | 24.85 us | +0.10 us | +0.20 us | +0.03 us | +0.09 us | +0.11 us | -0.00 us | +0.25 us | +0.04 us | +0.14 us | +0.02 us | yes |
| 3 | 25.24 us | 25.01 us | 25.05 us | 25.44 us | 24.96 us | 25.10 us | 25.31 us | +0.05 us | +0.14 us | +0.10 us | +0.28 us | +0.02 us | +0.05 us | -0.04 us | -0.20 us | +0.04 us | -0.08 us | yes |
| 4 | 24.46 us | 24.08 us | 24.65 us | 24.39 us | 24.65 us | 24.58 us | 24.41 us | +0.09 us | -0.10 us | +0.11 us | +0.02 us | -0.02 us | +0.35 us | -0.06 us | +0.09 us | +0.24 us | -0.36 us | yes |
| 5 | 24.50 us | 24.70 us | 24.72 us | 24.77 us | 24.55 us | 24.85 us | 24.93 us | +0.24 us | +0.17 us | +0.08 us | +0.36 us | +0.33 us | +0.10 us | +0.25 us | -0.10 us | -0.05 us | -0.10 us | yes |
| 6 | 24.35 us | 24.55 us | 24.84 us | 24.70 us | 24.49 us | 24.65 us | 24.69 us | +0.35 us | +0.38 us | +0.08 us | +0.14 us | +0.38 us | +0.11 us | +0.21 us | +0.32 us | +0.02 us | +0.12 us | yes |
| 7 | 24.68 us | 24.30 us | 25.00 us | 24.98 us | 25.37 us | 25.25 us | 24.62 us | +0.36 us | +0.25 us | +0.25 us | +0.48 us | +0.18 us | +0.15 us | -0.03 us | -0.21 us | +0.27 us | -0.16 us | yes |
| 8 | 24.09 us | 23.76 us | 24.76 us | 24.27 us | 24.70 us | 24.49 us | 24.53 us | +0.31 us | -0.04 us | +0.22 us | +0.17 us | -0.05 us | +0.28 us | +0.13 us | +0.15 us | +0.25 us | -0.29 us | yes |
| 9 | 24.18 us | 24.49 us | 25.74 us | 25.11 us | 25.42 us | 25.15 us | 25.13 us | +0.57 us | +0.45 us | +0.39 us | +0.43 us | +0.39 us | +0.30 us | +0.24 us | +0.25 us | +0.33 us | -0.08 us | yes |
| 10 | 24.08 us | 24.30 us | 24.87 us | 24.60 us | 24.32 us | 24.80 us | 24.29 us | +0.79 us | +0.26 us | +0.25 us | +0.77 us | +0.35 us | +0.31 us | +0.42 us | +0.02 us | +0.45 us | -0.04 us | yes |

#### select-index / prepared

| Lifetime | A | A' | B | K1 | K2 | K3 | KA | B - A | K1 - A | K2 - A | K3 - A | KA - A | B - K1 | B - K2 | B - K3 | B - KA | A' - A | clean |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 23.37 us | 23.25 us | 23.59 us | 23.39 us | 23.28 us | 23.42 us | 23.35 us | +0.30 us | -0.06 us | -0.01 us | +0.06 us | +0.01 us | +0.27 us | +0.31 us | +0.21 us | +0.28 us | -0.07 us | yes |
| 2 | 23.68 us | 23.70 us | 23.88 us | 24.12 us | 23.97 us | 23.81 us | 23.52 us | +0.27 us | +0.26 us | -0.02 us | +0.13 us | -0.06 us | +0.04 us | +0.27 us | +0.13 us | +0.36 us | +0.13 us | yes |
| 3 | 23.59 us | 23.92 us | 24.11 us | 24.19 us | 24.08 us | 24.04 us | 23.91 us | +0.10 us | +0.24 us | +0.12 us | +0.40 us | +0.09 us | -0.08 us | +0.02 us | -0.14 us | +0.21 us | +0.01 us | yes |
| 4 | 23.63 us | 23.25 us | 23.68 us | 23.50 us | 23.60 us | 23.67 us | 23.62 us | +0.04 us | -0.12 us | +0.00 us | +0.04 us | -0.07 us | +0.19 us | +0.12 us | +0.05 us | +0.12 us | -0.38 us | yes |
| 5 | 23.65 us | 23.80 us | 24.06 us | 23.99 us | 23.62 us | 23.78 us | 23.86 us | +0.38 us | +0.14 us | +0.07 us | +0.10 us | +0.25 us | +0.20 us | +0.54 us | +0.15 us | +0.25 us | -0.27 us | yes |
| 6 | 23.45 us | 23.24 us | 23.86 us | 23.92 us | 23.83 us | 23.72 us | 24.06 us | +0.50 us | +0.29 us | +0.29 us | +0.33 us | +0.55 us | +0.12 us | +0.12 us | +0.25 us | -0.10 us | -0.15 us | yes |
| 7 | 23.61 us | 23.65 us | 24.31 us | 24.52 us | 24.03 us | 23.91 us | 23.73 us | +0.57 us | +0.32 us | +0.20 us | +0.31 us | +0.24 us | +0.15 us | +0.37 us | +0.42 us | +0.34 us | +0.00 us | yes |
| 8 | 23.37 us | 23.29 us | 24.00 us | 23.50 us | 23.40 us | 23.56 us | 23.42 us | +0.51 us | +0.17 us | +0.05 us | +0.22 us | +0.04 us | +0.32 us | +0.47 us | +0.29 us | +0.44 us | -0.10 us | yes |
| 9 | 23.38 us | 23.26 us | 23.83 us | 23.56 us | 23.59 us | 23.56 us | 23.63 us | +0.44 us | +0.22 us | +0.29 us | +0.11 us | +0.27 us | +0.33 us | +0.20 us | +0.22 us | +0.21 us | -0.14 us | yes |
| 10 | 23.99 us | 23.40 us | 24.41 us | 23.81 us | 24.30 us | 24.37 us | 24.13 us | +0.57 us | +0.14 us | +0.11 us | +0.55 us | +0.14 us | +0.50 us | +0.55 us | +0.04 us | +0.46 us | -0.19 us | yes |

### Latency and throughput (driver-side)

QPS is derived (`1,000,000 / mean µs`); each figure is the median over
lifetimes of the lifetime's per-arm statistic; the driver emits p0, p25, p50,
p90 and p99 (no p95) and no wait-event breakdown. 120,000 statements an arm and
cell. On `select-pk literal` the server is on-CPU about 21.4 µs of a client mean
of about 54 µs; the rest is the client's CPU and the socket, `relaxed` has no
fsync and one connection no lock wait.

| Cell | Arm | statements | QPS (1e6 / mean) | p0 | p25 | p50 | p90 | p99 | server CPU | driver CPU |
|---|---|---|---|---|---|---|---|---|---|---|
| select-pk literal | A | 120,000 | 18,343 qps | 47.3 us | 49.4 us | 50.5 us | 59.5 us | 76.9 us | 21.38 us | 31.75 us |
| select-pk literal | A' | 120,000 | 18,408 qps | 47.4 us | 49.5 us | 50.6 us | 59.7 us | 77.4 us | 21.27 us | 31.66 us |
| select-pk literal | B | 120,000 | 18,296 qps | 47.4 us | 49.6 us | 50.7 us | 60.1 us | 79.1 us | 21.62 us | 31.61 us |
| select-pk literal | K1 | 120,000 | 18,293 qps | 47.3 us | 49.5 us | 50.8 us | 60.2 us | 80.4 us | 21.46 us | 31.84 us |
| select-pk literal | K2 | 120,000 | 18,215 qps | 47.4 us | 49.4 us | 50.7 us | 59.8 us | 81.0 us | 21.50 us | 31.62 us |
| select-pk literal | K3 | 120,000 | 18,263 qps | 47.4 us | 49.6 us | 50.8 us | 59.9 us | 77.3 us | 21.45 us | 31.74 us |
| select-pk literal | KA | 120,000 | 18,314 qps | 47.3 us | 49.4 us | 50.6 us | 60.0 us | 77.9 us | 21.40 us | 31.68 us |
| select-pk prepared | A | 120,000 | 19,899 qps | 44.2 us | 45.9 us | 46.8 us | 54.8 us | 69.6 us | 20.56 us | 28.38 us |
| select-pk prepared | A' | 120,000 | 19,842 qps | 44.2 us | 45.9 us | 46.8 us | 55.5 us | 71.1 us | 20.48 us | 28.49 us |
| select-pk prepared | B | 120,000 | 19,812 qps | 44.3 us | 46.0 us | 47.0 us | 55.8 us | 71.5 us | 20.76 us | 28.43 us |
| select-pk prepared | K1 | 120,000 | 19,880 qps | 44.2 us | 45.9 us | 46.9 us | 55.2 us | 71.0 us | 20.66 us | 28.36 us |
| select-pk prepared | K2 | 120,000 | 19,734 qps | 44.2 us | 45.9 us | 46.7 us | 55.0 us | 72.5 us | 20.48 us | 28.54 us |
| select-pk prepared | K3 | 120,000 | 19,853 qps | 44.3 us | 46.0 us | 47.0 us | 55.2 us | 73.5 us | 20.58 us | 28.43 us |
| select-pk prepared | KA | 120,000 | 19,615 qps | 44.1 us | 45.9 us | 46.9 us | 56.2 us | 73.4 us | 20.75 us | 28.57 us |
| select-index literal | A | 120,000 | 17,431 qps | 49.0 us | 52.2 us | 53.6 us | 63.4 us | 82.3 us | 24.41 us | 31.53 us |
| select-index literal | A' | 120,000 | 17,359 qps | 49.2 us | 52.2 us | 53.7 us | 63.8 us | 80.7 us | 24.40 us | 31.42 us |
| select-index literal | B | 120,000 | 17,263 qps | 49.6 us | 52.6 us | 54.0 us | 64.1 us | 83.7 us | 24.85 us | 31.57 us |
| select-index literal | K1 | 120,000 | 17,338 qps | 49.3 us | 52.3 us | 53.8 us | 64.2 us | 83.4 us | 24.74 us | 31.58 us |
| select-index literal | K2 | 120,000 | 17,248 qps | 49.1 us | 52.3 us | 53.9 us | 64.6 us | 83.6 us | 24.61 us | 31.75 us |
| select-index literal | K3 | 120,000 | 17,216 qps | 49.5 us | 52.5 us | 54.0 us | 64.7 us | 85.0 us | 24.82 us | 31.85 us |
| select-index literal | KA | 120,000 | 17,366 qps | 49.3 us | 52.3 us | 53.9 us | 63.7 us | 82.4 us | 24.65 us | 31.41 us |
| select-index prepared | A | 120,000 | 18,677 qps | 46.0 us | 48.6 us | 49.9 us | 59.1 us | 74.7 us | 23.60 us | 28.62 us |
| select-index prepared | A' | 120,000 | 18,709 qps | 46.1 us | 48.7 us | 50.0 us | 58.5 us | 75.3 us | 23.34 us | 28.45 us |
| select-index prepared | B | 120,000 | 18,537 qps | 46.4 us | 49.0 us | 50.3 us | 59.9 us | 77.3 us | 23.94 us | 28.46 us |
| select-index prepared | K1 | 120,000 | 18,610 qps | 46.1 us | 48.8 us | 50.1 us | 59.3 us | 75.5 us | 23.86 us | 28.39 us |
| select-index prepared | K2 | 120,000 | 18,558 qps | 46.0 us | 48.8 us | 50.1 us | 59.3 us | 76.7 us | 23.72 us | 28.54 us |
| select-index prepared | K3 | 120,000 | 18,605 qps | 46.4 us | 49.0 us | 50.3 us | 59.3 us | 75.4 us | 23.75 us | 28.64 us |
| select-index prepared | KA | 120,000 | 18,565 qps | 46.2 us | 48.8 us | 50.1 us | 58.9 us | 75.4 us | 23.68 us | 28.88 us |

The percentiles and throughput do not separate the arms; the 0.1 to 0.4 µs under
discussion is below 2 % of the round trip.

## Time spent

| What | Time |
|---|---|
| Patching and building K1, K2, K3, KA | about 5 min (07:55:50 to 08:00:58); 75 to 79 s each |
| Running, ten seven-arm lifetimes | 10 min 18 s (08:01:03 to 08:11:21), 53 s a lifetime |
| Held by the process-level quiet gate | about 90 s (lifetime 4, another session's command) |
| Cell gate | 88 s, the driver's 2 s window over 40 cells and no retry |
| Whole round, task to last result | about 20 min |

## What was cut

Nothing from the design: seven arms (K3 kept as its own arm, one more than the
six asked), ten lifetimes, both paths of both cells, 40 of 40 cells clean.
As asked, no writes, no `cores = 8` and no row-count sweep.
