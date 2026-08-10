# The cabin optimizer over three business days: nightly retirement is the norm, not the edge case

Sequel to `bench/results-cabin-optimizer.md` (the PHY08 single-shot cases),
whose recorded honest tail was that DECAYING and DROP were never exercised
under real traffic. This scenario exercises them under a compressed market
week — a rotating hot set over three simulated trading days, three arms
(`cabin_optimizer` off / on / statically declared Cabins) — and the answer
is not the one the single-shot case suggests. Three findings:

1. **The lifecycle under business-day traffic is a nightly rebuild cycle.**
   Every Cabin the controller created died within 39–65 s of going cold
   (8–13 decay half-lives plus the 2-half-life DROP cooldown) and was
   re-created within the first ~3 interleave blocks of the next hot
   session. Over three days: **12 CREATEs, 10 DROPs, 252 EXTENDs, 0 HEALs,
   0 failures, 0 deferrals** — DROP fired more often than every other
   structural action except EXTEND. Scaled back to the default half-life,
   the measured DECAYING onsets correspond to ~55 min–2.2 h of real cold
   time — shorter than any real overnight — so at defaults this engine
   would retire and rebuild its whole managed set every single day. That is
   a property of the cost model, not a tuning accident: B decays to zero on
   the R1 clock while C keeps the constant floor `P_rel / T_amort`, so any
   gap longer than about `log2(B/C) + 2` half-lives kills a Cabin.
2. **On the hot median the three-way race is a tie at ~13× — the arms
   differ in the tail and the ledger.** Hot-probe p50 on the 10k-row board:
   walk 2,031–2,113 µs, optimizer-created Cabin 161–164 µs, declared Cabin
   155–159 µs, every day. What separates the arms is the miss share: the
   autonomous arm's nightly amnesia holds its hit rate at 81–82 % every
   day, while the declared arm's Cabins never forget and climb from 83.5 %
   (day 1) to 90.3 % (day 3) — worth +38 % busy-time throughput over the
   on arm by day 3. The declared arm pays for that in dead weight the
   optimizer refuses to carry: its `board_b` Cabin (3,982 entries) served
   **8 hits on day 3** and will pay write-hook and memory forever, while
   the on arm dropped its own `board_b` structure 65 s after the shape went
   cold and freed the budget.
3. **The advisory-family contract held under the whole lifecycle**: 18
   verification statements per day — hot probes, a zero-row probe, COUNT
   and SUM aggregates — compared **byte for byte** across all three arms,
   through CREATE, EXTEND, DROP and re-CREATE: identical on every day of
   both runs (6/6 PASS).

## Run stamp

