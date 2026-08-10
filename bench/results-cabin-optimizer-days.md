# The cabin optimizer over three business days: the amortization window *is* the lifecycle

One number — `cabin_optimizer_amort_windows`, the `T_amort` of
`docs/feat-physical-optimizer.md` §II.4 — decides whether a Cabin lives for
a session or for a week, and this document measures the same three-day
workload at both settings it has had.

- **Part I — `T_amort = 1`** (measured 2026-08-10 06:39–06:53 UTC at
  `7200fa2`, the pre-ratification default). The lifecycle is a nightly
  rebuild loop.
- **Part II — `T_amort = 64`** (measured 2026-08-10 08:06–08:41 UTC at
  `6ee1ce4`, the ratified and shipped default). Every Cabin survives every
  night, `DECAYING → ACTIVE` morning recovery fires for the first time
  under traffic, and the autonomous arm's throughput catches the declared
  arm's by day 3.

The two parts run the same driver, the same seeds, the same statement
stream and the same arms, so the tables are comparable line for line. Each
part carries its own stamp; neither describes the other's engine state.

---

# Part I — `T_amort = 1`: nightly retirement is the norm, not the edge case

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
half-lives** — `cabin_optimizer_amort_windows`. Every arm above measured
the pre-ratification default of 1. **Part II measures the ratified
setting** on this same workload and reports what changed.)*

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

---

# Part II — `T_amort = 64`: the ratified window, measured

Part I priced the nightly rebuild loop and named `T_amort` as the lever
without pulling it. The operator pulled it: `cabin_optimizer_amort_windows`
now defaults to **64** whole decay half-lives, which makes the admission
bar `P_rel / 64` and the DROP cooldown `2 × 64 = 128` half-lives — 21 h 20 m
at the shipped 600 s half-life. This part reruns the identical workload at
that setting. Four findings:

1. **The nightly rebuild loop is gone.** Across three days the controller
   performed **5 CREATEs, 172 EXTENDs, 0 HEALs, 0 DROPs, 0 deferrals, 0
   failures** — against Part I's 12 CREATEs and 10 DROPs on the same
   statement stream. No structure was ever rebuilt; every Cabin the
   controller made on day 1 was still serving at run end.
2. **`DECAYING → ACTIVE` morning recovery fired, which Part I recorded as
   never exercised.** `board_a`'s Cabin went ACTIVE on day 1, decayed to
   DECAYING mid-day-2 once its shape had been silent ~15 half-lives, and
   **recovered to ACTIVE in day 3's third interleave block keeping
   `cabin_id=1` and all 2,091 of its day-1 entries** — no rebuild, no
   re-observation, no pre-CREATE walk. Both runs show it at the same block.
3. **The autonomous arm caught the declared arm.** Day-3 busy TPS: on
   **2,405** against declared **2,327** and off 613 — the on arm is *ahead*
   by 3.4 %, where at `T_amort = 1` the declared arm led it by 38 % on the
   same day. Day-3 hot-board server CPU is 388 µs/op on the on arm against
   392 on the declared one: the same work, arrived at without an operator.
   The board hit rate climbs 81.3 → 87.2 % over the three days where Part
   I's was pinned at 81–82 %.
4. **The key reaches the live cooldown, and a long enough silence still
   drops.** A two-server control measured the DROP cooldown at
   **128.14 s against a predicted 128.0 s** at `amort_windows = 64`, and
   3.00 s against 2.0 s at the control setting of 1 — the weekend case,
   confirmed as a measurement rather than a unit-test assertion.

## Run stamp

| | |
|---|---|
| executed | 2026-08-10, 08:06–08:41 UTC (cooldown check 08:06, run 2 08:11, PostgreSQL twin 08:35, run 1 08:36) |
| branch / commit | `feat-assertion` @ `6ee1ce4` |
| tree | clean at session start; dirty at write time only with this task's own additions (`tools/cabinopt_cooldown_check.py`, four `bench/results/*.json`, this section). None links into `kds_server` |
| binary | `build-release/kds_server`, Release, mtime 07:27:36 UTC. HEAD's commit timestamp (07:47:04) is **later** than the binary, which normally means the binary is stale — it is not here: every source the ratification commit `1f29883` touched was written at 07:21–07:22, the build at 07:27 followed them, `cmake --build` re-run this session compiled nothing, and `find include src CMakeLists.txt -newer build-release/kds_server` is empty. The binary is at HEAD; the commit was simply made after the build |
| host / device | EC2, 2 vCPU; data files under `~/bench-cabinopt-days2/` and `~/bench-cabinopt-cool/` on the EBS root volume (`nvme0n1p1`, xfs) — **not tmpfs**; ~5 MB per data file at run end |
| machine state | a concurrent Release build (two `cc1plus` saturating both vCPUs) appeared at ~08:23 and invalidated one repeat run and one PostgreSQL twin; **both were discarded and re-run** after 60 consecutive seconds of measured quiet (no compiler, 1-minute load < 0.8). The discarded repeat's off arm read **203 TPS on day 1 against 623 on the clean re-run** — a 3.1× distortion the driver's own output cannot see. Run 2 preceded the build and its off arm is flat at 619/619/613 across three days, which is the in-run evidence that it was clean |
| server config | one file for all three arms: `cabin_optimizer = off` at boot, `cabin_optimizer_snapshot_interval_ms = 500`, `decay_half_life = 5`, **`cabin_optimizer_amort_windows = 64`** — named explicitly although 64 is now the default, so the stamp reads off the file rather than off a memory of what shipped. Everything else default: `cores = 1`, `durability = group`, `cabins = on`, θ_create 3.0, θ_drop 0.5, confirm 3, page budget 1024. One server process and one **fresh data file per arm per run** |
| arms | identical to Part I — `off` (no Cabin ever), `on` (`SET CABIN_OPTIMIZER ON` at day-1 open), `declared` (`CREATE CABIN` on all five symbol columns before day 1, optimizer off) |
| runs | run 2 = seed 20260811 (**primary**), run 1 = seed 20260810 (repeat, different hot sets); each 283 s wall, 0 client errors, 0 pacing overruns |
| drivers | `tools/scenario4_cabinopt_days.py` **unchanged** — no flag was needed, because `T_amort` is a server key and not a workload parameter — its twin `tools/pg_scenario4_cabinopt_days.py`, and the new `tools/cabinopt_cooldown_check.py`. All three are documented in `bench/docs/README.md` |
| raw data | `bench/results/cabinopt-days-a64-run1.json`, `-run2.json`, `-pg.json`, and `bench/results/cabinopt-cooldown-check.json` |

