# Scenarios 1 to 4 at `v3.4.17-166-g6c98f7c32`, every cell, on eight cores: interim results

**This file is an interim record of a measurement still running**, written at 01:12 UTC on 2026-11-27 because the operator asked for the results so far to be committed and pushed. The pinned series is complete; the historical unpinned series and PostgreSQL 17 are still running. The four results files and the summary replace this file when the run ends, and this file is then deleted. Nothing below is revised by what is still to run: every table here is of cells that have finished.

**One line so far.** On the pinned series, the engine at `6c98f7c32` reads as the engine of `9f8f8b354` (the commit before DT) and of `07f4552a` (`v3.5.0`). Over the 220 rows of the three-engine A/B of scenarios 1 to 3, new over mid (DT's price) is inside its spread on 151, carries no claim on 68 and is marked by the spread rule on one (scenario 3 `loans-by-book` unindexed at one core, 1.01x); new over old is inside on 152, without a claim on 59 and marked on nine, 0.96x to 1.12x, all listed below. Scenario 4's three pinned runs an engine read 0.97x to 1.01x on every group and arm that resolves. Every driver exited 0 in the 296 runs finished (three of them set aside and run again), and every scenario 4 byte comparison reported 0 mismatches.

## Stamp

| Field | Value |
|---|---|
| Version, commit | `v3.4.17-166-g6c98f7c32` (`git describe --tags` at `6c98f7c32`, the tip of `origin/main` after `git fetch origin` at 23:03 UTC on 2026-11-26), built from `git archive 6c98f7c32` outside every worktree, Release, target `kds_server`; measured copy sha256 `102230462176875ca858e85038b41e19cb78e96f5d839c5593fd53657fcd6bfd` |
| The three engines | new `6c98f7c32`; mid `9f8f8b354` (sha256 `a333ee48...`) and old `07f4552a` = `v3.5.0` (sha256 `03c8c2e9...`), copies of the binaries the `9f8f8b354` run built and measured on this host. One driver, byte-identical in the three commits |
| Orders | operator, 2026-11-26, after the budgeted run at `9f8f8b354`: "lift the time budget and run with every cell", then "run on 8 cores". Read as: nothing of the shape cut; every server pinned to this host's eight P-cores (`taskset -c 0-7`), every driver to its sixteen E-cores (`taskset -c 8-23`); and every cell family at `cores = 8` beside its `cores = 1` cells |
| Host and device | 24 logical CPUs, Intel Core Ultra 7 270K Plus (8 P-cores, CPUs 0 to 7; 16 E-cores), `powersave`, 30 GiB, Linux 7.3.4-300.fc45.x86_64; data files on `/dev/mapper/fedora-root`, xfs, an LVM volume on an NVMe device, never tmpfs; a fresh server and data file per cell |
| Time | started 23:28:50 UTC on 2026-11-26; the pinned series ended 01:10:03 UTC on 2026-11-27. The time budget is lifted by the operator for this measurement |
| Quiet gate and validity | a gate before each cell (6 s of hold in all so far); after each cell the monitor's samples inside it are judged, and a cell with foreign processes summing to 50 % or more of one CPU in a sample is set aside and run again: three attempts so far (`pin-r2-s1-c1-g`, `pin-ab-old3-s4`, `pin-ab-old1-c8-s4`; an Oracle container's job processes and `dnf`), each kept on its second attempt |
| Suite | not executed by this measurement |
| Not yet measured | the unpinned series (running), PostgreSQL 17 (queued) |

Rule: spread = (max - min) / median over a cell's runs; a difference of medians smaller than the larger spread is not a finding; a spread over 20 % carries no claim.

## The engine, isolated: three engines, pinned, five rounds

old = `07f4552a` (`v3.5.0`), mid = `9f8f8b354`, new = `6c98f7c32`, interleaved old, mid, new, each on a fresh server and data file. "new over mid" is DT; "new over old" is everything since `v3.5.0`.

