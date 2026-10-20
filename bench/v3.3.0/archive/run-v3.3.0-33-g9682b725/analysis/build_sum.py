import sys
sys.path.insert(0, "/home/cdkbs/bench-run")
from doc import *
from an import *
import s3_tables as S
import s4_tables as t4
from s1_tables import sweep, cellruns

K = cellruns("s1-c1-g")
O = cellruns("s1b-c1-g", True)
PGR = runs(new, "pg-s1")


def mw(rr, sh, key="warm"):
    return med([sweep(d)[sh][key]["qps"] for d in rr if sweep(d).get(sh) and sweep(d)[sh].get(key)])


def s1row(sh, label):
    a, b, p = mw(K, sh), mw(O, sh), mw(PGR, sh)
    return f"| 1 backtest, `{label}` (warm, `cores = 1`) | {f1(a, ' qps')} | {f1(b, ' qps')} | {delta(a, b)} | {f1(p, ' qps')} | {ratio(a, p)} |"


tp = lambda t: med([d["meta"]["tps"] for d in runs(new, t)])
tpo = lambda t: med([d["meta"]["tps"] for d in runs(lambda x: old(2, x), t)])
pg2 = tp("pg-s2-b1")


def s3row(sh, n, m, label):
    a, b, p = (med(S.cellq(k, 1, n, m, sh)) for k in ("new", "old", "pg"))
    return f"| 3 library, `{label}` ({n:,} loans, {'unindexed' if m == 'none' else 'indexed'}, `cores = 1`) | {f0(a, ' qps')} | {f0(b, ' qps')} | {delta(a, b)} | {f0(p, ' qps')} | {ratio(a, p)} |"


N, O4 = t4.runs4()
PG4 = runs(new, "pg-s4")


def s4row(g, label):
    a = 1e6 / t4.P(N["A"], 1, g, label[1])["mean_us"]
    b = 1e6 / t4.P(O4["A"], 1, g, label[1])["mean_us"]
    p = med([1e6 / ph(d, f"d1-{g}[pg]")["mean_us"] for d in PG4])
    return f"| 4 cabinopt, `{g}` day 1, arm `{label[1]}` (run A) | {f0(a, ' qps')} | {f0(b, ' qps')} | {delta(a, b)} | {f0(p, ' qps')} (no index) | {ratio(a, p)} |"


geo = S.COLS
import math


def geo3(kind, other, n, m):
    r = [med(S.cellq(kind, 1, n, m, sh)) / med(S.cellq(other, 1, n, m, sh)) for sh in S.SHAPES]
    return math.exp(sum(math.log(x) for x in r) / len(r))