## First: does the key reach the controller, or only its struct?

A results file measured at a new default is worthless if the key is
accepted at startup and then ignored, so the first measurement is a
behavioural one, not a startup log line. Two servers identical in every
respect except `cabin_optimizer_amort_windows`, warmed on the same hot
probe over the same 10,000-row relation until each controller held an
ACTIVE Cabin of its own, then left in silence and polled once a second
(`tools/cabinopt_cooldown_check.py`). Both ran `decay_half_life = 1`, which
is the affordability knob and deliberately not what is under test: it puts
the ratified cooldown at 128 s instead of 21 h 20 m and leaves the ratio
between the arms — the thing `amort_windows` sets — untouched.

| arm | `amort_windows` | C (cost floor, pages) | ACTIVE at | DECAYING at | DROP at | observed cooldown | predicted `2 × T_amort` half-lives |
|---|---|---|---|---|---|---|---|
| control | 1 | 139.000 | 0.74 s of warm | 9.0 s of silence | 12.0 s | 3.00 s | 2.0 s |
| test | 64 | 2.172 | 0.79 s of warm | 15.0 s of silence | 143.2 s | **128.14 s** | **128.0 s** |

Three things are settled by that table. The key is **live** — the same
relation, the same traffic and the same silence produce a DROP 131 s apart.
The cooldown is the specified multiple and not some other number: 128.14 s
measured against 128.0 s predicted, inside the 1 s poll granularity (the
control's 3.00 against 2.0 is that same granularity at a scale where one
poll is the whole answer). And **DROP still fires at 64** given a silence
that exceeds the cooldown — the weekend case the ratification kept
deliberately, measured rather than asserted.

The cost column is the other half of the same key. `C = P_rel / T_amort`
read 139.000 on the control and **2.172 = 139/64** on the test arm, so
raising the window lowered the admission bar by exactly the factor it
raised the cooldown by — PO2's "both sides of the model on one clock", now
observed on a live server rather than in a unit test.

The DECAYING onset moved with it, from 9 s to 15 s: **6 half-lives later,
and log2(64) = 6**. The whole state machine moved by one coherent amount.

One engine detail worth carrying out of that trace. The test arm's benefit
score reached **exactly zero** at 16 s of silence and stayed there for the
remaining 127 s. The R1 score is Q24.8 and underflows to zero after roughly
16 half-lives of silence, so past that point the score can no longer
distinguish "cold for 20 half-lives" from "cold for 120": for **127 of the
143 seconds of silence — 89 % of it — the cooldown timer was the only live
input to the decision.** That is exactly what the ratification intended,
but it means the only thing separating an overnight from a weekend at
`T_amort = 64` is a wall clock, not accumulated evidence.

## The time compression, re-justified at the new setting

The compression is **unchanged from Part I** — `decay_half_life` 5 s
against the 600 s default (120×), snapshot cadence 500 ms against 10 s,
45 s sessions and 45 s overnights — and that choice was made deliberately
rather than inherited, because at `T_amort = 64` the arithmetic changes
what the short night models.

At a 5 s half-life the DROP cooldown alone is 2 × 64 × 5 = **640 wall
seconds**, longer than a whole three-day arm at this pacing. So a `DROP`
anywhere inside the matrix was arithmetically impossible before the run
started, and saying so in advance is part of reading the result: the matrix
can measure *survival and recovery*, and it cannot measure retirement. That
is why the cooldown check above exists as a separate, targeted measurement
— it buys the retirement evidence at 128 s instead of 640, and it buys the
key-liveness proof the matrix cannot give.

Two reasons the compressed night stays representative for what the matrix
*does* measure, and one way it does not:

- **The survival verdict transfers.** The compressed night is 9 half-lives
  and a real market overnight at the shipped 600 s half-life is ~105. Both
  sit under the 128-half-life cooldown, so both end in survival, and the
  compressed one survives a fortiori.
- **Morning hit-rate behaviour is compression-invariant.** Entry sets do
  not decay — only scores do. A Cabin that survives the night wakes up with
  exactly the entries it went to sleep with, whether the night was 45 s or
  17.5 h, so every hit-rate number below is a property of the traffic and
  not of the clock.
