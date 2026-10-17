# CA-S0: what the advisory machinery costs, by scenario

Work order: `instructions/v3.2.0/workorder-ca-background-advisory-optimizer.md` (§1, §5a, CA-S0).
Harness and how to run it: `bench/v3.2.0/archive/ca-harness/README.md`. Raw files:
`bench/v3.2.0/archive/ca-s0-v3.2.0-152-ga9f0f5cd/`.

**Finding.** On this binary, switching off Waystone recording, Waystone replay and access
statistics together moves no scenario's headline outside the run's own noise floor except
scenario 1's non-pk reads, which get about 4-5% *slower* with the switches off (the direction
replay helping, not costing), and scenario 4's pk control, which gets 10-13% faster in the
`on`/`declared` arms but sits inside the 16% spread the same cell shows in the `off` arm. No
advisory cost larger than about 3% can be claimed for any scenario from this run. The sampled CPU
split agrees: functions that are Waystone or access-statistics code are 0.2-0.6% of server CPU
in every scenario (a lower bound, see Method), and the cabin write/serve path is 0.0-0.7%.

## Stamp

| Item | Value |
|---|---|
| Date and time | 2026-10-17, A: 11:24:30-11:34:59 UTC, B: 11:34:59-11:37:04 UTC |
| Worktree / branch | `bz-background-advisory-optimizer` / `worktree-bz-background-advisory-optimizer` |
| `git describe --tags` | `v3.2.0-152-ga9f0f5cd` (tree clean before the run; only new files under `bench/v3.2.0/`) |
| Engine code | identical to `e1333a77` (2026-10-17 00:40 UTC); `a9f0f5cd` (11:20:30 UTC) is an `instructions/` commit |
| Binary | copy of `build-release/kds_server` at `/home/cdkbs/bench-runs/bz/kds_server.base`, sha256 `1dac7afcbdfa8544001c805053fdce7458193cb9ea86c0cf5e267ffa90abc53e`; source binary mtime 2026-10-17 11:20:24, copy 11:20:30 (newer than `e1333a77`, so it is the engine at HEAD). Every server started from the copy |
| Build type | Release (`build-release`); no frame pointers |
| Device | `/dev/root`, ext4, data files under `/home/cdkbs/bench-runs/bz/data/<cell>` (fresh per server, deleted after). Not tmpfs |
| Host | AMD EPYC 9V74, 8 CPUs; loadavg 0.3-1.9 at cell starts, `pgrep cc1plus|cmake --build|ctest` empty in all 26 prechecks (`*/sN.precheck`). Not a gate: the cells were not pinned |
| Server config | `durability = group`, `log_level = warn`, `buffer_pool_frames = 65536`; `cores = 8` for s1-s3; **s4 `cores = 1`** per server, `decay_half_life = 5`, `cabin_optimizer_snapshot_interval_ms = 500` (three servers, off/on/declared, as the s4 docstring and the v3.1.0 runs). s4 uses one core because the optimizer's tick runs on core 0's reactor, which is what CA moves; at 8 cores the tick would hold one of eight |
| Arms | `base` = defaults. `adv-off` = `waystone_recording = false`, `waystone_replay = false`, `access_statistics = false` appended to every server |

## Method and every cut

- Order ABAB: base r1, adv-off r1, base r2, adv-off r2, base r3, adv-off r3. Each pass runs s1, s2, s3, then s4.
  The arm always ran second in a pair, so a position effect is not separated from an arm effect.
- **Cuts to fit the 20-minute budget.** s1-s3: 3 reps. **s4: 2 reps** (rep 3 omitted). The s4 numbers carry n=2, so
  a spread from two points is a weak noise floor. **No `none` index-mode cell for s3.**
  No row-count sweep (rule 9): s3 ran `--loans 10000` only (relations 10000 loans; s4's tapes of 200/1k/10k rows are
  the only sweep, and only `tape10k` is in the table). s1 at `--years 5 --symbols 8 --exchanges 2` (8 symbols x 1,260 sessions = 10,080 bars).
  Rule 3's wait decomposition does not apply: this is a switch-cost measurement, not a latency study.
