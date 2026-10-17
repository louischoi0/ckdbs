# CA round 2: c1 and c13 against base, and where a read statement spends its CPU

Exploratory round, not a milestone close. Two findings. (1) The c1 dip on s1 `load-bars`
seen in round 1 does not repeat (-0.3%, inside noise), and the read-side gains of c1/c13
(+15-28% on the s1 walked-read shapes) repeat and are tight. (2) In the sampled read phases
of s3 and s4 the cost is the row-accept path (`AcceptTupleAt`, `ScanKernelRejects`, the walk
lambda: 26% of s3 read samples) and the socket send, not the MVCC visibility lookup
(`LookupCommit`: 2.1%) that c1 targets; `ExecStats::For` no longer appears as a frame at
all in c13.

## Stamp

| Item | Value |
|---|---|
| Executed | 2026-10-17, Part A 12:11-12:29 UTC (17.7 min), Part B 12:29-12:32 UTC |
| Worktree / branch | `bz-background-advisory-optimizer` / `worktree-bz-background-advisory-optimizer` |
| `git describe --tags` | `v3.2.0-157-g0ed684bb` (HEAD; C2 reverted at `123eb86a`) |
| Tree | `src/`, `include/`, `tools/`, `CMakeLists.txt` clean; untracked `bench/v3.2.0/...` only |
| Binaries (copies, run from `/home/cdkbs/bench-runs/bz/`) | see next table |
| Build type | release (`build-release`); source binary mtime `build-release/kds_server` 2026-10-08 05:38 is older than HEAD and was not used |
| Device | `/dev/root`, ext4 (`df -T`); data files under `/home/cdkbs/bench-runs/bz/data`, fresh per cell |
| Host | quiet precheck: no `cc1plus`/`cmake --build`/`ctest`; loadavg 0.9-2.5 at cell starts (`sN.precheck` in each cell) |
| Server config | s1-s3: `cores = 8`; s4: `cores = 1` x3 servers, `decay_half_life = 5`, `cabin_optimizer_snapshot_interval_ms = 500`; all `durability = group`, `log_level = warn`, `buffer_pool_frames = 65536` |

| Arm | Commit | sha256 (prefix) | binary mtime |
|---|---|---|---|
| base | engine `e1333a77` | `1dac7afc` | 2026-10-17 11:20 |
| c1 | `4e11968a` VisibilityMemo | `6b23fa97` | 2026-10-17 11:45 |
| c13 | `0ed684bb` = c1 + `ExecStats::For` forced inline | `8703afe1` | 2026-10-17 12:10 |

c13's binary mtime (12:10:26) equals its commit's timestamp to the second, so it was built
from or alongside the committed state; `src/` is clean now, and the binary's content was not
rebuilt or verified by this round. Candidate C2 is not in any arm.

## Part A: base / c1 / c13

Drivers and reduced sizes as rounds S0/R1 (`bench/v3.2.0/archive/ca-harness/README.md`;
sizes in `ca-r2-run.sh`). Rotating order rep1 base c1 c13, rep2 c1 c13 base, rep3 c13 base c1;
3 reps for s1-s3, **s4 in reps 1-2 only (n=2)** to stay in the 20-minute budget. All 33
cell drivers exited 0 (s1 ran `--verify`). Cells are medians of reps in QPS; `~` = |delta| <=
the larger spread of the two arms (not a finding). Sizes: s1 5y x 8 sym x 2 exch bars, s2 10k
cargos / 9k bookings, s3 10k loans (2k users, 2k books), s4 31,200-row boards and 200/1k/10k
tapes; the sweep over 200/1K/10K is the S0/R1 reduced set, not repeated here.
Raw: `archive/ca-r2-v3.2.0-157-g0ed684bb/runs/`, table in `compare.md` there.

