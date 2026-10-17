# CA milestone close: the walked read is 15-23% cheaper, the writes and the point reads did not move

The closing candidate (`c134`, commit `954abc20`) against the commit CA opened from (`base`, engine
`e1333a77`) across the four scenario drivers. The finding is read-side only: s1's three-relation
walks gain +15.3 / +17.4 / +20.5 / +22.4% and s4's `board` and `tape10k` shapes gain +8 to +23%
depending on the Cabin arm, every one outside the arms' own spread. Every write cell (s1
`load-bars` and `result-insert`, all of s2, s4 `open`), every tuple-key point read, and all of
s3 are unchanged within noise. The s4 gain is c134's own: the intermediate `c13` (`0ed684bb`)
holds s1's gain but only +2 to +5% on s4. The sampled pass shows why: `DecodeCell` and
`CheckCellSpan`, 9.9% of s4-off samples in the c13 dump, are not in the c134 top 30.

## Stamp

| Item | Value |
|---|---|
| Executed | 2026-10-17, A/B 12:51:57-13:15:59 UTC (24.0 min, 12 quad cells), sampled pass 13:16-13:18 UTC |
| Worktree / branch | `bz-background-advisory-optimizer` / `worktree-bz-background-advisory-optimizer` |
| `git describe --tags` | `v3.2.0-159-g954abc20` (the commit c134 was built from). HEAD moved to `603e77bf` (CA C4 review, `src/storage/tagged_cell.cpp`, `visibility.hpp`, one test) at 12:54:08 UTC, two minutes into the run, by another session; **no arm is built from `603e77bf`** |
| Tree during the run | `src/`, `include/`, `tools/`, `CMakeLists.txt` unmodified at the start; later `docs/` edits by another session are uncommitted/untracked and not engine inputs |
| Build type | Release; binaries copied to `/home/cdkbs/bench-runs/bz/`, every server started from the copy |
| Device | `/dev/root`, ext4 (`df -T`), data files under `/home/cdkbs/bench-runs/bz/data`, fresh per server |
| Host | 8 CPUs; no `cc1plus`/`cmake`/`ctest` at any of the 12 cell starts (checked by the run script, no `BUILD ACTIVE` line in the timeline); loadavg at cell starts 0.67-2.54; watchdog (60 s status file) logged no stall |
| Server config | s1-s3 `cores = 8`; s4 `cores = 1` x3 servers, `decay_half_life = 5`, `cabin_optimizer_snapshot_interval_ms = 500`; all `durability = group`, `log_level = warn`, `buffer_pool_frames = 65536` |
| Tests | no code change by this run; the correctness suite was not executed here. s1 ran with `--verify` and s2 with `--verify 100`; all 48 cell drivers exited 0 |

| Arm | Commit | sha256 | binary mtime (UTC) |
|---|---|---|---|
| base | engine `e1333a77` | `1dac7afcbdfa8544001c805053fdce7458193cb9ea86c0cf5e267ffa90abc53e` | 2026-10-17 11:20:30 |
| c13 | `0ed684bb`: VisibilityMemo + `ExecStats::For` forced inline | `8703afe181803a097944720b4918bc979f492f4ff20809e6e513965d0a1765d5` | 2026-10-17 12:10:26 (= commit time) |
| c134 | `954abc20`: c13 + Classify refactor + `InlineCellBytes` | `5939ff82743b62c18a1d35762b691822473897182de1861db05e2a3ed01db781` | 2026-10-17 12:41:34 (commit 12:40:49) |

Binaries are the files the operator supplied; the run verified the hashes, not a rebuild from source.
c134's mtime is 45 s after its commit and 12 min before `603e77bf`, so it matches `954abc20` and not HEAD.

## Method