- Sizes: s1 `--years 5 --symbols 8 --exchanges 2 --ops 300 --qps-ops 300 --compare-rounds 4 --conn-ops 100 --write-ops 1000
  --write-batches 1,100 --connections 1,4 --bars-clustered btree --verify --sync` (BTREE copy of the driver; identical to a fresh
  3-keyword `HEAP`->`BTREE` sed of today's `tools/scenario1_backtest.py`, so not regenerated).
  s2 `--organizations 300 --ships 30 --operations 300 --cargos 10000 --bookers 8 --bookings 9000 --verify 100 --seed 1 --sync`
  (prior run's args except cargos 4000->10000 and bookings 3000->9000, so the measured window is about 5 s). s3 `--loans 10000
  --index-mode all --server-indexes on --seed 1 --assert-index-reads`. s4 `--days 2 --blocks 4 --session-seconds 10 --overnight-seconds 6`.
- Throughput = the driver's ops/elapsed, or `1e6/mean_us` (derived, exact for a serial driver) where the driver prints elapsed 0.0 (s3).
  Cell = median of reps; spread = (max-min)/median over the arm's reps; `~` = |delta| inside the larger of the two arms' spread.
- Sampler (B): `tools/kds_sampler.c` as an LD_PRELOAD (ITIMER_PROF, 500 Hz), one `base` pass, signalled around each driver; s4 samples
  the `on` server only. The binary has no frame pointers, so a chain is the interrupted frame and rarely more. **Categories are therefore the
  leaf function's, and "advisory" is a lower bound**: advisory work that runs inside an unnamed or a generic callee (a hash lookup, a mutex, a
  `memcpy`) is booked there.
- Correctness: s1 `--verify` passed in every cell. s4 byte-identical-reply check PASS in both days of every cell. **s2's own `--verify`
  reports 40-52 failed I1 checks of 400 in every cell, base and adv-off alike** (e.g. `booked_cbm=3820000, SUM(freights.cbm)=3935000`);
  the v3.1.0-30 s2 logs show 13 and 28 of 400 for the same driver. Not investigated here; it is present at the same rate in both arms, so it
  does not bias the comparison, but the s2 TPS is a TPS over a workload whose I1 sample is not clean. The driver exits 0 regardless.
  s2 also reports 2-3 retryable `TXN_CONFLICT` per run (0.00 retries/booking) and ends early because the cargo pool is booked out.
- Server logs (`log_level = warn`): only s2 logged, in every s2 cell of both arms - about 30 `TXN_CONFLICT` UPDATE refusals (retried by the driver) and, in 4 of 6 s2 cells (1-3 each), `[btree] insert into the relation rooted at page 149 failed: btree descent for key 136 ... gave up after 5 attempts` (a refused insert, `TxnConflict`, BA-S14 / AT-S15 territory). s1, s3 and s4 logged nothing.

## Base vs adv-off, per scenario (qps, median of reps)

Full machine-generated table: `archive/ca-s0-v3.2.0-152-ga9f0f5cd/compare.md`. Arms' reps: 3 (s1-s3), 2 (s4).

### Scenario 1 (BTREE copy, 10,080 bars, 8 models)

| Headline | base | spread | adv-off | delta | adv-off spread |
|---|---|---|---|---|---|
| load-bars (insert) | 20,123 | 1.1% | 19,323 | -4.0% ~ | 28.5% |
| result-insert | 769 | 2.2% | 763 | -0.8% ~ | 3.9% |
| backtest-read (3-relation join, cold) | 1,613 | 0.5% | 1,542 | -4.4% ~ | 7.6% |
| backtest-replay (same join, warm) | 1,712 | 1.3% | 1,654 | -3.4% | 2.4% |
| read-day-slice | 1,859 | 4.4% | 1,758 | -5.4% ~ | 10.0% |
| read-bar-lookup (pk) | 12,150 | 3.5% | 10,791 | -11.2% ~ | 30.4% |
| read-join-point (pk) | 12,294 | 8.8% | 12,736 | +3.6% ~ | 28.7% |
| agg-day-slice | 2,109 | 2.1% | 1,998 | -5.2% ~ | 8.3% |

The pk shapes run 100 ops in about 8 ms, so one scheduler hiccup is a 30% spread; they say nothing. The five scans/joins
move together by -3.4% to -5.4% with the switches off, with `base` spreads of 0.5-4.4%. The consistent sign over five shapes is more
than five independent coin flips would give, and `backtest-replay` (-3.4% against spreads of 1.3% and 2.4%) is outside both spreads.
Read it as: replay, not recording or statistics, may be paying for itself on s1's walked shapes by a few percent, or the second position
in the pair costs that much. This run cannot separate the two.

### Scenario 2 (freight, 8 bookers, `group`, 8 cores)

| Headline | base | spread | adv-off | delta | adv-off spread |
|---|---|---|---|---|---|
| booking txn (ops/s, 8 bookers) | 2,349 | 5.9% | 2,426 | +3.3% ~ | 2.2% |
| commit (committed TPS) | 1,725 | 6.2% | 1,772 | +2.7% ~ | 2.3% |
| cargo-lookup | 2,349 | 5.9% | 2,426 | +3.2% ~ | 2.1% |
| charge-insert | 9,706 | 6.1% | 9,966 | +2.7% ~ | 2.4% |

All rows are one run's phase counters divided by one shared elapsed, so they move together by construction; this is one
number, not four. +2.7% against a 6.2% noise floor is not a finding. Refusals: 2-3 retryable conflicts per run in both arms.

### Scenario 3 (library, 10,000 loans, `--index-mode all`, 8 cores)

| Headline | base | spread | adv-off | delta | adv-off spread |
|---|---|---|---|---|---|
| pk-user (control) | 14,306 | 5.3% | 14,728 | +2.9% ~ | 1.9% |
| loans-by-user | 10,616 | 1.7% | 10,650 | +0.3% ~ | 1.5% |
| loans-by-book | 11,403 | 0.5% | 11,325 | -0.7% ~ | 3.5% |
| books-by-genre | 1,993 | 1.9% | 2,013 | +1.0% ~ | 1.1% |
| loans-by-daterange | 609 | 2.5% | 621 | +2.0% ~ | 1.2% |
| overdue | 991 | 2.3% | 995 | +0.3% ~ | 1.3% |
| join-loan-user | 9,606 | 21.4% | 10,070 | +4.8% ~ | 1.1% |
| exists-correlated | 6,345 | 9.3% | 6,545 | +3.1% ~ | 1.5% |
| count-by-user | 13,532 | 26.6% | 14,749 | +9.0% ~ | 1.3% |

Nothing outside the floor. `join-loan-user` and `count-by-user` show 21-27% spread in `base` and 1% in `adv-off`: two of three
base reps ran slow, which the ABAB order cannot explain by arm and which is why those two deltas are not read.

### Scenario 4 (cabin optimizer days, 1 core per server, n=2)

Arm names are the driver's (off = no cabins, on = optimizer, declared = static cabins); `base`/`adv-off` are the server switches
applied to all three.

| Headline | base | spread | adv-off | delta | adv-off spread |
|---|---|---|---|---|---|
| board probe, off arm | 1,968 | 0.9% | 1,947 | -1.1% | 0.5% |
| board probe, **on arm** | 3,232 | 0.1% | 3,278 | +1.4% | 1.2% |
| board probe, declared arm | 3,388 | 0.4% | 3,459 | +2.1% | 0.7% |
| tape10k probe, off arm | 2,234 | 2.0% | 2,210 | -1.1% ~ | 0.2% |
| tape10k probe, **on arm** | 4,699 | 0.3% | 4,842 | +3.0% | 0.3% |
| tape10k probe, declared arm | 4,566 | 0.1% | 4,710 | +3.2% | 0.0% |
| pk-ops control, off arm | 13,614 | 16.0% | 13,865 | +1.8% ~ | 0.8% |
| pk-ops control, **on arm** | 13,407 | 0.0% | 15,130 | +12.9% | 3.9% |
| pk-ops control, declared arm | 13,648 | 6.3% | 14,960 | +9.6% | 0.6% |
| open insert, off arm | 847 | 2.0% | 826 | -2.5% ~ | 7.4% |
| open insert, **on arm** | 796 | 1.0% | 791 | -0.6% ~ | 3.1% |
| open insert, declared arm | 822 | 0.3% | 847 | +3.0% ~ | 3.9% |

The optimizer's serving win is intact with the switches off: the `on` arm answers the board probe 1.64x the `off` arm in both
configurations (3,232/1,968 and 3,278/1,947). The pk-ops control gains 10-13% in the `on` and `declared` arms with the switches
off, and 1.8% in the `off` arm, where the same cell's base reps differ by 16%. The control is 240 ops in about 17 ms per day, so
this is the weakest-measured row in the table; with n=2 it is not a result. Cabin-bearing tapes and boards gain a consistent +1 to
+3% in all three arms' probe rows with n=2, 0.0-1.2% spreads, which is the size of a position effect.

## Where the server CPU goes (one sampled `base` pass per scenario)

Leaf-function categories, share of CPU-time samples (`prof-base-r1/*.parts.json`; samples: s1 1,881, s2 6,038, s3 1,083, s4-on 976).
"idle" is time in `epoll_wait` and the poll path that the process was charged for.

| Category | s1 | s2 | s3 | s4 (on server) |
|---|---|---|---|---|
| row accept / scan kernel / decode / btree | 28.1% | 36.7% | 7.8% | 39.8% |
| socket / wire encode | 18.1% | 10.4% | 13.4% | 23.0% |
| idle (epoll / yield) | 13.8% | 9.0% | 27.8% | 8.7% |
| locks / latches (mutex, `LockTable`) | 9.5% | 7.4% | 1.8% | 2.2% |
| mvcc visibility (`InstanceVisibility::LookupCommit`) | 6.9% | 5.7% | 0.6% | 0.3% |
| parse / fingerprint / compile | 4.6% | 6.7% | 4.4% | 7.2% |
| WAL / durability (`fdatasync`, `pwrite`, `wal::`) | 2.0% | 4.7% | 24.7% | 1.6% |
| per-row exec counters (`ExecStats::For`) | 2.1% | 2.0% | 0.1% | 2.0% |
| **advisory: Waystone / access stats / optimizer signals** | **0.4%** | **0.6%** | **0.2%** | **0.6%** |
| cabin (witness / key / serve / build) | 0.3% | 0.0% | 0.0% | 0.7% |
| other (unattributed leaf) | 14.2% | 16.8% | 19.4% | 14.0% |

The advisory row is a lower bound (Method). Its members, by name, in s1: `TrailReplay::Find`, `TryReplay`, `ReadWaystoneEntry`,
`RecordOptimizerSignals`; in s2: `Catalog::RecordAccess`, `StatementKindOf`, `LookupWaystonePage`, `FormatWaystonePage`,
`TrailRecorder::OnPatternResult`, `FindPattern`; in s4: `StatementStats::Record`, `OptimizerSignals::*`, `GetSysPatternRow`.
The 14-19% "other" holds leaves with no category (allocator, `std::function`, heap sorts, chains ending in unmapped frames, 14.5-28.5% of inclusive samples).

## What the run says about the engine

- **The advisory write-side machinery is not where these four scenarios spend CPU.** Waystone, access statistics and the
  optimizer's signal collection together are under 1% of sampled leaf CPU, and turning their three boot switches off moves no
  throughput outside the noise floor of s2, s3 or s4's insert/probe rows. A CA design justified by "the advisory path is a measurable
  tax on these scenarios" has no support in this data; one justified by "it holds a reactor / a latch under contention" is not tested by it.
- **What the sampler does not see.** A cost that is a *wait* (a latch held across the optimizer's tick, a 50 ms core-0 task) is
  not CPU and does not appear. s4 ran at one core, where the tick would show as probe latency; the `on`-arm probe qps with all three
  switches off is the same as with them on, but the tick itself is not switched by these keys. This run does not price it.
- **`ExecStats::For` is a per-row counter (2% in s1, s2, s4) that the three switches do not remove.** It feeds ANALYZE and the
  statement's counters, not only access statistics. It is the largest CPU item that an "advisory" label might wrongly claim; whether the
  access statistics fold needs it is a CA question that this run only sizes at 2%.
- **Scenario 1's reads may profit from replay (a few percent).** The sign is consistent over five shapes; the position effect is not
  excluded. If CA moves replay off the statement path, s1's `backtest-replay` and `read-day-slice` are the rows to watch.
- **s3 is a durability and wire scenario at this size.** 25% of its CPU is `fdatasync`/`pwrite` and 28% idle; the engine's own work
  (accept/decode) is 8%. It can only show a regression in the loan *load* path or in anything that adds a round trip, not in the walk.
- **s2's I1 verification fails in about 10% of its sampled checks in every cell** (and did in v3.1.0-30's s2 logs: 13 and 28 of 400).
  Before the s2 TPS is used as a go/no-go for a change that touches the write path, the failing check should be understood (driver read skew
  versus a real invariant slip); the driver exits 0 on it.
- **Noise floor of this harness** (same arm, same config, interleaved reps): s1 scans 0.5-4.4%, s1 pk shapes 3-9% (tiny phases),
  s2 6%, s3 0.5-5% with two outliers at 21-27%, s4 probes 0.1-2% and pk 0-16% (n=2). A candidate must move a headline by more than that
  to count; with 3 reps the median delta below about 5% is not a finding in s1/s2/s3.

## Reproduction

    bench/v3.2.0/archive/ca-s0-v3.2.0-152-ga9f0f5cd/ca-s0-run.sh <OUTDIR>   # A (ABAB, 3 reps; s4 2 reps) then B (sampled)
    python3 bench/v3.2.0/archive/ca-harness/compare.py <OUTDIR> base adv-off

The total run was 12.6 minutes (A 10.5, B 2.1), 18 minutes from the start of setup.
