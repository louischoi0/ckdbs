import sys, math
sys.path.insert(0, "/home/cdkbs/bench-run")
from doc import *
from an import *
import host
import s3_tables as S


def win(tags):
    s = [host.STARTS[t] for t in tags if t in host.STARTS]
    e = [host.ENDS[t][0] for t in tags if t in host.ENDS]
    return min(s)[11:19], max(e)[11:19]


kt = [f"{p}s3-c{c}-n{n}-{m}" for p in PFX for c in (1, 8) for n in (200, 1000, 10000) for m in ("none", "all")]
pt = [f"{p}pg-s3-n{n}-{m}" for p in PFX for n in (200, 1000, 10000) for m in ("none", "all")]
plt = [f"pl-s3-{o}-{m}-r{i}" for o in ("planned", "written") for m, rr in (("none", (1, 2, 3)), ("all", (1,))) for i in rr]


def q1(kind, c, n, m, sh):
    return med(S.cellq(kind, c, n, m, sh))


def index_table():
    rows = ["| shape | n=200: all over none | n=1,000: all over none | n=10,000: all over none | n=10,000 none (qps) | n=10,000 all (qps) | PG18 n=10,000: all over none |", "|---|---|---|---|---|---|---|"]
    for sh in S.SHAPES:
        r = [ratio(q1("new", 1, n, "all", sh), q1("new", 1, n, "none", sh)) for n in (200, 1000, 10000)]
        pr = ratio(q1("pg", 1, 10000, "all", sh), q1("pg", 1, 10000, "none", sh))
        rows.append(f"| {sh} | " + " | ".join(r) + f" | {f0(q1('new', 1, 10000, 'none', sh), ' qps')} | {f0(q1('new', 1, 10000, 'all', sh), ' qps')} | {pr} |")
    return "\n".join(rows) + "\n"


def pct_tables():
    out = []
    for n in (200, 1000, 10000):
        for m in ("none", "all"):
            out.append(S_pct(f"s3-c1-n{n}-{m}"))
    return "\n".join(out)


def S_pct(tag):
    d = new(tag)
    r = [f"#### `{tag}` run 1 (cores = 1)\n", "| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |", "|---|---|---|---|---|---|---|---|---|"]
    for p in d["phases"]:
        r.append(f"| {p['phase']} | " + pct_row(p) + " |")
    return "\n".join(r) + "\n"


def perrow():
    rows = ["| shape | KDS mean n=200 | KDS mean n=10,000 | KDS per extra row | PG mean n=200 | PG mean n=10,000 | PG per extra row |", "|---|---|---|---|---|---|---|"]
    for sh in ("pk-user", "loans-by-user", "loans-by-book", "books-by-genre"):
        a, b = ph(new("s3-c1-n200-none"), sh)["mean_us"], ph(new("s3-c1-n10000-none"), sh)["mean_us"]
        pa, pb = ph(new("pg-s3-n200-none"), sh)["mean_us"], ph(new("pg-s3-n10000-none"), sh)["mean_us"]
        rows.append(f"| {sh} | {a:,.1f} µs | {b:,.1f} µs | {(b - a) / 9800 * 1000:,.1f} ns | {pa:,.1f} µs | {pb:,.1f} µs | {(pb - pa) / 9800 * 1000:,.1f} ns |")
    return "\n".join(rows) + "\n"


def geo(kind, other, c=1):
    out = []
    for n, m in S.COLS:
        r = [q1(kind, c, n, m, sh) / q1(other, c, n, m, sh) for sh in S.SHAPES if q1(kind, c, n, m, sh) and q1(other, c, n, m, sh)]
        out.append(math.exp(sum(math.log(x) for x in r) / len(r)))
    return out