Drivers, sizes and flags are those of rounds S0/R1/R2 (`bench/v3.2.0/archive/ca-harness/README.md`;
exact flags in `archive/ca-close-v3.2.0-159-g954abc20/ca-close-run.sh`). Reduced sizes: s1 5 years x 8
symbols x 2 exchanges of bars; s2 10,000 cargos / 9,000 bookings / 8 bookers; s3 10,000 loans
(2,000 users, 2,000 books); s4 31,200-row boards and 200 / 1K / 10K tapes over 2 days x 4 blocks.
Four reps per arm with the rotating order r1 base c13 c134, r2 c13 c134 base, r3 c134 base c13,
r4 base c13 c134; each cell is a fresh server and data file; s4 ran in all four reps. Cells are
medians of four, in QPS (`ops / elapsed`, or `1e6 / mean_us` for s3's serial driver, which is
exact); **spread** is (max-min)/median over the arm's four reps; `~` marks |delta| <= the larger
spread of the two arms, which is not a finding. The noise floor comes from inside the run: the
s3 `pk-user` control (a tuple-key point read no candidate touches) moves +2.6% / +0.0%, and the
repeated base arm's own spreads are the table's spread columns.

**Cut.** The 200 / 1K / 10K row-set sweep (rule 9) was not repeated: the close compares the
three engines at the S0 reduced sizes, and s4's tape cell is the 10K member. Per-statement
latency percentiles and a wait breakdown (rule 3, 6) are out of scope for a throughput matrix.
Nothing here approached a limit (rule 4b): s1-s3 are one driver at single-digit connections on
8 cores under `group` durability, s4 a one-core fixed-window session, no `strict` cell. These are
fixed-cost per-statement comparisons.

## Result: A/B matrix (QPS, median of 4)

