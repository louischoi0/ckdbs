import sys
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/analysis")
from s4 import *
from stamp import *

tA, tB = "s4", "cd2-s4"
abt = [f"ab-{v}{i}-s4" for i in (1, 2, 3) for v in ("old", "new")]
A, B = now(tA), now(tB)
MA_, MB_ = mor(4, tA), mor(4, tB)


def ratio_on_off(d, g, day):
    return P(d, day, g, "off")["mean_us"] / P(d, day, g, "on")["mean_us"]


def pooled(g, arm):
    rs = []
    for tag in (tA, tB):
        for day in (1, 2, 3):
            rs.append(qd(P(now(tag), day, g, arm)) / qd(P(mor(4, tag), day, g, arm)))
    return med(rs)


def abmed(g, arm, v, day=1):
    return med([qd(P(now(f"ab-{v}{i}-s4"), day, g, arm)) for i in (1, 2, 3)])


def abr(g, arm):
    return abmed(g, arm, "new") / abmed(g, arm, "old")


def rng(g):
    v = [100 * (pooled(g, a) - 1) for a in ARMS]
    return f"{min(v):+.0f} % to {max(v):+.0f} %"


def pkmean(fn):
    return med([P(fn(t), d, "pk", a)["mean_us"] for t in (tA, tB) for d in (1, 2, 3) for a in ARMS])


def walk_ns(fn, arm="off"):
    p200 = med([P(fn(t), 1, "tape200", arm)["p50_us"] for t in (tA, tB)])
    p10 = med([P(fn(t), 1, "tape10k", arm)["p50_us"] for t in (tA, tB)])
    return (p10 - p200) / 9800 * 1000, p200, p10


def served_p50(fn, arm="on"):
    return med([P(fn(t), d, "tape10k", arm)["p50_us"] for t in (tA, tB) for d in (2, 3)])


def open_p50(fn):
    return med([P(fn(t), d, "open", a)["p50_us"] for t in (tA, tB) for d in (1, 2, 3) for a in ARMS])


def ab_spreads():
    v = []
    for g in GROUPS:
        for arm in ARMS:
            for vv in ("old", "new"):
                x = [qd(P(now(f"ab-{vv}{i}-s4"), 1, g, arm)) for i in (1, 2, 3)]
                v.append(spread(x))
    return min(v), max(v)


def run_ab_diff():
    r = []
    for g in GROUPS:
        for arm in ARMS:
            for day in (1, 2):
                a, b = qd(P(A, day, g, arm)), qd(P(B, day, g, arm))
                r.append(abs(a / b - 1) * 100)
    return max(r)


def tail_tbl():
    s = "| day | arm | run A mean | run A p50 | run A p95 | run A p99 | run B p95 | morning run A p95 |\n|---|---|---|---|---|---|---|---|\n"
    for day in (1, 2, 3):
        for arm in ARMS:
            a, b, m = P(A, day, "board", arm), P(B, day, "board", arm), P(MA_, day, "board", arm)
            s += f"| day {day} | {arm} | {a['mean_us']:,.1f} µs | {a['p50_us']:,.1f} µs | {a['p95_us']:,.1f} µs | {a['p99_us']:,.1f} µs | {b['p95_us']:,.1f} µs | {m['p95_us']:,.1f} µs |\n"
    return s


kt = [tA, tB]
PT = {t: t + '-off' for t in [tA, tB] + abt}
VOK = sum(1 for t in kt + abt if now(t) and all(not v["mismatches"] for v in now(t)["verify"]))
VN = len([t for t in kt + abt if now(t)])
CE = sum(now(t)["meta"]["client_errors"] for t in kt + abt if now(t))
SO = sum(now(t)["meta"]["slot_overruns"] for t in kt + abt if now(t))
wn, w200n, w10n = walk_ns(now)
wm, w200m, w10m = walk_ns(lambda t: mor(4, t))
cpu_off = (sum(v for k, v in A["server_cpu_s"]["off"].items() if k.endswith("-board")), sum(v for k, v in MA_["server_cpu_s"]["off"].items() if k.endswith("-board")))

