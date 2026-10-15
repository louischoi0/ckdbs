import os
import sys, json, math
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/analysis")
from docs_common import *

A = "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/analysis"
md = open(f"{A}/s1.md").read()


def sec(text, head):
    i = text.index(head)
    j = text.find("\n### ", i + 5)
    body = text[i:j if j > 0 else len(text)]
    return body.split("\n", 1)[1].strip("\n")


REPS = ["", "r2-", "r3-"]
CELLS = ["c1-g", "c8-g", "c1-s"]
SHAPES = ["bar-lookup", "bar-range", "day-slice", "symbol-history", "cross-join", "point-join", "model-join"]
PGD = f"{OLDDIR}/archive/scenario1-pg18-v2.7.0-545-gf2f1ee7/json"
pg = json.load(open(f"{PGD}/s1-pg-r1.json"))


def sweep(d):
    return {s["shape"]: s for s in d["meta"]["qps_sweep"]}


def m(shape, case, cell='c1-g'):
    return med('{r}s1b-' + cell, shape, case)


def med(tagpat, shape, case):
    v = []
    for r in REPS:
        if not os.path.exists(f"{J}/{tagpat.format(r=r)}.json"):
            continue
        s = sweep(load(tagpat.format(r=r)))[shape]
        if s[case]:
            v.append(s[case]["qps"])
    return statistics.median(v) if v else None


# --- size sweep (rule 9) ---
SIZE_TAGS = [("252 bars", "{r}s1s-n252-c1-g", 252), ("1,008 bars", "{r}s1s-n1008-c1-g", 1008), ("10,080 bars", "{r}s1s-n10080-c1-g", 10080), ("60,480 bars (defaults)", "{r}s1b-c1-g", 60480)]
T_size = ["| shape | " + " | ".join(f"{lab} cold" for lab, _, _ in SIZE_TAGS) + " |", "|---|" + "---|" * len(SIZE_TAGS)]
for sh in SHAPES:
    T_size.append(f"| {sh} | " + " | ".join(f"{med(tp, sh, 'cold'):,.1f} qps" for _, tp, _ in SIZE_TAGS) + " |")
T_size = "\n".join(T_size)
T_sizecab = ["| shape | " + " | ".join(f"{lab} Cabin" for lab, _, _ in SIZE_TAGS) + " |", "|---|" + "---|" * len(SIZE_TAGS)]
for sh in ("day-slice", "cross-join"):
    T_sizecab.append(f"| {sh} | " + " | ".join(f"{med(tp, sh, 'cabin'):,.1f} qps" for _, tp, _ in SIZE_TAGS) + " |")
T_sizecab = "\n".join(T_sizecab)

# fit: mean latency (µs) = a + b * bars for day-slice cold
def fitline(sh):
    xs = [n for _, _, n in SIZE_TAGS]
    ys = [1e6 / med(tp, sh, "cold") for _, tp, _ in SIZE_TAGS]
    nn = len(xs)
    mx, my = sum(xs) / nn, sum(ys) / nn
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
    return my - b * mx, b * 1000


fits = {sh: fitline(sh) for sh in ("day-slice", "cross-join", "symbol-history", "bar-range")}

# --- percentile tables (size-sweep cells too) ---
T_pct_sw = ["| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |", "|---|---|---|---|---|---|---|---|---|---|"]
for lab, tp, n in SIZE_TAGS[:3]:
    d = load(tp.format(r=""))
    for p in d["phases"]:
        if p["phase"].startswith(("backtest-read", "result-insert", "read-", "load-bars", "agg-day", "compare-one")):
            T_pct_sw.append(f"| `{tp.format(r='')}` | {p['phase']} | {pct_row(p)} |")
T_pct_sw = "\n".join(T_pct_sw)