- **The compressed night understates the DECAYING dwell.** At `T_amort = 64`
  a shape needs 15–19 half-lives of silence to enter DECAYING, so a 9
  half-life night does not reach it while a 105 half-life night does: real
  Cabins spend most of a real night in DECAYING and these spent it in
  ACTIVE. That costs nothing for survival (both states serve identically —
  DECAYING is a controller label, not a serving mode) and it is why the
  morning-recovery evidence below comes from `board_a`'s **whole cold day**
  (~20+ half-lives) rather than from a night.

Re-proportioning to model the real night-to-cooldown ratio was considered
and rejected on two grounds. `decay_half_life` is a whole-second key, so
the closest fit is 1 s, which would compress the *session* to the point
where B accumulates and decays inside a single interleave block — changing
what the session measures in order to fix what the night measures. And it
would break the line-for-line comparability with Part I that is this
document's reason for existing.

## Throughput: the autonomous arm catches the declared one

Busy-time TPS (timed ops ÷ summed statement latencies, single connection
per arm), 4,080 timed ops per arm per day. Run 2, the primary:

| day | off | on | on/off | declared | declared/off | on vs declared |
|---|---|---|---|---|---|---|
| 1 | 619 | 1,717 | 2.8× | 1,850 | 3.0× | −7.2 % |
| 2 | 619 | 1,837 | 3.0× | 1,858 | 3.0× | −1.1 % |
| 3 | 613 | **2,405** | **3.9×** | 2,327 | 3.8× | **+3.4 %** |

The repeat (run 1, seed 20260810) reproduces the shape: off 623/568/605, on
1,697/1,730/2,313, declared 1,766/1,877/2,267 — the on arm again crossing
above the declared arm on day 3 (+2.0 %).

Against Part I's figures on the identical statement stream (off
608/602/558; on 1,680/1,729/1,669; declared 1,724/1,875/2,309), the change
is confined to one arm and one direction: the **on** arm's day-3 TPS went
1,669 → 2,405, **+44 %**, while the off and declared arms moved inside
their own repeat spread. The declared arm cannot benefit from `T_amort` —
it has no managed Cabin — and it did not, which is this configuration
change's own control.

The two parts are separate sessions, so the cross-part comparison needs its
drift bounded, and the off arm is what bounds it: identical code and
identical statements in both, reading 608/602/558 there and 619/619/613
here — under 10 % apart, against a 44 % change on the arm under test.

The on arm's day-1 deficit against declared (−7.2 %) is the one thing
`T_amort` does not fix and is not meant to: on day 1 the autonomous arm
still spends its first ~3 confirm snapshots walking before any Cabin
exists, and still observes at n=2 where a declaration observes at n=1.

## Per-phase latency, run 2

All values µs. **Day 1** (hot board `board_a`):

| phase | ops | mean | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| open[off] | 240 | 1,048.7 | 933.6 | 1,018.7 | 1,039.8 | 1,141.4 | 1,232.5 | 1,489 |
| open[on] | 240 | 1,039.0 | 940.0 | 1,014.9 | 1,037.1 | 1,109.5 | 1,231.4 | 1,248 |
| open[declared] | 240 | 1,048.5 | 927.9 | 1,017.6 | 1,044.2 | 1,118.6 | 1,245.7 | 1,846 |
| board[off] | 2,400 | 2,179.2 | 2,018.9 | 2,080.0 | 2,098.6 | 2,598.7 | 3,237.6 | 10,931 |
| board[on] | 2,400 | 690.5 | 122.4 | 152.4 | 160.6 | 2,218.2 | 3,016.2 | 7,024 |
| board[declared] | 2,400 | 632.2 | 93.3 | 150.5 | 157.4 | 2,979.5 | 3,308.2 | 4,491 |
| tape200[off] | 396 | 179.2 | 130.2 | 163.3 | 168.6 | 203.1 | 360.0 | 2,182 |
| tape200[on] | 396 | 154.3 | 98.6 | 130.1 | 135.2 | 184.2 | 226.5 | 4,811 |
| tape200[declared] | 396 | 140.4 | 96.2 | 129.8 | 133.9 | 189.5 | 236.7 | 494 |
| tape1k[off] | 396 | 331.5 | 278.2 | 317.7 | 323.7 | 363.3 | 482.7 | 805 |
| tape1k[on] | 396 | 185.7 | 95.4 | 130.9 | 135.7 | 394.7 | 466.0 | 874 |
| tape1k[declared] | 396 | 164.2 | 96.3 | 129.6 | 133.9 | 428.3 | 498.3 | 712 |
| tape10k[off] | 396 | 2,134.9 | 1,986.7 | 2,024.0 | 2,039.6 | 2,575.6 | 3,282.3 | 7,191 |
| tape10k[on] | 396 | 701.1 | 95.8 | 134.5 | 140.4 | 2,145.7 | 2,877.1 | 9,373 |
| tape10k[declared] | 396 | 655.0 | 97.5 | 133.4 | 139.1 | 2,943.0 | 3,715.2 | 4,538 |
| pk[off] | 240 | 126.1 | 75.1 | 120.4 | 126.3 | 149.7 | 230.6 | 448 |
| pk[on] | 240 | 128.0 | 83.6 | 121.1 | 125.0 | 155.4 | 178.9 | 269 |
| pk[declared] | 240 | 122.7 | 77.3 | 120.2 | 124.3 | 147.7 | 178.1 | 182 |
| close[off] | 12 | 2,283.1 | 1,500.8 | 1,560.5 | 1,688.2 | 4,070.9 | 4,070.9 | 4,071 |
| close[on] | 12 | 2,264.6 | 1,549.5 | 1,567.8 | 1,584.1 | 4,074.5 | 4,074.5 | 4,074 |
| close[declared] | 12 | 2,269.0 | 1,532.5 | 1,554.4 | 1,619.7 | 4,093.4 | 4,093.4 | 4,093 |

