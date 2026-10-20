import sys, subprocess
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/analysis")
from s3 import *
import abx
from stamp import *

kt = [p + f"s3-c{c}-n{n}-{m}" for p in PFX for c in (1, 8) for n in SZ for m in MODES]
abt = [f"ab-{v}{i}-s3-c1-n10000-none" for i in (1, 2, 3) for v in ("old", "new")]
plt = [f"pl-s3-{o}-{m}-r{i}" for o in ("planned", "written") for m, rr in (("none", (1, 2, 3)), ("all", (1,))) for i in rr]


def q1(c, n, m, sh, w="now"):
    return med(cellq("k", c, n, m, sh, w))


def qp(n, m, sh):
    return med(cellq("pg", 0, n, m, sh))


def sp1(c, n, m, sh, w="now"):
    return spread(cellq("k", c, n, m, sh, w))


def geo_vs_pg(n, m, w="now"):
    return geomean([q1(1, n, m, sh, w) / qp(n, m, sh) for sh in SH])


def geo_ab():
    o = [now(f"ab-old{i}-s3-c1-n10000-none") for i in (1, 2, 3)]
    n_ = [now(f"ab-new{i}-s3-c1-n10000-none") for i in (1, 2, 3)]
    return {sh: (med([q(d, sh) for d in o]), med([q(d, sh) for d in n_])) for sh in SH}


ABG = geo_ab()
VOK = sum(1 for t in kt + abt + plt if now(t) and now(t)["meta"]["verify_problems"] == [])
VN = len([t for t in kt + abt + plt if now(t)])


def plan_tbl():
    r = subprocess.run(["python3", "-I", "/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/analysis/plan_ab2.py"], capture_output=True, text=True)
    t = r.stdout
    return "Scenario 3," + t.split("Scenario 3,")[1]


def PX(sh, which):
    def mean(rr):
        return med([ph(d, sh)["mean_us"] for d in rr])
    if which == "now":
        a, b = mean(N("s3-c1-n200-none")), mean(N("s3-c1-n10000-none"))
    elif which == "morn":
        a, b = mean(Mo(3, "s3-c1-n200-none")), mean(Mo(3, "s3-c1-n10000-none"))
    else:
        a, b = mean(Mo(3, "pg-s3-n200-none")), mean(Mo(3, "pg-s3-n10000-none"))
    return (b - a) / 9800 * 1000


def cmp_pct3(shapes, m="none"):
    out = "| phase | run | ops | p0 | p25 | p50 | p95 | p99 |\n|---|---|---|---|---|---|---|---|\n"
    for sh in shapes:
        for lab, d in (("now", now(f"s3-c1-n10000-{m}")), ("morning (old client)", mor(3, f"s3-c1-n10000-{m}")), ("PostgreSQL 18.6 (morning's run)", mor(3, f"pg-s3-n10000-{m}"))):
            x = ph(d, sh)
            out += f"| {sh} | {lab} | {x['ops']:,} | {x['p0_us']:,.1f} µs | {x['p25_us']:,.1f} µs | {x['p50_us']:,.1f} µs | {x['p95_us']:,.1f} µs | {x['p99_us']:,.1f} µs |\n"
    return out


def pr(sh, key):
    return ph(now("s3-c1-n10000-none"), sh)[key] / ph(mor(3, "s3-c1-n10000-none"), sh)[key]


