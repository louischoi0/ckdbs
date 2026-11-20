# DI-S0 premise: what `StatementStats::Record` costs a point `SELECT`, and what a wider key adds

`instructions/v3.2.0/workorder-di-shape-identity.md` DI-S0, its "Measure" clause: price `Record` today, then price the binding DI proposes to add to its key, in two widths, before anything is built.

**Finding.** Neither `Record` nor the binding is resolved on this host at either core count: every paired delta of the three variants against A is smaller than the arms' own spread (1.0 to 1.9 us at `cores = 1`, 0.6 to 1.2 us at `cores = 8`), and the signs disagree between the two cells. `Record`'s whole share (A - A0) is -0.36 us at `cores = 1` (A0 slower, the wrong sign for an ablation, so noise) and +0.42 us (1.2 % of A's 34.04 us a statement) at `cores = 8`, where A0 was cheaper in 5 of 5 rounds but by an amount of the order of the host's known one-binary floor (about 0.27 us). The binding spike with 16 oids (B1) adds +0.02 us at `cores = 1` and +0.43 us at `cores = 8` against A; the one-relation binding (B2) adds +0.29 and -0.24 us. B1 - B2, the width, is -0.27 us at `cores = 1` (1 of 5 slower) and +0.66 us at `cores = 8` (5 of 5 slower, but inside B2's 0.80 us spread and with the opposite sign at `cores = 1`). What the run supports is an upper bound, not a price: `Record` plus a 146-byte binding together cost under about 1 us on a 22 to 34 us point read (under about 3 to 4 %), and whether the key's width matters cannot be told apart from the host's noise.

## Stamp

| item | value |
|---|---|
| executed | 2026-11-20, driver 03:28:07-03:34:01 UTC (build 01:58:40-02:03:30 UTC) |
| branch | `worktree-di-shape-identity` |
| commit | `1c7bb774`, `git describe --tags` = `v3.4.15-4-g1c7bb774` (the commit DI opened from) |
| tree | every arm built from one clean `git archive` export of `1c7bb774` (`head.tar`, unpacked into `/home/louis/bench-runs/di-s0/src`); the worktree's own uncommitted DI edits were not built and the worktree `build/` was not touched |
| arm A | the commit as exported; Release; `kds_A` mtime 2026-11-20 02:00:48 UTC, sha256 `b743a60ce0d685061ee63d73ed71c2ae0bb7667782ff453a03938471f1ac42cd` (the same hash as `kds_B` of DA's close at `89c26252`, so A is byte-identical to that engine) |
| arm A0 | A with `if (false)` before `RecordStatement`'s call of `StatementStats::Record` (`a0.py`); mtime 02:01:21 UTC, sha256 `35c918b6ca19f6905aa5a834fc32659de7cbbdf4502a47ae0e16476fdcdcaa6d` |
| arm B1 | A plus the binding spike (`b1.patch`, `statement_binding.hpp`): key `(fetch_id, kind, namespace oid, up to 16 distinct relation oids)`, 146-byte binding recorded at each bind and carried on `DispatchOutcome`; mtime 02:02:23 UTC, sha256 `9cecf15e4f66d5c1677f07a9f84f68704b84aee62881edf3bfbb169a2651854f` |
| arm B2 | B1 with the binding cut to the statement's first relation plus the namespace word (`b2.py`); mtime 02:03:30 UTC, sha256 `29d77836ca8d18664ad8a2fa7e66afb7ab1695f9c9315183af3134b8b4ac7015` |
| build | Release, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`, four builds in sequence in one directory; the spike is never committed. Every server started from a copy in `/home/louis/bench-runs/di-s0/bin/`; all four binaries are newer than the commit |
| device | data files under `/home/louis/bench-runs/di-s0/run` on `/dev/nvme0n1p1` (ext4, checked with `df -T`); host 8 vCPU |
| server config | `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, `access_statistics = on` (the default), `cores` 1 (server pinned to CPU 2) or 8; all else default |
| quiet gate | held from 02:04 UTC to 03:28 UTC, **84 minutes**, outside the budget: other sessions' builds and suites (worktrees `dh-page-image-volume`, `cv-ordered-index-walk`) ran back to back, `busy` never reached zero until the coordinator held its own worktree's builds. At release (03:28:07 UTC) `/proc/loadavg` was `4.45 7.39 10.95`, no `cc1plus`, `ctest` or `kds_tests` process; the 4.45 included the resident `build-sandbox/kds_server` (pid 560863, another project, on every arm alike). Load at the end of each lifetime is in `analysis.txt` (3 to 11, mostly this run's own 16 clients at `cores = 8`); no other session's compiler or suite was seen after release |
| budget | build about 5 minutes, run 6 minutes, 11 minutes against the 20-minute stage budget, hold 84 minutes outside it; nothing cut: 5 rounds x 4 arms x 2 cells = 40 lifetimes as briefed |

## Method

Driver `di_arms.py` (archived beside the raw data), DA's close driver with the two arms made a list; the arm order rotates by one each round, so every arm is first, last and in between equally often. One server lifetime per arm per round, a fresh data file, 2 s warm then 6 s measured, `SELECT * FROM t WHERE id = <random>` over 10,000 rows. Server CPU is `utime + stime` from `/proc/<pid>/stat` over the measured window divided by completed statements; QPS is client side. `select1` is one serial client; `select8` is 16 closed-loop client processes against `cores = 8`. Spread is max - min over the 5 lifetimes of an arm. A paired delta is taken within one round. A delta is called outside the spread only when its mean exceeds the larger of the two arms' spreads. Only 10,000 rows were run: a pk point lookup does not scale with rows, and `Record` is a per-statement cost, so rule 9's three sizes would measure the same thing (departure stated, not hidden; same as DA's close). No wait decomposition: this driver measures server CPU per statement between binaries, which has no wait breakdown, so none is given. Command: see the archive's `driver.log` header; flags `--rounds 5 --cells select1,select8 --secs 6 --warm 2`.

## Results

| cell | arm | CPU us/stmt, mean | CPU spread | QPS mean | QPS spread | statements |
|---|---|---|---|---|---|---|
| select1 | A | 22.39 us | 1.93 us | 23,765 /s | 1,819 /s | 713,536 |
| select1 | A0 | 22.74 us | 0.88 us | 23,347 /s | 727 /s | 700,961 |
| select1 | B1 | 22.41 us | 0.33 us | 23,901 /s | 397 /s | 717,596 |
| select1 | B2 | 22.68 us | 1.08 us | 23,626 /s | 1,186 /s | 709,343 |
| select8 | A | 34.04 us | 1.20 us | 109,984 /s | 5,882 /s | 3,309,667 |
| select8 | A0 | 33.62 us | 1.05 us | 109,698 /s | 4,136 /s | 3,300,926 |
| select8 | B1 | 34.47 us | 0.59 us | 108,945 /s | 1,867 /s | 3,278,657 |
| select8 | B2 | 33.80 us | 0.80 us | 110,190 /s | 865 /s | 3,316,240 |

Client round-trip latency (us; median of the five per-lifetime percentiles; includes the Python client and the socket, not server time):

| cell | arm | n | p0 | p25 | p50 | p90 | p99 |
|---|---|---|---|---|---|---|---|
| select1 | A | 713,536 | 34 us | 39 us | 39 us | 45 us | 56 us |
| select1 | A0 | 700,961 | 34 us | 39 us | 39 us | 49 us | 58 us |
| select1 | B1 | 717,596 | 33 us | 39 us | 39 us | 45 us | 56 us |
| select1 | B2 | 709,343 | 35 us | 39 us | 39 us | 48 us | 56 us |
| select8 | A | 3,309,667 | 31 us | 86 us | 125 us | 243 us | 431 us |
| select8 | A0 | 3,300,926 | 29 us | 86 us | 123 us | 238 us | 442 us |
| select8 | B1 | 3,278,657 | 31 us | 86 us | 127 us | 242 us | 422 us |
| select8 | B2 | 3,316,240 | 30 us | 84 us | 123 us | 239 us | 436 us |

## Paired deltas

Server CPU, us a statement, per round 1 to 5 (X minus Y; positive means X is slower), then the mean, the rounds X was slower, and whether the mean is outside the larger arm spread of the pair.

| cell | X - Y | rounds 1..5 | mean | X slower in | share of A | outside spread | QPS delta mean |
|---|---|---|---|---|---|---|---|
| select1 | A0 - A | +0.92 +0.39 +0.49 +0.66 -0.67 us | +0.36 us | 4 / 5 | +1.6 % | no (spread 1.93 us) | -1.7 % |
| select1 | B1 - A | +0.09 +0.52 +0.43 +0.55 -1.48 us | +0.02 us | 4 / 5 | +0.1 % | no (1.93 us) | +0.7 % |
| select1 | B2 - A | +0.34 +1.22 +0.39 +0.58 -1.08 us | +0.29 us | 4 / 5 | +1.3 % | no (1.93 us) | -0.5 % |
| select1 | B1 - B2 | -0.24 -0.70 +0.04 -0.03 -0.40 us | -0.27 us | 1 / 5 | -1.2 % | no (1.08 us) | +1.2 % |
| select8 | A0 - A | -0.36 -0.33 -0.55 -0.40 -0.45 us | -0.42 us | 0 / 5 | -1.2 % | no (1.20 us) | -0.2 % |
| select8 | B1 - A | +0.78 +1.03 -0.11 +0.82 -0.39 us | +0.43 us | 3 / 5 | +1.3 % | no (1.20 us) | -0.9 % |
| select8 | B2 - A | -0.18 +0.51 -0.62 -0.04 -0.86 us | -0.24 us | 1 / 5 | -0.7 % | no (1.20 us) | +0.2 % |
| select8 | B1 - B2 | +0.96 +0.51 +0.52 +0.87 +0.47 us | +0.66 us | 5 / 5 | +2.0 % | no (0.80 us) | -1.1 % |

`Record`'s whole share, A - A0 (ablation):

| cell | A - A0 | as a share of A's CPU a statement | resolved |
|---|---|---|---|
| select1 | -0.36 us | -1.6 % (A0 was the slower; no cost shown) | no |
| select8 | +0.42 us | +1.2 % | no (inside the 1.20 us spread; A0 cheaper in 5 of 5 rounds, spread of the paired deltas only 0.08 us) |

## Against this engine's previous number

DA's close (`bench/v3.4.14/results-da-close-v3.4.14-32-g89c26252.md`) measured the same two shapes on `89c26252`, and arm A here is byte-identical to that run's B binary.

| cell | DA close, B | this run, A | difference |
|---|---|---|---|
| select1 CPU | 22.30 us (spread 0.91 us) | 22.39 us (1.93 us) | +0.09 us, inside both spreads |
| select1 QPS | 23,957 /s | 23,765 /s | -0.8 % |
| select8 CPU | 34.39 us (1.34 us) | 34.04 us (1.20 us) | -0.35 us, inside both spreads |
| select8 QPS | 110,509 /s | 109,984 /s | -0.5 % |

They agree: the same engine reads the same on this host a day-fraction apart, which is the one cross-run check this stage has on its own harness.

## What it says

- **`Record` is not a visible cost on a point read.** Removing the call entirely moved server CPU by -0.36 us and +0.42 us, opposite in sign across the two cells and both inside the spread; the best claim is that it is below about 1 us a statement (about 3 to 4 % of 22 to 34 us), not that it is free. A per-statement hash probe and counter bump into a per-core flat table is small next to a descent, a copy and a socket round trip, which is what the statement is made of.
- **The select8 ablation is suggestive and not resolved.** A0 was cheaper in all 5 rounds, with the paired deltas tightly grouped (-0.33 to -0.55 us), but the deltas have the size of the host's one-binary floor (earlier work here: two servers of one binary differ by up to about 0.27 us over many rounds) plus a lifetime spread of 1.2 us, and the same ablation at `cores = 1` goes the other way. Five rounds cannot separate a 0.4 us effect from per-binary layout; more rounds would be needed for the claim, and nothing in DI turns on it.
- **The width of the key is not distinguishable from its presence, and neither is distinguished from nothing.** B1 - A and B2 - A each straddle zero (B1: +0.02 / +0.43 us; B2: +0.29 / -0.24 us), and B1 - B2 flips sign between cells (-0.27 us at `cores = 1`, +0.66 us at `cores = 8`, the latter 5 of 5 rounds slower but 0.21 us tight and inside the arm spread). If the +0.66 us at `cores = 8` were real it would be 2 % of a point read and would say the 16-oid binding costs more than the one-oid binding when cores contend; this run cannot say so, and a second run of `select8` with B1 and B2 only would be the cheap way to find out. For the work order's decision the safe reading is that DI's binding costs at most about 1 us a statement, either width, on the cheapest statement the engine has.
- **Floor of this host for this shape.** The within-arm spread of five lifetimes is 0.3 to 1.9 us at 22 to 34 us a statement (1.5 to 8 %); no PMU, so wall and CPU ticks are all there is. Nothing under about 1 us a statement is resolvable here with five rounds, which is also about the size of the whole question DI-S0 asked, so the result is a bound.
- No contradiction with an expectation in the work order: the work order proposed measuring because the cost was unknown, and the answer is "small enough to hide in the noise".

## Not measured

Writes (`INSERT`, the foreign-key insert, which records two rows per statement); `cores = 8` beyond 16 clients (the clients share the 8 vCPUs with the server, so the server was not saturated on every core); durability other than `relaxed`; a statement with several relations (a join, where the 16-oid binding is actually full: only a single-relation `SELECT` was run, so B1's extra oids were never populated and B1 - B2 differs here only by the cost of carrying and comparing the wider key, not by a binding that is really wide); the read side of `sys.statement_stats` and the table's growth with distinct bindings; memory. The suite was not run by this measurement (no engine code was committed). Other sessions' builds and suites overlapped the hold, not the run; none was seen after 03:28:07 UTC, but the sampled loadavg rose to 11 at `cores = 8` from this run's own clients.

Raw data and tools: `bench/v3.4.15/archive/di-s0-premise-v3.4.15-4-g1c7bb774/` (`raw.jsonl`, `di_arms.py`, `arms.sh`, `a0.py`, `b2.py`, `b1.patch`, `statement_binding.hpp`, `analyze.py`, `analysis.txt`, `driver.log`, `run.status`, `sha256`, `quietwait.py`, `quiet.status`, build logs).