**Day 2** (hot board `board_b` — the rotation day):

| phase | ops | mean | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| open[off] | 240 | 1,042.9 | 948.8 | 1,014.7 | 1,038.0 | 1,109.9 | 1,195.1 | 1,256 |
| open[on] | 240 | 1,057.2 | 950.7 | 1,022.7 | 1,047.0 | 1,135.6 | 1,239.2 | 2,046 |
| open[declared] | 240 | 1,061.7 | 949.1 | 1,020.1 | 1,046.6 | 1,134.1 | 1,275.2 | 2,686 |
| board[off] | 2,400 | 2,176.4 | 2,019.8 | 2,082.0 | 2,099.1 | 2,654.5 | 3,249.4 | 13,678 |
| board[on] | 2,400 | 675.0 | 104.5 | 152.5 | 160.5 | 2,184.3 | 3,016.2 | 6,404 |
| board[declared] | 2,400 | 630.8 | 103.6 | 151.4 | 158.6 | 2,999.4 | 4,020.6 | 6,619 |
| tape200[off] | 396 | 174.8 | 142.0 | 165.8 | 169.7 | 202.7 | 234.0 | 324 |
| tape200[on] | 396 | 141.6 | 110.3 | 131.3 | 135.3 | 176.0 | 205.5 | 735 |
| tape200[declared] | 396 | 155.8 | 86.6 | 130.7 | 134.5 | 165.7 | 256.9 | 5,995 |
| tape1k[off] | 396 | 336.0 | 289.9 | 319.1 | 326.4 | 385.7 | 506.0 | 912 |
| tape1k[on] | 396 | 165.2 | 98.3 | 131.6 | 136.5 | 359.8 | 446.8 | 575 |
| tape1k[declared] | 396 | 227.5 | 128.0 | 133.3 | 138.5 | 454.1 | 1,506.0 | 5,909 |
| tape10k[off] | 396 | 2,171.3 | 1,977.0 | 2,026.3 | 2,041.5 | 2,727.2 | 3,992.8 | 17,856 |
| tape10k[on] | 396 | 421.8 | 97.1 | 133.1 | 138.4 | 2,096.9 | 2,837.7 | 2,920 |
| tape10k[declared] | 396 | 539.1 | 98.7 | 134.5 | 139.8 | 2,950.2 | 3,173.6 | 7,731 |
| pk[off] | 240 | 133.6 | 74.7 | 119.4 | 127.0 | 156.4 | 184.1 | 2,871 |
| pk[on] | 240 | 130.8 | 78.1 | 122.3 | 126.7 | 152.5 | 164.1 | 356 |
| pk[declared] | 240 | 137.8 | 119.3 | 121.8 | 125.0 | 153.7 | 252.4 | 1,624 |
| close[off] | 12 | 2,249.5 | 1,504.5 | 1,527.1 | 1,594.4 | 3,968.8 | 3,968.8 | 3,969 |
| close[on] | 12 | 2,313.0 | 1,545.2 | 1,558.1 | 1,910.3 | 4,042.8 | 4,042.8 | 4,043 |
| close[declared] | 12 | 2,434.5 | 1,553.6 | 1,591.3 | 1,692.4 | 5,676.4 | 5,676.4 | 5,676 |

**Day 3** (`board_a` again, hot symbols disjoint from day 1's — the day
Part I re-created from scratch and this run recovers instead):