| measure | old `07f4552a` | spread | mid `9f8f8b354` | spread | new `6c98f7c32` | spread | new over old | read | new over mid | read |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 backtest, `day-slice` warm, `c1-g` | 1,362.1 qps | 4.1 % | 1,392.2 qps | 2.4 % | 1,395.8 qps | 7.2 % | 1.02x | inside the spread | 1.00x | inside the spread |
| 1 backtest, `cross-join` warm, `c1-g` | 1,322.7 qps | 7.7 % | 1,351.4 qps | 1.8 % | 1,356.0 qps | 6.2 % | 1.03x | inside the spread | 1.00x | inside the spread |
| 1 backtest, `bar-range` warm, `c1-g` | 1,745.6 qps | 1.0 % | 1,734.3 qps | 3.8 % | 1,740.7 qps | 5.3 % | 1.00x | inside the spread | 1.00x | inside the spread |
| 1 backtest, `model-join` warm, `c1-g` | 1,538.0 qps | 5.1 % | 1,528.6 qps | 1.1 % | 1,519.6 qps | 2.2 % | 0.99x | inside the spread | 0.99x | inside the spread |
| 1 backtest, phase `backtest-read`, `c1-g` | 1,297.6 qps | 9.0 % | 1,325.9 qps | 1.5 % | 1,343.2 qps | 3.5 % | 1.04x | inside the spread | 1.01x | inside the spread |
| 1 backtest, phase `result-insert`, `c1-g` | 4,813.8 qps | 50.0 % | 4,825.6 qps | 2.4 % | 4,859.0 qps | 1.7 % | 1.01x | spread > 20 %, no claim | 1.01x | inside the spread |
| 1 backtest, phase `load-bars`, `c1-g` | 44,803.6 qps | 1.8 % | 44,979.1 qps | 2.2 % | 45,174.8 qps | 1.3 % | 1.01x | inside the spread | 1.00x | inside the spread |
| 1 backtest, `day-slice` warm, `c8-g` | 1,326.3 qps | 4.1 % | 1,365.3 qps | 1.3 % | 1,347.8 qps | 8.4 % | 1.02x | inside the spread | 0.99x | inside the spread |
| 1 backtest, `cross-join` warm, `c8-g` | 1,273.4 qps | 3.4 % | 1,311.4 qps | 3.4 % | 1,326.1 qps | 13.6 % | 1.04x | inside the spread | 1.01x | inside the spread |
| 1 backtest, `bar-range` warm, `c8-g` | 1,697.6 qps | 3.1 % | 1,706.7 qps | 4.3 % | 1,719.4 qps | 3.0 % | 1.01x | inside the spread | 1.01x | inside the spread |
| 1 backtest, `model-join` warm, `c8-g` | 1,412.6 qps | 1.2 % | 1,426.1 qps | 1.1 % | 1,421.2 qps | 1.2 % | 1.01x | inside the spread | 1.00x | inside the spread |
| 1 backtest, phase `backtest-read`, `c8-g` | 1,259.9 qps | 3.4 % | 1,293.1 qps | 0.9 % | 1,290.4 qps | 6.7 % | 1.02x | inside the spread | 1.00x | inside the spread |
| 1 backtest, phase `result-insert`, `c8-g` | 4,701.8 qps | 2.9 % | 4,704.0 qps | 3.1 % | 4,821.2 qps | 3.3 % | 1.03x | inside the spread | 1.02x | inside the spread |
| 1 backtest, phase `load-bars`, `c8-g` | 44,901.2 qps | 4.0 % | 45,783.0 qps | 1.2 % | 45,454.5 qps | 5.4 % | 1.01x | inside the spread | 0.99x | inside the spread |
| 2 freight, 8 bookers, cores 1, group | 4,221.6 tps | 1.5 % | 4,261.1 tps | 2.3 % | 4,218.2 tps | 1.5 % | 1.00x | inside the spread | 0.99x | inside the spread |
| 2 freight, 1 booker, cores 1, strict | 1,752.9 tps | 0.8 % | 1,748.0 tps | 26.1 % | 1,751.4 tps | 0.8 % | 1.00x | inside the spread | 1.00x | spread > 20 %, no claim |
| 2 freight, 8 bookers, cores 8, group | 9,494.0 tps | 3.4 % | 9,579.1 tps | 11.6 % | 9,547.9 tps | 56.7 % | 1.01x | spread > 20 %, no claim | 1.00x | spread > 20 %, no claim |
| 2 freight, 8 bookers, cores 8, strict | 8,747.5 tps | 3.5 % | 8,952.4 tps | 4.0 % | 8,886.3 tps | 3.8 % | 1.02x | inside the spread | 0.99x | inside the spread |
| 3 library, `pk-user` (unindexed), `cores = 1` | 29,673.6 qps | 17.5 % | 30,211.5 qps | 4.1 % | 30,487.8 qps | 7.5 % | 1.03x | inside the spread | 1.01x | inside the spread |
| 3 library, `loans-by-user` (unindexed), `cores = 1` | 6,963.8 qps | 2.0 % | 6,807.4 qps | 4.0 % | 6,877.6 qps | 1.9 % | 0.99x | inside the spread | 1.01x | inside the spread |
| 3 library, `join-no-literal` (unindexed), `cores = 1` | 4,217.6 qps | 1.1 % | 4,120.3 qps | 0.6 % | 4,115.2 qps | 2.2 % | 0.98x | **finding** | 1.00x | inside the spread |
| 3 library, `exists-correlated` (unindexed), `cores = 1` | 4,171.9 qps | 2.6 % | 4,115.2 qps | 2.0 % | 4,132.2 qps | 2.2 % | 0.99x | inside the spread | 1.00x | inside the spread |
| 3 library, `count-by-user` (unindexed), `cores = 1` | 7,622.0 qps | 1.1 % | 7,385.5 qps | 5.2 % | 7,429.4 qps | 2.8 % | 0.97x | inside the spread | 1.01x | inside the spread |
| 3 library, `pk-user` (indexed), `cores = 1` | 18,484.3 qps | 70.0 % | 19,267.8 qps | 16.3 % | 18,115.9 qps | 24.1 % | 0.98x | spread > 20 %, no claim | 0.94x | spread > 20 %, no claim |
| 3 library, `loans-by-user` (indexed), `cores = 1` | 25,252.5 qps | 1.0 % | 24,449.9 qps | 16.1 % | 24,630.5 qps | 3.6 % | 0.98x | inside the spread | 1.01x | inside the spread |
| 3 library, `join-no-literal` (indexed), `cores = 1` | 8,960.6 qps | 2.0 % | 8,665.5 qps | 1.6 % | 8,583.7 qps | 3.2 % | 0.96x | **finding** | 0.99x | inside the spread |
| 3 library, `exists-correlated` (indexed), `cores = 1` | 16,977.9 qps | 5.0 % | 16,420.4 qps | 9.5 % | 16,420.4 qps | 1.6 % | 0.97x | inside the spread | 1.00x | inside the spread |
| 3 library, `count-by-user` (indexed), `cores = 1` | 34,843.2 qps | 11.5 % | 34,246.6 qps | 2.4 % | 31,055.9 qps | 18.7 % | 0.89x | inside the spread | 0.91x | inside the spread |
| 3 library, `pk-user` (unindexed), `cores = 8` | 26,246.7 qps | 12.1 % | 28,735.6 qps | 16.3 % | 27,173.9 qps | 19.5 % | 1.04x | inside the spread | 0.95x | inside the spread |
| 3 library, `loans-by-user` (unindexed), `cores = 8` | 5,592.8 qps | 5.4 % | 5,618.0 qps | 5.1 % | 5,586.6 qps | 4.0 % | 1.00x | inside the spread | 0.99x | inside the spread |
| 3 library, `join-no-literal` (unindexed), `cores = 8` | 3,640.3 qps | 0.3 % | 3,665.7 qps | 0.9 % | 3,679.2 qps | 1.0 % | 1.01x | **finding** | 1.00x | inside the spread |
| 3 library, `exists-correlated` (unindexed), `cores = 8` | 4,024.1 qps | 0.9 % | 4,100.0 qps | 0.9 % | 4,086.6 qps | 2.3 % | 1.02x | inside the spread | 1.00x | inside the spread |
| 3 library, `count-by-user` (unindexed), `cores = 8` | 5,941.8 qps | 1.2 % | 5,995.2 qps | 10.2 % | 5,988.0 qps | 1.1 % | 1.01x | inside the spread | 1.00x | inside the spread |
| 3 library, `pk-user` (indexed), `cores = 8` | 14,144.3 qps | 80.4 % | 10,846.0 qps | 22.8 % | 11,350.7 qps | 24.3 % | 0.80x | spread > 20 %, no claim | 1.05x | spread > 20 %, no claim |
| 3 library, `loans-by-user` (indexed), `cores = 8` | 23,980.8 qps | 7.6 % | 24,330.9 qps | 4.8 % | 24,213.1 qps | 5.5 % | 1.01x | inside the spread | 1.00x | inside the spread |
| 3 library, `join-no-literal` (indexed), `cores = 8` | 8,123.5 qps | 2.4 % | 8,354.2 qps | 4.2 % | 8,347.2 qps | 3.4 % | 1.03x | inside the spread | 1.00x | inside the spread |
| 3 library, `exists-correlated` (indexed), `cores = 8` | 15,220.7 qps | 12.9 % | 15,267.2 qps | 2.6 % | 15,197.6 qps | 5.1 % | 1.00x | inside the spread | 1.00x | inside the spread |
| 3 library, `count-by-user` (indexed), `cores = 8` | 34,013.6 qps | 21.1 % | 34,364.3 qps | 2.4 % | 34,482.8 qps | 2.4 % | 1.01x | spread > 20 %, no claim | 1.00x | inside the spread |