| | |
|---|---|
| executed | 2026-08-10, 06:39–06:53 UTC (run 1 06:39, PostgreSQL twin 06:45, run 2 06:48) |
| branch / commit | `feat-assertion` @ `7200fa2` |
| tree | dirty only with this task's own additions (`tools/scenario4_cabinopt_days.py`, `tools/pg_scenario4_cabinopt_days.py`, `bench/docs/README.md`); none links into `kds_server` |
| binary | `build-release/kds_server`, Release. Its mtime (05:16 UTC) predates HEAD's commit (06:13) because `7200fa2` and its parent changed only docs, tools and tests; `cmake --build` re-run this session compiled nothing, and no file linking into the binary changed since the commit it was built at |
| host / device | EC2, 2 vCPU; data files under `~/bench-cabinopt-days/` on the EBS root volume (`nvme0n1p1`, xfs) — **not tmpfs**; ~5 MB per data file at run end |
| machine state | two concurrent Release builds were detected and waited out before starting; both runs began with zero compiler processes. Run 1's day 1 still carried decaying residual load (its off-arm walk CPU fell 18 % from day 1 to day 3); run 2 ran on a quiet machine and its off arm is flat across days, so **run 2 is the primary and run 1 the repeat** |
| server config | one config for all three arms: defaults (`cores = 1`, `durability = group`, checkpoint 5000 ms, `cabins = on`) plus `cabin_optimizer = off` at boot, `cabin_optimizer_snapshot_interval_ms = 500`, `decay_half_life = 5`. Controller thresholds at defaults (θ_create 3.0, θ_drop 0.5, confirm 3, page budget 1024). One server process and one **fresh data file per arm per run** |
| arms | `off` — no Cabin ever; `on` — `SET CABIN_OPTIMIZER ON` at day-1 open (PO8's runtime switch); `declared` — `CREATE CABIN` on all five symbol columns before day 1, optimizer off |
| runs | run 2 = seed 20260811 (primary), run 1 = seed 20260810 (repeat, different hot sets); each 283.5 s wall, 0 client errors, 0 pacing overruns |
| drivers | `tools/scenario4_cabinopt_days.py` and its twin `tools/pg_scenario4_cabinopt_days.py` — what they measure, every flag, and the exact invocations are in `bench/docs/README.md` |
| raw data | `bench/results/cabinopt-days-run1.json`, `-run2.json`, `-pg.json` — every phase summary, the full evidence stream (every `SHOW CABIN_OPTIMIZER` capture with per-entry B/C), the per-block server-CPU ledger and the verify record |

## The scenario, and the time compression

Five BTREE relations of `(id, symbol varchar, qty, price)`: two 10,000-row
**boards** and three **tapes** of 200 / 1,000 / 10,000 rows — the row-set
sweep; each symbol matches exactly 10 rows at load. Each of 3 days runs:
**open** (240 logged inserts into the day's hot board, 60 % on its hot
symbols), **trading** (a 45 s wall-paced session: 2,400 hot-board equality
probes per arm drawn 80/20 hot-set/uniform over 8 hot symbols, 396 probes
per tape with 6 hot values, 240 pk-lookup controls), **close** (COUNT /
SUM / GROUP BY full scans), **overnight** (45 s idle, the on arm polled
every 3 s). Arms interleave per block inside every phase, with one drawn
plan replayed on all three — equal work, identical statements.

The hot set rotates two ways, deliberately, because the controller's
managed unit is a `(relation, column)` **shape**, not a value: the boards
rotate as a-b-a with day 3's hot symbols fully disjoint from day 1's (a
whole shape goes cold — the only rotation that can trigger a DROP), and the
tapes are probed every day with hot **values** rotating disjointly (the
shape stays hot; value rotation can cost only re-observation).

**Compression mapping, stated exactly.** `decay_half_life` runs at 5 s
against the 600 s default — **120×**, so 1 simulated hour = 30 wall
seconds; the 45 s session and 45 s overnight are 1.5 simulated hours each.
The unit the lifecycle rules actually consume is the **half-life**: the
overnight is 9 half-lives of cold, while a real 17.5 h overnight at the
default half-life is **105** half-lives — the simulated night is far
*shorter* than a real one in decay terms, so every nightly-retirement
observation below holds a fortiori at defaults. The snapshot interval runs
at 500 ms against the 10 s default (~90 ticks per session against ~2,340
for a real 6.5 h day). The condensed session is a stand-in for a full
day's traffic, not a scale model of its op count.

## Throughput and the per-phase latency tables

TPS is **busy-time** TPS (timed ops / summed statement latencies, single
connection per arm) because the sessions are wall-paced and wall TPS would
measure the pacing. 4,080 timed ops per arm per day.

| day | off | on | on/off | declared | declared/off |
|---|---|---|---|---|---|
| 1 | 608 | 1,680 | **2.8×** | 1,724 | 2.8× |
| 2 | 602 | 1,729 | **2.9×** | 1,875 | 3.1× |
| 3 | 558 | 1,669 | **3.0×** | 2,309 | **4.1×** |

(Run 1, for the repeat: off 401/494/558 — its day-1 depression is the
residual machine load named in the stamp; on 1,400/1,270/1,635; declared
1,282/1,512/2,256. The cross-arm ratios agree with run 2; the absolute
day-1 numbers do not, which is exactly why the arms interleave.)

Full percentile tables, run 2, all in µs. **Day 1** (hot board `board_a`):

| phase | ops | mean | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| open[off] | 240 | 1,056.5 | 896.9 | 1,017.2 | 1,044.8 | 1,145.7 | 1,291.4 | 2,194 |
| open[on] | 240 | 1,075.3 | 963.9 | 1,022.3 | 1,044.0 | 1,157.7 | 1,335.0 | 6,046 |
| open[declared] | 240 | 1,071.1 | 941.9 | 1,022.5 | 1,046.9 | 1,175.8 | 1,693.2 | 2,448 |
| board[off] | 2,400 | 2,213.4 | 1,933.9 | 1,996.3 | 2,030.7 | 2,975.5 | 3,302.8 | 12,290 |
| board[on] | 2,400 | 713.4 | 107.1 | 153.0 | 163.5 | 2,673.1 | 3,154.8 | 14,847 |
| board[declared] | 2,400 | 677.6 | 100.1 | 150.4 | 158.8 | 3,332.0 | 4,237.7 | 8,969 |
| tape200[off] | 396 | 172.1 | 126.7 | 162.6 | 167.1 | 207.9 | 255.3 | 497 |
| tape200[on] | 396 | 144.3 | 85.0 | 130.2 | 135.1 | 186.2 | 270.4 | 757 |
| tape200[declared] | 396 | 136.4 | 93.8 | 129.4 | 134.1 | 184.5 | 209.8 | 270 |
| tape1k[off] | 396 | 333.5 | 269.8 | 309.0 | 317.7 | 404.8 | 658.7 | 987 |
| tape1k[on] | 396 | 180.2 | 96.9 | 131.5 | 137.5 | 358.4 | 425.8 | 472 |
| tape1k[declared] | 396 | 166.5 | 96.3 | 129.6 | 134.1 | 423.5 | 492.6 | 730 |
| tape10k[off] | 396 | 2,248.7 | 1,901.8 | 1,956.8 | 2,000.9 | 3,000.1 | 4,708.4 | 6,579 |
| tape10k[on] | 396 | 678.0 | 94.8 | 135.1 | 143.6 | 2,178.4 | 2,833.2 | 2,913 |
| tape10k[declared] | 396 | 770.1 | 99.1 | 133.3 | 139.6 | 3,951.9 | 4,749.3 | 11,877 |
| pk[off] | 240 | 121.6 | 74.1 | 109.1 | 123.6 | 156.0 | 215.4 | 310 |
| pk[on] | 240 | 130.1 | 90.8 | 122.2 | 125.7 | 153.4 | 183.3 | 253 |
| pk[declared] | 240 | 127.1 | 75.2 | 120.7 | 123.9 | 156.4 | 183.2 | 188 |
| close[off] | 12 | 2,494.5 | 1,487.3 | 1,523.5 | 1,848.4 | 5,936.2 | 5,936.2 | 5,936 |
| close[on] | 12 | 2,491.9 | 1,509.9 | 1,547.5 | 1,894.7 | 5,205.3 | 5,205.3 | 5,205 |
| close[declared] | 12 | 2,370.9 | 1,588.0 | 1,599.5 | 1,869.2 | 4,035.0 | 4,035.0 | 4,035 |

**Day 2** (hot board `board_b` — the rotation day):

| phase | ops | mean | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| open[off] | 240 | 1,055.5 | 939.8 | 1,017.4 | 1,044.9 | 1,154.3 | 1,313.5 | 2,081 |
| open[on] | 240 | 1,098.8 | 954.3 | 1,026.1 | 1,056.3 | 1,245.3 | 1,763.2 | 5,632 |
| open[declared] | 240 | 1,077.0 | 913.6 | 1,013.9 | 1,048.3 | 1,185.4 | 1,696.8 | 4,487 |
| board[off] | 2,400 | 2,258.4 | 1,923.6 | 2,003.2 | 2,037.4 | 3,023.7 | 4,946.6 | 9,584 |
| board[on] | 2,400 | 687.4 | 103.8 | 151.9 | 160.7 | 2,684.0 | 3,195.2 | 5,197 |
| board[declared] | 2,400 | 638.7 | 97.2 | 149.0 | 157.8 | 3,079.9 | 4,167.9 | 7,986 |
| tape200[off] | 396 | 177.7 | 137.6 | 161.9 | 166.6 | 218.6 | 311.0 | 1,431 |
| tape200[on] | 396 | 145.0 | 96.5 | 131.8 | 136.8 | 186.8 | 209.8 | 280 |
| tape200[declared] | 396 | 140.3 | 94.7 | 130.7 | 135.2 | 163.6 | 232.1 | 513 |
| tape1k[off] | 396 | 338.8 | 281.8 | 309.8 | 322.9 | 415.9 | 628.8 | 799 |
| tape1k[on] | 396 | 192.7 | 86.1 | 132.6 | 138.3 | 393.3 | 511.3 | 1,979 |
| tape1k[declared] | 396 | 158.5 | 96.9 | 130.0 | 134.6 | 412.1 | 489.0 | 625 |
| tape10k[off] | 396 | 2,133.9 | 1,888.0 | 1,947.6 | 1,970.7 | 2,913.9 | 3,172.6 | 7,306 |
| tape10k[on] | 396 | 641.2 | 86.6 | 133.6 | 141.6 | 2,510.1 | 3,265.7 | 3,824 |
| tape10k[declared] | 396 | 519.8 | 98.9 | 132.7 | 138.6 | 2,945.1 | 3,874.8 | 4,519 |
| pk[off] | 240 | 132.4 | 73.8 | 118.5 | 125.1 | 173.8 | 280.4 | 1,127 |
| pk[on] | 240 | 124.3 | 84.6 | 112.1 | 123.5 | 156.7 | 204.5 | 361 |
| pk[declared] | 240 | 130.3 | 74.6 | 121.9 | 125.5 | 156.6 | 218.5 | 382 |
| close[off] | 12 | 2,310.7 | 1,483.6 | 1,553.4 | 1,850.1 | 4,357.6 | 4,357.6 | 4,358 |
| close[on] | 12 | 2,373.0 | 1,566.2 | 1,569.0 | 1,933.7 | 4,719.3 | 4,719.3 | 4,719 |
| close[declared] | 12 | 2,412.3 | 1,536.8 | 1,577.3 | 1,863.0 | 4,843.2 | 4,843.2 | 4,843 |

**Day 3** (`board_a` again, disjoint hot symbols — the re-nomination day):

| phase | ops | mean | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| open[off] | 240 | 567.5 | 412.4 | 482.8 | 526.1 | 872.6 | 930.6 | 974 |
| open[on] | 240 | 654.4 | 409.4 | 482.9 | 539.3 | 1,078.2 | 1,160.4 | 1,297 |
| open[declared] | 240 | 673.4 | 404.7 | 489.8 | 552.7 | 1,117.2 | 1,286.9 | 1,872 |
| board[off] | 2,400 | 2,498.0 | 1,964.7 | 2,050.0 | 2,113.1 | 3,528.6 | 6,375.6 | 14,206 |
| board[on] | 2,400 | 755.9 | 102.4 | 148.7 | 161.8 | 2,904.5 | 4,083.6 | 8,579 |
| board[declared] | 2,400 | 485.8 | 99.7 | 141.0 | 154.6 | 3,040.1 | 4,267.3 | 8,121 |
| tape200[off] | 396 | 173.3 | 129.2 | 156.5 | 165.1 | 224.7 | 359.1 | 466 |
| tape200[on] | 396 | 144.6 | 95.0 | 107.3 | 133.3 | 214.6 | 466.2 | 854 |
| tape200[declared] | 396 | 140.5 | 84.3 | 110.5 | 131.5 | 162.7 | 705.7 | 1,262 |
| tape1k[off] | 396 | 349.5 | 284.5 | 311.3 | 331.4 | 420.6 | 492.2 | 3,659 |
| tape1k[on] | 396 | 185.3 | 84.9 | 106.4 | 132.1 | 415.0 | 845.8 | 1,363 |
| tape1k[declared] | 396 | 154.6 | 95.1 | 128.5 | 132.1 | 369.0 | 726.7 | 1,257 |
| tape10k[off] | 396 | 2,305.7 | 1,884.2 | 1,953.0 | 2,049.5 | 2,958.8 | 3,827.9 | 5,395 |
| tape10k[on] | 396 | 717.4 | 90.7 | 130.8 | 141.1 | 2,765.1 | 3,009.9 | 5,005 |
| tape10k[declared] | 396 | 664.5 | 97.1 | 120.6 | 139.8 | 3,661.9 | 7,220.7 | 10,233 |
| pk[off] | 240 | 154.9 | 75.3 | 108.9 | 126.3 | 164.0 | 202.4 | 7,495 |
| pk[on] | 240 | 124.4 | 82.7 | 102.3 | 123.1 | 156.1 | 169.7 | 1,010 |
| pk[declared] | 240 | 126.5 | 74.4 | 101.4 | 121.6 | 163.7 | 267.2 | 1,460 |
| close[off] | 12 | 2,351.2 | 1,496.7 | 1,542.1 | 1,945.6 | 4,167.2 | 4,167.2 | 4,167 |
| close[on] | 12 | 2,437.1 | 1,514.9 | 1,577.1 | 1,951.2 | 4,971.3 | 4,971.3 | 4,971 |
| close[declared] | 12 | 2,419.4 | 1,532.7 | 1,564.4 | 1,915.7 | 4,570.7 | 4,570.7 | 4,571 |

Three controls inside these tables. The **pk lookups** touch no Cabin: p50
121.6–126.3 across all nine arm-days, a ≤ 4.7 µs spread — the noise floor;
any p50 delta below ~5 µs in this document is not a finding. The **close
aggregates** read whole relations through the scan path Cabins do not
serve: p50 1,848–1,951 across arms, flat. And the **open inserts** carry
the write hook on the on/declared arms and not on off: p50 deltas are
−0.8 to +26.6 µs on a ~1,045 µs fsync-bound statement, inside the phase's
own day-to-day swing — **no resolvable write-hook cost**, consistent with
`bench/results-cabin.md`. (Day 3's open p50 halved to ~530 µs on *all
three* arms at once — a device-side shift on the EBS volume, not an
engine effect; it moves no comparison because every arm rode it.)

**The row-set sweep** is the tape axis: `tape_200` / `tape_1k` /
`tape_10k` hold 200 / 1,000 / 10,000 rows (`--tape-probes 396` per size
per arm-day). Walk p50 grows linearly with size — 167 / 318 / 2,001 µs —
while the served probe is flat at 133–144 µs at every size: the Cabin
replaces a per-row cost with a fixed one, and the crossover sits below 200
rows even here (167 → 135 µs is 1.24× at three data pages — the same "no
size floor worth defending" the single-shot run measured at 1.17×).

## The lifecycle, as the controller reported it

`SHOW CABIN_OPTIMIZER` captured untimed at every phase boundary and every
3 s overnight; t is wall seconds from run start (run 2). B/C is the
per-entry `benefit_q16 / cost_q16` ratio; the DROP threshold is 0.5 with a
2-half-life (10 s) cooldown.

| t (s) | event |
|---|---|
| 10.9 | `SET CABIN_OPTIMIZER ON` at day-1 open; `ticks=0 managed=0` |
| ≤ 20.1 | **CREATE ×4** — `board_a` + all three tapes, inside the session's first 3 blocks; the board entry's cost floor is 143.0 (q16→dec), exactly the 143 pages the off-arm's ANALYZE prints for the same walk |
| 20–57 | ACTIVE, serving; **EXTENDs** (103 by day-1 close) as the 20 % uniform tail keeps observing new values; board B/C 294–4,812 through the session |
| 83.9 | `tape_200` **DECAYING** (B/C 0.50) — 27 s ≈ 5.4 half-lives cold |
| 95.9 / 101.9 / 102.6 | tapes **DROP** (39–46 s cold); `pages_committed` 13 → 8 |
| 102.6 | `board_a` survives night 1 still ACTIVE at B/C 0.64 — the largest B/C had the most half-lives to burn |
| 111.1 | day-2 trading: **CREATE ×4** gen-2 (`board_b` + tapes re-created); `board_a` **DECAYING** (B/C 0.19) |
| ≤ 122.2 | `board_a` **DROP, mid-day-2 trading** — 65 s ≈ 13 half-lives after its last probe: the stale shape retired *as day 2 took over*, which is the scenario's title event |
| 174.8–192.8 | overnight 2: gen-2 tapes DECAYING then DROP (drops 5–6); `board_b` rides its B/C down from 350 |
| 201.6 | day-3 trading: `board_a` **re-CREATED as `cabin_id=9`** — re-nomination from scratch after its own DROP (PO5's "starts from scratch as CANDIDATE", now exercised under traffic); `board_b` DECAYING at B/C 0.22 |
| ≤ 212.8 | `board_b` **DROP** (65 s cold) |
| 265.4–283.4 | overnight 3: gen-3 tapes DECAYING → 2 more drops; `board_a` gen-3 still ACTIVE at run end, B/C 0.78 and falling |

End state: `creates=12 extends=252 heals=0 drops=10 deferred=0 failures=0`,
546 ticks, peak `pages_committed` 15 of the 1,024 budget. Every DROP is the
sustained-decay path by its observed inputs — B/C sat below 0.5 through the
cooldown in the trace above, no HEAL was ever attempted (quality-collapse
needs one), and the budget never came within an order of magnitude of
forcing a swap. Run 1's timeline is the same cycle shifted by its noisier
day 1 (12 creates, 10 drops by its run end, `board_a` dropped at its
t=122.1 mid-day-2 and re-created day 3).

What a DROP actually freed, measured at the boundaries: overnight 1 took
`pages_committed` from 13 to 8 and the managed store from four Cabins
(3,161 entries) to one; the final on-arm store holds 2 Cabins / 2,409
entries against the declared arm's permanent 5 / 13,503. What it cost:
the next morning's first ~2 blocks walk until the CREATE lands (~8 s of
session here, 3 snapshots at 500 ms — the same 3-snapshot confirm the
single-shot case measured at 1.4 s), plus n=2 re-observation of every
value, visible below as a hit rate that never climbs.

`SHOW CABINS` per day (hits/misses are per-day deltas; auto Cabins observe
at n=2, declared at n=1 — `feat-cabin.md`'s split, visible here as
`misses = observed` exactly on every declared row):

| day | on: board hit rate | declared: board hit rate | on: tape200/1k/10k | declared: tape200/1k/10k |
|---|---|---|---|---|
| 1 | 81.3 % (1,789/412) | 83.5 % (2,005/396) | 92.3 / 85.7 / 79.4 % | 95.2 / 90.2 / 81.9 % |
| 2 | 82.2 % (1,810/391) | 84.1 % (2,020/381) | 90.9 / 84.3 / 82.7 % | 99.7 / 93.0 / 87.2 % |
| 3 | 81.7 % (1,799/402) | **90.3 %** (2,168/233) | 91.2 / 84.8 / 80.8 % | 100 / 96.5 / 85.5 % |

The on arm's rate is pinned at ~81–82 % — each morning's Cabin starts
empty, so the 20 % uniform tail re-pays observation daily. The declared
arm accumulates: day 3's probes on `board_a` find 397 values already
observed since day 1, and `tape_200`'s 20-value domain is fully observed by
day 2 (100 % thereafter). ANALYZE confirms the plan shape on every day and
arm: `FilterScan … pages=143–146` (off) against
`CabinProbe … cabin_optimizer=true, pages=2×rows` (on, PHY06's managed
mark; the declared arm's probe is identical minus the mark).

## Where the latency goes

The measured unit is one statement round trip from a Python client, single
connection. Decomposition for the day-2 hot-board probe (run 2), server
CPU from `/proc/<pid>/stat` per interleave block:

| wait type | off (walk, 2,258 µs mean) | on (81 % served, 687 µs mean) | how measured |
|---|---|---|---|
| server CPU | 2,113 µs (94 %) | 621 µs (90 %) | per-block `/proc` deltas ÷ ops |
| client + socket | ~120–145 µs | ~66–120 µs of the mean | remainder; consistent with the pk control (121–126 µs p50, which itself contains a ~30 µs descent) and this harness's documented ~100 µs floor |
| read (page-fetch) wait | 0 | 0 | all pages resident after load; nothing evicts engine-wide, so a warm walk does no I/O |
| durability / commit wait | n/a | n/a | probes are read-only. The open inserts are the durability row: ~1,045 µs p50, group-commit fsync on EBS (a batch of one is a batch), identical across arms |
| lock / conflict wait | n/a | n/a | single connection per server; core-local latches have no contention at `cores = 1` |

The on arm's mean is a mixture, and the mixture is the point: ~82 % of
consulted probes at ~160 µs p50, ~18 % misses plus the first pre-CREATE
block at the full ~2.1 ms walk price, mean 687 µs — the walking minority
owns about three-quarters of the phase's total time. The
same arithmetic is why busy TPS improves only 2.9× while p50 improves
12.7×. What cannot be decomposed further with today's surfaces: the
in-server split between queueing and execution (`docs/observability.md` is
still a proposal), and the declared arm's day-3 CPU advantage (408 vs 662
µs/op) beyond attributing it to the 90.3 % vs 81.7 % hit rate.

## Against PostgreSQL

PostgreSQL 17.10, the scratch cluster of `tools/pg_setup.sh` on port 15433
at defaults, same five relations, same rows from the same generator, same
day-1 statement stream, via `tools/pg_scenario4_cabinopt_days.py` —
measured this session. The twin runs **one** unpaced day, because at
defaults PostgreSQL builds nothing for the hot predicate and retires
nothing: the rotation axis has nothing to act on, and day 300's plan is
day 1's. `EXPLAIN (ANALYZE, BUFFERS)` confirmed `Seq Scan` for the board
probe (76 buffers), the tape probe (74) and the zero-row probe (76 —
absence costs PostgreSQL a full scan, where an observed Cabin value
answers from the entry set).

| phase | ops | mean | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| open[pg] | 240 | 1,202.0 | 1,043.2 | 1,115.2 | 1,144.0 | 1,364.7 | 2,374.9 | 4,443 |
| board[pg] | 2,400 | 1,841.8 | 1,521.2 | 1,635.6 | 1,677.8 | 2,476.4 | 3,062.5 | 11,025 |
| tape200[pg] | 396 | 271.4 | 199.2 | 241.2 | 250.2 | 318.2 | 551.3 | 2,822 |
| tape1k[pg] | 396 | 384.4 | 335.9 | 346.6 | 360.3 | 509.5 | 734.7 | 832 |
| tape10k[pg] | 396 | 1,724.6 | 1,494.7 | 1,539.6 | 1,574.6 | 2,370.5 | 2,639.6 | 3,876 |
| pk[pg] | 240 | 208.4 | 163.4 | 187.4 | 193.3 | 253.3 | 350.2 | 988 |
| close[pg] | 12 | 2,717.1 | 1,020.9 | 1,093.5 | 1,222.1 | 7,544.6 | 7,544.6 | 7,545 |

Side by side on p50 (each column carries its own client's socket cost —
~100 µs KDS, ~150–190 µs PG per the pk controls):

| shape | KDS walk | PG seq scan | KDS served (on) | served vs PG |
|---|---|---|---|---|
| board 10k | 2,030.7 µs | 1,677.8 µs | 163.5 µs | **10.3×** |
| tape 10k | 2,000.9 µs | 1,574.6 µs | 143.6 µs | **11.0×** |
| tape 1k | 317.7 µs | 360.3 µs | 137.5 µs | **2.6×** |
| tape 200 | 167.1 µs | 250.2 µs | 135.1 µs | **1.9×** |

Two honest notes, both repeats of the single-shot document's. Before any
Cabin exists, **PostgreSQL's 10k walk beats KDS's by ~18–21 %** — 74–76
buffers against 139–146 pages for the same rows, the 64-byte
`inline_cell_width` padding tax on row density. And an operator who
declared an index on either engine would beat both walks; the at-defaults
comparison is the honest one for a feature whose point is acting without
the operator — which is also why the twin has no third arm: PostgreSQL has
no counterpart to either the optimizer or a declarable Cabin.

## What this run says about the engine

**DROP is not an edge case of the cost model — it is the model's steady
state under any workload with a day/night rhythm.** B is decayed frequency
times saved pages; C keeps the constant floor `P_rel / T_amort`. So a
Cabin's post-close survival time is `log2(B/C at close) + 2` half-lives —
measured here as 39–65 s at B/C 20–350, in exact agreement with that
arithmetic — and any real overnight (105 half-lives at defaults) retires
everything, every night. The controller as configured is therefore not a
curator of long-lived structures; it is a **daily rebuild loop** whose
morning cost is 3 confirm snapshots plus one relation scan per Cabin and
whose nightly payoff is a zeroed ledger. If long-lived Cabins are ever
wanted across quiet periods, the lever is already named in the spec:
`T_amort` (`amort_windows`) sets C's floor, and a longer amortization
window would let B/C stay above θ_drop through a night — a decision for
`feat-physical-optimizer.md` §II.4's owner, not something this benchmark
settles.

*(Settled the same day, operator-decided: `T_amort` was ratified at **64
half-lives** — `cabin_optimizer_amort_windows`, cooldown 21 h 20 m at the
default half-life — so Cabins now survive a market overnight and recover
DECAYING→ACTIVE on the morning rebound, while a weekend still drops them.
This run's arms all measured the pre-ratification default of 1; a rerun at
64 would show the on arm's day-2/day-3 hit rates approaching the declared
arm's, since overnight amnesia was the entire gap. See spec §II.4.)*

**Autonomy's price is amnesia; a declaration's price is immortality.** On
the hot median they are the same structure (155 vs 162 µs p50 — the
single-shot document's "a self-created Cabin is a declared Cabin plus a
decision" surviving three days of lifecycle). The measurable differences
are: the declared arm's hit rate climbs (83.5 → 90.3 %) because its store
never forgets, worth +38 % busy TPS by day 3; and the declared arm carries
every stale structure forever — `board_b`'s 3,982 entries served 8 probes
on day 3 and will bill its write hook and memory indefinitely, the exact
shape `CABIN AUTO`'s un-earning threshold (`feat-cabin.md` §8.1) was left
open for. At five relations an operator can declare everything and win;
the autonomous arm's case is the schema where "declare everything" stops
being a plan, and this run prices what its nightly amnesia costs: about
9 percentage points of hit rate on an 80/20 workload.

**Only shape rotation exercises the lifecycle; value rotation is
absorbed by observation.** The tapes' hot values rotated disjointly every
day and their Cabins never dropped *for that reason* — new values cost
n=2 observation inside the same structure. What killed the tape Cabins
nightly was idle decay of the whole shape. The distinction matters for
expectations: a workload whose hot *values* churn does not thrash the
controller; a workload whose hot *predicates* migrate across relations
or columns does — and cleanly, as the mid-session DROP of yesterday's
board shows.

**Where the single-shot 10.9× went.** This scenario reproduces it on the
median (12.4–13.7× at 10k rows) and dilutes it everywhere else: 2.8–3.0×
on busy TPS, 3.2–3.4× on trading-phase server CPU. The dilution is the hit
rate — 81–82 % here against 99.8 % single-shot — and the hit rate is a
property of the workload's draw distribution, which is the same conclusion
`bench/results-cabin.md` reached from the opposite direction (23.9 % hit
rate, a wash). Across the three documents the line is now continuous:
uniform draw ≈ wash, 80/20 draw ≈ 3× throughput / 13× median, single-value
draw ≈ 11–19×. The Cabin's value on a workload is readable off one number
that `SHOW CABINS` already prints.

**What was not exercised, stated so the next scenario can aim at it.**
DECAYING→ACTIVE recovery never fired: every shape that went DECAYING was
genuinely dead (its next hot day arrived long after the 2-half-life
cooldown), so the recovery rung of PO5's ladder still has no
under-traffic data point — it needs a lull shorter than the cooldown, a
lunch hour rather than a night. HEAL and hint failures stayed at zero
because nothing in this scenario moves a row (no UPDATE/DELETE, no
relayout — the page epochs never bump), and the budget-swap DROP reason
needs a page budget under pressure (peak here: 15 of 1,024). Aggregates
over the served relations were measured (close phase, flat across arms)
but aggregate-over-CabinProbe shapes were not part of the probe mix.