# --- phase p50 vs PG
pgph = {p["phase"]: p for p in pg["phases"]}
T_phase = ["| phase | KDS ops | KDS p50 (c1-g, median of 3) | PG18 p50 (reused) | KDS / PG p50 | KDS mean | PG18 mean |", "|---|---|---|---|---|---|---|"]
for name in ["load-sessions", "load-bars", "load-stats", "backtest-read", "result-insert", "backtest-replay", "compare-all", "compare-one", "read-bar-lookup", "read-bar-range", "read-symbol-history",
             "read-day-slice", "read-join-point", "read-join-exists", "agg-global", "agg-by-symbol", "agg-by-session", "agg-day-slice", "agg-distinct"]:
    ks = [ph(load(f"{r}s1b-c1-g"), name) for r in REPS]
    k50 = statistics.median([x["p50_us"] for x in ks])
    km = statistics.median([x["mean_us"] for x in ks])
    p = pgph[name]
    T_phase.append(f"| {name} | {ks[0]['ops']:,} ops | {fmt_us(k50)} | {fmt_us(p['p50_us'])} | {k50/p['p50_us']:.2f}x | {fmt_us(km)} | {fmt_us(p['mean_us'])} |")
T_phase = "\n".join(T_phase)

_c8 = [load(f'{r}s1b-c8-g')['meta']['concurrency_sweep'][3]['qps'] for r in REPS]
C8SPR = 100 * (max(_c8) / min(_c8) - 1)
def _spr(shape):
    out = []
    for c in CELLS:
        v = [sweep(load(f'{r}s1b-{c}'))[shape]['warm']['qps'] for r in REPS]
        out.append(f"{100*(max(v)/min(v)-1):.1f} %")
    return ", ".join(out)


SPR_DS = _spr('day-slice')
SPR_CJ = _spr('cross-join')
LS50 = statistics.median([ph(load(f'{r}s1b-{c}'), 'load-sessions')['p50_us'] for r in REPS for c in CELLS])
# durations
elapsed = {}
for c in CELLS:
    elapsed[c] = [load(f"{r}s1b-{c}")["phases"] for r in REPS]
ld = [ph(load(f"{r}s1b-c1-g"), "load-bars")["mean_us"] * ph(load(f"{r}s1b-c1-g"), "load-bars")["ops"] / 1e6 for r in REPS]

# stamp rows
tags = [f"s1u-{c}" for c in CELLS] + [f"{r}s1b-{c}" for r in REPS for c in CELLS] + [f"{r}s1s-n{n}-c1-g" for r in REPS for n in (252, 1008, 10080)]
T_host = ["| cell | window UTC | loadavg 1/5/15 | gate wait | build procs | driver exit | monitor samples | build/test | foreign >= 50 % | minor |", "|---|---|---|---|---|---|---|---|---|---|"]
tags = [t for t in tags if os.path.exists(f"{L}/{t}.rc") and (t.startswith("s1u") or os.path.exists(f"{J}/{t}.json"))]
for (t, tm, e, la, gw, pr, n, b, h, mi) in gate_tbl(tags):
    rc = open(f"{L}/{t}.rc").read().strip()
    T_host.append(f"| `{t}` | {tm[:10]} {tm[11:19]} to {e[11:19]} | {la} | {gw} s | {pr} | exit {rc} | {n} | {b} | {h} | {mi} |")
T_host = "\n".join(T_host)

# verify
vp = [len(load(f"{r}s1b-{c}")["meta"]["verify_problems"]) for r in REPS for c in CELLS]
errs = [sum(p["errors"] for p in load(f"{r}s1b-{c}")["phases"]) for r in REPS for c in CELLS]
u_out = open(f"{L}/s1u-c1-g.stdout").read().strip().split("\n")
u_err = [l for l in u_out if "server said" in l][0].strip()

wr = lambda c, b: statistics.median([[w for w in load(f"{r}s1b-{c}")["meta"]["write_sweep"] if w["batch"] == b][0]["qps"] for r in REPS])
cs = lambda c, n: statistics.median([[w for w in load(f"{r}s1b-{c}")["meta"]["concurrency_sweep"] if w["connections"] == n][0]["qps"] for r in REPS])

diff = """    256c256 -        "HEAP"),            +        "BTREE"),     (daily_stats)
    263c263 -        "HEAP"),            +        "BTREE"),     (model_results)
    1663c1663 -  ... c int64, d int64) HEAP")   + ... c int64, d int64) BTREE")   (the write sweep's write_probe)"""