| headline (QPS) | base | spread | c13 | delta | spread | c134 | delta | spread | c134 verdict |
|---|---|---|---|---|---|---|---|---|---|
| s1 load-bars (insert) | 19927 | 8.1% | 18999 | -4.7% ~ | 9.6% | 19773 | -0.8% ~ | 8.7% | unchanged |
| s1 result-insert | 802 | 8.5% | 800 | -0.2% ~ | 13.2% | 817 | +1.9% ~ | 11.4% | unchanged |
| s1 backtest-read (3-rel join, cold) | 1618 | 2.4% | 1869 | +15.5% | 5.9% | 1865 | +15.3% | 4.3% | improved |
| s1 backtest-replay (warm) | 1736 | 1.2% | 1938 | +11.6% | 11.5% | 2037 | +17.4% | 5.7% | improved |
| s1 read-day-slice | 1876 | 0.9% | 2246 | +19.7% | 6.6% | 2260 | +20.5% | 2.2% | improved |
| s1 read-bar-lookup (tuple key) | 12414 | 1.2% | 12303 | -0.9% ~ | 25.4% | 12396 | -0.1% ~ | 10.6% | unchanged |
| s1 read-join-point (tuple key) | 12443 | 2.8% | 12324 | -1.0% ~ | 30.8% | 12162 | -2.3% ~ | 4.0% | unchanged |
| s1 agg-day-slice | 2110 | 1.7% | 2622 | +24.2% | 5.6% | 2583 | +22.4% | 4.5% | improved |
| s2 booking (txn, 8 bookers) | 2344 | 8.1% | 2388 | +1.9% ~ | 2.2% | 2392 | +2.1% ~ | 7.2% | unchanged |
| s2 commit (committed TPS) | 1715 | 8.2% | 1748 | +1.9% ~ | 2.1% | 1751 | +2.1% ~ | 7.3% | unchanged |
| s2 cargo-lookup | 2344 | 8.1% | 2388 | +1.9% ~ | 2.2% | 2393 | +2.1% ~ | 7.2% | unchanged |
| s2 charge-insert | 9647 | 8.2% | 9832 | +1.9% ~ | 2.1% | 9849 | +2.1% ~ | 7.3% | unchanged |
| s3 pk-user (control) | 13900 | 18.4% | 14265 | +2.6% ~ | 22.7% | 13907 | +0.0% ~ | 5.7% | control |
| s3 loans-by-user | 10395 | 4.8% | 10582 | +1.8% ~ | 2.1% | 10515 | +1.2% ~ | 4.1% | unchanged |
| s3 loans-by-book | 11231 | 4.8% | 10644 | -5.2% ~ | 16.9% | 11217 | -0.1% ~ | 0.9% | unchanged |
| s3 books-by-genre | 2014 | 1.2% | 1980 | -1.7% ~ | 4.3% | 2009 | -0.2% ~ | 1.4% | unchanged |
| s3 loans-by-daterange | 624 | 1.1% | 627 | +0.5% ~ | 0.7% | 628 | +0.6% ~ | 2.3% | unchanged |
| s3 overdue | 999 | 1.4% | 997 | -0.1% ~ | 2.2% | 998 | -0.0% ~ | 0.7% | unchanged |
| s3 join-loan-user | 9685 | 1.6% | 9075 | -6.3% ~ | 22.6% | 9775 | +0.9% ~ | 2.9% | unchanged |
| s3 exists-correlated | 6500 | 1.0% | 6414 | -1.3% ~ | 11.1% | 6525 | +0.4% ~ | 1.8% | unchanged |
| s3 count-by-user | 14144 | 2.1% | 14306 | +1.1% ~ | 3.4% | 14065 | -0.6% ~ | 2.3% | unchanged |
| s4 board [off] | 1935 | 1.8% | 2000 | +3.4% ~ | 5.4% | 2308 | +19.3% | 1.6% | improved |
| s4 tape10k [off] | 2197 | 1.1% | 2314 | +5.3% | 5.0% | 2699 | +22.8% | 2.5% | improved |
| s4 pk [off] | 14209 | 13.5% | 14563 | +2.5% ~ | 5.2% | 14215 | +0.0% ~ | 15.1% | unchanged |
| s4 open (insert) [off] | 781 | 16.0% | 795 | +1.8% ~ | 13.5% | 778 | -0.4% ~ | 42.3% | unchanged (see note) |
| s4 board [on] | 3237 | 4.4% | 3305 | +2.1% ~ | 2.9% | 3638 | +12.4% | 1.4% | improved |
| s4 tape10k [on] | 4774 | 4.8% | 4937 | +3.4% ~ | 3.4% | 5347 | +12.0% | 2.5% | improved |
| s4 pk [on] | 13843 | 16.4% | 14278 | +3.1% ~ | 13.8% | 13719 | -0.9% ~ | 15.9% | unchanged |
| s4 open (insert) [on] | 824 | 14.1% | 759 | -7.9% ~ | 16.5% | 659 | -20.0% ~ | 58.1% | unchanged (see note) |
| s4 board [declared] | 3351 | 2.4% | 3479 | +3.8% | 3.0% | 3629 | +8.3% | 2.3% | improved |
| s4 tape10k [declared] | 4563 | 5.0% | 4748 | +4.0% ~ | 5.4% | 5008 | +9.8% | 1.7% | improved |
| s4 pk [declared] | 13476 | 12.7% | 14538 | +7.9% ~ | 14.9% | 14513 | +7.7% ~ | 5.8% | unchanged |
| s4 open (insert) [declared] | 769 | 11.8% | 795 | +3.4% ~ | 17.8% | 811 | +5.5% ~ | 38.5% | unchanged (see note) |

Per-rep values for the s1 write/read cells and every s4 cell are in
`archive/ca-close-v3.2.0-159-g954abc20/perrep.txt`; the table is `compare.md` there.

### Verdicts, c134 against base

| Scenario | Verdict | Evidence |
|---|---|---|
| s1 (backtest) | **improved** on the four walked reads (+15.3, +17.4, +20.5, +22.4%); **unchanged within noise** on `load-bars`, `result-insert` and the two tuple-key point reads | read cells are tight: base spreads 0.9-2.4%, c134 2.2-5.7%, and no c134 rep of a walked read falls under any base rep (e.g. `read-day-slice` base 1866-1884, c134 2223-2273) |
| s2 (freight, txn) | **unchanged within noise** | all four cells +2.1% against 7.2-8.2% spreads |
| s3 (library) | **unchanged within noise** | all 9 cells and the control within +-1.2%; spreads 0.7-5.7% on c134 |
| s4 (boards, off / on / declared) | **improved** on `board` and `tape10k` in every arm (+19.3 / +22.8 off, +12.4 / +12.0 on, +8.3 / +9.8 declared); **unchanged** on `pk` and `open` | c134 `board [off]` reps 2333 2311 2297 2305 against base 1901-1936, all four reps apart |