| headline (QPS) | n | base | spread | c1 | delta | spread | c13 | delta | spread |
|---|---|---|---|---|---|---|---|---|---|
| s1 load-bars (insert) | 3 | 19761 | 11.0% | 19699 | -0.3% ~ | 5.3% | 20105 | +1.7% ~ | 5.1% |
| s1 result-insert | 3 | 688 | 7.9% | 532 | -22.7% ~ | 31.0% | 665 | -3.3% ~ | 12.8% |
| s1 backtest-read (3-rel join, cold) | 3 | 1614 | 2.2% | 1784 | +10.6% ~ | 22.2% | 1856 | +15.1% | 3.4% |
| s1 backtest-replay (warm) | 3 | 1728 | 5.9% | 1982 | +14.7% | 5.3% | 2072 | +20.0% | 4.2% |
| s1 read-day-slice | 3 | 1873 | 1.3% | 2153 | +14.9% | 1.4% | 2234 | +19.3% | 8.3% |
| s1 read-bar-lookup (tuple key) | 3 | 12457 | 22.7% | 11110 | -10.8% ~ | 17.5% | 12188 | -2.2% ~ | 1.6% |
| s1 read-join-point (tuple key) | 3 | 12393 | 3.5% | 12211 | -1.5% ~ | 13.6% | 12287 | -0.9% ~ | 1.2% |
| s1 agg-day-slice | 3 | 2067 | 17.3% | 2563 | +24.0% | 1.1% | 2634 | +27.5% | 1.4% |
| s2 booking (txn, 8 bookers) | 3 | 2068 | 5.0% | 2058 | -0.5% ~ | 1.0% | 2089 | +1.0% ~ | 2.4% |
| s2 commit (committed TPS) | 3 | 1510 | 4.6% | 1502 | -0.6% ~ | 0.9% | 1527 | +1.1% ~ | 1.7% |
| s2 cargo-lookup | 3 | 2068 | 5.1% | 2058 | -0.5% ~ | 1.0% | 2090 | +1.0% ~ | 2.4% |
| s2 charge-insert | 3 | 8498 | 4.7% | 8445 | -0.6% ~ | 0.8% | 8589 | +1.1% ~ | 1.7% |
| s3 pk-user (control) | 3 | 14409 | 30.8% | 14368 | -0.3% ~ | 2.7% | 14327 | -0.6% ~ | 2.7% |
| s3 loans-by-user | 3 | 10225 | 21.2% | 10672 | +4.4% ~ | 1.2% | 10571 | +3.4% ~ | 0.7% |
| s3 loans-by-book | 3 | 11351 | 29.0% | 11429 | +0.7% ~ | 1.4% | 11325 | -0.2% ~ | 1.9% |
| s3 books-by-genre | 3 | 1976 | 5.5% | 1981 | +0.3% ~ | 3.9% | 2004 | +1.4% ~ | 0.7% |
| s3 loans-by-daterange | 3 | 620 | 4.4% | 631 | +1.7% ~ | 2.5% | 627 | +1.0% ~ | 0.5% |
| s3 overdue | 3 | 983 | 0.7% | 1004 | +2.1% ~ | 2.2% | 994 | +1.1% ~ | 8.8% |
| s3 join-loan-user | 3 | 8453 | 22.4% | 9843 | +16.4% ~ | 4.3% | 9579 | +13.3% ~ | 21.6% |
| s3 exists-correlated | 3 | 6414 | 18.2% | 6575 | +2.5% ~ | 1.1% | 6361 | -0.8% ~ | 19.9% |
| s3 count-by-user | 3 | 13158 | 30.6% | 14514 | +10.3% ~ | 1.9% | 13004 | -1.2% ~ | 31.9% |
| s4 board [off] | 2 | 1933 | 0.6% | 1982 | +2.5% | 1.7% | 2021 | +4.5% | 0.5% |
| s4 tape10k [off] | 2 | 2200 | 2.1% | 2248 | +2.2% | 1.4% | 2299 | +4.5% | 0.3% |
| s4 pk [off] | 2 | 13063 | 12.0% | 14162 | +8.4% ~ | 0.0% | 13176 | +0.9% ~ | 4.3% |
| s4 open insert [off] | 2 | 644 | 6.5% | 661 | +2.6% ~ | 1.0% | 679 | +5.6% ~ | 7.1% |
| s4 board [on] | 2 | 3209 | 0.8% | 3204 | -0.2% ~ | 7.7% | 3298 | +2.8% ~ | 4.4% |
| s4 tape10k [on] | 2 | 4872 | 0.7% | 4631 | -4.9% ~ | 15.1% | 4898 | +0.5% ~ | 0.8% |
| s4 pk [on] | 2 | 14994 | 0.3% | 13328 | -11.1% ~ | 25.6% | 14014 | -6.5% | 5.6% |
| s4 open insert [on] | 2 | 667 | 4.2% | 700 | +4.9% | 0.9% | 654 | -2.0% ~ | 2.9% |
| s4 board [declared] | 2 | 3388 | 1.3% | 3409 | +0.6% ~ | 0.6% | 3453 | +1.9% ~ | 2.6% |
| s4 tape10k [declared] | 2 | 4603 | 1.7% | 4691 | +1.9% ~ | 2.2% | 4749 | +3.2% | 1.4% |
| s4 pk [declared] | 2 | 13855 | 6.1% | 14570 | +5.2% ~ | 6.5% | 14059 | +1.5% ~ | 13.9% |
| s4 open insert [declared] | 2 | 661 | 4.5% | 657 | -0.7% ~ | 13.6% | 650 | -1.7% ~ | 8.7% |

