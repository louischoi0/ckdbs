import sys, re
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/analysis")
from docs_common import *

A = "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/analysis"
md = open(f"{A}/s4.md").read()


def sec(text, head):
    i = text.index(head)
    j = text.find("\n### ", i + 5)
    body = text[i:j if j > 0 else len(text)]
    return body.split("\n", 1)[1].strip("\n")


LIFE = sec(md, '### optimizer lifecycle').split('old finals:')[0].strip()
dA, dB, oA, oB = load("s4"), load("cd2-s4"), load("s4", 4), load("cd2-s4", 4)
ARMS = ["off", "on", "declared"]


def q(d, day, g, arm):
    return 1e6 / ph(d, f"d{day}-{g}[{arm}]")["mean_us"]


def rng(d, g, arm):
    v = [q(d, day, g, arm) for day in (1, 2, 3)]
    return f"{min(v):,.0f} to {max(v):,.0f}"


# CPU table
cpu = ["| arm | board A | board B | board 9a0525d A / B | tape A | tape B | tape 9a0525d A / B | open A | close A |", "|---|---|---|---|---|---|---|---|---|"]
for arm in ARMS:
    def s_(d, g):
        return sum(v for k, v in d["server_cpu_s"][arm].items() if k.endswith(g))
    cpu.append(f"| {arm} | {s_(dA,'board'):.2f} s | {s_(dB,'board'):.2f} s | {s_(oA,'board'):.2f} s / {s_(oB,'board'):.2f} s | {s_(dA,'tape'):.2f} s | {s_(dB,'tape'):.2f} s | {s_(oA,'tape'):.2f} s / {s_(oB,'tape'):.2f} s | {s_(dA,'open'):.2f} s | {s_(dA,'close'):.2f} s |")
cpu = "\n".join(cpu)

# busy tps from stdout
def busy(path):
    t = open(path).read()
    rows = re.findall(r"day (\d) \[(\w+)\s*\]\s+ops=\s*(\d+)\s+busy=\s*([0-9.]+)s\s+tps=\s*(\d+)", t)
    return {(int(d), a): (int(tps), float(b)) for d, a, o, b, tps in rows}


bA, bB = busy(f"{L}/s4.stdout"), busy(f"{L}/cd2-s4.stdout")
T_busy = ["| day | off A | on A | declared A | off B | on B | declared B |", "|---|---|---|---|---|---|---|"]
for day in (1, 2, 3):
    T_busy.append(f"| {day} | " + " | ".join(f"{bx[(day, a)][0]:,} tps" for bx in (bA, bB) for a in ARMS) + " |")
T_busy = "\n".join(T_busy)

# board tail table
T_tail = ["| day | arm | run A mean | run A p50 | run A p95 | run A p99 | run B p95 | 9a0525d run A p95 |", "|---|---|---|---|---|---|---|---|"]
for day in (1, 2, 3):
    for arm in ARMS:
        p, pb, po = ph(dA, f"d{day}-board[{arm}]"), ph(dB, f"d{day}-board[{arm}]"), ph(oA, f"d{day}-board[{arm}]")
        T_tail.append(f"| {day} | {arm} | {fmt_us(p['mean_us'])} | {fmt_us(p['p50_us'])} | {fmt_us(p['p95_us'])} | {fmt_us(p['p99_us'])} | {fmt_us(pb['p95_us'])} | {fmt_us(po['p95_us'])} |")
T_tail = "\n".join(T_tail)

# on over off and declared; deltas
walk_now = [ (ph(d, "d1-tape10k[off]")["p50_us"] - ph(d, "d1-tape200[off]")["p50_us"]) / 9800 * 1000 for d in (dA, oA)]
board_on_over_off = [q(dA, d, "board", "on") / q(dA, d, "board", "off") for d in (1, 2, 3)]
board_on_over_off_o = [q(oA, d, "board", "on") / q(oA, d, "board", "off") for d in (1, 2, 3)]
t10_on_over_off = [q(dA, d, "tape10k", "on") / q(dA, d, "tape10k", "off") for d in (1, 2, 3)]
t10_on_over_off_o = [q(oA, d, "tape10k", "on") / q(oA, d, "tape10k", "off") for d in (1, 2, 3)]
d3A, d3B = q(dA, 3, "board", "on"), q(dB, 3, "board", "on")
d3Ao, d3Bo = q(oA, 3, "board", "on"), q(oB, 3, "board", "on")
pkr = [q(dA, d, "pk", a) / q(oA, d, "pk", a) for d in (1, 2, 3) for a in ARMS] + [q(dB, d, "pk", a) / q(oB, d, "pk", a) for d in (1, 2, 3) for a in ARMS]
pkoff = [ph(dA, f"d{d}-pk[{a}]")["mean_us"] for d in (1, 2, 3) for a in ARMS] + [ph(dB, f"d{d}-pk[{a}]")["mean_us"] for d in (1, 2, 3) for a in ARMS]
openA = [ph(dA, f"d1-open[{a}]")["p50_us"] for a in ARMS]
openB = [ph(dB, f"d1-open[{a}]")["p50_us"] for a in ARMS]

