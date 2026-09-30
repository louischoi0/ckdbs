# Scenario rebaseline at `v2.7.0-531-g9a0525d`: summary

The operator deleted every earlier result under `bench/v3.0.0/` and ordered
the five scenario drivers re-run against the engine at `9a0525d` ("AY-S11:
AY closed", `v2.7.0-531-g9a0525d`). This file is the overview; each
scenario has its own results file beside it and its raw output under
`bench/v3.0.0/archive/scenario<N>-v2.7.0-531-g9a0525d/`. **Four of five
drivers ran. Scenario 1 is refused by the engine and produced no number.**

## Stamp

| Field | Value |
|---|---|
| Date | 2026-09-30, measurements 06:41 to 07:12 UTC |
| Worktree / branch | `bench-rerun-scenarios` / `worktree-bench-rerun-scenarios` |
| HEAD | `fc2d343`; over `9a0525d` it only deletes `bench/v3.0.0/` and edits `bench/README.md`. `src/`, `include/`, `tools/` and `CMakeLists.txt` are identical to `9a0525d` |
| Engine measured | `9a0525d`, `v2.7.0-531-g9a0525d` |
| Binary | copy of `build-release/kds_server` (Release, built after both commits, mtime 2026-09-30 06:30:43 UTC), **sha256 `1d07d33386541ec3a13090d37918e229957e29c2c777ff15f2a63db6957f8746`**, the same hash AY-S11's arm B recorded for `0552d55`; every server started from the copy |
| Device | `/dev/root`, `ext4` (`df -T`); data files under `/home/cdkbs/bench-runs/rebaseline-9a0525d/data/`, never tmpfs |
| Host | 8 logical CPUs, AMD EPYC 9V74 (4 cores x 2 threads), Linux 7.0.0-1014-azure; **no `cc1plus`, `cmake --build` or `ctest` process at any cell**, per-cell loadavg in each file |
| Ports | 15600 to 15653, never 15432 |
| Config | `log_level = warn`; `cores` and `durability` per cell; scenario4 also `decay_half_life = 5`, `cabin_optimizer_snapshot_interval_ms = 500` (and `cabin_optimizer_cooldown_half_lives = 2` in its second run) |
| PostgreSQL | not installed on this host; **no PostgreSQL floor was measured for any scenario** |
| Drivers | unmodified `tools/` scripts; every relation BTREE |

## What ran and what did not

| Scenario | Cells | Repeats | Status |
|---|---|---|---|
| 0 stock market | `cores` {1, 8} x `durability` {group, strict} | 3 each (12 cells) | ran; the `c8-s` cell exits 1 in all three runs (one refused insert, below) |
| 1 backtest | `cores` 1 group, 8 group, 1 strict | 1 each | **not executed**: refused at `CREATE TABLE daily_stats ... HEAP` (SUS-1); no number |
| 2 freight | `cores` {1, 8} x `durability` {group, strict}, plus two `--isolation repeatable-read` probes | 3 each (12 cells) + 2 | ran |
| 3 library | `cores` {1, 8} x loans {200, 1,000, 10,000} x index {none, all} | 3 each (36 cells) | ran; all 36 exit 0, verify clean; `--cabin` and the other index modes not executed |
| 4 cabin optimizer, business days | 3 arms (off, on, declared), 3 simulated days, `cores = 1`, `group` | 2 runs (defaults; cooldown 2 half-lives) | ran |

Every scenario0 and scenario2 delta below is **against the f6ed10c BTREE
run** (`bench/v3.0.0/results-scenario0-stockmarket-btree-v2.7.0-157-gf6ed10c.md`
and `bench/v3.0.0/results-scenario2-freight-btree-v2.7.0-157-gf6ed10c.md`,
section 3 of each, both read with `git show 9a0525d:<path>`). The two runs
are 374 commits and many milestones apart (`v2.7.0-157-gf6ed10c` to
`v2.7.0-531-g9a0525d`), so **no delta is attributable to one change**, and
the `cores = 8` cells changed configuration (f6ed10c ran `peer_listeners =
on` and `placement = namespace`, both retired), so a delta at eight cores
compares two architectures. Scenarios 1, 3 and 4 have no v3 predecessor:
no delta.

## Scenario 0: stock market ([file](results-scenario0-stockmarket-v2.7.0-531-g9a0525d.md))

Median of three runs; `txn` is the four-statement business transaction,
run 1.

| Cell | cores | durability | tps (median of 3) | spread | txn p50 | f6ed10c | delta |
|---|---|---|---|---|---|---|---|
| `s0-c1-g` | 1 | group | 626.0 tps | 3.8 % | 11,769.4 us | 750.6 tps | -16.6 % |
| `s0-c8-g` | 8 | group | 661.0 tps | 6.8 % | 10,338.3 us | 794.3 tps | -16.8 % (single-draw comparator, not a finding) |
| `s0-c1-s` | 1 | strict | 169.4 tps | 6.1 % | 41,228.3 us | 212.1 tps | -20.1 % |
| `s0-c8-s` | 8 | strict | 331.7 tps | 20.0 % | 20,250.3 us | 213.0 tps | +55.7 % (different configuration) |

**Insights.** Durability is still the axis at one core (group is 3.70x
strict). At eight cores `strict` now doubles (169.4 to 331.7 tps) where
f6ed10c had it flat, consistent with the durability wait overlapping
across cores (this run has no server-side breakdown to show it). The
one-core path is 17 to 20 % slower than at f6ed10c, outside this run's
floor. **Anomaly**: `s0-c8-s` refuses one trade insert in each of its three
runs (`ERR TXN_CONFLICT retryable=1 btree descent for key 113 from page 138
gave up after 5 attempts`, the same key and page every time), never in
scenario0's nine other runs; the driver does not retry, so the cell has one
torn trade and exits 1. The same refusal also reached scenario2's `s2-c8-g`
three times in two of its three runs, under `group`, where that driver
retried it. The bounded re-descent and its retryable refusal are documented
(`docs/spec/heap-and-tuple.md` §5, AT-S5c); `docs/inflight/known-gaps.md`
has no entry recording that this workload reaches it.

## Scenario 1: backtest ([file](results-scenario1-backtest-v2.7.0-531-g9a0525d.md))

Not executed. Three cells each stopped at the fifth `CREATE TABLE` with
`ERR HEAP storage is suspended (SUS-1) and no new heap relation is created
(byte 219)`: `tools/scenario1_backtest.py` creates `daily_stats`,
`model_results` and the write sweep's `write_probe` as `HEAP`, and no flag
changes that (`--bars-clustered` moves only `daily_bars`). The driver was
not patched. There is no number for this scenario until its schema is
BTREE-only.

## Scenario 2: freight ([file](results-scenario2-freight-v2.7.0-531-g9a0525d.md))

| Cell | cores | durability | tps (median of 3) | spread | booking p50 | f6ed10c | delta |
|---|---|---|---|---|---|---|---|
| `s2-c1-g` | 1 | group | 537.6 tps | 1.1 % | 12,869.2 us | 589.0 tps | -8.7 % |
| `s2-c8-g` | 8 | group | 1,519.0 tps | 4.5 % | 4,234.6 us | 308.5 tps | +392.4 % (different configuration) |
| `s2-c1-s` | 1 | strict | 482.5 tps | 4.3 % | 14,325.9 us | 553.0 tps | -12.7 % |
| `s2-c8-s` | 8 | strict | 864.2 tps | 42.0 % | 5,864.2 us | 294.0 tps | +193.9 % (different configuration; spread 42 %) |

**Insights.** Retiring ownership turned f6ed10c's worst case (eight cores at
half of one) into the best: 2.83x under `group`. At eight cores `COMMIT` is
55.4 % of a booking and the only phase that got slower. At one core the
same statements run 1.7x to 10x slower at p50 for the same p0: one reactor
is a queue, so a one-core number here measures saturation. Under `strict`
at eight cores 50 to 125 of 3,000 bookings meet a held row and are refused
`TXN_CONFLICT` (retried by the driver). **The invariant check fails in all
twelve default cells, 13 to 31 of 400** (also at `cores = 1`); with
`--isolation repeatable-read` both probe cells verify 400 of 400 at
unchanged throughput (541.8 and 1,579.5 tps, one run each): the failures
are the read-committed lost update of the driver's client-side
read-modify-write, not engine corruption.