txt = f"""# Scenarios 1 to 4 at `{DESC}`: summary

**One line.** The engine at `{DESC}` is, within the run's spreads, the engine of v3.1.0 on most shapes this suite measures; the largest movers are two: the unserved equality **walk** (scenario 4's `off` arm is
1.18x to 1.20x faster on a 10,000-row board and tape) and the **inner-built two-relation join** (scenario 3's `join-no-literal` at 10,000 loans runs 1.98x). Smaller movers (eight-core one-connection reads, eight contended bookers) are listed below; reads, writes, the Cabin multiples and the controller lifecycle are inside the
run's own spreads. **Scenario 1's driver now runs unmodified** (BZ made its `HEAP` tables hidden-key btrees; no BTREE copy was needed).
**The join planner (CB/CC) reorders two of scenario 1's joins and moves neither beyond noise, and it changes nothing in scenario 3's joins**: A/B against `join_order = written` on the same binary.
One durable client is PostgreSQL's equal (0.99x), because both engines wait on the same fsync. PostgreSQL 18.6 was **re-measured fresh in this window**, so every ratio below is same-window.

This file indexes the four results files and carries no number they do not. Measured by `ck-tester` from 2026-10-20 03:37 UTC to 04:44 UTC on one `build-release` binary built at `9682b725` (sha256
`{SHA[:8]}...`, section 1 of each file). **The operator lifted the time budget for this run** ("lift the budget, re-measure PG too"): every cell of the v3.1.0 plan ran, with its repeat counts, nothing was cut,
and the size sweep (scenario 1) and booker sweep (scenario 2) that v3.1.0 left unfinished or unwritten were run and written up.

**Comparators.** KDS: v3.1.0 at `cd7c7c4e` (`v3.1.0-30-gcd7c7c4e`, measured 2026-10-13 to 14), 240 commits and 101 first-parent merges away, so every KDS delta is **cross-version** and attributable to no
single change; re-derived from that run's archived JSON. Scenario 1's v3.1.0 cells ran a BTREE-only copy of the driver. PostgreSQL 18.6: **fresh**, `/home/cdkbs/pg-bench-ba` port 15434 at its defaults, three runs
per cell wherever a twin exists, run after the KDS cells and never concurrent with one. PostgreSQL is the floor, not the reference.

## Headlines

Warm qps (scenario 1), tps (2), derived qps (3, 4), median of three runs (scenario 4: run A, day 1). Delta = now over v3.1.0 (cross-version); ratio = now over PostgreSQL (above 1.00x = KDS faster).

| measure | KDS now | KDS v3.1.0 | delta (cross-version) | PostgreSQL 18.6 (fresh) | KDS/PG |
|---|---|---|---|---|---|
{s1row('day-slice', 'day-slice')}
{s1row('cross-join', 'cross-join')}
{s1row('point-join', 'point-join')}
{s1row('bar-range', 'bar-range')}
{s1row('symbol-history', 'symbol-history')}
{s1row('model-join', 'model-join')}
| 2 freight, 8 bookers, `cores = 8`, `group` | {f1(tp('s2-c8-g'), ' tps')} | {f1(tpo('s2-c8-g'), ' tps')} | {delta(tp('s2-c8-g'), tpo('s2-c8-g'))} | no counterpart (one booker) | - |
| 2 freight, 8 bookers, `cores = 8`, `strict` | {f1(tp('s2-c8-s'), ' tps')} | {f1(tpo('s2-c8-s'), ' tps')} | {delta(tp('s2-c8-s'), tpo('s2-c8-s'))} | no counterpart (one booker) | - |
| 2 freight, 1 booker, durable, `group` | {f1(tp('b1-s2-c1-g'), ' tps')} | {f1(tpo('b1-s2-c1-g'), ' tps')} | {delta(tp('b1-s2-c1-g'), tpo('b1-s2-c1-g'))} | {f1(pg2, ' tps')} | {ratio(tp('b1-s2-c1-g'), pg2)} |
| 2 freight, 1 booker, durable, `strict` | {f1(tp('b1-s2-c1-s'), ' tps')} | {f1(tpo('b1-s2-c1-s'), ' tps')} | {delta(tp('b1-s2-c1-s'), tpo('b1-s2-c1-s'))} | {f1(pg2, ' tps')} | {ratio(tp('b1-s2-c1-s'), pg2)} |
{s3row('loans-by-user', 10000, 'none', 'loans-by-user')}
{s3row('join-no-literal', 10000, 'none', 'join-no-literal')}
{s3row('exists-correlated', 10000, 'none', 'exists-correlated')}
{s3row('loans-by-user', 10000, 'all', 'loans-by-user')}
| 3 library, geometric mean over 12 shapes (10,000 loans, unindexed / indexed) | - | - | {geo3('new', 'old', 10000, 'none'):.2f}x / {geo3('new', 'old', 10000, 'all'):.2f}x | - | {geo3('new', 'pg', 10000, 'none'):.2f}x / {geo3('new', 'pg', 10000, 'all'):.2f}x |
{s4row('board', ('board', 'off'))}
{s4row('board', ('board', 'on'))}
{s4row('board', ('board', 'declared'))}
{s4row('tape10k', ('tape10k', 'off'))}
{s4row('tape10k', ('tape10k', 'on'))}

Files: `results-scenario1-backtest-{DESC}.md`, `results-scenario2-freight-{DESC}.md`, `results-scenario3-library-{DESC}.md`, `results-scenario4-cabinopt-days-{DESC}.md`.

## The movers, and what they are

- **The unserved equality walk is cheaper (scenario 4 `off`).** 1.18x on `board`, 1.20x on `tape10k`, every day of both runs, against a `pk` control that reads 0.85x to 1.07x; the per-row cost of an unserved equality falls from
  34.6 to 26.5 ns and the server CPU of an `off` run's boards from 2.80 s to 2.22 s. Scenario 3's integer-column equality walk shows **no** such gain (`loans-by-user` 2,779 qps in both versions), so the gain is not general; the filtered
  column here is a `varchar`, among other differences. Likely cause: not separated by this run, 240 commits.
- **`join-no-literal` at 10,000 loans is 1.98x (scenario 3).** The plan shows `inner_built=1` (a statement-local hash build of `loans`: one 10,074-row scan for a 16-row outer instead of 16 scans). The join planner is not the cause: with
  `join_order = written` the same cell is 5 % *faster*, and its plan carries the same build. Whether v3.1.0 already built cannot be read from its JSON (step kinds only). Likely cause: the inner build's reach (`join-inner-build.md`) or the scan beneath it.
- **`cores = 8` one-connection reads gained 28 % to 31 % (scenario 1).** At v3.1.0 a one-connection read on an eight-core instance ran 22 % slower than on one core; now 6 % slower. The one-core engine did not move (+7 %, inside its 7 % spread).
- **Eight contended bookers: +3.3 % `group`, +5.6 % `strict`** (scenario 2), above their spreads (0.5 %, 3.4 %), with 54 / 42 / 39 `strict` conflicts per run against 90 / 63 / 45. Small, real, and carried by the commit (its mean fell 5 % to 9 % at eight cores and 18 % with one booker), not by the statements: every statement p50 in scenario 2's comparison table is 1.01x to 1.11x of v3.1.0's, and scenario 4's `pk` control is +4.7 % (73.4 against 70.1 µs). A 3 % to 7 % per-statement slowdown is visible and is **not separated between engine and host** by this run.
- **One durable booker: +9 % to +11 % against v3.1.0, and equal to PostgreSQL.** That is the **device**: the commit mean fell from 1,646 to 1,344 µs while no statement moved, and PostgreSQL's commit in this window is 1,343 µs. A delta
  on a fsync-bound row is the sync's, not the engine's.
- **The join planner** (CB/CC): `compare-all` and `compare-one` are reordered (`reordered=1`) and read 14x fewer pages and 6 % to 11 % fewer examined rows, and run 1 % to 6 % faster, inside the noise; the three-relation join and scenario 3's joins keep the written order.
  The planner's price is not measurable in this suite and neither is its payoff.

## Where KDS stands against PostgreSQL

- **Ahead**: an unindexed non-pk equality walk (scenario 1 `day-slice` {ratio(mw(K, 'day-slice'), mw(PGR, 'day-slice'))}, scenario 3 `loans-by-user` {ratio(med(S.cellq('new', 1, 10000, 'none', 'loans-by-user')), med(S.cellq('pg', 1, 10000, 'none', 'loans-by-user')))}, scenario 4 `board` {ratio(1e6 / t4.P(N['A'], 1, 'board', 'off')['mean_us'], med([1e6 / ph(d, 'd1-board[pg]')['mean_us'] for d in PG4]))});
  the pk-anchored join (1.79x) and the three-relation join (1.46x); the correlated `EXISTS` ({ratio(med(S.cellq('new', 1, 10000, 'none', 'exists-correlated')), med(S.cellq('pg', 1, 10000, 'none', 'exists-correlated')))} at 10,000 loans); a Cabin-served equality (scenario 4 `board` 2.1x, `tape10k` 2.5x of a PostgreSQL seq scan);
  the write statements of a booking (p50 0.73x to 0.84x of PostgreSQL's); loads (0.96x to 1.62x).
- **Even**: one durable client (0.99x), bound by the device's fsync on both engines; the round trip of a pk read at 10,000 loans (`pk-user` 0.98x).
- **Behind**: every shape that returns many rows (scenario 1 `bar-range` 0.49x, `symbol-history` 0.43x, `model-join` 0.56x; scenario 3 `books-by-genre`, `loans-by-daterange`, `overdue` 0.61x to 0.77x), whole-relation folds
  (scenario 1 `agg-*` 0.65x to 0.69x, scenario 4 `close` 0.57x), and the fixed round trip at small sizes (scenario 3 at 200 loans 0.62x to 0.99x on ten of twelve shapes; scenario 4 `tape200` 0.73x). The
  per-row result cost is about 2x PostgreSQL's (7.5 to 9.3 µs against 3.6 to 4.1 µs per returned row, at every size from 252 to 60,480 bars).

## What this run does not show

- **No limit was found for scenario 2.** The booker sweep reaches 3,768 tps at 32 bookers with the host at most 67 % busy; the cells are 3,000 bookings (under two seconds of booking time) driven from one Python process, so the client may be the bound.
  A saturation measurement needs more bookings and a second client process (rule 4b).
- **Scenario 3's cells at 200 and 1,000 loans carry 20 % to 43 % spreads** (one slow run in three of a 20 ms cell); no comparison there, cross-version or against PostgreSQL, is read.
- **Scenario 2's row-set axis is not swept** (rule 9): its sizes are fixed by the driver's arguments, as in v3.1.0.
- **Scenario 4's PostgreSQL twin has no index** (a Cabin's fair counterpart would be one on `symbol`), runs day 1 only, and is unpaced.
- Whether the factor of two in per-row result cost lives in the engine or in the two Python wire decoders (`tools/kwp.py` against `tools/pg_wire.py`) is not separated.
- None of the five files has had a `critics-developer` review.

## Contamination

No KDS cell was discarded: a 1-second monitor and a per-cell validity check found no build, `ctest` or foreign process at 50 % CPU in any window (the only foreign process above 10 % CPU was a `claude` process, 23 samples of 1,518). One batch of
six PostgreSQL scenario-3 attempts failed in seconds on a connection error (`role "ec2-user" does not exist`, the twin's default `--user`); they are kept in `contaminated/` of the run archive and the six cells were re-run with `--user cdkbs`.
The unrelated `kds_server` on port 15432 was not touched.

## Archive

- `bench/v3.3.0/archive/scenarioN-{DESC}/`: each scenario's per-cell JSON (KDS and PostgreSQL), driver stdout, server configs. Scenarios 1 and 3 also hold `plans/` (the full ANALYZE replies of the join-order probe).
- `bench/v3.3.0/archive/run-{DESC}/`: what the four scenarios share: the run scripts (`lib.sh`, `run_all.sh`, `run_plan.sh`, watchdog and monitor), the timeline, the monitor and watchdog logs, the validity log, the binary hash, the contaminated attempt,
  and the table generators under `analysis/`.
"""
write("summary-scenarios", txt)