No cell regressed. Three notes on how far to read it:

- **s4 `open` is a noise-bound cell, not a result.** Its spreads are 12-58%. The 58% (c134 `[on]`)
  and 42% (`[off]`) come from one cell, rep 2 of c134, where all three arms' open rates fell
  together (495 / 447 / 545 against 700-880 elsewhere) while that cell's board and tape rates
  were normal: a device stall during the insert phase's fsyncs (inferred from the simultaneity; no
  device counter was read). It is a reason to read `open` as unchanged and not
  as a c134 regression; rerunning the pair would settle it and was not done.
- **c13's own contribution is the s1 reads.** c13 equals c134 on s1 (differences of 0.2 to 5.8
  points, inside the c13 spread of 5.9-11.5%) and sits at +2 to +5% on s4 `board`/`tape10k`,
  which crosses the noise rule only on `tape10k [off]` (+5.3%, spread 5.0%) and `board [declared]`
  (+3.8%, spread 3.0%). `backtest-replay` for c13 (+11.6% against an 11.5% spread) is on the
  line; c134's +17.4% is not.
- The s4 `pk` and s1 point-read cells have base spreads of 1-16% and c13's reach 30%; a
  1-2% movement there is invisible, so "unchanged" means "no change larger than the spread",
  not "identical".

## Sampled pass: c134, where the read CPU goes

`kds_sampler.so`, 500 Hz, LD_PRELOAD, one run each, categories by leaf function
(`ca-harness/parts.json`); the binary carries no frame pointers so inclusive figures above a leaf
are unreliable. s4: `--session-seconds 20 --overnight-seconds 6`, both off and on servers
sampled, windows include load and the open-insert phase. s3: sampler armed 30 s after the
driver starts (`ca-close-s3reads.sh`), so it holds the read phases only; the window is shorter
than R2's (1,069 samples against 1,855), so s3 shares are a different mixture of shapes and
compare only in presence/absence of a function. Raw: `archive/.../prof/`.

| Category (share of samples) | s3 reads (1,069) | s4 off (1,445) | s4 on (934) |
|---|---|---|---|
| row accept / scan kernel / decode / btree | 32.7% | 50.7% | 32.9% |
| socket / wire encode | 16.7% | 17.2% | 25.3% |
| idle (epoll/sleep/yield) | 8.5% | 10.1% | 11.8% |
| parse / fingerprint / compile | 10.2% | 4.5% | 8.0% |
| locks / latches | 6.1% | 2.7% | 2.5% |
| mvcc visibility (`LookupCommit`) | 0.7% | 0.1% | 0.1% |
| WAL / durability syscalls | 0.1% | 1.5% | 1.4% |
| advisory (waystone / access stats / optimizer) | 0.4% | 0.3% | 0.6% |
| cabin | 0.0% | 0.0% | 0.9% |
| per-row exec counters (`ExecStats::For`) | 0.0% | 0.0% | 0.0% |
| other | 24.7% | 13.0% | 16.6% |

| Top self functions (c134) | s4 off | % | s4 on | % |
|---|---|---|---|---|
| 1 | `ChainRunner::AcceptTupleAt` | 19.0 | `__libc_send` | 20.6 |
| 2 | `__libc_send` | 13.7 | `AcceptTupleAt` | 12.3 |
| 3 | `__memcmp_evex_movbe` | 10.7 | `epoll_wait` | 10.9 |
| 4 | `epoll_wait` | 9.3 | `ScanKernelRejects` | 5.1 |
| 5 | `ScanKernelRejects` | 8.2 | `__memcmp_evex_movbe` | 4.7 |
| 6 | `RunWalkStep::{lambda#1}` | 6.9 | `RunWalkStep::{lambda#1}` | 4.3 |
| 7 | `ApplyCompare` | 2.7 | `__GI___libc_read` | 2.7 |