### What Part A says

- **Does c1's s1 load-bars dip repeat? No.** Per-rep QPS (reps 1-3): base 19761 / 18058 / 20222;
  c1 19337 / 19699 / 20385; c13 20105 / 19270 / 20292. c1 is -0.3%, c13 +1.7%, both inside the
  base's own 11% spread (base rep 2 is the low cell). Round 1's -9.1% was measured against a
  base spread of 1.7%; with this round's wider base spread it cannot be told from noise. Treat
  it as not reproduced, not as disproved.
- **s1 walked reads repeat as a gain, and c13 is at least c1.** `backtest-replay`,
  `read-day-slice` and `agg-day-slice` are outside every spread for both candidates: c1
  +14.7 / +14.9 / +24.0%, c13 +20.0 / +19.3 / +27.5%. c13's extra over c1 (+5, +4, +3 points)
  is within one spread of c1's cells, so the inline force is not separately established on
  s1. `backtest-read` (cold) is +15.1% outside noise for c13; c1's +10.6% is inside its
  22% spread.
- **s2 (write, group commit) and the s3 point/index shapes are flat**: every delta <= 4.4%
  and inside noise. The s3 control (`pk-user`) is -0.3% / -0.6%. Several s3 cells carry
  spreads of 20-31% on base (one slow base rep), so s3's `join-loan-user` +16.4% / +13.3% is
  marked noise and is not a finding.
- **s4 (n=2, single core):** the off arm's `board` and `tape10k` are +2.2-4.5% with spreads
  under 2%, outside the noise rule but small and n=2; the on and declared arms are inside
  noise except `pk [on]` c13 -6.5% (spread 5.6%, n=2, a 14994 base cell with spread 0.3%): one
  cell, not repeated in the declared or off arms, so not read as a regression.
- Nothing here approached a limit: s1-s3 run 8 cores under `group` durability with a
  single-digit-connection driver, so these are fixed-cost per-statement comparisons, not
  saturation or `strict` cells (rule 4b not met this round; stated).

## Part B: where a read statement spends CPU (c13, `0ed684bb`)

`kds_sampler.so` LD_PRELOAD, 500 Hz, categories by leaf function (`parts.json`); the binary
has no frame pointers, so "inclusive" above a leaf is unreliable and 12-21% of inclusive
samples are `[unmapped]`. Samples are the server's own; the sampler and the driver are
bracketed by SIGUSR1/SIGUSR2.

- **s3**: `--ops 3000`, otherwise as S0. A first attempt armed the sampler for the whole
  driver and was dominated by the 29 s load (WAL/fdatasync 10.5%, read category 27%);
  it is archived as `prof-c13-s3-r1/` and not used. The reported run
  (`prof-c13-s3reads-r1/`) arms the sampler 30 s after the driver starts (load and
  create-index end at ~29.3 s), so the dump holds the read phases (pk-user's first
  ~0.7 s may be missing). The 1,855 samples are weighted by time per shape, i.e. by the
  read phases' wall: `loans-by-daterange` 4.8 s, `overdue` 3.0 s, `books-by-genre` 1.5 s,
  `join-no-literal` 0.9 s, everything else <= 0.5 s. This is a time-weighted picture of
  the heavy shapes, not a per-statement picture of the cheap ones.
- **s4**: `--session-seconds 20 --overnight-seconds 6 --days 2 --blocks 4`; the off and on
  servers were both sampled (`quad-b.sh`, a copy of the harness that preloads the off
  server too). The window also contains each arm's load and open-insert phases (31,200 rows,
  240 inserts per day), so the s4 splits include a write share. Samples: off 1,522, on 982.

### Category split (share of samples)

