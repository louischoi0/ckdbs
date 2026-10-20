import sys
sys.path.insert(0, "/home/cdkbs/bench-run")
from doc import *
from an import *
import host
import s4_tables as t4

N, O = t4.runs4()
PG = runs(new, "pg-s4")


def win(tags):
    s = [host.STARTS[x] for x in tags if x in host.STARTS]
    e = [host.ENDS[x][0] for x in tags if x in host.ENDS]
    return min(s)[11:19], max(e)[11:19]


def qps(d, day, g, a):
    p = t4.P(d, day, g, a)
    return 1e6 / p["mean_us"]


def mq(src, g, a, days=(1, 2, 3)):
    return med([qps(src[k], day, g, a) for k in "AB" for day in days if src[k]])


pkv = [t4.P(N[k], d, "pk", a)["mean_us"] for k in "AB" for d in (1, 2, 3) for a in t4.ARMS]
pko = [t4.P(O[k], d, "pk", a)["mean_us"] for k in "AB" for d in (1, 2, 3) for a in t4.ARMS]
walk_n = med([(t4.P(N[k], 1, "tape10k", "off")["p50_us"] - t4.P(N[k], 1, "tape200", "off")["p50_us"]) / 9800 * 1000 for k in "AB"])
walk_o = med([(t4.P(O[k], 1, "tape10k", "off")["p50_us"] - t4.P(O[k], 1, "tape200", "off")["p50_us"]) / 9800 * 1000 for k in "AB"])
pg_walk = med([(ph(d, "d1-tape10k[pg]")["p50_us"] - ph(d, "d1-tape200[pg]")["p50_us"]) / 9800 * 1000 for d in PG])
cabin_srv = med([t4.P(N[k], 2, g, a)["p50_us"] for k in "AB" for g in ("tape200", "tape1k", "tape10k") for a in ("on", "declared")])
open_p50 = med([t4.P(N[k], d, "open", a)["p50_us"] for k in "AB" for d in (1, 2, 3) for a in t4.ARMS])
pg_open = med([ph(d, "d1-open[pg]")["p50_us"] for d in PG])
pgpk = med([ph(d, "d1-pk[pg]")["mean_us"] for d in PG])

board_off_gain = mq(N, "board", "off") / mq(O, "board", "off")
tape10_off_gain = mq(N, "tape10k", "off") / mq(O, "tape10k", "off")

