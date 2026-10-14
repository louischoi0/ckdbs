import sys, re, math
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/analysis")
from docs_common import *

A = "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/analysis"
md = open(f"{A}/s3.md").read()
mdb = open(f"{A}/s3b.md").read()


def sec(text, head):
    i = text.index(head)
    j = text.find("\n### ", i + 5)
    body = text[i:j if j > 0 else len(text)]
    return body.split("\n", 1)[1].strip("\n")


REPS = ["", "r2-", "r3-"]
SIZES = [200, 1000, 10000]
COLS = [(n, m) for n in SIZES for m in ("none", "all")]
SHAPES = ["pk-user", "loans-by-user", "loans-by-book", "resv-by-user", "books-by-author", "books-by-genre",
          "loans-by-daterange", "overdue", "join-loan-user", "join-no-literal", "exists-correlated", "count-by-user"]


def mq(c, n, m, s, old=None):
    return statistics.median([qps_from_mean(ph(load(f"{r}s3-c{c}-n{n}-{m}", old), s)) for r in REPS])


def gm(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs))


# geomeans
gm_x1 = {k: gm([mq(1, k[0], k[1], s) / mq(1, k[0], k[1], s, 3) for s in SHAPES]) for k in COLS}
gm_x8 = {k: gm([mq(8, k[0], k[1], s) / mq(8, k[0], k[1], s, 3) for s in SHAPES]) for k in COLS}

# host stamp table
tags = [f"{r}s3-c{c}-n{n}-{m}" for r in REPS for c in (1, 8) for n in SIZES for m in ("none", "all")]
T_host = ["| cell | window UTC | loadavg 1/5/15 | gate wait | build procs | driver exit | verify problems | samples | build/test | foreign >= 50 % | minor |", "|---|---|---|---|---|---|---|---|---|---|---|"]
for (t, tm, e, la, gw, pr, n, b, h, mi) in gate_tbl(tags):
    rc = open(f"{L}/{t}.rc").read().strip()
    vp = len(load(t)["meta"]["verify_problems"])
    T_host.append(f"| `{t}` | {tm[5:10]} {tm[11:19]} to {e[11:19]} | {la} | {gw} s | {pr} | exit {rc} | {vp} problems | {n} | {b} | {h} | {mi} |")
T_host = "\n".join(T_host)

rows_host = gate_tbl(tags)
win = {}
for (t, tm, e, la, gw, pr, n, b, h, mi) in rows_host:
    k = "run 1" if t.startswith("s3-") else ("run 2" if t.startswith("r2-") else "run 3")
    a, z = win.get(k, ("9", "0"))
    win[k] = (min(a, tm), max(z, e))
WIN = ", ".join(f"{k} {v[0][11:16]} to {v[1][11:16]}" for k, v in sorted(win.items()))
c8c1 = [mq(8, n, m, s) / mq(1, n, m, s) for s in SHAPES for n, m in COLS]
c8c1o = [mq(8, n, m, s, 3) / mq(1, n, m, s, 3) for s in SHAPES for n, m in COLS]
pgg = re.findall(r"([0-9.]+)x \((\d+) of 12\)", [l for l in sec(md, "### versus PostgreSQL 18.6").split("\n") if l.startswith("| geometric mean")][0])
# neat cross-version ratio summary rows
x1_n10k_none = [mq(1, 10000, "none", s) / mq(1, 10000, "none", s, 3) for s in ("loans-by-user", "loans-by-book", "resv-by-user", "count-by-user", "join-loan-user")]
ratio_idx_now = mq(1, 10000, "all", "loans-by-user") / mq(1, 10000, "none", "loans-by-user")
ratio_idx_then = mq(1, 10000, "all", "loans-by-user", 3) / mq(1, 10000, "none", "loans-by-user", 3)

allpk = [ph(load(f"{r}s3-c{c}-n{n}-{m}"), "pk-user")["mean_us"] for r in REPS for c in (1, 8) for n in SIZES for m in ("none", "all")]
pk_q1 = [mq(1, n, m, "pk-user") for n, m in COLS]

load_means = [ph(load(f"{r}s3-c1-n{n}-{m}"), "load")["mean_us"] for r in REPS for n in SIZES for m in ("none", "all")]
load_p50 = [ph(load(f"{r}s3-c{c}-n{n}-{m}"), "load")["p50_us"] for r in REPS for c in (1, 8) for n in SIZES for m in ("none", "all")]