| Category | s3 reads (1,855) | s4 off (1,522) | s4 on (982) |
|---|---|---|---|
| row accept / scan kernel / decode / btree | 45.4% | 58.5% | 40.1% |
| socket / wire encode | 15.4% | 13.7% | 22.7% |
| idle (epoll/sleep/yield) | 5.9% | 8.7% | 10.9% |
| parse / fingerprint / compile | 7.7% | 4.6% | 5.4% |
| locks / latches | 4.4% | 3.1% | 2.3% |
| mvcc visibility (`LookupCommit`) | 2.1% | 0.2% | 0.3% |
| WAL / durability syscalls | 0.1% | 1.5% | 1.6% |
| advisory (waystone / access stats / optimizer) | 0.2% | 0.5% | 0.3% |
| cabin | 0.0% | 0.0% | 0.7% |
| per-row exec counters (`ExecStats::For`) | 0.0% | 0.0% | 0.0% |
| other | 18.8% | 9.3% | 15.6% |

The `ExecStats::For` row is 0.0% because the symbol no longer exists as a frame in c13
(forced inline); the category cannot say what its inlined cost is, only that it is not a
separate call. Its cost now sits inside whichever scan function inlined it.

### Top self functions (top 15 of 30; full lists in the `.report.txt` files)

| # | s3 reads | % | s4 off | % | s4 on | % |
|---|---|---|---|---|---|---|
| 1 | `ChainRunner::AcceptTupleAt` | 10.6 | `AcceptTupleAt` | 19.7 | `__libc_send` | 19.3 |
| 2 | `ScanKernelRejects` | 9.8 | `__libc_send` | 11.2 | `AcceptTupleAt` | 12.7 |
| 3 | `__libc_send` | 6.9 | `RunWalkStep::{lambda#1}` | 9.4 | `epoll_wait` | 10.3 |
| 4 | `RunWalkStep::{lambda#1}` | 5.8 | `ScanKernelRejects` | 8.8 | `RunWalkStep::{lambda#1}` | 5.4 |
| 5 | `epoll_wait` | 5.4 | `epoll_wait` | 8.0 | `ScanKernelRejects` | 5.4 |
| 6 | `btree::SearchLeaf` | 4.3 | `storage::DecodeCell` | 6.6 | `__memcmp_evex_movbe` | 4.6 |
| 7 | `wire::EncodeValue` | 3.2 | `__memcmp_evex_movbe` | 5.7 | `storage::DecodeCell` | 4.1 |
| 8 | `PageView::PayloadAt` | 2.1 | `storage::CheckCellSpan` | 3.3 | `__GI___libc_read` | 2.6 |
| 9 | `InstanceVisibility::LookupCommit` | 2.1 | `ApplyCompare` | 2.2 | `CheckCellSpan` | 1.6 |
| 10 | `DecodeColumnsInto` | 1.8 | `LockTable::AcquireInner` | 1.5 | `ApplyCompare` | 1.5 |
| 11 | `WireResultSink::EncodeProjectedRow` | 1.6 | `__GI___libc_read` | 1.4 | `LockTable::AcquireInner` | 1.2 |
| 12 | `___pthread_mutex_trylock` | 1.6 | `__GI_tolower` | 0.8 | `Lexer::ScanToken` | 0.9 |
| 13 | `DevicePageStore::PinResidentAndRelease` | 1.5 | `_Hashtable::find` | 0.7 | `_Hashtable::find` | 0.9 |
| 14 | `std::_Function_handler::_M_invoke` | 1.5 | `LockTable::PartitionOf` | 0.7 | `KwpSession::OnBind` | 0.8 |
| 15 | `DecodeOneValueInto` | 1.4 | `FingerprintAccumulator::Feed` | 0.5 | `FingerprintAccumulator::Feed` | 0.8 |

Ranks 16-30 for s3 reads: `__memcmp_evex_movbe` 1.3, `_Hashtable::find` 1.2, `KeystoneIdOfPayload`
1.2, `__GI___libc_read` 1.0, `btree::DescendTo` 1.0, `__memcpy_avx512_unaligned_erms` 1.0,
`lll_mutex_unlock_optimized` 0.9, `DevicePageStore::UnpinFrame` 0.9, `_int_malloc` 0.8,
`__GI___libc_malloc` 0.7, `_Hashtable::_M_insert_unique` 0.7, `index::IndexVisitFrom` 0.7,
`InternalView::ChildFor` 0.6, `__GI___libc_free` 0.6, `DecodeAndResolve` 0.6.

### Top inclusive (s3 reads; same caveat on unmapped frames)