OPEN_DIFF = max(abs(t4.P(N['B'], d, 'open', a)['p50_us'] / t4.P(N['A'], d, 'open', a)['p50_us'] - 1) * 100 for d in (1, 2, 3) for a in t4.ARMS)
WALK_DIFF = max(abs(qps(N['B'], d, g, a) / qps(N['A'], d, g, a) - 1) * 100 for d in (1, 2, 3) for g in ('board', 'tape200', 'tape1k', 'tape10k', 'pk') for a in t4.ARMS if not (d == 3 and a == 'on'))
txt = f"""# Scenario 4 (cabin optimizer over business days) at `{DESC}`

**One line.** Three business days on three servers (a Cabin controller `off`, `on`, and a Cabin `declared` up front), run twice: the controller still lifts an equality filter that a walk serves slowly by 1.6x to 2.3x (`board` {mq(N, 'board', 'on') / mq(N, 'board', 'off'):.2f}x on, {mq(N, 'board', 'declared') / mq(N, 'board', 'off'):.2f}x declared; `tape10k` {mq(N, 'tape10k', 'on') / mq(N, 'tape10k', 'off'):.2f}x and {mq(N, 'tape10k', 'declared') / mq(N, 'tape10k', 'off'):.2f}x, medians over the six days) and changes nothing it cannot serve
(`tape200`, `pk`, `open`, `close` inside the floor), and its lifecycle is identical to v3.1.0's: four Cabins in the first block, a fifth on day 2, `board_a` DECAYING on day 2 and back to ACTIVE on day 3 with its pages
doubled, and with `cabin_optimizer_cooldown_half_lives = 2` two DROPs and one re-nomination. The change against v3.1.0 (cross-version) is in the walk: the arm that has no Cabin, `off`, is
**{board_off_gain:.2f}x on `board` and {tape10_off_gain:.2f}x on `tape10k`** ({f0(mq(N, 'board', 'off'), ' qps')} against {f0(mq(O, 'board', 'off'), ' qps')}), the per-row cost of an unserved equality falls from {walk_o:.1f} to {walk_n:.1f} ns, and the server CPU an `off` run spends on its boards
falls from 2.80 s to 2.22 s, while the `pk` control does not move (mean {sum(pkv) / len(pkv):.1f} µs against {sum(pko) / len(pko):.1f} µs). Against PostgreSQL 18.6, **re-measured in the same window**, day 1 with no index: `board`
{ratio(mq(N, 'board', 'off', (1,)), med([1e6 / ph(d, 'd1-board[pg]')['mean_us'] for d in PG]))} walking, {ratio(mq(N, 'board', 'on', (1,)), med([1e6 / ph(d, 'd1-board[pg]')['mean_us'] for d in PG]))} with the controller; `close` is 0.57x.

Driver: `tools/scenario4_cabinopt_days.py`, unmodified, two complete runs: **run A** at the server defaults (`s4`) and **run B** with `cabin_optimizer_cooldown_half_lives = 2` (`cd2-s4`; run A cannot exercise DROP, section 6),
both with `decay_half_life = 5` and `cabin_optimizer_snapshot_interval_ms = 500`. Measured by `ck-tester`. Comparator (cross-version): `bench/v3.1.0/results-scenario4-cabinopt-days-v3.1.0-30-gcd7c7c4e.md`, re-derived from
its archived JSON. PostgreSQL 18.6: `tools/pg_scenario4_cabinopt_days.py`, **fresh, three runs**.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-20; run A {win(['s4'])[0]} to {win(['s4'])[1]} UTC, run B {win(['cd2-s4'])[0]} to {win(['cd2-s4'])[1]} UTC; PostgreSQL {win(['pg-s4', 'r2-pg-s4', 'r3-pg-s4'])[0]} to {win(['pg-s4', 'r2-pg-s4', 'r3-pg-s4'])[1]} UTC |
{COMMON.replace('results-scenario{N}-*', 'results-scenario4-cabinopt-days')}| Driver | **unmodified** `tools/scenario4_cabinopt_days.py`, `--port-off/-on/-declared` with the three servers' PIDs for the CPU readings, `--suffix a`; exit 0 in both runs, `client_errors` {N['A']['meta']['client_errors']} and {N['B']['meta']['client_errors']}, `slot_overruns` {N['A']['meta']['slot_overruns']} and {N['B']['meta']['slot_overruns']}; the driver's byte-for-byte comparison of 18 statements per day across the three arms passed on all 6 days |
| Server config | the common config plus `decay_half_life = 5`, `cabin_optimizer_snapshot_interval_ms = 500` on all three servers, and in run B `cabin_optimizer_cooldown_half_lives = 2`; `cores = 1`, `durability = group`; ports 15651 to 15653 |

## 2. What was run

Two complete runs of the driver, three days of three arms each. Per day: an open (240 inserts into the day's board), a paced 45 s trading session (2,400 hot-board probes, 396 probes on each of three tape sizes,
240 pk controls, per arm, interleaved in 12 blocks), a close (`COUNT` / `SUM` / `GROUP BY` full scans, three rounds) and a 45 s idle night. The equal work per arm is fixed by the driver's counts, not by time.

    python3 tools/scenario4_cabinopt_days.py --port-off 15651 --port-on 15652 --port-declared 15653 \\
        --pid-off <p> --pid-on <p> --pid-declared <p> --suffix a --json <run>.json

**Row-set axis (rule 9).** The tape relations are 200, 1,000 and 10,000 rows (`tape_200`, `tape_1k`, `tape_10k`); the boards are 10,000 rows each (`board_a`, `board_b`). Every phase is a row of the matrix, the size is
a row label. The probes are serial on one connection, so qps is derived as 1,000,000 / mean µs (the table cells give the p50 in µs beside the derived qps). The hot set rotates: day 1 hot on `board_a`, day 2 on `board_b`,
day 3 on `board_a` with a different symbol set; the tapes are hot every day with disjoint values.

Cut: nothing. Time budget lifted by the operator for this run; the v3.1.0 plan (two runs) ran in full. PostgreSQL's twin runs day 1 only, unpaced, no index, by its own design.

## 3. Throughput and latency by arm

p50 and derived qps (from the mean) per group, day and arm for **run A**; the last two columns are the ratio of mean latencies, off over on and off over declared, for run A and run B:

{T('s4_tables.py', 'tp')}
**What is resolved and what is not.** The `pk` control touches no Cabin, and its on-over-off ratio ranges 0.97x to 1.07x (declared 0.98x to 1.10x) across days and runs: **a ratio below about 1.10x is not a finding**. That puts `tape200`
(0.98x to 1.08x) at the floor, `tape1k` (1.13x to 1.26x) over it by a little, and `tape10k` (1.69x to 2.42x), `board` (1.54x to 2.08x) clear of it; `open` and `close` (0.87x to 1.09x) are not measurably different between arms. The
`tape200` result is the row-count lesson (rule 9): at 200 rows the walk is 102 µs against 100 µs for the Cabin, and the fixed round trip is all there is.

**Cross-version (now over v3.1.0, derived qps per arm, run A and run B):**

{T('s4_tables.py', 'cross')}
The `pk` control reads 0.85x to 1.07x, so ratios inside that are not findings. The `off` arm on `board` (1.17x to 1.20x) and `tape10k` (1.19x to 1.22x) is above it in all six days of both runs; the `on` and `declared` arms there are
0.97x to 1.12x, mostly inside it. `open` in run B (1.10x to 1.27x) is the device's sync moving between runs, section 9.

## 4. Percentiles

Run A, every phase (microseconds; the five percentiles are the driver's; PostgreSQL's day-1 phases follow):

{T('s4_tables.py', 'pct')}
PostgreSQL 18.6, day 1, run 1 of 3:

{T('s4_pg.py', 'pct')}
## 5. Where the time goes

A probe is one round trip on one connection, so the decomposition is fixed cost plus rows walked; there is no commit or lock in the read phases and one durable commit per `open` statement.

| Component | Now | v3.1.0 (cross-version) | PostgreSQL | How derived |
|---|---|---|---|---|
| Fixed round trip (client, socket, parse, one pk descent) | **{sum(pkv) / len(pkv):.1f} µs** ({min(pkv):.1f} to {max(pkv):.1f}) | {sum(pko) / len(pko):.1f} µs | {pgpk:.1f} µs | `pk` mean across arms, days and runs |
| Rows walked by an unserved equality | **{walk_n:.1f} ns per row** plus about {med([t4.P(N[k], 1, 'tape200', 'off')['p50_us'] for k in 'AB']):.0f} µs | {walk_o:.1f} ns | {pg_walk:.1f} ns | `off` tape p50 at 200 and 10,000 rows, day 1: (p50 at 10,000 - p50 at 200) / 9,800 |
| A Cabin-served equality | **about {cabin_srv:.0f} µs** at any size once built (day 2) | about 98 to 100 µs | - | `on` and `declared` tape p50 on day 2; flat in n |
| Write statement with its commit (`open`) | **{open_p50:,.0f} µs** p50 per autocommit insert | 1,021 to 1,280 µs | {pg_open:,.0f} µs | `open` p50 over arms and days: the durable commit is nearly all of it |
| Lock or conflict wait | none: one connection | none | none | - |

The board's latency tail by arm (run A day by day; run B and v3.1.0 for the p95), microseconds:

{T('s4_tables.py', 'tail')}
**The declared arm has the heavier tail**, as at v3.1.0: its board p95 is 779 to 794 µs against the `on` arm's 396 to 445 µs and the walk's 467 to 486 µs, in every day and both runs. A probe the declared Cabin cannot answer costs about 1.6x a plain
walk and the controller's does not. This is why the `on` arm's mean (239 µs, day 1 run A) is lower than the `declared` arm's (276 µs) although the `declared` p50 is lower (176 against 182 µs). This run does not say why the declared arm's miss path is costlier.

Server CPU seconds by arm (the driver's `/proc` reading), now and at v3.1.0:

{T('s4_tables.py', 'cpu')}
## 6. The controller's lifecycle

Snapshots of `SHOW CABIN_OPTIMIZER` on the `on` arm at chosen points:

{T('s4_tables.py', 'life')}
Run A, defaults: four Cabins are created in the first 13.6 s of day 1 (`board_a`, `tape_200`, `tape_1k`, `tape_10k`), grown to 104 extends by the end of the day, and a fifth (`board_b`) on day 2's first block. `board_a` goes **DECAYING** on day 2 and **returns to ACTIVE** on
day 3 with its pages doubled (9 to 18) when its hot set comes back; then `board_b` goes DECAYING. At the end: `creates=5 extends=172 heals=0 drops=0 deferred=0 failures=0`. **No DROP occurs in a 3-day run at the defaults**: the DECAYING dwell before a DROP is
`cabin_optimizer_cooldown_half_lives`, default 128 (`docs/spec/physical-optimizer.md` II.6), which at `decay_half_life = 5` is 640 s; the driver's whole run is 278 s.

Run B, cooldown 2 half-lives: the same creates on day 1, `board_a` DECAYING at 127.3 s and **DROPPED** by 138.5 s (`drops=1`); its Cabin is gone while `board_b` is hot; on day 3 `board_a` is **re-nominated from scratch** (`creates=6`, 5 pages at 195.7 s, 14 by the end)
and `board_b` is dropped at day 3's last block (`drops=2`). Final: `creates=6 extends=193 heals=0 drops=2 failures=0`. The cost of re-nomination shows in the served rate of the day it happens: `d3-board[on]` runs {f0(qps(N['A'], 3, 'board', 'on'), ' qps')} in run A (the Cabin survived the night) and {f0(qps(N['B'], 3, 'board', 'on'), ' qps')} in
run B (rebuilt), {delta(qps(N['B'], 3, 'board', 'on'), qps(N['A'], 3, 'board', 'on'))}, against a run A / run B difference of {delta(qps(N['B'], 1, 'board', 'on'), qps(N['A'], 1, 'board', 'on'))} and {delta(qps(N['B'], 2, 'board', 'on'), qps(N['A'], 2, 'board', 'on'))} on days 1 and 2. Single draw per run. The lifecycle is, snapshot for snapshot, the one v3.1.0 records
(4 / 5 / 5 Cabins, the same DECAYING and ACTIVE transitions, 2 DROPs in run B), with extends within 3 of it (172 against 170, 193 against 196).

## 7. Correctness

The driver ran its 18-statement comparison across the three arms each day: **PASS on all three days in both runs** (`verify` in the JSON), so a Cabin, declared or controller-built, changed no reply. `client_errors` 0 in both runs.

## 8. Against PostgreSQL 18.6 (fresh, same window)

PostgreSQL is the floor. The PG twin runs **day 1 only, unpaced, one server, no index**, by its own design: at defaults a PostgreSQL probe seq-scans on day 1 as on day 300 (the `explain` blocks in its JSON say `Seq Scan`). Three runs, KDS side: run A and run B day 1, derived qps,
the median of the two, ratio above 1.00x = KDS faster:

{T('s4_pg.py', 'cmp')}
The comparison with a Cabin is not a like-for-like with PostgreSQL's *indexed* case, which this twin does not run (no `symbol` index on PostgreSQL); that is a gap of this scenario, so "KDS on / PG" says what a Cabin buys over a PostgreSQL seq scan, not over a PostgreSQL index.
`tape200` is 0.73x to 0.75x on every arm: at 200 rows the statement is a round trip and PostgreSQL's is 26 µs cheaper ({pgpk:.0f} against {sum(pkv) / len(pkv):.0f} µs for the `pk` control the same gap, 0.92x). `close` (three full-relation folds) is 0.57x on every arm, the whole-relation fold cost
that scenario 1 measures as 0.65x to 0.69x on three of its four `agg-*` folds.

## 9. Noise floor, contamination and host

The floor is the `pk` control: on-over-off 0.97x to 1.07x, per-arm absolute {min(1e6 / x for x in pkv):,.0f} to {max(1e6 / x for x in pkv):,.0f} qps across arms, days and runs; PostgreSQL's three runs spread 0.2 % to 10.9 % (`open` 10.9 %, the durable group). The two runs of KDS give the repeat:
`open` p50 is {open_p50:,.0f} µs overall and run B's differs from run A's by up to {OPEN_DIFF:.0f} % (the device's sync); every probe group and arm differs between runs by up to {WALK_DIFF:.1f} % (the largest is a `pk` control cell; day 3 `on` is excluded, where run B re-nominated its Cabin) (derived qps, e.g. `off` on day 1: {f0(qps(N['A'], 1, 'board', 'off'), ' qps')} and {f0(qps(N['B'], 1, 'board', 'off'), ' qps')}).

**Contamination.** No cell was discarded. Per-cell host stamps (run A and B hold three servers each; the precheck of the `-off` server stands for the cell, the monitor covered the whole window):

{T('host.py', 's4=s4-off', 'cd2-s4=cd2-s4-off', 'pg-s4', 'r2-pg-s4', 'r3-pg-s4')}
## 10. What the run says about the engine

1. **The unserved walk got cheaper and nothing else about the Cabin story changed.** `off` is {board_off_gain:.2f}x and {tape10_off_gain:.2f}x v3.1.0 on the 10,000-row shapes, and the per-row cost of an unserved equality fell from {walk_o:.1f} to {walk_n:.1f} ns, against
   PostgreSQL's {pg_walk:.1f} ns. The served arms did not move (a Cabin-served probe is a round trip, about {cabin_srv:.0f} µs), so the Cabin's multiple over a walk moved from {mq(O, 'board', 'on') / mq(O, 'board', 'off'):.2f}x/{mq(O, 'tape10k', 'on') / mq(O, 'tape10k', 'off'):.2f}x (v3.1.0, medians over the six days) to {mq(N, 'board', 'on') / mq(N, 'board', 'off'):.2f}x/{mq(N, 'tape10k', 'on') / mq(N, 'tape10k', 'off'):.2f}x on `board`/`tape10k` where it moved at all because the *denominator* improved.
   Scenario 3's integer-column equality walk does not show the same gain (`loans-by-user` 1.00x at 10,000 loans); the two differ in the filtered column's type (`varchar` symbol here, `int64` there) among other things, and this run does not separate them.
2. **Below about 1,000 rows a Cabin is no tool** (`tape200` 0.98x to 1.08x; `tape1k` near the floor); at 10,000 rows it doubles the probe. The controller found the same four Cabins in the same 13.6 s that it did at v3.1.0, and created none for the pk control.
3. **The declared arm's tail is the open question this scenario keeps asking**: p95 779 to 794 µs against a 467 to 486 µs walk. It repeated across both runs, three days and two versions; it is a property of the declared Cabin's miss path, and nothing in this run explains it.
4. **DROP exists only at a 2-half-life cooldown.** The default dwell (128 half-lives) never fires inside a three-day run, so a 3-day business cycle at the defaults will not reclaim a Cabin whose hot set has moved; that is the controller's documented default, now with a second data point.

## 11. Archive

`bench/v3.3.0/archive/scenario4-{DESC}/`: both runs' JSON (including the full controller evidence and the 18-statement comparison), driver stdout, the three servers' configs per run, PostgreSQL's three JSON. What the four scenarios share is in `bench/v3.3.0/archive/run-{DESC}/`.
"""
write("results-scenario4-cabinopt-days", txt)