In s3 reads the top is `__libc_send` 9.3, `epoll_wait` 7.3, `btree::SearchLeaf` 6.3,
`AcceptTupleAt` 5.5, `ScanKernelRejects` 3.9.

**Did `DecodeCell` / `CheckCellSpan` drop? In s4, yes; in s3 there is nothing to show.** In the
round-2 dump of c13 (`results-ca-r2-v3.2.0-157-g0ed684bb.md`, same s4 flags) `storage::DecodeCell`
was 6.6% and `storage::CheckCellSpan` 3.3% of s4-off self samples (4.1% and 1.6% of s4-on).
Neither appears in c134's top 30 for either s4 server (the 30th entries sit below 0.5%), and
the `.report.txt` files hold no line for them. In s3 reads neither function was in c13's top 30
either, so the s3 profile is not evidence on C4, consistent with s3's walks being index
probes and fixed-width compares and its throughput not moving.

Two things the sampler also shows. `LookupCommit` is 0.1-0.7%, so the visibility memo has taken
what it targeted and further visibility work is bounded under one point. And
`__memcmp_evex_movbe` rose to 10.7% of s4-off (5.7% in c13's dump): removing a 9.9% category
would scale the rest by about 1.1, not 1.9, so memcmp is now the compare of the inline cell
bytes (the filter on the varchar column), reached from `ApplyCompare` or inlined `InlineCellBytes`
callers. Without frame pointers that attribution is inference, not measurement.

## What the run teaches about the engine

- **The read path pays per row, and the milestone's gain is a per-row saving.** Both gains land
  on shapes that touch many rows per statement (s1 day slices and joins, s4 board and tape scans)
  and none on shapes that touch few (s3 index probes, tuple-key lookups, all writes). That is the
  profile of a per-row constant cut, and the A/B shows the cut is worth 15-23% on the shapes
  where the accept path is half of server CPU (s4-off: 50.7%).
- **The inline-cell `Status` was the larger cost than its share.** Sampled
  `DecodeCell`+`CheckCellSpan` self time was 9.9% of s4 off, but c134's s4-off gain over c13 is
  +17% (2699 against 2314 on `tape10k`) and +15% on `board`. A saving larger than the removed
  self share is the usual sign that the removed code also kept its callers from inlining or
  optimising; this run cannot separate the two (no frame pointers, no counter run), and CLA does
  not claim the mechanism.
- **Visibility and the exec counters are closed as targets.** `LookupCommit` under 1% and
  `ExecStats::For` absent as a frame leave the accept path, the socket send and the `memcmp` of
  cell bytes as the remaining read-side CPU: `__libc_send` is 13.7-20.6% in s4, a per-statement
  syscall cost that no engine-side read optimisation reaches (`known-gaps.md`'s cadence `pwrite`s
  are a different matter).
- **A write cell cannot see a read-side change, and none was expected to.** The writes are
  `group`-commit bound (the host-memory note: group durability is 82-85% of an update), so the
  s2 and s1 write cells are flat by construction. They show that CA did not slow the writes
  within a 7-12% spread; they cannot show a 3% cost. The milestone's overhead claim for writes is
  bounded by that spread.

## Reproduction

`archive/ca-close-v3.2.0-159-g954abc20/ca-close-run.sh <outdir>` (A/B), `ca-close-prof.sh` and
`ca-close-s3reads.sh <outdir>` (sampled), `python3 ca-harness/compare.py <runs> base c13 c134 --md`.
Driver documentation: `bench/docs/README.md`; harness: `archive/ca-harness/README.md`. The
`s1`-`s3` `*.serverlog` files (warn-level, otherwise empty) were deleted from the archive before
filing; the driver JSON, stdout, configs, prechecks, return codes, and the sampler dumps are kept.