The rows the rule marks, over all 220 rows of the 13 A/B tables:

| table | row | new over old | new over mid |
|---|---|---|---|
| scenario 1, `c8-g`, sweep | `model-join` cabin | 1.03x, marked | 0.99x |
| scenario 2, 8 bookers, 8 cores, `strict` | `charge-insert` | 1.07x, marked | 1.01x |
| scenario 2, 8 bookers, 8 cores, `strict` | `operation-update` | 1.07x, marked | 1.02x |
| scenario 2, 8 bookers, 8 cores, `strict` | `org-update` | 1.06x, marked | 1.01x |
| scenario 3, 1 core, indexed | `join-no-literal` | 0.96x, marked | 0.99x |
| scenario 3, 1 core, unindexed | `join-no-literal` | 0.98x, marked | 1.00x |
| scenario 3, 1 core, unindexed | `loans-by-book` | 0.99x | 1.01x, marked |
| scenario 3, 8 cores, indexed | `create-index` | 1.12x, marked | 1.00x |
| scenario 3, 8 cores, indexed | `overdue` | 1.05x, marked | 1.01x |
| scenario 3, 8 cores, unindexed | `join-no-literal` | 1.01x, marked | 1.00x |

Every marked new-over-old row is within 2 % of its new value in the mid engine already, so none of them is DT's.