`[unmapped]` 20.7, `AcceptTupleAt` 10.7, `ScanKernelRejects` 9.8, `RunWalkStep [actor]` 9.1,
`__libc_send` 6.9, `TcpServer::FlushOutbox` 6.7, walk `lambda#1` 5.8, `epoll_wait` 5.4,
`EpollIoBackend::PollReady` 5.3, `btree::SearchLeaf` 4.3, `index::IndexVisitFrom` 3.8,
`wire::EncodeValue` 3.2, `operator new` 3.0, `InstanceVisibility::LookupCommit` 2.7,
`malloc` 2.2, `WireResultSink::EncodeProjectedRow` 2.2, `PageView::PayloadAt` 2.1,
`DecodeColumnsInto` 1.8, `PinResidentAndRelease` 1.7, `_Function_handler::_M_invoke` 1.7,
`UnpinFrame` 1.6, `___pthread_mutex_trylock` 1.6, `CoroTask::Poll` 1.4,
`_Hashtable::_M_insert_unique` 1.4, `SystemClock::Now` 1.4, `DecodeOneValueInto` 1.4,
`memcmp` 1.3, `_Hashtable::find` 1.2, `EmitRow` 1.2. s4 inclusive lists are in
`prof/prof-c13-s4-r1/s4-{off,on}.report.txt`.

### What Part B says

- **The read statement is the accept path.** `AcceptTupleAt` + `ScanKernelRejects` + the walk
  lambda are 26.2% of s3-read samples, 37.9% of s4-off and 23.5% of s4-on. Nothing in the
  engine's advisory or visibility layers is within a factor of ten of them. This matches the
  S0 picture (leaf-function split) and supports the same direction: the remaining headroom on
  a read is per-row work in the scan kernel, not background machinery.
- **MVCC visibility is no longer the hotspot c1 targeted.** `LookupCommit` is 2.1% self /
  2.7% inclusive in s3 reads and 0.2-0.3% in s4. A further visibility optimisation has an
  upper bound of about the 2% it holds in s3.
- **Allocation and hash work appear per statement**: `operator new` 3.0% inclusive,
  malloc/free about 2%, `_Hashtable::find` + `_M_insert_unique` about 2% in s3 reads.
  These are small each; the sampler cannot say whether the hash inserts are per row or per
  statement (no frame pointers). Naming a caller needs a build with frame pointers; not done
  here (no build allowed).
- **`___pthread_mutex_trylock` + `lll_mutex_unlock_optimized` = 2.5% in s3 reads** and the
  locks category is 4.4% (2.3-3.1% in s4): a latch taken on the read path. Which latch is
  not recoverable from leaf attribution; `LockTable::AcquireInner/PartitionOf` carry 2.2% in
  s4-off (reads take the relation `IS`).
- **s4 off versus on**: the same `AcceptTupleAt`-family reads are 37.9% (off, board scans a
  10k-row board) versus 23.5% (on, served from the Cabin); on, the socket send is 19.3%
  self, the single largest item, because the optimiser's reads cost so little that the
  reply's `send` dominates. The on arm's `DecodeCell`/`memcmp`/`CheckCellSpan` (varchar
  compare, 15.7% off) shrinks to 10.3%: the Cabin-served arm does less of it per statement (the sampler shows the difference, not the mechanism).
- The 1 core of s4 spends 8.7-10.9% idle inside the window (driver think time), so s4's
  split is of busy CPU plus this idle share.

## Limits of this round

- s4 has n=2 in Part A; its spreads are two-point ranges.
- Part B is one sampled run per scenario; no repeat, no noise floor for the percentages.
  Differences of one or two points between the three columns are not findings.
- The s3 dump begins ~30 s after driver start by timer, not by phase marker.
- Frames above the leaf are not symbolised (no frame pointers); categories are leaf-based.
- Rule 9 (200/1K/10K sweep) is not re-run; the reduced S0 sizes are the one cardinality per
  scenario. The baseline here is the in-round `base` arm; round 1's archive
  (`archive/ca-r1-v3.2.0-154-g7112d4a1/`) was used only to check that the load-bars dip does
  not repeat. PostgreSQL floor not measured for these shapes.
- Tests: no code change was made by this round; the correctness suite was not run by it
  (s1 ran with `--verify`, s2 with `--verify 100`).

Files: `archive/ca-r2-v3.2.0-157-g0ed684bb/` (`runs/`, `prof/`, `compare.md`, the four
`ca-r2-*.sh` scripts and `quad-b.sh`).