| phase | ops | mean | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| open[off] | 240 | 1,048.7 | 947.4 | 1,012.5 | 1,039.3 | 1,132.1 | 1,235.6 | 1,874 |
| open[on] | 240 | 1,052.5 | 947.1 | 1,018.4 | 1,042.0 | 1,134.0 | 1,255.7 | 1,370 |
| open[declared] | 240 | 1,076.8 | 926.6 | 1,023.6 | 1,055.8 | 1,246.6 | 1,486.1 | 1,743 |
| board[off] | 2,400 | 2,218.2 | 2,060.4 | 2,126.4 | 2,149.6 | 2,820.1 | 3,202.8 | 6,427 |
| board[on] | 2,400 | **449.8** | 107.8 | 151.8 | 157.8 | 2,187.9 | 3,094.6 | 3,694 |
| board[declared] | 2,400 | 456.5 | 101.1 | 150.1 | 155.8 | 3,019.2 | 3,293.7 | 15,253 |
| tape200[off] | 396 | 175.6 | 128.4 | 164.3 | 169.0 | 203.8 | 282.6 | 1,072 |
| tape200[on] | 396 | 143.6 | 118.4 | 130.8 | 135.1 | 166.4 | 286.0 | 828 |
| tape200[declared] | 396 | 140.4 | 127.7 | 131.2 | 134.8 | 165.2 | 182.0 | 692 |
| tape1k[off] | 396 | 334.9 | 277.9 | 319.0 | 325.4 | 385.0 | 440.2 | 866 |
| tape1k[on] | 396 | 149.1 | 88.7 | 131.2 | 135.4 | 313.7 | 413.6 | 432 |
| tape1k[declared] | 396 | 148.9 | 122.0 | 130.8 | 135.1 | 177.9 | 451.3 | 467 |
| tape10k[off] | 396 | 2,068.0 | 1,970.5 | 2,025.2 | 2,039.2 | 2,188.3 | 2,815.2 | 3,044 |
| tape10k[on] | 396 | 478.8 | 98.0 | 133.6 | 139.0 | 2,171.8 | 2,979.0 | 3,277 |
| tape10k[declared] | 396 | 559.8 | 95.7 | 133.9 | 138.8 | 2,914.4 | 3,617.6 | 4,218 |
| pk[off] | 240 | 126.6 | 75.5 | 121.3 | 124.6 | 152.0 | 203.5 | 367 |
| pk[on] | 240 | 128.6 | 75.0 | 120.6 | 125.6 | 156.4 | 207.8 | 222 |
| pk[declared] | 240 | 135.6 | 85.6 | 120.7 | 125.7 | 153.4 | 228.8 | 2,017 |
| close[off] | 12 | 2,571.0 | 1,531.0 | 1,544.7 | 1,943.7 | 6,168.4 | 6,168.4 | 6,168 |
| close[on] | 12 | 2,301.3 | 1,548.5 | 1,581.2 | 1,635.1 | 4,068.6 | 4,068.6 | 4,069 |
| close[declared] | 12 | 2,561.6 | 1,546.0 | 1,619.0 | 1,959.9 | 5,705.6 | 5,705.6 | 5,706 |

**The noise floor, from inside the run.** The pk lookups touch no Cabin and
are the control: p50 124.3–127.0 across all nine arm-days, a **2.7 µs
spread**. Any p50 delta below ~3 µs in this part is not a finding — which
is why the day-3 on-versus-declared hot-probe p50 (157.8 vs 155.8) is
reported as *the same number*, and why the throughput and CPU tables carry
the day-3 verdict instead. The close aggregates read whole relations
through the scan path no Cabin serves: p50 1,584–1,960, flat. The open
inserts carry the write hook on two arms and not on the third: p50 deltas
−2.7 to +16.5 µs on a ~1,045 µs fsync-bound statement, inside the phase's
own day-to-day swing — **no resolvable write-hook cost**, the same verdict
Part I and `bench/results-cabin.md` reach.

**The row-set sweep**, day 3, p50 in µs (`tape_200` / `tape_1k` /
`tape_10k` hold 200 / 1,000 / 10,000 rows; `--tape-probes 396` per size per
arm-day):

| rows | off (walk) | on (served) | declared (served) | speedup |
|---|---|---|---|---|
| 200 | 169.0 | 135.1 | 134.8 | 1.25× |
| 1,000 | 325.4 | 135.4 | 135.1 | 2.40× |
| 10,000 | 2,039.2 | 139.0 | 138.8 | 14.7× |

The walk is a fixed cost plus a per-row one and the served probe is only
the fixed cost. Fitting the three off-arm points gives **135 µs +
0.19 µs/row** — which predicts 173 µs at 200 rows against 169 measured —
and the served probe sits at 135–139 µs at every size, i.e. exactly the
intercept. That is the cleanest statement of what a Cabin does to a
`FilterScan`: it deletes the per-row term and leaves the constant, so the
crossover is below 200 rows even at three data pages, and the win at any
size is readable off the row count alone. The sweep is unchanged from Part
I, as it must be — `T_amort` decides which structures exist, never what one
costs to use.

## The lifecycle, as the controller reported it

`SHOW CABIN_OPTIMIZER`, captured untimed at every phase boundary and every
3 s overnight; t is wall seconds from run start (run 2). B/C is the
per-entry `benefit_q16 / cost_q16`; θ_create is 3.0, θ_drop 0.5, and the
cooldown is 640 s at this half-life.

| t (s) | event |
|---|---|
| 10.6 | `SET CABIN_OPTIMIZER ON` at day-1 open; `ticks=0 managed=0` |
| ≤ 19.7 | **CREATE ×4** — `board_a` + all three tapes, inside the session's first 3 blocks; board B/C 12,922 |
| 19.7–56.5 | ACTIVE, serving; **EXTENDs** (106 by day-1 close) as the 20 % uniform tail keeps observing values; board B/C 20,242–33,202 |
| 59.5–101.5 | **overnight 1: nothing changes state.** All four ride their B/C down — board_a 14,975 → 45.1, tape_200 825 → 2.42 — and every one stays above θ_drop = 0.5. This is the ratified behaviour, observed |
| 110.5 | day-2 trading: **CREATE ×1**, `board_b` (`cabin_id=5`). `board_a` is still ACTIVE at B/C 12.6, now genuinely cold |
| 133.1 | `board_a` **DECAYING** (B/C 0.25) — ~15 half-lives after its last probe, and **not dropped**: the 640 s cooldown has 507 s to run |
| 144.3–192.4 | `board_a` sits in DECAYING at B/C 0.00 (the Q24.8 score has underflowed) through the rest of day 2 and all of night 2, still holding its 2,091 entries |
| **201.4** | day-3 trading, block 3: **`board_a` DECAYING → ACTIVE**, B/C 0.00 → 21,542, **`cabin_id=1` unchanged**. The morning recovery — the rung Part I recorded as never exercised |
| 223.9 | `board_b` **DECAYING** (B/C 0.25) as day 3's rotation leaves it cold; still ACTIVE-equivalent for serving, still not dropped |
| 241.3–283 | overnight 3: `board_a` and all three tapes ACTIVE, `board_b` DECAYING. **No DROP anywhere in the run** |