starts, ends = timeline_windows()
rows_host = gate_tbl(["s4", "cd2-s4"])
T_host = ["| run | arm precheck | window UTC | loadavg 1/5/15 | gate wait | build/test at precheck | monitor samples | build/test | foreign >= 50 % | minor |", "|---|---|---|---|---|---|---|---|---|---|"]
for (t, tm, e, la, gw, pr, n, b, h, mi) in rows_host:
    T_host.append(f"| `{t}` | `{t}-off` (the three arms' prechecks are identical in time) | {tm[:10]} {tm[11:19]} to {e[11:19]} | {la} | {gw} s | {pr} | {n} | {b} | {h} | {mi} |")
T_host = "\n".join(T_host)

pre = {}
for t in ("s4", "cd2-s4"):
    pre[t] = [precheck(f"{t}-{a}") for a in ARMS]

doc = f"""# Scenario 4 (cabin optimizer over business days) at `{DESC}`

**One line.** The autonomous Cabin controller still delivers what a hand-declared
Cabin does on a hot non-pk equality, and the walk it replaces got twice as cheap. On a 10,000-row board
(run A, days 1 to 3) the `off` arm that walks serves {rng(dA,'board','off')} qps (comparator, cross-version: {rng(oA,'board','off')}), the `on` arm
{rng(dA,'board','on')} qps ({rng(oA,'board','on')}) and the `declared` arm {rng(dA,'board','declared')} qps ({rng(oA,'board','declared')}), with byte-identical
answers on all three days in both runs. A Cabin's multiple over the walk fell with the walk's cost: `on` over `off` on the board is
{min(board_on_over_off):.2f}x to {max(board_on_over_off):.2f}x ({min(board_on_over_off_o):.2f}x to {max(board_on_over_off_o):.2f}x at the comparator) and on the 10,000-row tape {min(t10_on_over_off):.2f}x to {max(t10_on_over_off):.2f}x
({min(t10_on_over_off_o):.2f}x to {max(t10_on_over_off_o):.2f}x). **At the default `cabin_optimizer_cooldown_half_lives = 128` the run still never DROPs a Cabin
(`drops=0`)**; run B at cooldown 2 drops and re-nominates `board_a`, which costs day 3's board throughput
{100*(1-d3B/d3A):.0f} % (the comparator: {100*(1-d3Bo/d3Ao):.0f} %).

Driver: `tools/scenario4_cabinopt_days.py`, unmodified, three servers (arms `off`, `on`, `declared`) on fresh data
files, blocks interleaved so only one arm is active at a time; the 2026-09-30 rebaseline's arguments and two
runs. Comparator (cross-version): `bench/v3.0.0/results-scenario4-cabinopt-days-v2.7.0-531-g9a0525d.md`, held at
`36449d28`. PostgreSQL 18.6 figures are **reused, not re-measured**, from
`results-scenario4-cabinopt-days-pg18-v2.7.0-545-gf2f1ee7.md` (held at `36449d28`, measured 2026-09-30,
**14 days before this run**). The time budget was lifted by the operator for this run.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-14; run A {pre['s4'][0][0][11:19]} to {ends['s4'][0][11:19]} UTC ({dA['meta']['elapsed_s']:.1f} s), run B {pre['cd2-s4'][0][0][11:19]} to {ends['cd2-s4'][0][11:19]} UTC ({dB['meta']['elapsed_s']:.1f} s) |
{COMMON_STAMP}
| Server config (all three servers) | `cores = 1`, `durability = group`, `log_level = warn`, **`buffer_pool_frames = 65536`** (required since BE; the comparator's config predates the key), `decay_half_life = 5` (seconds; the driver docstring's 120x compression of the 600 s default), `cabin_optimizer_snapshot_interval_ms = 500`; **run B only** `cabin_optimizer_cooldown_half_lives = 2`. Ports 15651 to 15653, never 15432. The `on` arm runs `SET CABIN_OPTIMIZER ON` at day 1 open; the `declared` arm declares `CREATE CABIN` on every board and tape symbol column up front |
| Driver arguments | defaults: `--days 3 --blocks 12 --session-seconds 45 --overnight-seconds 45 --open-inserts 240 --board-probes 2400 --tape-probes 396 --pk-ops 240 --close-rounds 3 --seed 20260810 --suffix a`, plus `--port-off/-on/-declared` and the three server pids |
{HOST_ROWS}
| PostgreSQL | **reused, not re-measured**: PostgreSQL 18.6 at its defaults, no index and no Cabin, day 1 only, `results-scenario4-cabinopt-days-pg18-v2.7.0-545-gf2f1ee7.md` (held at `36449d28`), measured 2026-09-30 at `f2f1ee7`. Section 8 |

## 2. What was run

Two complete runs of the driver, three days of three arms each: **run A** at the server defaults with the driver's
compression, **run B** identical but for `cabin_optimizer_cooldown_half_lives = 2` (run A cannot exercise DROP:
section 7). Per day: an open (240 inserts into the day's board), a paced 45 s trading session (2,400 hot-board
probes, 396 probes on each of three tape sizes, 240 pk controls, per arm, interleaved in 12 blocks), a close
(`COUNT` / `SUM` / `GROUP BY` full scans, three rounds) and a 45 s idle night. The equal work per arm is fixed by the
driver's counts, not by time. The driver verifies an identical 18-statement set across the three arms byte for byte
each day. Command (`run_all4.sh`, `lib.sh` in the archive):

    python3 tools/scenario4_cabinopt_days.py --port-off 15651 --port-on 15652 --port-declared 15653 \\
        --pid-off <p> --pid-on <p> --pid-declared <p> --suffix a --json <run>.json

**Row-set axis (rule 9).** The tape relations are 200, 1,000 and 10,000 rows (`tape_200`, `tape_1k`, `tape_10k`);
the boards are 10,000 rows each (`board_a`, `board_b`). Every phase is a row of the matrix and the size is a
row label. The measured statements are serial on one connection, so qps is derived as 1,000,000 / mean µs. The
driver paces its sessions by wall clock (45 s per session), so its own busy-time TPS (section 3) is a derived
figure as well.

**The hot set rotates**: day 1 hot on `board_a`, day 2 on `board_b`, day 3 on `board_a` with a different symbol set;
the tapes are hot every day with disjoint values. No build or test process was live in any reported window (section 9).

## 3. Throughput and latency by arm

p50 and derived qps per group, day and arm, run A / run B. The last column is the ratio of mean latencies, off
over on:

{sec(md, '### p50 and derived qps by arm and day')}

Driver's busy-time TPS (timed ops / summed latencies, 4,080 ops per arm per day):

{T_busy}

**What is resolved and what is not.** The `pk` control (touches no Cabin) reads {min(pkr):.2f}x to {max(pkr):.2f}x against the comparator and
{min(1e6/x for x in pkoff):,.0f} to {max(1e6/x for x in pkoff):,.0f} qps across arms, days and runs; the on-over-off ratio on `pk` is 0.95x to 1.14x, so
**a ratio below about 1.15x is not a finding**, as at the comparator. That puts `tape200` (0.97x to 1.09x) at the floor, `tape1k`
(1.14x to 1.33x) barely over it, `tape10k` (2.15x to 2.84x) and `board` (1.95x to 2.40x) clear of it, and `open` and `close` (0.96x to
1.07x) as *not measurably different between arms*.

**Cross-version (now over `9a0525d`, derived qps, per arm; run A and run B):**

{sec(md, '### cross-version: derived qps')}

## 4. Percentiles

Every phase, in the driver's own order, microseconds; `max` and `err` as the driver reports them. Run A (server defaults):

{sec(md, '### full percentile table, run A')}

Run B (cooldown 2 half-lives):

{sec(md, '### full percentile table, run B')}

## 5. Where the time goes

A probe is one round trip on one connection, so the decomposition is fixed cost plus rows walked:

| Component | Now | Comparator | How derived |
|---|---|---|---|
| Fixed round trip (client, socket, parse, one pk descent) | **{statistics.median(pkoff):.1f} µs** | about 70 µs | `pk` mean across arms, days and runs ({min(pkoff):.1f} to {max(pkoff):.1f} µs) |
| Rows walked by an unserved equality | **{walk_now[0]:.0f} ns per row** plus about {ph(dA,'d1-tape200[off]')['p50_us']-200*walk_now[0]/1000:.0f} µs | 87 ns per row plus about 102 µs | `off` tape p50 at 200 rows {ph(dA,'d1-tape200[off]')['p50_us']:.1f} µs and at 10,000 rows {ph(dA,'d1-tape10k[off]')['p50_us']:.1f} µs (day 1, run A): ({ph(dA,'d1-tape10k[off]')['p50_us']:.1f} - {ph(dA,'d1-tape200[off]')['p50_us']:.1f}) / 9,800 |
| A Cabin-served equality | about 98 to 100 µs at any size once built (day 2); 104 to 125 µs on day 1 while the Cabins are still being created and extended | about 104 to 113 µs | `on` and `declared` `tape200` / `tape1k` / `tape10k` p50 after the first day-1 observation: {ph(dA,'d1-tape200[on]')['p50_us']:.1f}, {ph(dA,'d1-tape1k[on]')['p50_us']:.1f}, {ph(dA,'d1-tape10k[on]')['p50_us']:.1f} µs on day 1 (still building), {ph(dA,'d2-tape200[on]')['p50_us']:.1f}, {ph(dA,'d2-tape1k[on]')['p50_us']:.1f}, {ph(dA,'d2-tape10k[on]')['p50_us']:.1f} µs on day 2; flat in n |
| Unobserved values and coverage misses | a walked tail on both served arms; see the tail table | `board` p95 1,130 µs `on`, 1,061 µs `declared` | a Cabin is authoritative only for observed values (`docs/spec/cabin.md` C1), so a probe of a value not yet observed walks in either arm; the uniform draw (20 % of each board draw) keeps meeting new ones |
| Write-side upkeep | not resolvable: `open` p50 {openA[0]:,.0f} / {openA[1]:,.0f} / {openA[2]:,.0f} µs (`off` / `on` / `declared`, day 1, run A) | 1,328 / 1,353 / 1,335 µs | inside the floor between arms; **run B's `open` is {openB[0]:,.0f} / {openB[1]:,.0f} / {openB[2]:,.0f} µs**, 31 % slower than run A's for the same code, five minutes later: the device's sync moves by about a third between runs (section 9) |
| Durability / commit | not applicable to the probes; `open` and `load` are the only writers and each statement is one commit | | |
| Lock or conflict wait | none: one connection | | |

The board's latency tail by arm (run A day by day; run B and the comparator for the p95), microseconds:

{T_tail}

**The declared arm now has the heavier tail.** In every day and in both runs the `declared` arm's board p95 is 840 to 853 µs against
the `on` arm's 474 to 529 µs and the walk's 542 to 564 µs; at the comparator the three arms' p95 lay within 5 % of each other
(1,018 to 1,139 µs). A probe that the declared Cabin cannot answer therefore costs about 1.5x a plain walk, and the controller's
does not. This is why the `on` arm's mean (259 µs, day 1 run A) is lower than the `declared` arm's (285 µs) although the `declared`
p50 is lower (174 against 190 µs): the order of the two arms in the mean is the reverse of the comparator's (416 against 329 µs). This run does not say why the
declared arm's miss path is costlier; the controller arm's tail is not.

Server CPU seconds by arm (the driver's `/proc` reading), now and at the comparator:

{cpu}

`off` burns {sum(v for k,v in dA['server_cpu_s']['off'].items() if k.endswith('board')) + sum(v for k,v in dA['server_cpu_s']['off'].items() if k.endswith('tape')):.2f} s of server CPU on a run's boards and tapes (comparator
{sum(v for k,v in oA['server_cpu_s']['off'].items() if k.endswith('board')) + sum(v for k,v in oA['server_cpu_s']['off'].items() if k.endswith('tape')):.2f} s), the controller
{sum(v for k,v in dA['server_cpu_s']['on'].items() if k.endswith('board')) + sum(v for k,v in dA['server_cpu_s']['on'].items() if k.endswith('tape')):.2f} s and the declared arm {sum(v for k,v in dA['server_cpu_s']['declared'].items() if k.endswith('board')) + sum(v for k,v in dA['server_cpu_s']['declared'].items() if k.endswith('tape')):.2f} s.

## 6. The controller's lifecycle

Snapshots of `SHOW CABIN_OPTIMIZER` on the `on` arm at chosen points, run A then run B:

{LIFE}

Run A, defaults: four Cabins are created in the first 13.5 s of day 1 (`board_a`, `tape_200`, `tape_1k`, `tape_10k`: 4 creates,
31 extends by block 3), grown by 103 extends by the end of the day, and a fifth (`board_b`) on day 2's first block. `board_a`
goes **DECAYING** on day 2 and **returns to ACTIVE** on day 3 with its pages doubled (9 to 18) when its hot set comes back; then
`board_b` goes DECAYING. At the end: `creates=5 extends=170 heals=0 drops=0 deferred=0 failures=0`. **No DROP occurs in a 3-day
run at the defaults**: the DECAYING dwell before a DROP is `cabin_optimizer_cooldown_half_lives`, default 128
(`docs/spec/physical-optimizer.md` II.6), which at `decay_half_life = 5` is 640 s; the driver's whole run is 278 s and its nights are
45 s. The driver's docstring assumes a 2-half-life cooldown, which is not the engine's default and is what run B sets.

Run B, cooldown 2 half-lives: the same creates on day 1, `board_a` DECAYING at 127.7 s and **DROPPED** by 139.0 s (`drops=1`); its Cabin is gone
while `board_b` is hot; on day 3 `board_a` is **re-nominated from scratch** (`creates=6`, 5 pages at 196 s, 14 by the end) and
`board_b` is dropped at day 3's last block (`drops=2`). Final: `creates=6 extends=196 heals=0 drops=2 failures=0`. The cost of
re-nomination shows in the served rate of the day it happens: `d3-board[on]` runs {d3A:,.0f} qps in run A (the Cabin survived the night)
and {d3B:,.0f} qps in run B (rebuilt), {100*(d3B/d3A-1):+.0f} %, against a run A / run B difference of 0.1 % and 0.5 % on days 1 and 2 (3,858 / 3,854 and 3,929 / 3,908
qps, run A / run B). The comparator's same cell was {100*(d3Bo/d3Ao-1):+.0f} % (3,283 and 2,467 qps). Single draw per run.

## 7. Correctness

The driver compares an identical verification set (hot probes, a zero-row probe, `COUNT` / `SUM`, 18 statements) across the
three arms byte for byte each day: **PASS on all three days in both runs**, `client_errors 0`, `slot_overruns 0`. A Cabin,
however created, dropped or re-nominated, changed no reply.

## 8. Against PostgreSQL 18.6 (reused, not re-measured)

PostgreSQL stays the floor. The PG twin (`tools/pg_scenario4_cabinopt_days.py`) runs **day 1 only, unpaced, one server, no
index**, by its own design: at defaults a PostgreSQL probe seq-scans on day 1 as on day 300. Its figures are the three
runs measured at `f2f1ee7` on 2026-09-30, 14 days before this run, and are not re-measured. KDS side: run A day 1,
derived qps, ratio KDS over PostgreSQL (above 1.00x KDS is faster):

| group | PG18 (reused, no index) | KDS off (walks) | KDS on (controller) | KDS declared (Cabin) | KDS off / PG | KDS on / PG | KDS declared / PG |
|---|---|---|---|---|---|---|---|
""" + "\n".join(
    f"| {g} | {pgq:,} qps | {q(dA,1,g,'off'):,.0f} qps | {q(dA,1,g,'on'):,.0f} qps | {q(dA,1,g,'declared'):,.0f} qps | {q(dA,1,g,'off')/pgq:.2f}x | {q(dA,1,g,'on')/pgq:.2f}x | {q(dA,1,g,'declared')/pgq:.2f}x |"
    for g, pgq in (("open", 823), ("board", 1994), ("tape200", 12690), ("tape1k", 8985), ("tape10k", 2113), ("pk", 15649), ("close", 1183))) + f"""

PostgreSQL's three-run spread was 1.7 % to 11.4 % per group (its file, section 3). **The walk is no longer the weak side**: KDS's `off`
arm on the board and `tape10k` is {q(dA,1,'board','off')/1994:.2f}x and {q(dA,1,'tape10k','off')/2113:.2f}x of PostgreSQL's seqscan (it was 0.48x on both), and the
controller and the declared arm are {q(dA,1,'board','on')/1994:.2f}x / {q(dA,1,'board','declared')/1994:.2f}x on the board and {q(dA,1,'tape10k','on')/2113:.2f}x / {q(dA,1,'tape10k','declared')/2113:.2f}x on `tape10k`
(they were 1.20x / 1.52x and 1.30x / 1.72x). `close` (full-scan aggregates) is {q(dA,1,'close','off')/1183:.2f}x to {q(dA,1,'close','declared')/1183:.2f}x, a walk that
decodes columns to fold (it was 0.49x to 0.51x). The fixed round trip (`pk`) is {q(dA,1,'pk','off')/15649:.2f}x to {q(dA,1,'pk','declared')/15649:.2f}x (it was 0.79x to
0.85x). **Not measured**: PostgreSQL with an index on `symbol`, the fair counterpart of a *declared* Cabin; the comparison is against
PostgreSQL's seqscan only.

## 9. Noise floor and host stamps

Inside the run: the `pk` control, {min(pkr):.2f}x to {max(pkr):.2f}x of the comparator and 0.95x to 1.14x between arms. Across the two runs: the `off` arm,
which does not depend on any run difference, has `board` p50 509.0 / 510.4 / 518.5 µs in A and 508.1 / 506.2 / 525.3 µs in B (day by
day up to 1.3 %), and `tape10k` p50 441.5 / 439.4 / 439.3 against 442.2 / 440.6 / 447.4 µs (up to 1.8 %): **the `off` walk is reproducible
to about 2 %**, tighter than the comparator's 4 %. The served arms are noisier (`board[on]` day 3 {d3A:,.0f} against {d3B:,.0f} qps, section 6).

**The durable-write thermometer moved.** `open` (240 durable inserts per day, one commit each) has p50 {openA[0]:,.0f} to {openA[2]:,.0f} µs on day 1 of run A and
{openB[0]:,.0f} to {openB[2]:,.0f} µs on day 1 of run B, with the same binary, same code and the same host five minutes apart; day 3 of run A is 1,281 to 1,343 µs.
The device's sync, not the engine, moves about 30 % here, which is the floor for any durable-write delta read from this file. `load` (non-durable
batches) is stable: p50 {ph(dA,'load[off]')['p50_us']:.1f} / {ph(dA,'load[on]')['p50_us']:.1f} / {ph(dA,'load[declared]')['p50_us']:.1f} µs in run A and {ph(dB,'load[off]')['p50_us']:.1f} / {ph(dB,'load[on]')['p50_us']:.1f} / {ph(dB,'load[declared]')['p50_us']:.1f} µs in run B.
`recovery_checkpoint_us` was not sampled.

{T_host}

Both runs were clean (no build or test sample in either window).

## 10. What the run says about the engine

1. **A walked row is half what it was**: {walk_now[0]:.0f} ns per row (87 at the comparator), so the hot-equality `off` arm is {q(dA,1,'board','off')/q(oA,1,'board','off'):.2f}x to {q(dA,1,'tape10k','off')/q(oA,1,'tape10k','off'):.2f}x
   faster and the Cabin's multiple over it fell. A Cabin is still worth {min(t10_on_over_off):.1f}x to {max(t10_on_over_off):.1f}x at 10,000 rows and nothing at 200; its
   value is the row count times the per-row cost, and both of those moved together with the walk.
2. **The controller gets the declared ceiling without being told and, in the mean, now exceeds it** ({q(dA,1,'board','on'):,.0f} against {q(dA,1,'board','declared'):,.0f} qps on day 1), because the
   declared arm's coverage-miss path costs more than a plain walk (p95 {ph(dA,'d1-board[declared]')['p95_us']:.0f} against {ph(dA,'d1-board[off]')['p95_us']:.0f} µs) and the controller's does not. The
   mechanism is not shown by this run; `docs/spec/cabin.md` §6 is where a declared set's miss path is specified.
3. **Nothing in the default configuration ever retires a Cabin in the working week the driver models.** With a 128-half-life cooldown DECAYING is reversible
   and DROP is out of reach of any run shorter than 128 half-lives; a fact about the default, not a defect.
   The driver's stated intent (DROP and re-nomination) needs `cabin_optimizer_cooldown_half_lives` lowered, and `docs/spec/physical-optimizer.md`
   §II.4 states the price.
4. **Re-nomination costs less than it did**: {100*(d3B/d3A-1):+.0f} % on the rebuilt day against {100*(d3Bo/d3Ao-1):+.0f} %, because the walk it falls back on is half as slow.
5. **Nothing here approached a limit of the engine**: one connection, one core, 10,000-row relations, `page_budget=1024` with 24 to 32 pages
   committed. The scenario measures a mechanism, not a ceiling.

Archive: `bench/v3.1.0/archive/scenario4-{DESC}/`.
"""
open(f"{OUT}/results-scenario4-cabinopt-days-{DESC}.md", "w").write(doc)
print("written", len(doc))