txt = f"""# Scenario 4 (cabin optimizer over business days) at `{DESC}`

**One line.** Three business days on three servers (a Cabin controller `off`, `on`, and a Cabin `declared` up front), run twice, on the morning's binary with the new client decoder. The controller's lifecycle is the morning's, snapshot for snapshot
(four Cabins in the first block, a fifth on day 2, `board_a` DECAYING on day 2 and ACTIVE again on day 3 with its pages doubled, two DROPs and one re-nomination under `cabin_optimizer_cooldown_half_lives = 2`), the 18-statement
byte comparison across arms passed on all six days, and the engine's cost did not move: the unserved equality walk costs {wn:.1f} ns per row (morning {wm:.1f} ns) and the server CPU an `off` run spends on its boards is {cpu_off[0]:.2f} s ({cpu_off[1]:.2f} s).
What moved is the client's cost of the replies: every group that returns rows is faster, by {rng('board')} on `board`, {rng('tape200')} on `tape200`, {rng('tape1k')} on `tape1k`, {rng('tape10k')} on `tape10k` and {rng('close')} on `close`
(per arm, medians over three days and two runs, now over the morning), and the `pk` control (one row) and the durable `open` did not move ({rng('pk')} and {rng('open')}). The interleaved A/B (section 8) agrees arm by arm: `board` {abr('board', 'off'):.2f}x / {abr('board', 'on'):.2f}x / {abr('board', 'declared'):.2f}x
(`off` / `on` / `declared`). Because a Cabin-served probe is almost all fixed cost and round trip, the Cabin's multiple over the walk grows ({ratio_on_off(A, 'board', 1):.2f}x on `board` day 1 against {ratio_on_off(MA_, 'board', 1):.2f}x in the morning).
Against PostgreSQL 18.6, **reused from the morning**, day 1 with no index: `board` {med([qd(P(now(t), 1, 'board', 'off')) for t in kt]) / med([qd(ph(d, 'd1-board[pg]')) for d in Mo(4, 'pg-s4')]):.2f}x walking and {med([qd(P(now(t), 1, 'board', 'on')) for t in kt]) / med([qd(ph(d, 'd1-board[pg]')) for d in Mo(4, 'pg-s4')]):.2f}x with the controller; `close` is 0.67x (from 0.57x).

Driver: `tools/scenario4_cabinopt_days.py`, unmodified, two complete runs: **run A** at the server defaults (`s4`) and **run B** with `cabin_optimizer_cooldown_half_lives = 2` (`cd2-s4`), both with `decay_half_life = 5` and
`cabin_optimizer_snapshot_interval_ms = 500`. Measured by `ck-tester`. Comparator (this engine's own last run, same binary, old client): `bench/v3.3.0/results-scenario4-cabinopt-days-{MD}.md`. PostgreSQL 18.6: that file's twin cells
(`tools/pg_scenario4_cabinopt_days.py`, three runs), **not re-run**: {BUDGET}.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-20; run A {wtext([tA])}, run B {wtext([tB])}; A/B pairs (six runs of the driver, interleaved with the scenario 1 and 3 A/B cells) {wtext(abt)} |
{COMMONROWS}| Server config | the common config plus `decay_half_life = 5`, `cabin_optimizer_snapshot_interval_ms = 500` on all three servers, and in run B `cabin_optimizer_cooldown_half_lives = 2`; `cores = 1`, `durability = group`; ports 15651 to 15653 |
| Driver | **unmodified** `tools/scenario4_cabinopt_days.py`, `--port-off/-on/-declared` with the three servers' PIDs for the CPU readings, `--suffix a`; exit 0 in every run, `client_errors` {CE} and `slot_overruns` {SO} over {VN} runs; the driver's byte-for-byte comparison of 18 statements per day across the three arms passed in {VOK} of {VN} runs (3 days each) |
| Unchanged from the morning | {PATHS} |

## 2. What was run

Two complete runs of the driver, three days of three arms each, plus six more runs of the first for the A/B. Per day: an open (240 inserts into the day's board), a paced 45 s trading session (2,400 hot-board probes, 396 probes on each of three tape sizes,
240 pk controls, per arm, interleaved in 12 blocks), a close (`COUNT` / `SUM` / `GROUP BY` full scans, three rounds) and a 45 s idle night. The equal work per arm is fixed by the driver's counts, not by time.

    python3 tools/scenario4_cabinopt_days.py --port-off 15651 --port-on 15652 --port-declared 15653 \\
        --pid-off <p> --pid-on <p> --pid-declared <p> --suffix a --json <run>.json

**Row-set axis.** The tape relations are 200, 1,000 and 10,000 rows (`tape_200`, `tape_1k`, `tape_10k`); the boards are 10,000 rows each (`board_a`, `board_b`). Every phase is a row of the matrix, the size is
a row label. The probes are serial on one connection, so qps is derived as 1,000,000 / mean µs (the table cells give the p50 in µs beside it). The hot set rotates: day 1 hot on `board_a`, day 2 on `board_b`,
day 3 on `board_a` with a different symbol set; the tapes are hot every day with disjoint values.

Cut: nothing. PostgreSQL's twin was not re-run (it runs day 1 only, unpaced, no index, by its own design).

## 3. Throughput and latency by arm

p50 and derived qps (from the mean) per group, day and arm for **run A**; the last two columns are the ratio of mean latencies, off over on and off over declared, for run A and run B:

{arm_table()}
**What is resolved and what is not.** The `pk` control touches no Cabin and returns one row; its on-over-off ratio ranges {min(ratio_on_off(d, 'pk', day) for d in (A, B) for day in (1, 2, 3)):.2f}x to {max(ratio_on_off(d, 'pk', day) for d in (A, B) for day in (1, 2, 3)):.2f}x across days and runs,
so **a ratio below about 1.10x is not a finding**, as in the morning. That puts `tape200` at the floor (the Cabin does not help a 200-row walk: {min(P(d, day, 'tape200', 'off')['mean_us'] / P(d, day, 'tape200', a)['mean_us'] for d in (A, B) for day in (1, 2, 3) for a in ('on', 'declared')):.2f}x to {max(P(d, day, 'tape200', 'off')['mean_us'] / P(d, day, 'tape200', a)['mean_us'] for d in (A, B) for day in (1, 2, 3) for a in ('on', 'declared')):.2f}x), `tape1k` over it by a little and `tape10k` and `board` clear of it.

**Against the morning (now over morning, derived qps (1,000,000 / mean µs) per arm, run A and run B):**

{vs_morning()}
Pooled per group and arm over the six days (median, with the range):

{pooled_vs_morning()}
The moved groups are the ones that return rows (`board` and `tape*` return the hot symbol's rows, `close` returns grouped rows). `tape10k` moves least in the `off` arm (1.05x) because 345 µs of its 10,000-row walk is engine
time the client cannot shorten; its served arms move 1.10x to 1.11x, a round trip plus a handful of rows. The `pk` and `open` rows are the control: {min(pooled('pk', a) for a in ARMS):.2f}x to {max(pooled('pk', a) for a in ARMS):.2f}x and {min(pooled('open', a) for a in ARMS):.2f}x to {max(pooled('open', a) for a in ARMS):.2f}x, inside the 0.89x to 1.12x
that `pk` ranges over cell by cell.

## 4. Percentiles

Run A, every phase (microseconds; the five percentiles are the driver's):

{pct_table(A)}
PostgreSQL 18.6, day 1, no index, run 1 of the morning's twin:

{pct_table(mor(4, 'pg-s4'))}
## 5. Where the time goes

A probe is one round trip on one connection, so the decomposition is fixed cost plus rows walked plus rows returned; there is no commit or lock in the read phases and one durable commit per `open` statement.

| Component | Now | Morning (old client) | PostgreSQL (morning's run) | How derived |
|---|---|---|---|---|
| Fixed round trip (client, socket, parse, one pk descent) | **{pkmean(now):.1f} µs** | {pkmean(lambda t: mor(4, t)):.1f} µs | {med([ph(d, 'd1-pk[pg]')['mean_us'] for d in Mo(4, 'pg-s4')]):.1f} µs | `pk` mean across arms, days and runs (one row returned: the client change does not reach it) |
| Rows walked by an unserved equality | **{wn:.1f} ns per row** plus about {w200n:.0f} µs | {wm:.1f} ns plus about {w200m:.0f} µs | 40.5 ns plus about 76 µs (morning's derivation) | `off` tape p50 at 200 and 10,000 rows, day 1, median of two runs: (p50 at 10,000 - p50 at 200) / 9,800. The walk is the engine's; the fixed part is the client's reply |
| A Cabin-served equality | **about {served_p50(now):.0f} µs** at 10,000 rows (days 2 and 3) | about {served_p50(lambda t: mor(4, t)):.0f} µs | - | `on` tape10k p50, flat in n |
| Write statement with its commit (`open`) | **{open_p50(now):,.0f} µs** p50 per autocommit insert | {open_p50(lambda t: mor(4, t)):,.0f} µs | {med([ph(d, 'd1-open[pg]')['p50_us'] for d in Mo(4, 'pg-s4')]):,.0f} µs | `open` p50 over arms, days and runs: the durable commit is nearly all of it |
| Lock or conflict wait | none: one connection | none | none | - |

The board's latency tail by arm (run A day by day; run B and the morning for the p95), microseconds:

{tail_tbl()}
**The declared arm's tail is the open question this scenario keeps asking** and the client change did not touch it: its board p95 is {min(P(A, d, 'board', 'declared')['p95_us'] for d in (1, 2, 3)):,.0f} to {max(P(A, d, 'board', 'declared')['p95_us'] for d in (1, 2, 3)):,.0f} µs
against the `on` arm's {min(P(A, d, 'board', 'on')['p95_us'] for d in (1, 2, 3)):,.0f} to {max(P(A, d, 'board', 'on')['p95_us'] for d in (1, 2, 3)):,.0f} µs and the walk's {min(P(A, d, 'board', 'off')['p95_us'] for d in (1, 2, 3)):,.0f} to {max(P(A, d, 'board', 'off')['p95_us'] for d in (1, 2, 3)):,.0f} µs, as in the morning.

Server CPU seconds by arm (the driver's `/proc` reading), now and in the morning. **This is the engine's side of the statement and it did not move**:

{cpu()}
## 6. The controller's lifecycle

Snapshots of `SHOW CABIN_OPTIMIZER` on the `on` arm at chosen points:

run A:

{lifecycle(tA)}
run B:

{lifecycle(tB)}
Run A, defaults: four Cabins are created in the first 13 s of day 1, a fifth (`board_b`) on day 2's first block, `board_a` goes DECAYING on day 2 and returns to ACTIVE on day 3 with its pages doubled (9 to 18) when its hot set
comes back, then `board_b` goes DECAYING; the final header reads `creates=5 extends=172 heals=0 drops=0 deferred=0 failures=0`, **equal to the morning's**. Run B (cooldown 2 half-lives): `board_a` is DROPPED on day 2, re-nominated on day 3
(`creates=6`) and `board_b` dropped at day 3's last block; `creates=6 extends=194 drops=2 failures=0` (morning 193 extends). The controller does not see the client: its ticks are the engine's.

## 7. Correctness

The driver ran its 18-statement comparison across the three arms each day: **PASS on all three days in both runs and in all six A/B runs** (`verify` in the JSON, {VOK} of {VN} runs with no mismatch), so a Cabin, declared or controller-built, changed no reply, and the client change changed none either.

## 8. The client, isolated: old against new, interleaved

Three runs of the driver (each three servers, one run A) per client, old, new, old, new, old, new, the same binary copy, derived qps of day 1 (the tables above give days 2 and 3 for the matrix runs):

{ab()}
**The client moves what it can move, arm by arm.** `board` {abr('board', 'off'):.2f}x (`off`), {abr('board', 'on'):.2f}x (`on`), {abr('board', 'declared'):.2f}x (`declared`); `tape200`/`tape1k` served arms 1.18x to 1.21x; `close` 1.17x to 1.21x; `tape10k` `off` {abr('tape10k', 'off'):.2f}x
(the walk dominates). `open` is flat (0.98x to 1.01x). **The `pk` control is not flat in the `off` arm: {abr('pk', 'off'):.2f}x** ({med([qd(P(now(f'ab-old{i}-s4'), 1, 'pk', 'off')) for i in (1, 2, 3)]):,.0f} against {med([qd(P(now(f'ab-new{i}-s4'), 1, 'pk', 'off')) for i in (1, 2, 3)]):,.0f} qps, spreads 7.9 % and 1.0 %),
while its `on` and `declared` arms read +1.6 % and +6.9 % inside their spreads. A -11 % in a one-row statement that the change cannot reach is this scenario's noise floor, so the `board` `off` figure of +7.5 % (spreads 2.6 % and 1.9 %), smaller than the control's own excursion, is read as
"in the direction the other arms show", not as a separate result. The matrix runs (section 3) have no such excursion in their `pk` rows (`off` {pooled('pk', 'off'):.2f}x, six days).

## 9. Against PostgreSQL 18.6 (the morning's run, reused)

PostgreSQL is the floor. The twin runs **day 1 only, unpaced, one server, no index**, by its own design: at defaults a PostgreSQL probe seq-scans on day 1 as on day 300. Its numbers are the morning's three runs. KDS side: run A and run B day 1,
derived qps, the median of the two; ratio above 1.00x = KDS faster; the morning's KDS ratio after the table:

{pg_cmp()}
The comparison with a Cabin is not a like-for-like with PostgreSQL's *indexed* case, which this twin does not run (no `symbol` index on PostgreSQL); "KDS on / PG" says what a Cabin buys over a PostgreSQL seq scan, not over an index.
`tape200` is 0.82x to 0.83x on every arm (from 0.73x to 0.75x): the 200-row probe is still about 13 µs dearer than PostgreSQL's ({w200n:.0f} against 76 µs p50) while the `pk` round trip differs by {pkmean(now) - med([ph(d, 'd1-pk[pg]')['mean_us'] for d in Mo(4, 'pg-s4')]):.0f} µs ({pkmean(now):.1f} against {med([ph(d, 'd1-pk[pg]')['mean_us'] for d in Mo(4, 'pg-s4')]):.1f} µs mean), so the
gap is not the round trip; whether it is the scan statement's setup or the 200-row walk, this run does not separate. `close` (three full-relation folds) is 0.67x to 0.68x on every arm (from 0.57x): the fold returns grouped rows and gained from the decoder, and the rest is the whole-relation fold cost that scenario 1 measures as 0.68x on `agg-global` and `agg-distinct`.

## 10. Noise floor, contamination and host

The floor is the `pk` control: on-over-off {min(ratio_on_off(d, 'pk', day) for d in (A, B) for day in (1, 2, 3)):.2f}x to {max(ratio_on_off(d, 'pk', day) for d in (A, B) for day in (1, 2, 3)):.2f}x in the matrix runs, and the A/B spreads of the group cells are {ab_spreads()[0]:.1f} % to {ab_spreads()[1]:.1f} % (the widest is `open`, the durable group). PostgreSQL's three runs spread 0.2 % to 10.9 %
(`open` 10.9 %, the durable group). The two KDS runs give the repeat: run A and run B differ by up to {run_ab_diff():.0f} % per group and arm on days 1 and 2 (day 3 is excluded: run B rebuilds a Cabin there). No KDS cell was contaminated in this scenario (`logs/validity.log`).

Per-cell host stamps (run A and B hold three servers each; the precheck of the `-off` server stands for the cell, the monitor covered the whole window):

{hostrows(kt + abt, PT)}
## 11. What the run says about the engine

1. **Only the client's half of a served probe changed, and a Cabin-served probe is mostly client.** The engine's side did not move (walk {wn:.1f} ns per row, server CPU 2.20 s for the `off` boards), while a served `board` probe is {A and ph(A, 'd1-board[on]')['p50_us']:.0f} µs
   against {ph(MA_, 'd1-board[on]')['p50_us']:.0f} µs: the Cabin's multiple over a walk rose from {ratio_on_off(MA_, 'board', 1):.2f}x to {ratio_on_off(A, 'board', 1):.2f}x on `board` without the Cabin doing anything differently. A cache that answers in a round trip is measured by its client.
2. **The controller is independent of the client**: the lifecycle, the page counts and the extends are the morning's within one extend, in both runs.
3. **The declared arm's tail** (p95 {P(A, 1, 'board', 'declared')['p95_us'] / P(A, 1, 'board', 'off')['p95_us']:.1f}x the walk's on day 1) is untouched by the decoder, which places it in the engine's miss path of a declared Cabin; this run, like the morning's, does not explain it.

## 12. Archive

`bench/v3.3.0/archive/scenario4-{DESC}/`: both matrix runs' JSON (including the full controller evidence and the 18-statement comparison), the six A/B runs' JSON, driver stdout, the three servers' configs per run. What the four scenarios share is in
`bench/v3.3.0/archive/run-{DESC}/`. The PostgreSQL twin's JSON is in the morning's archive, `bench/v3.3.0/archive/scenario4-{MD}/json/`.
"""
write("results-scenario4-cabinopt-days", txt)