## Scenario 2, pinned

| cell | cores | durability | bookers | run 1 | run 2 | run 3 | median | spread |
|---|---|---|---|---|---|---|---|---|
| `pin-s2-c1-g` | 1 | group | 8 | 4,223.9 tps | 3,817.1 tps | 4,242.3 tps | **4,223.9 tps** | 10.1 % |
| `pin-s2-c8-g` | 8 | group | 8 | 9,827.8 tps | 9,201.8 tps | 9,505.3 tps | **9,505.3 tps** | 6.6 % |
| `pin-s2-c1-s` | 1 | strict | 8 | 3,962.9 tps | 3,983.1 tps | 3,952.8 tps | **3,962.9 tps** | 0.8 % |
| `pin-s2-c8-s` | 8 | strict | 8 | 8,717.4 tps | 8,893.8 tps | 8,794.6 tps | **8,794.6 tps** | 2.0 % |
| `pin-b1-s2-c1-g` | 1 | group | 1 | 1,751.2 tps | 1,747.5 tps | 1,743.3 tps | **1,747.5 tps** | 0.5 % |
| `pin-b1-s2-c1-s` | 1 | strict | 1 | 1,730.8 tps | 1,731.7 tps | 1,739.7 tps | **1,731.7 tps** | 0.5 % |
| `pin-b1-s2-c8-g` | 8 | group | 1 | 1,719.5 tps | 1,733.3 tps | 1,680.7 tps | **1,719.5 tps** | 3.1 % |
| `pin-b1-s2-c8-s` | 8 | strict | 1 | 1,721.0 tps | 1,644.9 tps | 1,701.4 tps | **1,701.4 tps** | 4.5 % |