End state: `creates=5 extends=172 heals=0 drops=0 deferred=0 failures=0`,
546 ticks, `pages_committed` 35 of the 1,024 budget, five managed entries.
Run 1 is the same timeline shifted by its own seed: 5 creates, 164 extends,
0 drops, `board_a` DECAYING at t=144.9 and recovering to ACTIVE at t=213.3
— again in day 3's third block, again keeping `cabin_id=1`.

**Recovery is free and a rebuild is not, and that is the whole difference.**
Part I's day 3 re-created `board_a`'s Cabin from scratch as `cabin_id=9`:
three confirm snapshots of walking, then a relation scan, then n=2
re-observation of every value from an empty set. This run's day 3 walked
nothing, scanned nothing, and re-observed nothing — the state transition
is a label change on a structure that never stopped serving. `SHOW CABINS`
shows `board_a`'s entry count going 2,091 (day 1) → 2,091 (day 2, untouched)
→ 4,964 (day 3, extended), where Part I's went 2,091 → dropped → rebuilt.

## Hit rates: where the throughput actually came from

`SHOW CABINS` per day, hits/misses as per-day deltas. Auto Cabins observe at
n=2, declared at n=1 (`feat-cabin.md`'s split, visible as
`misses = observed` exactly on every declared row).

| day | on: board | declared: board | on: tape 200/1k/10k | declared: tape 200/1k/10k |
|---|---|---|---|---|
| 1 | 81.3 % (1,789/412) | 83.5 % (2,005/396) | 92.3 / 85.7 / 79.7 % | 95.2 / 90.2 / 81.9 % |
| 2 | 82.1 % (1,808/393) | 84.1 % (2,020/381) | 98.7 / 88.7 / 86.2 % | 99.7 / 93.0 / 87.2 % |
| 3 | **87.2 %** (2,094/307) | 90.3 % (2,168/233) | 99.7 / 95.0 / 84.0 % | 100 / 96.5 / 85.5 % |

Two movements, both new. The **tapes now accumulate**: `tape_200` climbs
92.3 → 98.7 → 99.7 % where Part I's held 92.3 → 90.9 → 91.2 %, and
`tape_1k` climbs 85.7 → 88.7 → 95.0 % against Part I's 85.7 → 84.3 → 84.8 %.
That flat-versus-climbing pair is the nightly ledger-clearing, measured
directly. The tapes are the shapes that stay hot every day, and they are
where the difference is cleanest: at `T_amort = 1` their Cabins were
retired in each of the three overnights and re-created each morning from an
empty set, so their rate could never move off day 1's; here they were never
retired at all, so each morning began with everything the day before had
observed. Nothing about the traffic changed — only whether the structure
survived the gap between two identical sessions.

And the **board's day-3 rate rose 81.7 → 87.2 %** against Part I on the
identical draw. The mechanism is worth stating precisely, because it is not
the obvious one: day 3's hot symbols on `board_a` are **disjoint from day
1's** by construction, so the surviving entry set cannot serve a single hot
draw — those eight symbols are re-observed at n=2 either way. What it
serves is the **20 % uniform tail**, drawn from the board's 1,000-symbol
domain, of which the surviving Cabin had already observed 193. The within-
run evidence is day 1 against day 3 on the same relation with the same draw
structure: an empty start missed **412 of 2,201 consulted probes (18.7 %)**,
and a start carrying 2,091 entries over 193 symbols missed **307 of 2,401
(12.8 %)** — and every probe on day 3 was consulted, where at `T_amort = 1`
about 200 of them ran before the rebuilt Cabin existed and walked the
relation outright. A surviving structure pays off in the tail, which is
precisely the part of an 80/20 workload a rebuilt one cannot reach.