## Scenario 3: library ([file](results-scenario3-library-v2.7.0-531-g9a0525d.md))

Derived qps (1,000,000 / mean us, serial connection), `cores = 1`, median
of three runs; no v3 predecessor, no delta.

| Shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |
|---|---|---|---|---|---|---|
| `pk-user` (control) | 13,928 qps | 13,774 qps | 13,661 qps | 13,831 qps | 14,025 qps | 14,388 qps |
| `loans-by-user` | 10,173 qps | 11,186 qps | 6,443 qps | 11,074 qps | 1,277 qps | 10,881 qps |
| `loans-by-book` | 10,730 qps | 11,614 qps | 6,739 qps | 11,601 qps | 1,270 qps | 11,737 qps |
| `books-by-genre` | 13,298 qps | 13,316 qps | 7,570 qps | 9,285 qps | 1,646 qps | 2,101 qps |
| `loans-by-daterange` (no index covers it) | 9,398 qps | 9,355 qps | 3,814 qps | 3,855 qps | 492 qps | 514 qps |
| `count-by-user` | 13,038 qps | 14,599 qps | 7,358 qps | 14,430 qps | 1,323 qps | 14,306 qps |

**Insights.** The index makes a non-pk equality independent of the row count
(10,881 qps at 10,000 rows against 11,186 at 200) where unindexed it falls
with the rows: about 84 us fixed plus 70 ns per row walked. The gain is
1.1x at 200 rows (inside the floor), 1.4x to 2.0x at 1,000 and 5.5x to 10.8x
at 10,000 on the `loans` and `reservations` equalities, and the residual cost
of an indexed read is the round trip (about 90 us against 70 us for a pk
lookup). `cores = 8` from one connection reads 0.86x to 1.18x of `cores = 1`
(median 0.95x) and is the slower of the two in 65 of 72 cells. Noise floor is large
and heavy-tailed (median shape 1.6 to 8.2 % between runs, four groups at 21
to 33 %, worst shapes to 89 %), so only differences over about 10 % are
read.