The booker sweep at eight cores:

| bookers | durability | tps | runs | spread | conflicted (run 1) | abandoned (run 1) | verify failures / checks (run 1) |
|---|---|---|---|---|---|---|---|
| 1 | group | **1,280.9 tps** | 1 | n/a | 0 | 0 | 0 / 400 |
| 1 | strict | **1,547.0 tps** | 1 | n/a | 0 | 0 | 0 / 400 |
| 2 | group | **3,250.8 tps** | 1 | n/a | 0 | 0 | 4 / 400 |
| 2 | strict | **3,009.0 tps** | 1 | n/a | 0 | 0 | 3 / 400 |
| 4 | group | **5,727.2 tps** | 1 | n/a | 0 | 0 | 5 / 400 |
| 4 | strict | **5,197.9 tps** | 1 | n/a | 16 | 0 | 8 / 400 |
| 8 | group | **9,505.3 tps** | 3 | 6.6 % | 1 | 0 | 25 / 400 |
| 8 | strict | **8,794.6 tps** | 3 | 2.0 % | 42 | 0 | 24 / 400 |
| 16 | group | **14,732.9 tps** | 1 | n/a | 0 | 0 | 53 / 400 |
| 16 | strict | **12,623.9 tps** | 1 | n/a | 106 | 0 | 39 / 400 |
| 32 | group | **16,898.3 tps** | 1 | n/a | 1 | 0 | 79 / 400 |
| 32 | strict | **14,268.9 tps** | 1 | n/a | 235 | 0 | 58 / 400 |

The extreme cells (20,000 committed bookings at eight cores), one run each; the unpinned ones are still to run:

| cell | series | tps | committed | conflicted | abandoned | verify failures / checks | booking p50 | booking p99 | commit p50 | commit p99 |
|---|---|---|---|---|---|---|---|---|---|---|
| `pin-big-b32-c8-g` | pinned | **19,104.7 tps** | 20,000 | 0 | 0 | 6 / 400 | 1,160.1 µs | 6,051.9 µs | 364.4 µs | 5,006.0 µs |
| `pin-big-b32-c8-s` | pinned | **18,856.8 tps** | 20,000 | 191 | 0 | 8 / 400 | 1,316.0 µs | 2,668.1 µs | 354.1 µs | 660.3 µs |
| `pin-big2p-c8-g` (two clients) | pinned | **18,085.0 tps** (8,743.0 tps + 9,342.0 tps) | 20,000 | 3 | 0 | 3 / 800 | 1,262.3 µs | 6,359.4 µs | 441.7 µs | 5,121.4 µs |
| `pin-big2p-c8-s` (two clients) | pinned | **15,629.8 tps** (7,853.3 tps + 7,776.5 tps) | 20,000 | 96 | 1 | 2 / 800 | 1,475.1 µs | 7,301.7 µs | 439.3 µs | 3,853.1 µs |
| `big-b32-c8-g` | unpinned | not run | - | - | - | - | - | - | - | - |
| `big-b32-c8-s` | unpinned | not run | - | - | - | - | - | - | - | - |
| `big2p-c8-g` | unpinned | not run | - | - | - | - | - | - | - | - |
| `big2p-c8-s` | unpinned | not run | - | - | - | - | - | - | - | - |