The residual 3.1-point gap to the declared arm is now **entirely the n=2
observation rule**, not the lifecycle: by day 3 the on arm had observed 459
values against the declared arm's 630, because a value seen once is never
recorded by the autonomous path and always recorded by a declaration. That
is a different open question (`feat-cabin.md`'s n=2 split) and `T_amort`
does not touch it.

`ANALYZE` confirms the plan on every day and arm: `FilterScan …
pages=139–146` on off against `CabinProbe … cabin_optimizer=true,
pages=2×rows` on the on arm (PHY06's managed mark), the declared arm's
probe identical minus the mark.

## The advisory-family contract, under the whole lifecycle

18 verification statements per day — the eight hot board probes, six tape
probes, a zero-row probe, two COUNTs and a SUM — executed on all three arms
and compared **byte for byte**, through CREATE, EXTEND, DECAYING and
recovery: **identical on every day of both runs (6/6 PASS, 108 statements
executed on three arms each)**. A Cabin, however it was created and
whatever state its controller thinks it is in, chooses where to look and
never what is visible.

## Where the latency goes

The measured unit is one statement round trip from a Python client on a
single connection. Server CPU is `/proc/<pid>/stat` deltas per interleave
block, which is what client latency cannot resolve. Day-3 hot-board probe,
run 2:

| wait type | off (walk, 2,218 µs mean) | on (87 % served, 450 µs mean) | declared (90 % served, 457 µs mean) | how measured |
|---|---|---|---|---|
| server CPU | 2,146 µs (97 %) | **388 µs (86 %)** | **392 µs (86 %)** | per-block `/proc` deltas ÷ 2,400 ops |
| client + socket | ~72 µs | ~62 µs | ~65 µs | remainder; consistent with the pk control (124.6–125.7 µs p50, itself containing a ~30 µs descent) and this harness's documented ~100 µs floor |
| read (page-fetch) wait | 0 | 0 | 0 | every page resident after load; nothing evicts engine-wide, so a warm walk does no I/O |
| durability / commit wait | n/a | n/a | n/a | probes are read-only. The open inserts are the durability row: ~1,040 µs p50, group-commit fsync on EBS (a batch of one is a batch), flat across arms |
| lock / conflict wait | n/a | n/a | n/a | one connection per server; core-local latches have no contention at `cores = 1` |

**388 against 392 µs of server CPU is the run's sharpest single number.**
At `T_amort = 1` the same comparison was 662 (on) against 408 (declared) —
the autonomous arm paid 62 % more CPU per probe than the declared one for
the same statement, and it now pays the same to within 1 %. The whole of
that difference was structures being rebuilt, and none of it was the
structure itself.

Day 1 and day 2 still separate the arms slightly (on 629 / 613 µs per probe
against declared 567 / 563): the on arm's first session pays its
pre-CREATE walk, and its n=2 rule keeps a few more probes on the walking
path. The tape phases show the same convergence at a smaller scale — day 3
on 210 µs/op against declared 236, off 800.

What still cannot be decomposed with today's surfaces: the in-server split
between queueing and execution (`docs/observability.md` is a proposal), and
the per-probe split between a Cabin hit and the entry-set resolve behind it.

## Against PostgreSQL

PostgreSQL 17.10, the scratch cluster of `tools/pg_setup.sh` on port 15433
**at defaults** — no tuning, on purpose — same five relations, same rows
from the same generator and the same seed, same day-1 statement stream, via
`tools/pg_scenario4_cabinopt_days.py`, measured this session on the quiet
machine. The twin runs **one** unpaced day: at defaults PostgreSQL builds
nothing for the hot predicate and retires nothing, so day 300's plan is day
1's and the rotation axis has nothing to act on. `EXPLAIN (ANALYZE,
BUFFERS)` confirmed `Seq Scan` for the board probe (76 buffers), the tape
probe (74) and the zero-row probe (76 — absence costs PostgreSQL a full
scan, where an observed Cabin value answers from the entry set without
opening the relation).

| phase | ops | mean | p0 | p25 | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| open[pg] | 240 | 528.4 | 465.8 | 495.3 | 521.0 | 603.3 | 663.0 | 723 |
| board[pg] | 2,400 | 1,675.0 | 1,497.8 | 1,580.9 | 1,606.6 | 2,185.9 | 2,430.2 | 5,002 |
| tape200[pg] | 396 | 256.3 | 236.3 | 240.5 | 242.4 | 287.7 | 381.8 | 1,960 |
| tape1k[pg] | 396 | 367.5 | 311.5 | 343.2 | 346.0 | 460.3 | 728.0 | 1,413 |
| tape10k[pg] | 396 | 1,581.8 | 1,471.2 | 1,489.5 | 1,503.6 | 2,043.8 | 2,230.1 | 2,573 |
| pk[pg] | 240 | 194.9 | 165.9 | 187.3 | 189.1 | 222.1 | 294.5 | 553 |
| close[pg] | 12 | 2,531.0 | 1,009.1 | 1,035.9 | 1,449.4 | 6,440.9 | 6,440.9 | 6,441 |

Side by side on p50, day 3 (each column carries its own client's socket
cost — ~100 µs KDS, ~150–190 µs PG per the pk controls):

| shape | KDS walk | PG seq scan | KDS served (on) | served vs PG |
|---|---|---|---|---|
| board 10k | 2,149.6 µs | 1,606.6 µs | 157.8 µs | **10.2×** |
| tape 10k | 2,039.2 µs | 1,503.6 µs | 139.0 µs | **10.8×** |
| tape 1k | 325.4 µs | 346.0 µs | 135.4 µs | **2.6×** |
| tape 200 | 169.0 µs | 242.4 µs | 135.1 µs | **1.8×** |

The two honest notes from Part I both repeat, unchanged by `T_amort`.
Before any Cabin exists, **PostgreSQL's 10k walk beats KDS's by 25–26 %** —
74–76 buffers against 139–146 pages for the same rows, the 64-byte
`inline_cell_width` padding tax on row density. And an operator who
declared an index on either engine would beat both walks; the at-defaults
comparison is the honest one for a feature whose point is acting *without*
the operator, which is also why the twin has no third arm.

One number moved between sessions and should not be read as an engine
result: PostgreSQL's insert phase measured 521 µs p50 here against 1,144 µs
in Part I's twin, while KDS's stayed at ~1,040 µs in both. The device is
the same EBS volume and neither engine changed; the fsync-bound write path
on this host is simply not stable across sessions, so the insert row is
comparable *within* a session and not across the two parts. Every read
shape above is served from resident pages and does no I/O, which is why the
probe comparisons are stable and the insert one is not.

## What this run says about the engine

**`T_amort` is not a tuning knob; it is the declaration of what a Cabin
is for.** The measurement makes the two settings look like two different
features. At 1, the controller is a per-session cache: it builds what today
needs, serves it, and discards it at the first long silence — 12 CREATEs
and 10 DROPs over three days, an 81–82 % hit-rate plateau, and 62 % more
CPU per probe than the equivalent declaration. At 64, it is a curator: 5
CREATEs, 0 DROPs, hit rates that climb every day, and CPU per probe within
1 % of a declaration's. Nothing else changed — same code, same workload,
same seeds. The cost model's one free parameter moved the feature's whole
character, and that is a statement about the model's shape as much as about
the number: `B` decays and `C` does not, so `T_amort` alone fixes how long
evidence is allowed to be stale.

**The `DECAYING → ACTIVE` rung is real, and it is the cheapest thing in the
lifecycle.** PO5's ladder had never been climbed upward under traffic. It
now has, twice, in both runs, at the same interleave block, and it cost a
label change: no scan, no confirm streak, no re-observation, and
`board_a`'s served p50 after the excursion is indistinguishable from its
p50 before it (157.8 µs on day 3 against 160.6 on day 1 — a 2.8 µs delta on
a 2.7 µs floor). The
asymmetry that justifies a long window is now measured rather than argued —
recovery is free and re-nomination is not, so the window wants to be long
enough that recovery is the common case.

**Autonomy no longer costs a hit rate; it costs an observation rule.** The
plain answer to "is the on arm now simply equal to a declaration" is: on
throughput and CPU, yes — and it was not at `T_amort = 1`, where the
declared arm won by 38 % on day-3 TPS. What the autonomy still buys, stated
without inflation, is **three things, all smaller than they were**. It
carried 9,156 entries against the declared arm's 13,503 (68 %) and 35 pages
of a 1,024 budget, because n=2 refuses to record a value seen once — a
declaration records every singleton forever. It created `board_b`'s Cabin
on day 2 when traffic asked, where the declaration created it on day 0 and
paid for an empty structure through all of day 1. And it holds `board_b` in
DECAYING at run end with a DROP pending, which the declared arm will never
perform for a shape that went permanently cold. At five relations, an
operator can still declare everything and match it; the autonomous arm's
case remains the schema where "declare everything" stops being a plan —
but this run removes the *performance* argument for declaring, which Part I
supplied.

**The R1 score has a resolution floor, and at this window the timer
outlives it.** The cooldown trace showed the Q24.8 benefit reaching exactly
zero after ~16 half-lives of silence and staying there for the remaining
127, and the business-days trace shows `board_a` sitting at `B/C = 0.00`
for 57 s before recovering. So beyond ~16 half-lives the controller has no
evidence at all, only a clock — DROP is a timeout, not a judgement. That is
sound at `T_amort = 64` (the timeout is the judgement the operator made),
but it bounds what a future per-consumer half-life or a promotion
comparison can be built on: **anything that wants to distinguish degrees of
coldness needs more score resolution than Q24.8 gives at these ratios**,
and `docs/feat-physical-optimizer.md` R1's decay implementation is where
that would be decided.

**Does 64 look right?** The evidence points at *right, possibly slightly
long*, and this run cannot settle it alone. Right, because it delivers
exactly what it was ratified for: no structure was retired across a night
in either run, the morning recovery fires, and the cooldown check confirms
a long enough silence still drops. Possibly long, on two observations.
First, the admission bar fell by the same factor — the board's `C` went
143 → 2.23, so a shape needs `B > 6.7` instead of `> 429` to be nominated,
and in this workload every candidate cleared it in the first three
snapshots; a narrower workload where marginal shapes exist would show
whether PO6's budget really is what bounds the population, and this
scenario's peak of 35 pages against 1,024 cannot answer that. Second,
`board_b` ended run 2 holding 1,942 entries in DECAYING with ~580 s of its
640 s cooldown still to run against a shape that had gone permanently cold,
and at the shipped 600 s half-life that is close to 21 hours of a dead
structure billing its write hook and its memory.
Both point the same direction — *the cooldown may want to be shorter than
the amortization window rather than twice it* — and that is a change to the
model's shape rather than to its number, which belongs to
`docs/feat-physical-optimizer.md` §II.4's owner and not to this benchmark.
What would settle it is the measurement neither part has: a workload with
marginal candidates and a page budget under pressure.

**What is still not exercised.** HEAL and hint failures stayed at zero in
both runs, because nothing here moves a row — no UPDATE, no DELETE, no
relayout, so no page epoch ever bumps. The budget-swap DROP reason needs a
page budget under pressure (peak 35 of 1,024). And no DROP occurred in the
matrix at all, by the arithmetic stated above — the cooldown check is the
only retirement evidence in Part II, and it is a synthetic single-relation
setup on two servers rather than a business-day one. A scenario that wants
a measured weekend needs `overnight_seconds` above
`2 × T_amort × decay_half_life`, which at any honest compression is a run
several times longer than this one.