## Scenario 4: cabin optimizer over business days ([file](results-scenario4-cabinopt-days-v2.7.0-531-g9a0525d.md))

Hot equality probe on a 10,000-row board, derived qps by arm and day; run
A is the server defaults with the driver's compression (`decay_half_life =
5`). No v3 predecessor, no delta.

| Day | off | on (controller) | declared | on over off |
|---|---|---|---|---|
| 1 | 960 qps | 2,403 qps | 3,039 qps | 2.50x |
| 2 | 954 qps | 2,374 qps | 2,990 qps | 2.49x |
| 3 | 921 qps | 3,283 qps | 3,827 qps | 3.56x |

Tape relations (200 / 1,000 / 10,000 rows), `on` over `off` mean ratio on
day 1: 1.11x (at the floor), 1.44x, 2.67x. The pk control reads 0.97x to
1.15x and is the floor.

**Insights.** The controller gets most of the hand-declared ceiling without
being told; the difference is in the mean (416 us against 329 us declared,
day 1), not at p50 (187 against 183 us) or p95 (1,130 against 1,061 us),
so the controller walks a larger share of probes than the declared arm,
not slower ones. Answers were
byte-identical across arms on all three days in both runs. **At the
default `cabin_optimizer_cooldown_half_lives = 128` and `decay_half_life =
5` the run never DROPs a Cabin (`drops=0`)**: the controller creates
(5), extends (173), decays and recovers, but a DROP needs 640 s and the run
is 279 s; the driver's docstring assumes a 2-half-life cooldown. Run B at
cooldown 2 drops `board_a` mid-day-2 and re-nominates it on day 3 (`creates=6`,
`drops=2`), at a cost of about 25 % of that day's board throughput (2,467
against 3,283 qps, single draws).

## What the rebaseline says about the engine

1. **Cross-core execution now pays where it is measured**: scenario2 at eight
   cores under `group` is 2.83x one core, and scenario0's `strict` doubles;
   both were flat or negative at f6ed10c. In scenario2 the commit path is
   55 % of a booking at eight cores; the host's eight CPUs are shared with
   the client processes, and this box cannot separate the two.
2. **The one-core path is slower than at f6ed10c**: 8.7 to 20.1 % on
   scenario0 and scenario2, outside their floors and not attributable to
   any single milestone. This is the number a per-milestone overhead
   measurement should be read against.
3. **One reactor is a queue**: every one-core booking statement but `COMMIT` is
   1.7x to 10x slower at p50 than at eight cores for the same p0.
4. **A Cabin, declared or automatic, and a secondary index both deliver the
   same thing, a read that costs one round trip instead of the relation**;
   their value scales with the row count (nothing to gain at 200 rows).
5. **Two driver gaps surfaced; no engine defect is established**: the btree's documented
   bounded re-descent refusal (retryable `TXN_CONFLICT`) is reached under
   eight concurrent appenders (scenario0 `strict`, 3 of 3 runs; scenario2
   `group`, 3 refusals in 2 of 3 runs), and scenario0's driver, which does
   not retry, turns it into a torn trade; and scenario1's driver still
   creates `HEAP` relations, which SUS-1 refuses by design. The scenario2
   invariant failures are diagnosed as the driver's read-committed lost
   update, not an engine defect.

## Not done

- No PostgreSQL floor, for any scenario (not installed).
- Scenario1: no number.
- Scenario3: `--cabin` and the `single`, `composite`, `covering` index modes.
- `recovery_checkpoint_us` was not sampled in any cell; the load phases'
  p50s (scenario0 `load-users` 1.24 to 1.42 ms, scenario2 `load-cargos` 1.30
  to 1.38 ms) stood in as the
  device thermometer and show no stall.
- `cores` above the CPU count was not tried.