vs_pg = sec(md, "### versus PostgreSQL 18.6")
pg_gm_line = [l for l in vs_pg.split("\n") if l.startswith("| geometric mean")][0]

doc = f"""# Scenario 3 (library) at `{DESC}`

**One line.** Reading a row costs 2.4x to 2.8x less than it did at `9a0525d`:
an unindexed non-pk equality now costs about 27 ns per row walked (it cost
70) plus an unchanged fixed 58 to 87 µs, so at 10,000 loans the unindexed
`loans-by-user` runs {mq(1,10000,'none','loans-by-user'):,.0f} qps against {mq(1,10000,'none','loans-by-user',3):,.0f} qps
({mq(1,10000,'none','loans-by-user')/mq(1,10000,'none','loans-by-user',3):.2f}x, cross-version) while the
indexed read is where it was ({mq(1,10000,'all','loans-by-user'):,.0f} against {mq(1,10000,'all','loans-by-user',3):,.0f} qps, {mq(1,10000,'all','loans-by-user')/mq(1,10000,'all','loans-by-user',3):.2f}x), so
**the index's gain at 10,000 rows shrank from {ratio_idx_then:.1f}x to {ratio_idx_now:.1f}x** because the thing it
saves became cheaper. Shapes that are not a plain equality (range
predicates, the join without a literal, the correlated `EXISTS`) gain only 1.0x
to 1.5x per row.

Driver: `tools/scenario3_library.py`, unmodified, 36 cells, three runs each,
the 2026-09-30 rebaseline's arguments. Comparator (cross-version):
`bench/v3.0.0/results-scenario3-library-v2.7.0-531-g9a0525d.md`, held at
`36449d28`. PostgreSQL 18.6 figures are **reused, not re-measured**, from
`results-scenario3-library-pg18-v2.7.0-545-gf2f1ee7.md` (held at `36449d28`,
measured 2026-09-30, **14 days before this run**). The time budget was lifted
by the operator for this run.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-13 23:53 UTC to 2026-10-14 00:25 UTC ({WIN}; the day changes at midnight; gaps are waits for another session's builds; per-cell windows in section 10) |
{COMMON_STAMP}
| Server config | `log_level = warn`, `durability = group`, **`buffer_pool_frames = 65536`** (required since BE; the comparator's config predates the key), `cores` 1 or 8 per cell, `indexes` at its default (on; recorded as `--server-indexes on`), all else default; port 15620, never 15432 |
{HOST_ROWS}
| PostgreSQL | **reused, not re-measured**: PostgreSQL 18.6 at its defaults, `results-scenario3-library-pg18-v2.7.0-545-gf2f1ee7.md` (held at `36449d28`), measured 2026-09-30 at `f2f1ee7`. Section 8 |

## 2. What was run

Thirty-six cells: `cores` {{1, 8}} x `--loans` {{200, 1000, 10000}} x
`--index-mode` {{none, all}}, **three runs each**; fresh server and data file per
cell; `--seed 1 --matches 5 --ops 200 --verify 25` (the defaults),
`--index-when after` (the default: indexes declared on the loaded relations,
which times the backfill), and for `all` also `--assert-index-reads`. Command:

    python3 tools/scenario3_library.py --port 15620 --suffix <cell> --loans <n> \\
        --index-mode <none|all> --server-indexes on --seed 1 [--assert-index-reads] --json <cell>.json

**Row-set axis (rule 9), stated once.** `--loans N` is the bulk relation; users
and books scale to hold `--matches 5` constant. At `N = 10,000`: loans 10,000,
users 2,000, books 2,000, reservations 5,000; at 200 and 1,000 the same ratios.
Every matrix below carries the three sizes as columns.

**All 36 cells exited 0, all 36 `verify` runs reported no problems, and all 18
`all` cells passed `--assert-index-reads`** (section 10). Durability does not enter
a read shape; the load, which is where `group` acts, is in section 6. `--cabin` and the
other `--index-mode` values (`single`, `composite`, `covering`) were **not executed**,
as at the comparator. Host load and build check per cell: section 10.

## 3. Throughput

A serial single-connection driver reports no qps; each cell is **derived**:
1,000,000 / mean µs, then the median of three runs. `cores = 1`:

{sec(md, '### QPS, cores = 1')}

`cores = 8`:

{sec(md, '### QPS, cores = 8')}

`cores = 8` reads {min(c8c1):.2f}x to {max(c8c1):.2f}x of `cores = 1` over the 72 cells (median {statistics.median(c8c1):.2f}x) and is the
slower of the two in {sum(x < 1 for x in c8c1)} of them (comparator: {min(c8c1o):.2f}x to {max(c8c1o):.2f}x, median {statistics.median(c8c1o):.2f}x, slower in {sum(x < 1 for x in c8c1o)}): one serial connection lands on one core and the
other seven have nothing to do, so no gain is expected; the 5 to 6 % that is lost is inside the cell-to-cell spread of the
`pk-user` control (section 10) and is not a finding.

**Row-count independence of the control.** `pk-user`, a `Lookup`, does not scale with
rows or index mode: {min(pk_q1):,.0f} to {max(pk_q1):,.0f} qps across the six `cores = 1` cells; five of the six lie within 5 % of each other (14,749 to 15,432 qps), and the low one (`n=200 none`) is the
cell whose median was pulled by the host's slow mode (section 10), not by the row count.

## 4. Delta against the previous run of this shape (cross-version)

The comparator is `bench/v3.0.0/results-scenario3-library-v2.7.0-531-g9a0525d.md`, three runs
per cell, held at `36449d28`. {version_distance_note()}
**495 commits apart**, so no difference is attributable to one change. Ratio of median qps,
now over the comparator (above 1.00x = faster now), `cores = 1`:

{sec(md, '### cross-version: now over 9a0525d')}

And `cores = 8`:

{sec(md, '### cross-version, cores = 8')}

The comparator's `cores = 1` matrix, for side-by-side reading (qps):

{sec(md, '### 9a0525d QPS')}

**Reading it.** The floor of this table is the `pk-user` row (0.94x to 1.10x): a ratio inside about
0.90x to 1.10x is not a finding, and the `n=200 none` column (geometric mean {gm_x1[(200,'none')]:.3f}x) is the one the
host's slow mode touched (run 3 of that cell has a `pk-user` mean of 95.9 µs against 66 to 76 µs, section 10),
so its sub-1.0 ratios are noise. Outside the floor: the unindexed equality shapes at 1,000 rows (1.3x to 1.6x)
and at 10,000 rows (**1.65x to 2.20x**, `loans-by-user` {mq(1,10000,'none','loans-by-user')/mq(1,10000,'none','loans-by-user',3):.2f}x, `count-by-user` {mq(1,10000,'none','count-by-user')/mq(1,10000,'none','count-by-user',3):.2f}x), and
`loans-by-daterange` (1.19x to 1.34x in the five columns other than `n=200 none`, a full scan with a range predicate). The indexed
columns are flat (geometric mean {gm_x1[(10000,'all')]:.3f}x at 10,000 rows). The eight-core cells show the same
(unindexed 10,000 rows {gm_x8[(10000,'none')]:.2f}x geometric mean).

## 5. What the index buys now

The ratio none / all of mean latency (`cores = 1`, medians of three), now and at the comparator. Above 1 the
index is faster:

{sec(md, '### index effect')}

The pk control (`pk-user`) reads 0.96x to 1.15x: **a ratio inside about 1.00 +/- 0.15 is not a finding**
here. The gain at 10,000 rows roughly halved ({ratio_idx_then:.1f}x to {ratio_idx_now:.1f}x for `loans-by-user`) and at 1,000 rows
fell from 1.7x to 1.2x, because the unindexed side got cheaper while the indexed side (one round trip) did not;
at 200 rows the index buys 0.9x to 1.4x, at the floor.

## 6. Percentiles

`cores = 1`, run 1, in full: 200 operations per shape. Microseconds; `plan` is what ANALYZE
reported the shape compiled to. The `pk-user` p50 in `n=200 none` of run 1 is {ph(load('s3-c1-n200-none'),'pk-user')['p50_us']:.1f} µs against about
66 µs in clean cells (the slow mode, section 10).

{sec(md, '### percentiles, cores = 1')}

`cores = 8`, run 1, the same:

{sec(md, '### percentiles, cores = 8')}

## 7. Where the time goes

A read is one round trip plus the work. The decomposition is fixed cost and rows
(least squares over the three sizes, medians of three runs, `cores = 1`, **unindexed**;
`rows walked` is the relation the statement walks, as a multiple of `--loans`):

{sec(mdb, '### per-row fit')}

| Component | Now | How derived |
|---|---|---|
| Fixed round trip (client, socket, parse, one pk descent) | **{statistics.median(allpk):.1f} µs** (`pk-user` mean, median of 36 cells; {min(allpk):.1f} to {max(allpk):.1f} µs) | the control |
| Fixed cost of a FilterScan statement | 58 to 87 µs for the single-relation equality statements, 108 to 255 µs for the joins (`a` above); comparator 65 to 91 and 94 to 245 µs | intercept of the fit |
| Rows walked by an equality FilterScan | **25 to 29 ns per row** (comparator 69 to 73) | slope of the fit: `loans-by-user`, `loans-by-book`, `resv-by-user`, `books-by-author`, `count-by-user`, `join-loan-user` |
| Rows walked by a range or multi-column predicate | 107 to 198 ns per row (comparator 158 to 268) | `loans-by-daterange`, `overdue`, `books-by-genre` (the last returns many rows) |
| Nested range + scan or probe | 51 to 92 ns per row (comparator 52 to 104) | `exists-correlated`, `join-no-literal` |
| Durability / commit | not applicable to a read. The load is the only write path: **{min(load_p50):,.0f} to {max(load_p50):,.0f} µs p50 per 500-row batch** (a batch is one transaction and one commit), mean {min(load_means):,.0f} to {max(load_means):,.0f} µs; the same at both core counts and all row counts, so the commit is fixed per batch and ~2.5 µs per row | `load` phase, 36 cells |
| Index backfill | `create-index` (8 indexes after load): 1.2 ms at 200 rows, 1.8 to 3.9 ms at 1,000, 7.2 to 8.0 ms at 10,000 (`create-index` mean per index) | one-shot phases below |
| Lock or conflict wait | none: one reader, no concurrent writer | |
| Client / socket | in the fixed cost; the client is the same Python driver both runs | |

One-shot phases (not matrix rows; units stay in µs; `cores = 1`, run 1):

{sec(md, '### one-shot phases')}

**The per-row cost of a walked row fell from about 70 ns to 27 ns, but only for the shapes whose
predicate is an equality the scan can reject on the cell's bytes.** Those are the six shapes
with 2.4x to 2.8x lower slope; the range-predicate and join shapes are 1.0x to 1.5x. The fixed
cost did not move (58 to 87 µs against 65 to 91 for the single-relation equality statements). The rows-per-statement arithmetic is why the
index lost half its multiple: a 10,000-row equality scan was 700 µs of which 84 was fixed; it is now 360 µs
of which 87 is fixed.

## 8. Against PostgreSQL 18.6 (reused, not re-measured)

PostgreSQL is the floor: it answers "is the engine in the right league" and nothing finer. The twin
(`tools/pg_scenario3_library.py`) was run at `f2f1ee7` on 2026-09-30, 14 days before these cells,
three runs per cell, with PostgreSQL's defaults (`shared_buffers = 128MB`, `synchronous_commit = on`); its per-shape
medians are in its own file and are not re-measured here. Ratio of KDS `cores = 1` median qps (this run)
to the PG18 median qps, per shape (above 1.00x = KDS faster):

{vs_pg}

Against the comparator-era floor (`results-kds-vs-pg18-summary-v2.7.0-545-gf2f1ee7.md`: geometric mean
0.87x / 0.96x / 0.77x / 0.97x / 0.62x / 0.95x for the same six columns), **the unindexed gap closed**: at 10,000 rows
the unindexed equality shapes are now 1.12x to 1.15x of PostgreSQL's seqscan (they were 0.52x to 0.54x), a KDS walked row
(27 ns) is faster than PostgreSQL's (34 to 36 ns by the same fit on its reused medians: `loans-by-user` 34, `loans-by-book` 36,
`resv-by-user` 34, `books-by-author` 40, `count-by-user` 34), and the geometric mean over twelve shapes at 10,000 rows
unindexed went from 0.62x to {pgg[4][0]}x (the table's last row). **Still behind**: the shapes
that are not a plain equality or that return many rows (`books-by-genre`, `loans-by-daterange`, `overdue`: 0.54x to 0.77x at
10,000 rows), and the fixed round trip (`pk-user` 0.83x to 1.03x; the KDS statement costs 4 to 10 µs more than
PostgreSQL's 62 to 69 µs). **Ahead**: `exists-correlated` in every column (1.13x to 2.09x), `join-loan-user` at 1,000
and 10,000 rows with indexes (1.25x, 1.14x), `join-no-literal` indexed at 10,000 rows (2.43x). The
PostgreSQL side was one set of runs 14 days earlier on a host that has since run other work; the KDS cells carry the
slow-mode noise of section 10, so ratios inside 0.90x to 1.10x are not findings.

## 9. What the run says about the engine

1. **The row scan is now at or below PostgreSQL's per-row cost for an equality**, 27 against 34 ns, after being
   at twice it. The mechanism the specs name is BM (a scan kernel rejects on the cell's bytes before any
   decode) and BT (a rejected row reads its tuple without a `Status`; `docs/spec/parser-v2.md` §7); this run measures the
   outcome, not the milestone, and BM's and BT's own close files at `bench/v3.0.0/` carry their A/B.
2. **The win does not reach shapes whose predicate is not that equality.** Range and multi-column predicates walk at 107 to
   198 ns per row and the join shapes at 51 to 92 ns; those are where the next row-walk cost lives.
3. **An index is worth less than it was, and still decisive at 10,000 rows**: {ratio_idx_now:.1f}x for `loans-by-user`,
   against a fixed cost of about 87 µs that no index removes. At 200 rows the index buys nothing outside the floor.
4. **The fixed round trip did not change** ({statistics.median(allpk):.1f} µs median `pk-user`; comparator 69 to 73 µs by its means): the
   per-statement cost floor that sets the indexed ceiling of about {max(mq(1,10000,'all',s) for s in ('loans-by-user','loans-by-book','count-by-user')):,.0f} qps
   per connection is client, socket and the statement's own setup.
5. **One connection does not use eight cores**: `cores = 8` is 0.94x of `cores = 1` (median), as at the comparator
   (0.95x). Nothing was tried above one connection in this scenario.

## 10. Noise floor, contamination and host stamps

This host has a **slow mode**: a run in which every statement is about 40 % slower for the whole run
(`pk-user` 94 to 96 µs against 64 to 68). It appears here in three of 36 cells, one run each
(`r3-s3-c1-n200-none`, `r2-s3-c1-n10000-none`, `r2-s3-c1-n10000-all`), with no foreign process in
the monitor's samples for those windows; it is the same effect the comparator named. A median of three votes
a single slow run out; for `n=200 none` it picked a run 12 % above the clean value, which is why that
column is the least reliable. `pk-user` mean (µs) per run, the thermometer:

{sec(mdb, '### pk-user mean')}

Run-to-run spread (max/min - 1) of each cell's per-shape mean, median and worst over the twelve shapes:

{sec(md, '### noise floor')}

The medians are the spreads of a cell where no run was slow (2 to 15 %); the 37 to 38 % medians are the
three slow-mode cells. **A difference under about 15 % in a single cell, or under 10 % in the geometric mean of
a column, is not a result.** The comparator's own noise was 1.6 to 8.2 % per shape with four groups at
21 to 33 %.

**Contamination.** Another session's builds and test suite ran on this host during the measurement. Two cells were
discarded and re-run because the monitor saw a build or test process in their window: `s3-c1-n1000-all` (one
attempt) and `s3-c8-n200-none` (one attempt); their discarded outputs are in `contaminated/` in the archive.
Every reported cell has no build or test sample. The device thermometer, the `load` phase p50
({min(load_p50):,.0f} to {max(load_p50):,.0f} µs over all 36 cells, one commit each), shows no stall; `recovery_checkpoint_us` was not sampled.

{T_host}

Archive: `bench/v3.1.0/archive/scenario3-{DESC}/`.
"""
open(f"{OUT}/results-scenario3-library-{DESC}.md", "w").write(doc)
print("written", len(doc))