def c8c1():
    r = [q1("new", 8, n, m, sh) / q1("new", 1, n, m, sh) for n, m in S.COLS for sh in S.SHAPES]
    r.sort()
    return r[0], r[len(r) // 2], r[-1], sum(1 for x in r if x < 1), len(r)


GN = geo("new", "old")
GP = geo("new", "pg")
c8 = c8c1()
JN = {k: q1(k, 1, 10000, "none", "join-no-literal") for k in ("new", "old", "pg")}
EX = {k: q1(k, 1, 10000, "none", "exists-correlated") for k in ("new", "old", "pg")}
JNA = {k: q1(k, 1, 10000, "all", "join-no-literal") for k in ("new", "old", "pg")}

plan_ab = T("plan_ab.py").split("Scenario 3, ")[1]
plan_ab = "Scenario 3, " + plan_ab

txt = f"""# Scenario 3 (library) at `{DESC}`

**One line.** Twelve read shapes over four relations at 200, 1,000 and 10,000 loans, with and without server indexes, at `cores = 1` and `cores = 8`, three runs each: at 10,000 loans the engine is
where it was at v3.1.0 on 11 of 12 shapes (geometric means {GN[4]:.2f}x unindexed and {GN[5]:.2f}x indexed; the four smaller columns, {min(GN[:4]):.2f}x to {max(GN[:4]):.2f}x, sit inside spreads of 20 % to 40 % and are not read), and on one it is
**{JN['new'] / JN['old']:.2f}x faster: the unindexed two-relation join `join-no-literal` at 10,000 loans, {f0(JN['new'], ' qps')} against {f0(JN['old'], ' qps')}** (spread {sp(spread(S.cellq('new', 1, 10000, 'none', 'join-no-literal')))} now, {sp(spread(S.cellq('old', 1, 10000, 'none', 'join-no-literal')))} at v3.1.0), with the plan
reporting `inner_built=1` (a statement-local hash build of `loans`, 79 rows built from one 10,074-row scan instead of one scan per outer row). That is **not the join planner**: the same
cell with `join_order = written` is {f0(1719, ' qps')}, 5 % faster than the planned arm (section 6). Against PostgreSQL 18.6, **re-measured in the same window**, the unindexed 10,000-loan columns are
{GP[4]:.2f}x (geometric mean over 12 shapes) and the indexed ones {GP[5]:.2f}x; the unindexed equality (`loans-by-user`) is {ratio(q1('new', 1, 10000, 'none', 'loans-by-user'), q1('pg', 1, 10000, 'none', 'loans-by-user'))} of PostgreSQL's seqscan. At 200 loans KDS is behind
({GP[0]:.2f}x unindexed): there the statement is a round trip and PostgreSQL's is cheaper, and this run's small cells are noisy (section 10).

Driver: `tools/scenario3_library.py`, unmodified, `--index-mode none|all --server-indexes on --seed 1` (`--assert-index-reads` on the indexed cells), the v3.1.0 run's arguments. Measured by `ck-tester`.
Comparator (cross-version): `bench/v3.1.0/results-scenario3-library-v3.1.0-30-gcd7c7c4e.md`, re-derived from its archived JSON (three runs per cell). PostgreSQL 18.6:
`tools/pg_scenario3_library.py --user cdkbs --index-mode none|all`, **fresh, three runs per cell**, at its defaults.

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-20; KDS cells {win(kt)[0]} to {win(kt)[1]} UTC (72 cells: 2 core counts x 3 sizes x 2 index modes x 3 runs); PostgreSQL cells {win(pt)[0]} to {win(pt)[1]} UTC (18 cells); join-order probe {win(plt)[0]} to {win(plt)[1]} UTC (8 cells) |
{COMMON.replace('results-scenario{N}-*', 'results-scenario3-library')}| Driver | **unmodified** `tools/scenario3_library.py`; exit 0 in every cell; `verify_problems` empty in {sum(1 for t in kt if new(t) and new(t)['meta']['verify_problems'] == [])} of {len(kt)} KDS cells |

## 2. What was run

| axis | values |
|---|---|
| cores | 1, 8 (`group` durability; reads only, so durability does not enter) |
| index mode | `none` (the driver creates no index) and `all` (it creates the indexes each shape can use, after the load); `--server-indexes on` |
| rows (rule 9) | `--loans 200`, `1000`, `10000`; users, books and reservations scale with it (`sizes` in each JSON: at 200 loans 40 / 40 / 100 relation rows, at 10,000 loans 2,000 / 2,000 / 5,000) |
| runs | 3 per cell (`r2-`, `r3-` prefixes); a fresh server and data file per cell |
| shapes | 12, 200 operations each, serial on one connection, so the **qps is derived**: 1,000,000 / mean µs |
| PostgreSQL | `pg_scenario3_library.py` at the same `--loans` and `--index-mode`, `--seed 1`, 3 runs per cell, no `cores` axis |
| join-order probe | `join_order = planned` (default) and `written`, 10,000 loans, `none` x 3 runs and `all` x 1 run, with the full ANALYZE replies dumped |

Cut: nothing. Throughput below is derived qps (1,000,000 / mean µs), the median of three runs.

## 3. Throughput

{NOISE_NOTE}

`cores = 1` and `cores = 8`:

{T('s3_tables.py', 'tp')}
`cores = 8` reads {c8[0]:.2f}x to {c8[2]:.2f}x of `cores = 1` over the 72 cells (median {c8[1]:.2f}x) and is the slower of the two in {c8[3]} of {c8[4]}: one serial connection lands on one core and the other seven have nothing to do, so no gain is expected; the
loss is inside the cell-to-cell spread of the `pk-user` control and is not a finding.

**Row-count independence of the control.** `pk-user`, a `Lookup`, does not scale with rows or index mode: {f0(q1('new', 1, 200, 'none', 'pk-user'))} to {f0(q1('new', 1, 10000, 'all', 'pk-user'))} qps across the six `cores = 1` cells, 14,184 to 14,859 qps in four of them. The two low
cells (`n=200 none` 11,099 and `n=1,000 all` 10,060 qps) are the slow-mode cells of section 10, with spreads of 43 % and 43 %.

Spread of the three runs per cell (the noise floor of each cell), `cores = 1` then `cores = 8`:

{T('s3_tables.py', 'sp')}
## 4. Delta against the previous run of this shape (cross-version)

Ratio of median qps, now over v3.1.0 (above 1.00x = faster now), 240 commits apart, so no difference is attributable to one change. **Read it with the spreads above**: at 200 and 1,000
loans most cells carry 20 % to 40 % spreads (a cell is 200 statements of 100 µs, 20 ms, and one scheduler stall is a third of it), so the ratios there are not findings. At 10,000 loans the
spreads of the unindexed cells are 1 % to 10 % and the ratios are readable.

{T('s3_tables.py', 'delta')}
At 10,000 loans, unindexed, `cores = 1`: nine of the twelve shapes are within 0.95x to 1.07x; `join-no-literal` is **{JN['new'] / JN['old']:.2f}x** ({f0(JN['old'], ' qps')} to {f0(JN['new'], ' qps')}, spreads {sp(spread(S.cellq('new', 1, 10000, 'none', 'join-no-literal')))} and {sp(spread(S.cellq('old', 1, 10000, 'none', 'join-no-literal')))}) and `cores = 8` shows the same
step ({S.cellq('new', 8, 10000, 'none', 'join-no-literal') and f0(med(S.cellq('new', 8, 10000, 'none', 'join-no-literal')), ' qps')}). `exists-correlated` is {EX['new'] / EX['old']:.2f}x and `join-loan-user` {q1('new', 1, 10000, 'none', 'join-loan-user') / q1('old', 1, 10000, 'none', 'join-loan-user'):.2f}x.
Geometric mean over the 12 shapes, per column, `cores = 1` (new over old): {', '.join(f'{x:.2f}x' for x in GN)} for {', '.join(f'n={n:,} {m}' for n, m in S.COLS)}.

## 5. What the index buys now

Throughput of the indexed cell over the unindexed one, `cores = 1`, median of three, with PostgreSQL's ratio at 10,000 loans beside it (a B-tree on the same columns):

{index_table()}
An index is worth nothing at 200 loans (the walk is two pages and the statement is a round trip) and about 4x at 10,000 on the equality shapes (`loans-by-user` {ratio(q1('new', 1, 10000, 'all', 'loans-by-user'), q1('new', 1, 10000, 'none', 'loans-by-user'))},
PostgreSQL {ratio(q1('pg', 1, 10000, 'all', 'loans-by-user'), q1('pg', 1, 10000, 'none', 'loans-by-user'))}). Two shapes gain nothing on KDS: `books-by-genre` (1/16 of the relation, about 125 rows returned, plan stays `IndexProbe` but the cost is the rows) and `loans-by-daterange` (plan stays `Scan`); PostgreSQL gains 1.2x on `books-by-genre` and 1.0x on `loans-by-daterange`. `overdue` gains 1.2x on KDS and 1.7x on PostgreSQL.

## 6. The join planner (CB/CC) on the library's joins

ANALYZE of the three join shapes at 10,000 loans, unindexed and indexed (`pl-s3-planned-none-r1`, `pl-s3-planned-all-r1`; the replies in full are in the archive):

| shape | unindexed plan | `reordered` | examined | indexed plan |
|---|---|---|---|---|
| `join-no-literal` | `Range users` [1, 16] -> `Scan loans`, **`build on=col1`**, `inner_built=1`, `build_rows=79`, `build_probes=15` (order = written, est 0:16 1:91, gen 1) | no | 10,106 (pages 159) | `Range users` -> `IndexProbe loans` |
| `exists-correlated` | `Range users` [1, 20] with a correlated `EXISTS` sub-chain `Scan loans` (`build on=col1`, `inner_built=0`, `build_rows=4,603`, `corr_scans=3`) | n/a (one outer relation) | 4,652 (pages 61) | `Range users` -> `IndexProbe loans` |
| `join-loan-user` | not ANALYZEd by the driver (its plan is not recorded) | - | - | - |

The planner priced both orders of `join-no-literal`, kept the written one (`switch=1`), and the build on the inner `Scan` is what removed the per-outer-row scan: 10,074 rows examined for a 16-row outer
instead of 16 x 10,000. A and B, the planned default against `join_order = written` on the same binary:

{plan_ab}
**Both arms have the same plan** (`inner_built=1` in each; the `written` arm shows no `planned order=` line because it compiles no planned block), and the planned arm is 1 % to 5 % *slower* on the three join shapes unindexed (three runs; spreads 1 % to 8 %) and 6 % to 14 % slower indexed (one run, so no claim). The planner chose what the written order already was. So the 1.98x that this scenario shows against v3.1.0 is not CB/CC's order choice. It is the statement-local inner build
(`join-inner-build.md`) or the walk beneath it; the v3.1.0 JSON records only the step kinds (`Range`, `Scan`), not `inner_built`, so this run cannot say whether v3.1.0 built or not, and the two
runs are 240 commits apart. The A/B isolates what it can: the planner is not the cause.

## 7. Percentiles

Run 1 of each `cores = 1` cell, every phase (microseconds; the five percentiles are the driver's). The `cores = 8` cells' JSON is in the archive.

{pct_tables()}
## 8. Where the time goes

Reads on one connection, no writes after the load, no commit, no lock: the latency is a fixed round trip plus rows walked plus rows returned, with no durability or conflict wait to separate.

| Component | KDS | PostgreSQL | How derived |
|---|---|---|---|
| Fixed round trip (client, socket, parse, one pk descent) | **{ph(new('s3-c1-n10000-none'), 'pk-user')['mean_us']:,.1f} µs** | {ph(new('pg-s3-n10000-none'), 'pk-user')['mean_us']:,.1f} µs | `pk-user` mean at 10,000 loans, run 1 |
| Durability / commit | not applicable | not applicable | read-only shapes |
| Lock or conflict wait | none | none | one connection |

Cost per row walked by an unindexed equality (mean at 10,000 loans minus mean at 200, over the 9,800 extra rows of `loans`), and the cost of a returned row (`books-by-genre` returns about 125 rows):

{perrow()}
KDS's walk is cheaper per examined row (23.5 and 24.7 ns against PostgreSQL's 33.8 and 35.1 ns on the two `loans` equalities), which is the whole reason the unindexed equality is ahead of PostgreSQL at 10,000 loans. Where rows must be
returned it is the opposite way (`books-by-genre`, 125 rows: 38.8 ns per extra relation row against 27.6 ns), the per-row result cost that scenario 1 isolates.

## 9. Against PostgreSQL 18.6 (fresh, same window)

PostgreSQL is the floor: it answers "is the engine in the right league" and nothing finer. The twin ran after the KDS cells, three runs per cell, at its defaults (`shared_buffers = 128MB`, `synchronous_commit = on`); no
`cores` axis. Ratio of KDS `cores = 1` median qps to PostgreSQL's median qps (above 1.00x = KDS faster), then PostgreSQL's own medians:

{T('s3_tables.py', 'pg')}
**Reading.** At 10,000 loans the engine is in the PostgreSQL league: the unindexed equalities are 1.08x to 1.23x, the correlated `EXISTS` {ratio(EX['new'], EX['pg'])} unindexed, `join-no-literal` {ratio(JN['new'], JN['pg'])} unindexed and {ratio(JNA['new'], JNA['pg'])}
indexed. It is behind where rows must be returned or a range filter walks the whole relation (`books-by-genre` 0.69x, `loans-by-daterange` 0.61x, `overdue` 0.77x unindexed). At 200 loans it is behind on 10 of 12 shapes unindexed (0.62x to 0.99x) and ahead on `exists-correlated` and `count-by-user`; the behind is the round-trip gap (PostgreSQL's `pk-user` is {ph(new('pg-s3-n200-none'), 'pk-user')['mean_us']:,.1f} µs at 200 loans, KDS's {ph(new('s3-c1-n200-none'), 'pk-user')['mean_us']:,.1f} µs), amplified by this run's slow-mode spreads.

## 10. Noise floor, contamination and host

The noise floor is the spread table of section 3: 0.1 % to 4 % on the large unindexed cells and the join shapes, **20 % to 43 % on a third of the cells at 200 and 1,000 loans**. The cause is the cell length: a shape is 200
statements of 70 to 130 µs, 14 to 26 ms, and one slow run in three (a stall of 5 to 10 ms of the host) is a spread of that size. The pattern repeats at `pk-user`, the control, whose spread is 43 % in `n=200 none` and 43 % in `n=1,000 all`; a
cell where the control moves is a cell where the host moved, and **no comparison in the small columns, cross-version or against PostgreSQL, is read as a finding**. This is the same behaviour section 10 of the v3.1.0 file describes (its
"slow mode").

**Contamination.** No KDS cell was discarded. The PostgreSQL batch of this scenario failed on its first attempt, six cells in 3 to 4 seconds each, on `role "ec2-user" does not exist` (the twin's `--user` default); the master and the watchdog were
stopped by recorded PID, the six attempts are in `contaminated/pg-s3-no-user-attempt/` of the run archive, the command line got `--user cdkbs` and the cells were re-run in the same window. All 18 PostgreSQL cells in this file are the re-run.

Per-cell host stamps:

{T('host.py', *kt, *pt, *plt)}
## 11. What the run says about the engine

1. **Nothing in the 11 read shapes moved beyond noise since v3.1.0**: the large unindexed cells (readable) are within 0.95x to 1.07x and the indexed ones within 0.91x to 1.02x. The engine's read path is where v3.1.0 left it in this scenario.
2. **The one real mover is a join without a literal, and the join planner is not the reason**: `join-no-literal` is 1.98x v3.1.0, and the same cell with `join_order = written` is as fast. The statement-local inner build is in both arms. The planned arm
   is *not* faster than written on any of the three join shapes. This is the first data point on CB/CC in this scenario: the planner is conservative (it keeps the written order unless it wins by the margin) and costs nothing measurable here.
3. **KDS walks a row cheaper than PostgreSQL (about 24 ns against 34 ns) and returns a row dearer** (the `books-by-genre` figure above). The crossover is why the equality shapes are ahead at 10,000 loans and the
   range and wide-result shapes are behind.
4. **An index pays about 4x for an equality at 10,000 loans and nothing at 200** on both engines. A Cabin or index on a relation of a few pages is no tool.
5. **This scenario's small cells cannot support a finding**; a longer `--ops` would, and is the next change to the measurement, not to the engine.

## 12. Archive

`bench/v3.3.0/archive/scenario3-{DESC}/`: per-cell JSON (KDS and PostgreSQL), driver stdout, server configs, and `plans/` (the full ANALYZE replies of the join-order probe). What the four scenarios share is in
`bench/v3.3.0/archive/run-{DESC}/`.
"""
write("results-scenario3-library", txt)