doc = f"""# Scenario 1 (backtest) at `{DESC}`

**One line.** `tools/scenario1_backtest.py`, unmodified, is **refused again** at its fifth `CREATE TABLE`
(`HEAP storage is suspended (SUS-1)`, byte 220): no number exists for the driver as shipped, three cells,
one second each. A **BTREE-only copy** of the driver (three `HEAP` keywords changed, nothing else) ran in
full and is the **first KDS number for this scenario** (so this file has no KDS predecessor and becomes the baseline the next run is read
against): on 60,480 bars an unindexed one-session equality (`day-slice`) runs {m('day-slice','cold'):,.1f} qps and a
Cabin on its filter column lifts it {m('day-slice','cabin')/m('day-slice','warm'):.1f}x to {m('day-slice','cabin'):,.1f} qps; the backtest's
three-relation join runs {m('cross-join','cold'):,.1f} qps cold and {m('cross-join','cabin'):,.1f} qps with the Cabin; a durable autocommit insert
is {wr('c1-g',1):,.1f} rows/s and a 1,000-row batch {wr('c1-g',1000):,.1f} rows/s; and the join scales across connections only with
cores ({cs('c1-g',1):,.1f} to {cs('c1-g',8):,.1f} q/s at `cores = 1`, {cs('c8-g',1):,.1f} to {cs('c8-g',8):,.1f} q/s at `cores = 8`, one to eight connections). Against the reused
PostgreSQL 18.6 floor, KDS is ahead on the non-pk equality scan and the join and behind on every shape that returns many rows.

Driver: `tools/scenario1_backtest.py`; the BTREE copy is `scenario1_backtest_btree_copy.py` in the archive, made by `sed` from
the tree's driver at `cd7c7c4e`. PostgreSQL 18.6 figures are **reused, not re-measured**, from
`results-scenario1-backtest-pg18-v2.7.0-545-gf2f1ee7.md` (held at `36449d28`, one run at the driver's defaults, measured 2026-09-30,
**14 days before this run**). The time budget was lifted by the operator for this run.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-14; unmodified-driver cells 00:25:27 to 00:25:44 UTC; BTREE-copy cells 00:25:51 to 00:34:22 UTC (nine cells); size sweep (section 5) after the matrix |
{COMMON_STAMP}
| Server config | `log_level = warn`, **`buffer_pool_frames = 65536`** (required since BE), `cores` and `durability` per cell (`c1-g`: 1 group, `c8-g`: 8 group, `c1-s`: 1 strict, the rebaseline's three cells), all else default. Port 15610, never 15432 |
{HOST_ROWS}
| Driver | **unmodified** for the three refused cells (`s1u-*`); **a copy with three keywords changed** for every measured cell (`s1b-*`, `s1s-*`), see section 2. Both are run with `--seed 1 --bars-clustered btree --verify --sync --analyze` and the driver's defaults otherwise |
| PostgreSQL | **reused, not re-measured**: PostgreSQL 18.6 at its defaults, `results-scenario1-backtest-pg18-v2.7.0-545-gf2f1ee7.md`, one run. Section 8 |

## 2. What was run, and the one driver adaptation

The 2026-09-30 rebaseline ran three cells with the unmodified driver and they stopped at `CREATE TABLE ... HEAP`; the
operator ordered scenario 1 run again. The driver was **not** fixed in `tools/` (`bench/README.md`: "a driver change inside
a measurement stage measures the driver"). Three cells of the unmodified driver (`s1u-c1-g`, `s1u-c8-g`, `s1u-c1-s`) reproduce the refusal:

    {u_err}

and a copy under the run directory (and in the archive) with the three `HEAP` keywords of the schema changed to `BTREE` ran the rest:

```
{diff}
```

`daily_stats` (the feature row), `model_results` (the backtest's result rows) and the write sweep's `write_probe` are therefore **BTREE
relations here, where the driver's design (and the PostgreSQL twin's) makes them append-only heap relations**: the driver's
comments argue a heap tail-append is "exactly right" for both because nothing probes them by pk. On a BTREE each append pays a descent
and a split, so the write side (`result-insert`, `write-sweep`, `load-stats`) measures a different thing than the driver intended; the read
side (`daily_stats` is walked by a FilterScan either way) is comparable. The adaptation is the operator's order (BTREE-only,
`bench/README.md`, "What a v3 number is measured on") and AS-S3 is the stage that will change the driver itself.

Cells: the rebaseline's three (`cores = 1 group`, `cores = 8 group`, `cores = 1 strict`), **three runs each (nine cells; the rebaseline's plan had one)**,
plus a **size sweep** (rule 9) of `c1-g` at 252, 1,008 and 10,080 bars, three runs each (nine cells) beside the default 60,480:

    python3 scenario1_backtest_btree_copy.py --port 15610 --suffix <cell> --seed 1 --bars-clustered btree --verify --sync --analyze --json <cell>.json
    ... --years 1 --symbols 1 --exchanges 1    (252 bars)
    ... --years 1 --symbols 4 --exchanges 2    (1,008 bars)
    ... --years 5 --symbols 8 --exchanges 2    (10,080 bars)

Bars = 252 sessions per year x years x symbols; `daily_stats` has one row per bar and `daily_bars` one per bar; the defaults are `--years 30 --symbols 8`
= 60,480 bars, 128,536 rows over seven relations. **All nine default-size cells exited 0 with no verify problem and no error reply**
({sum(errs)} errors in total over the nine; `verify_problems` empty in {sum(1 for x in vp if x == 0)} of 9).

## 3. Results

The QPS sweep (the driver's own controlled matrix, 100 statements per cell, 8 distinct arguments in the warm cases): median of three
runs per cell, `cold` = every argument seen for the first time, `warm` = the same few arguments cycled, `cabin` = warm with a Cabin
declared on the filter column and observed, `dropped` = warm again after `DROP CABIN`. Qps is the driver's own figure.

{sec(md, '### QPS sweep per shape')}

**A Cabin lifts the two scan shapes by an order of magnitude and nothing else.** `day-slice` {m('day-slice','cabin')/m('day-slice','warm'):.1f}x and `cross-join` {m('cross-join','cabin')/m('cross-join','warm'):.1f}x at `cores = 1`; `symbol-history`
returns 7,560 rows and gains nothing ({m('symbol-history','cabin')/m('symbol-history','warm'):.2f}x), and `model-join` ({m('model-join','cabin')/m('model-join','warm'):.2f}x) is
a probe-per-row join over 359 rows. After `DROP CABIN` the shape returns to its warm rate ({m('day-slice','dropped'):,.1f} against {m('day-slice','warm'):,.1f} qps): un-observing is a performance
event only. Cold and warm agree within 3 % on every shape: the data is in the buffer pool and nothing in a cold probe is slower than a warm one.
The `point-join` row's 44 to 45 % warm spread is a single slow-mode run in each cell (section 9).

**Writes: the durability wait is the cost of a row.**

{sec(md, '### write sweep')}

Autocommit is {1e6/wr('c1-g',1):,.0f} µs per row (one durable commit each); a batch of 1,000 spreads the sync to {1e6/wr('c1-g',1000):,.0f} µs per row, {wr('c1-g',1000)/wr('c1-g',1):.0f}x, which is the driver's own
comment: "every row is one round trip in every case; what changes is how many durability points they cost". `strict` and `group` read the same
for one committer, as expected. The spreads (up to 64 % for `c1-s` at one row, 31 % for `c8-g` at 1,000) are the device's sync moving, not the engine: section 9.

**Concurrency: KDS scales with cores, not with connections.**

{sec(md, '### concurrency sweep')}

At `cores = 1` the join does not scale with connections ({cs('c1-g',8)/cs('c1-g',1):.2f}x at eight): the server runs one core's statements on one thread, so a rising aggregate is only
round-trip overlap (the driver's own note). At `cores = 8` it reaches {cs('c8-g',8)/cs('c8-g',1):.2f}x at eight connections with a {C8SPR:.0f} % run-to-run spread, because the sessions
are placed on cores as they arrive and the eight clients share eight CPUs with the reactors.

## 4. Percentiles

Every phase of run 1 of each default-size cell, microseconds (the driver's own order):

{sec(md, '### percentiles, all phases')}

## 5. Row-set sweep (rule 9)

The default is a single cardinality (60,480 bars). The sweep reruns `c1-g` at 252, 1,008 and 10,080 bars (`--years` and `--symbols`; section 2) so that a
fixed cost can be told from a per-row one. Cold qps, median of three runs:

{T_size}

Per-size fit of the mean statement latency over the four sizes, `mean = a + b * bars`: `day-slice` a = {fits['day-slice'][0]:.0f} µs, b = {fits['day-slice'][1]:.1f} ns per bar;
`cross-join` a = {fits['cross-join'][0]:.0f} µs, b = {fits['cross-join'][1]:.1f} ns per bar; `symbol-history` (which returns a symbol's whole history, bars/symbols rows) a = {fits['symbol-history'][0]:.0f} µs,
b = {fits['symbol-history'][1]:.1f} ns per bar; `bar-range` (200 rows returned at every size) a = {fits['bar-range'][0]:.0f} µs, b = {fits['bar-range'][1]:.1f} ns per bar. With a Cabin on the filter column:

{T_sizecab}

Percentiles of the sweep cells (run 1), the shapes that depend on the row count:

{T_pct_sw}

## 6. Where the time goes

| Component | Reading |
|---|---|
| Durability / commit | autocommit insert {1e6/wr('c1-g',1):,.0f} µs per row at `c1-g`; the load phases batch 200 rows per transaction, so `load-bars` mean {ph(load('s1b-c1-g'),'load-bars')['mean_us']:.1f} µs per row is about {ph(load('s1b-c1-g'),'load-bars')['mean_us']*200/1000:.1f} ms per 200-row transaction. `sync` after the run: {ph(load('s1b-c1-g'),'sync')['p50_us']/1000:.1f} ms |
| Write statement | `result-insert` p50 {ph(load('s1b-c1-g'),'result-insert')['p50_us']:,.1f} µs: a durable autocommit insert into a BTREE (the driver's heap design would differ) |
| Read | a pk lookup is the round trip: `read-bar-lookup` p50 {ph(load('s1b-c1-g'),'read-bar-lookup')['p50_us']:.1f} µs; `day-slice` is {fits['day-slice'][0]:.0f} µs fixed plus {fits['day-slice'][1]:.1f} ns per bar walked; `read-bar-range` {ph(load('s1b-c1-g'),'read-bar-range')['p50_us']:,.1f} µs for 200 rows ({ph(load('s1b-c1-g'),'read-bar-range')['p50_us']/200:.1f} µs per returned row); `read-symbol-history` {ph(load('s1b-c1-g'),'read-symbol-history')['p50_us']/1000:.1f} ms for 7,560 rows ({ph(load('s1b-c1-g'),'read-symbol-history')['p50_us']/7560:.1f} µs per returned row, mostly the client decoding the reply) |
| Join probe | `model-join` is {ph(load('s1b-c1-g'),'compare-one')['p50_us']/359:.1f} µs per probed outer row (`compare-one`, 359 probes); the pk-anchored `point-join` is a pk lookup's round trip |
| Client / socket | in every figure; the same Python client (`tools/kwp.py`) drives every row, the PostgreSQL twin uses its own (`tools/pg_wire.py`) |
| Lock or conflict wait | none on one connection; the connection sweep is a read workload and has no writer to wait for |

## 7. Waits and caveats of the adaptation

The write-side phases (`load-stats`, `result-insert`, `write-sweep`) are BTREE appends here; the PostgreSQL twin appends to heap tables. The
reads are the comparable half. `daily_bars` was already BTREE in the driver (`--bars-clustered btree`) and `load-bars` is unaffected by the change.

## 8. Against PostgreSQL 18.6 (reused, not re-measured)

PostgreSQL is the floor: it says whether the engine is in the right league. The twin (`tools/pg_scenario1_backtest.py`) ran once at the driver's defaults on
2026-09-30 (`f2f1ee7`), 14 days before this run, as a standalone floor "for the next KDS run of this scenario, once its schema is BTREE-only"; its figures are
not re-measured. Ratios are KDS `c1-g` (this run, median of three) over the PG18 single run, qps for the sweep shapes (above 1.00x = KDS faster).
PG's `index built` column is a btree index; KDS's Cabin is the counterpart, and they are different structures.

{sec(md, '### versus PostgreSQL 18.6')}

Per-phase p50 of the workload as it ran (the PG column is its single run; the KDS `load-stats`, `result-insert` and `compare-*` phases run against BTREE
relations where the PG twin's are heap):

{T_phase}

Write sweep and concurrency sweep side by side with the reused PG18 figures are in section 3. KDS is **ahead** where it scans with an equality (`day-slice` 1.33x,
`cross-join` 1.43x), on the pk-anchored join (`point-join` 2.17x) and on the durable autocommit rate ({wr('c1-g',1)/658.7:.2f}x) and 1,000-row batch ({wr('c1-g',1000)/18756.6:.2f}x); **behind**
wherever the reply carries many rows or a join probes per outer row (`bar-range` 0.49x, `symbol-history` 0.43x, `model-join` 0.57x) and in the
whole-relation folds (`agg-global`, `agg-by-session`, `compare-all`: 1.4x to 2.4x slower). The Cabin on `day-slice` ({m('day-slice','cabin'):,.1f} qps) is 0.74x of PostgreSQL's btree index on the same
column (8,892.4 qps); on `cross-join` it is 1.45x of PostgreSQL's index (3,982.7 qps). PostgreSQL's single run has the spread the PG file states (up to 25 % on its sweeps), and the two sides use
different Python clients, so ratios inside 0.80x to 1.25x are not findings.

## 9. Noise floor and host stamps

The nine default-size cells give a floor per figure (spread = max/min - 1 over three runs): the warm `day-slice` {SPR_DS} (`c1-g`, `c8-g`, `c1-s`); the
warm `cross-join` {SPR_CJ}. `point-join` and the single-row write rate
carry a **single slow-mode run** (spreads 33 % to 65 %; the host mode named in scenario 3's file, where a pk lookup reads 94 µs against 66), which the median of three votes out of the headline but not out
of the spread column. **A difference under about 10 % on a read shape and under 35 % on a durable write figure is not a result.** The device
thermometer, `load-sessions` p50 ({LS50:.1f} µs, 7,560 rows in 200-row transactions), is steady; `recovery_checkpoint_us` was not sampled.
**Contamination:** none of the reported cells had a build or test sample in its window; no cell of this scenario was re-run.

{T_host}

## 10. What the run says about the engine

1. **The driver cannot run on this engine as shipped, and a BTREE-only copy can.** Heap relations are suspended (SUS-1), so any v3 number for scenario 1 is
   a BTREE number and a different workload than the driver's design; this file is its baseline.
2. **A non-pk equality scan costs about {fits['day-slice'][1]:.0f} ns per bar** (the fit over four sizes) plus a fixed {fits['day-slice'][0]:.0f} µs; at 60,480 bars that is 1.8 ms and the unaided read is
   {m('day-slice','cold'):,.0f} qps. A Cabin turns it into one round trip ({m('day-slice','cabin'):,.0f} qps): an order of magnitude, available on the scenario's two hot scans and useless on the
   shapes that return many rows.
3. **The join probes cost per outer row.** `model-join` at {ph(load('s1b-c1-g'),'compare-one')['p50_us']/359:.1f} µs per probed row versus {fits['day-slice'][1]:.0f} ns per scanned row: a Probe step is two orders of
   magnitude dearer than a scanned row, which is where `model-join` (0.57x of PostgreSQL's hash join) and `compare-all` lose.
4. **Returning rows is the other cost**: `read-bar-range` {ph(load('s1b-c1-g'),'read-bar-range')['p50_us']/200:.1f} µs per row and `read-symbol-history` {ph(load('s1b-c1-g'),'read-symbol-history')['p50_us']/7560:.1f} µs per row, about 2x the PG twin's (3.6 and 4.0 µs);
   with different clients on the two sides the engine's share of that is not separable.
5. **Cores, not connections, scale the join**, and the durable write rate is the device's sync: {wr('c1-g',1):,.0f} rows/s at one row, {wr('c1-g',1000):,.0f} at 1,000.

Archive: `bench/v3.1.0/archive/scenario1-{DESC}/`.
"""
open(f"{OUT}/results-scenario1-backtest-{DESC}.md", "w").write(doc)
print("written", len(doc))