# the client-changed shapes, A/B
many = ["books-by-genre", "loans-by-daterange", "overdue"]
txt = f"""# Scenario 3 (library) at `{DESC}`

**One line.** Twelve read shapes over four relations at 200, 1,000 and 10,000 loans, with and without server indexes, at `cores = 1` and `cores = 8`, three runs each, the morning's binary and the new client decoder.
The decoder moves the shapes that return many rows and nothing else: at 10,000 loans unindexed, `books-by-genre` (about 125 rows) {ABG['books-by-genre'][1] / ABG['books-by-genre'][0]:.2f}x, `loans-by-daterange` {ABG['loans-by-daterange'][1] / ABG['loans-by-daterange'][0]:.2f}x and `overdue`
{ABG['overdue'][1] / ABG['overdue'][0]:.2f}x in three interleaved old/new pairs (section 8); the one-row and few-row shapes sit at 0.99x to 1.05x. Against the morning's run the same three are +40 %, +40 % and +29 %, and the unindexed
equality walks (`loans-by-user`, `loans-by-book`, `resv-by-user`, `count-by-user`, `join-loan-user`) are **-12 % to -16 %**, which the A/B places on the host (the old client in this window reads {f0(ABG['loans-by-user'][0], ' qps')} on `loans-by-user`, the morning's cell {f0(q1(1, 10000, 'none', 'loans-by-user', 'morn'), ' qps')}).
Against PostgreSQL 18.6, **reused from the morning**, the 10,000-loan columns are {geo_vs_pg(10000, 'none'):.2f}x (unindexed) and {geo_vs_pg(10000, 'all'):.2f}x (indexed) as geometric means over 12 shapes, from 1.06x and 0.98x; the three many-row shapes, 0.61x to 0.77x of PostgreSQL unindexed,
are now {ratio(q1(1, 10000, 'none', 'books-by-genre'), qp(10000, 'none', 'books-by-genre'))}, {ratio(q1(1, 10000, 'none', 'loans-by-daterange'), qp(10000, 'none', 'loans-by-daterange'))} and {ratio(q1(1, 10000, 'none', 'overdue'), qp(10000, 'none', 'overdue'))}. At `cores = 1` and 200 or 1,000 loans, 12 of the 48 cells carry a spread above 20 % in this window against 29 of 48 in the morning
(the median spread of the 200-loan cells is 4.5 % unindexed and 3.8 % indexed, from 27.7 % and 23.4 %), so more of the small-relation columns are readable: KDS is at {geo_vs_pg(200, 'none'):.2f}x of PostgreSQL unindexed at 200 loans (four of its twelve shapes above the 20 % line).
At `cores = 8` the order reverses (10 of 48 above 20 % now, 6 of 48 in the morning; 7 of 12 at 200 loans unindexed).

Driver: `tools/scenario3_library.py`, unmodified, `--index-mode none|all --server-indexes on --seed 1` (`--assert-index-reads` on the indexed cells). Measured by `ck-tester`.
Comparator (this engine's own last run, same binary, old client): `bench/v3.3.0/results-scenario3-library-{MD}.md`. PostgreSQL 18.6: that file's twin cells (`tools/pg_scenario3_library.py --user cdkbs`, three runs per cell), **not re-run**:
{BUDGET}.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-20; matrix cells (36) {wtext(kt)}; A/B pairs (six cells, interleaved with the scenario 1 and 4 A/B cells) {wtext(abt)}; join-order probe {wtext(plt)}. Per-cell windows: section 10 |
{COMMONROWS}| Driver | **unmodified** `tools/scenario3_library.py`; exit code 0 and no verify problem in every KDS cell (`verify_problems` empty in {VOK} of {VN}); `--assert-index-reads` passed on every indexed cell |
| Unchanged from the morning | {PATHS} |

## 2. What was run

| axis | values |
|---|---|
| cores | 1, 8 (`group` durability; reads only, so durability does not enter) |
| index mode | `none` (the driver creates no index) and `all` (it creates the indexes each shape can use, after the load); `--server-indexes on` |
| rows | `--loans 200`, `1000`, `10000`; users, books and reservations scale with it (`sizes` in each JSON: at 200 loans 40 / 40 / 100 relation rows, at 10,000 loans 2,000 / 2,000 / 5,000) |
| runs | 3 per cell (`r2-`, `r3-` prefixes); a fresh server and data file per cell |
| shapes | 12, 200 operations each, serial on one connection, so the **qps is derived**: 1,000,000 / mean µs |
| A/B | `ab-old{{1,2,3}}-s3-c1-n10000-none` against `ab-new{{1,2,3}}-s3-c1-n10000-none`, interleaved old, new, old, new, old, new |
| join-order probe | `join_order = planned` (default) and `written`, 10,000 loans, `none` x 3 runs and `all` x 1 run, with the full ANALYZE replies dumped |
| PostgreSQL | not run; the morning's `pg_scenario3_library.py` cells at the same `--loans` and `--index-mode` (three runs each) |

Cut: nothing from the morning's KDS plan. Throughput below is derived qps (1,000,000 / mean µs), the median of three runs.

## 3. Throughput, now against the morning

Findings are read by the rule of `{SUMMARY}` ("What counts as a finding"): a delta below the larger spread is not a finding; a spread above 20 % carries no claim. A delta against the morning is client plus host (section 8 separates them).

{main_table(1)}
`cores = 8`:

{main_table(8)}
Geometric mean over the 12 shapes of the ratio now over the morning, per cell, and how many shapes lie outside both spreads:

{geo_table()}
**Row-returning shapes up, scans down, the rest flat.** At 10,000 loans unindexed `books-by-genre` {delta(q1(1, 10000, 'none', 'books-by-genre'), q1(1, 10000, 'none', 'books-by-genre', 'morn'))}, `loans-by-daterange` {delta(q1(1, 10000, 'none', 'loans-by-daterange'), q1(1, 10000, 'none', 'loans-by-daterange', 'morn'))},
`overdue` {delta(q1(1, 10000, 'none', 'overdue'), q1(1, 10000, 'none', 'overdue', 'morn'))} (every one a finding by its spreads), the five integer-column equalities and joins -12 % to -16 %, `join-no-literal` and `exists-correlated` inside their spreads, `pk-user` flat.
Indexed, the same row-returning shapes move +51 %, +42 % and +51 %, the equalities +2 % to +8 %. At `cores = 1`, 200 and 1,000 loans, 29 of the morning's 48 cells carried a spread above 20 % (a slow mode, one run in three) and cannot be
read against; 12 of 48 do here (200 loans unindexed: 4 of 12 shapes, indexed 2; 1,000 unindexed 5, indexed 1) and are not read. At `cores = 8` it is the other way round
(10 of 48 now, 6 of 48 then), concentrated in the 200-loan unindexed column (7 of 12). A 20 ms cell falling into a slow mode one run in three is a property of the host and the cell length, not of the client; whether it has gone at `cores = 1` or did not fall
in this window is not separated by one run of three.

## 4. What the index buys

Throughput of the indexed cell over the unindexed one, `cores = 1`, median of three, the morning's ratio and PostgreSQL's beside it:

{index_buys()}
An index is worth nothing at 200 loans (the walk is two pages and the statement is a round trip: 0.94x to 1.03x on every shape), 0.97x to 1.39x at 1,000, and 4.4x to 5.6x on four of the equality shapes at 10,000
(`loans-by-user` {ratio(q1(1, 10000, 'all', 'loans-by-user'), q1(1, 10000, 'none', 'loans-by-user'))}, `loans-by-book`, `join-loan-user`, `count-by-user`; PostgreSQL {ratio(qp(10000, 'all', 'loans-by-user'), qp(10000, 'none', 'loans-by-user'))} on `loans-by-user`) and 3.2x on `resv-by-user`.
The morning's 3.96x on `loans-by-user` is lower than this run's because the morning's unindexed walk read 12 % faster (section 8) while its indexed cell read 6 % slower; the index did not change. `loans-by-daterange` and `books-by-genre` gain nothing on either side: their cost is the rows returned
(`books-by-genre` about 125 rows) or the range's whole-relation walk.

## 5. The join planner (CB/CC) on the library's joins

The client has no part in a plan. ANALYZE of the join shapes at 10,000 loans (`pl-s3-planned-none-r1`; the replies in full are in the archive) is the morning's plan, line for line: `join-no-literal` is
`Range users` [1, 16] -> `Scan loans`, **`build on=col1`**, `inner_built=1`, `build_rows=79`, `build_probes=15`, `examined=10,106` rows (159 pages), `planned order=memo ... switch=1` (order = written);
`exists-correlated` is `Range users` [1, 20] with a correlated `EXISTS` sub-chain `Scan loans` (`build on=col1`, `build_rows=4,603`, `corr_scans=3`), `examined=4,652`. The planned default against `join_order = written`, same binary, same window:

{plan_tbl()}
Both arms have the same plan (`inner_built=1` in each). Planned over written is the number to read; the decoder cost that hid it is smaller now, and the arms are within their spreads of each other wherever a spread is below 20 %, as in the morning.

## 6. Percentiles

Run 1 of each `cores = 1` cell, every phase (microseconds; the five percentiles are the driver's). The `cores = 8` cells' JSON is in the archive.

{pct_cells(1)}
The three many-row shapes at 10,000 loans unindexed, run 1 now against run 1 of the morning and of PostgreSQL (the morning's twin run):

{cmp_pct3(['books-by-genre', 'loans-by-daterange', 'overdue'])}
The whole distribution moved, not the median alone: `books-by-genre` p0 {pr('books-by-genre', 'p0_us'):.2f}x, p50 {pr('books-by-genre', 'p50_us'):.2f}x, p99 {pr('books-by-genre', 'p99_us'):.2f}x of the morning's; `loans-by-daterange` p50 {pr('loans-by-daterange', 'p50_us'):.2f}x
and `overdue` p50 {pr('overdue', 'p50_us'):.2f}x (the scan beneath them is the same engine work, so the saving is the rows' share of a statement). Every one of the three is tight: p99 is within {100 * (max(ph(now('s3-c1-n10000-none'), x)['p99_us'] / ph(now('s3-c1-n10000-none'), x)['p50_us'] for x in ('books-by-genre', 'loans-by-daterange', 'overdue')) - 1):.0f} % of p50.

PostgreSQL 18.6 at 10,000 loans, run 1 of the morning's cells, for every phase:

{pg_pct_10k()}

## 7. Where the time goes

Reads on one connection, no writes after the load, no commit, no lock: the latency is a fixed round trip plus rows walked plus rows returned, with no durability or conflict wait to separate.

| Component | KDS now | PostgreSQL (morning's run) | How derived |
|---|---|---|---|
| Fixed round trip (client, socket, parse, one pk descent) | **{ph(now('s3-c1-n10000-none'), 'pk-user')['mean_us']:.1f} µs** | {ph(mor(3, 'pg-s3-n10000-none'), 'pk-user')['mean_us']:.1f} µs | `pk-user` mean at 10,000 loans, run 1 |
| Durability / commit | not applicable | not applicable | read-only shapes |
| Lock or conflict wait | none | none | one connection |

Cost per row walked by an unindexed equality (mean at 10,000 loans minus mean at 200, over the 9,800 extra rows of `loans`), and the cost of a returned row (`books-by-genre` returns about 125 rows):

{perrow()}
This table carries the host drift: the walk costs {PX('loans-by-user', 'now'):.1f} ns per extra row here against {PX('loans-by-user', 'morn'):.1f} ns in the morning for `loans-by-user` and PostgreSQL's {PX('loans-by-user', 'pg'):.1f} ns, so **the morning's reading that KDS walks cheaper per examined row than PostgreSQL
({PX('loans-by-book', 'morn'):.1f} and {PX('loans-by-user', 'morn'):.1f} ns against {PX('loans-by-user', 'pg'):.1f} and {PX('loans-by-book', 'pg'):.1f} ns) is not reproduced in this window**: the old client reads the same walk as slowly (section 8). The returned-row term moved the other way: `books-by-genre` costs {PX('books-by-genre', 'now'):.1f} ns per extra relation row against {PX('books-by-genre', 'morn'):.1f} ns in the morning (PostgreSQL {PX('books-by-genre', 'pg'):.1f} ns),
which is the decoder. Which of the two windows has the engine's true walk cost is not separated by this run; the morning's PostgreSQL cells ran at 04:23 to 04:31 UTC, closer in time to the morning's KDS cells (03:58 to 04:07) than to these.

## 8. The client, isolated: old against new, interleaved

Three pairs on the same host, `s3-c1-n10000-none` each (a fresh server and data file per cell, the same binary copy; old = `a40882af^`'s client from `oldtools/`).

{subprocess.run(['python3', '-I', '/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/analysis/s3.py', 'ab'], capture_output=True, text=True).stdout}
**The decoder moves exactly the shapes that return rows.** `books-by-genre` {ABG['books-by-genre'][1] / ABG['books-by-genre'][0]:.2f}x, `loans-by-daterange` {ABG['loans-by-daterange'][1] / ABG['loans-by-daterange'][0]:.2f}x, `overdue` {ABG['overdue'][1] / ABG['overdue'][0]:.2f}x,
`join-no-literal` {ABG['join-no-literal'][1] / ABG['join-no-literal'][0]:.2f}x (the build keeps 79 rows, and the join returns them), against 0.99x to 1.05x for the shapes that return 5 rows or fewer. **The -12 % to -16 % on the equality walks is not the client**: the old client
reads `loans-by-user` {f0(ABG['loans-by-user'][0], ' qps')}, `loans-by-book` {f0(ABG['loans-by-book'][0], ' qps')}, `resv-by-user` {f0(ABG['resv-by-user'][0], ' qps')} and `count-by-user` {f0(ABG['count-by-user'][0], ' qps')} in this window, against the morning's {f0(q1(1, 10000, 'none', 'loans-by-user', 'morn'), ' qps')}, {f0(q1(1, 10000, 'none', 'loans-by-book', 'morn'), ' qps')},
{f0(q1(1, 10000, 'none', 'resv-by-user', 'morn'), ' qps')} and {f0(q1(1, 10000, 'none', 'count-by-user', 'morn'), ' qps')}, with the same binary. The same host effect as scenario 1's scans (section 9 of its file); not separated into frequency, neighbour or placement.

## 9. Against PostgreSQL 18.6 (the morning's run, reused)

PostgreSQL is the floor: it answers "is the engine in the right league" and nothing finer. Its twin was not re-run; ratios are KDS `cores = 1` median qps (this run) over the morning's PostgreSQL median qps (above 1.00x = KDS faster), the morning's KDS ratio after the slash:

{pg_ratio()}
{pg_qps()}
**Reading.** At 10,000 loans the engine is in the PostgreSQL league and the many-row shapes no longer set it apart: `books-by-genre` {ratio(q1(1, 10000, 'none', 'books-by-genre'), qp(10000, 'none', 'books-by-genre'))} (from 0.69x), `loans-by-daterange` {ratio(q1(1, 10000, 'none', 'loans-by-daterange'), qp(10000, 'none', 'loans-by-daterange'))} (from 0.61x),
`overdue` {ratio(q1(1, 10000, 'none', 'overdue'), qp(10000, 'none', 'overdue'))} (from 0.77x); indexed, 0.84x, 0.88x and 0.83x. The shapes that gained nothing from the decoder read lower against a PostgreSQL that was measured before the drift (unindexed equalities 1.00x to 1.10x, from 1.08x to 1.25x). At 200 loans KDS is at
{geo_vs_pg(200, 'none'):.2f}x unindexed and {geo_vs_pg(200, 'all'):.2f}x indexed (geometric means); the morning's 0.79x came with spreads of 20 % to 43 % on most of those cells (section 3), and this window's figure has four of its twelve shapes above the 20 % line, so neither is a settled number.

## 10. Noise floor, contamination and host

The noise floor is the 3-run spread of one cell: at 10,000 loans unindexed it is 0.7 % to 20.7 % across the 12 shapes, median {med([sp1(1, 10000, 'none', sh) for sh in SH]):.1f} %; the A/B spreads are 0.5 % to 19 %. `books-by-author` (20.7 %, 19.0 %) and `resv-by-user` (11.8 %, 12.9 %)
carry the widest. Contamination: none in this scenario (`logs/validity.log`; summary, "Contamination").

Per-cell host stamps (load average before the cell, gate wait, and what the 1-second monitor saw inside the window):

{hostrows(kt + abt + plt)}
## 11. What the run says about the engine

1. **A returned row costs the engine little; the Python decoder was most of what a client saw.** `books-by-genre` is one 10,000-loan scan returning about 125 rows, and its statement time falls by a third with the decoder fixed and nothing else touched.
   Scenario 3's shapes return 1 to 125 rows, so only three of its twelve move; the fixed round trip is untouched ({ph(now('s3-c1-n10000-none'), 'pk-user')['mean_us']:.1f} µs for `pk-user`).
2. **An index is worth about the same factor on both engines** (4.4x to 5.6x on KDS against 3.8x to 5.1x on PostgreSQL for `loans-by-user`, `loans-by-book`, `join-loan-user` and `count-by-user` at 10,000 loans), and nothing at 200 loans on either. The structure that pays for itself in this scenario is the same one that pays in PostgreSQL.
3. **Host drift is workload-specific and sizeable**: -11 % to -16 % on integer-column scans of a 10,000-row relation between a 03:58 and a 07:06 window, none on scenario 4's varchar walk (server CPU 2.20 s against 2.22 s). A comparison across hours that does not interleave the arms cannot read a 10 % difference on a scan shape in this environment.

## 12. Archive

`bench/v3.3.0/archive/scenario3-{DESC}/`: per-cell JSON, driver stdout, server configs, the join-order probe's ANALYZE replies (`plans/`). What the four scenarios share is in `bench/v3.3.0/archive/run-{DESC}/`. No data file is kept.
"""
write("results-scenario3-library", txt)