## Scenario 4, pinned: `07f4552a` against `6c98f7c32`, three runs each

Mean latency per group and arm over the three runs and three days of each engine; "new over old" = old mean / new mean.

| group | arm | old `07f4552a` mean latency, median of 9 day-runs | spread | new `6c98f7c32` | spread | new over old | read |
|---|---|---|---|---|---|---|---|
| board | off | 251.3 µs | 2.6 % | 248.5 µs | 3.6 % | 1.01x | inside the spread |
| board | on | 177.1 µs | 5.6 % | 178.8 µs | 21.5 % | 0.99x | spread > 20 %, no claim |
| board | declared | 109.3 µs | 16.4 % | 109.4 µs | 19.4 % | 1.00x | inside the spread |
| tape200 | off | 45.2 µs | 10.0 % | 45.3 µs | 5.3 % | 1.00x | inside the spread |
| tape200 | on | 44.9 µs | 3.3 % | 44.9 µs | 2.7 % | 1.00x | inside the spread |
| tape200 | declared | 43.7 µs | 10.5 % | 43.7 µs | 2.5 % | 1.00x | inside the spread |
| tape1k | off | 53.7 µs | 7.8 % | 53.7 µs | 0.9 % | 1.00x | inside the spread |
| tape1k | on | 53.2 µs | 7.1 % | 53.5 µs | 9.5 % | 0.99x | inside the spread |
| tape1k | declared | 45.0 µs | 7.6 % | 46.5 µs | 9.0 % | 0.97x | inside the spread |
| tape10k | off | 156.4 µs | 6.5 % | 155.4 µs | 3.9 % | 1.01x | inside the spread |
| tape10k | on | 153.2 µs | 4.9 % | 154.9 µs | 24.1 % | 0.99x | spread > 20 %, no claim |
| tape10k | declared | 79.6 µs | 18.1 % | 80.1 µs | 23.0 % | 0.99x | spread > 20 %, no claim |
| pk | off | 31.8 µs | 12.6 % | 31.9 µs | 5.6 % | 1.00x | inside the spread |
| pk | on | 31.6 µs | 7.9 % | 31.7 µs | 8.5 % | 1.00x | inside the spread |
| pk | declared | 31.4 µs | 1.6 % | 31.5 µs | 2.2 % | 1.00x | inside the spread |
| open | off | 176.6 µs | 17978.9 % | 180.5 µs | 15.0 % | 0.98x | spread > 20 %, no claim |
| open | on | 166.5 µs | 7.7 % | 166.9 µs | 14.9 % | 1.00x | inside the spread |
| open | declared | 165.4 µs | 12.2 % | 166.2 µs | 14.7 % | 1.00x | inside the spread |
| close | off | 1,020.4 µs | 32.7 % | 1,011.4 µs | 13.1 % | 1.01x | spread > 20 %, no claim |
| close | on | 778.5 µs | 18.3 % | 773.7 µs | 11.3 % | 1.01x | inside the spread |
| close | declared | 742.7 µs | 18.9 % | 749.8 µs | 18.8 % | 0.99x | inside the spread |

In all nine pinned runs the controller's Cabins hold one page, `extends` is 0 and no lookup share is recorded (`cabin-controller.md` PO11, as at `v3.5.0`); the 18-statement byte comparison reported 0 mismatches on every day of every run.

## Archive

Not yet in the tree. The raw output is under `/home/ukjin/bench-runs/scenarios-v3.4.17-166-g6c98f7c32/` on the measuring host and is archived under `bench/v3.5.0/archive/` with the final files.
