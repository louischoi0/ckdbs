#!/usr/bin/env python3
"""Diffs a ckdbs scenario1 run against its PostgreSQL baseline.

Reads the two `--json` files written by

    tools/scenario1_backtest.py       (ckdbs)
    tools/pg_scenario1_backtest.py    (PostgreSQL)

and prints them side by side: the QPS matrix per read shape and case, the
accelerator each engine offers for a non-primary-key equality, the write
sweep against transaction batch size, the concurrency sweep, and the phase
timings.

## What is comparable, and what is not

Three tiers, and the tool prints them in decreasing order of how much they
license a claim.

**Ratios within one engine are the strongest.** `cabin/warm` on one side and
`index/warm` on the other are each measured on one engine, over one data
set, in one process, with one thing changed between the two measurements.
Nothing about the client, the machine, or the file system enters them. When
this tool says ckdbs's Cabin bought 199x and PostgreSQL's index bought 40x,
those two numbers are each solid, and their comparison is the point.

**Absolute QPS across engines is weaker but real.** Both drivers send one
statement per round trip over a raw socket with inline literals, decode
every returned row, and build the same Python string from it - that is
fidelity choice 7 in the PostgreSQL tool, and it exists so this row of the
table means something. What it still carries: two different wire protocols,
and two servers with different default durability settings.

**The concurrency table is the weakest and is not a defect.** ckdbs
dispatches one core's statements on one thread; PostgreSQL runs a backend
process per connection. A rising curve on one side and a flat one on the
other is an architectural difference being reported correctly, not a
measurement error. It is printed because it is worth knowing, and labelled
because it is worth not over-reading.

## The identity check

Before any of that, this tool verifies the two runs actually ran the same
workload: same seed, same years, symbols, rebalance and sweep parameters,
and - the strong one - **the same model P&L to the basis point**. Both tools
generate their prices from the imported generator and score them with the
imported models, so identical parameters must produce identical rankings. If
they do not, the two files describe different workloads and every number
below them is meaningless; the tool says so and exits non-zero.

## Run them one at a time

Both tools saturate whatever they are pointed at, and both clients are
Python processes on the same box. Running them concurrently makes each one
measure the other, unequally - and nothing in the resulting files records
that it happened, so the identity check passes and the numbers are quietly
wrong. Run the second only after the first has printed its verdict.

Usage:
    # sequentially, and with matching parameters on both sides:
    python3 tools/scenario1_backtest.py    --port 15599 --json kds.json
    python3 tools/pg_scenario1_backtest.py --port 15433 --json pg.json
    python3 tools/compare_scenario1.py kds.json pg.json
    python3 tools/compare_scenario1.py kds.json pg.json --quiet-phases

Scale matters for one column in particular. PostgreSQL's planner declines an
index it correctly judges more expensive than a seqscan, so on a small run
(a few hundred rows a relation) the `index` case measures the planner making
the right call and the gain reads as ~1.0x. The default 30 years x 8 symbols
puts 60,480 rows in `daily_stats`, which is where that column starts
reporting what an index is worth.
"""

import argparse
import json
import sys

# The run parameters that must agree for the two files to describe one
# workload. `seed` and the data-shape keys decide what was loaded; the sweep
# keys decide what was measured over it. A mismatch in any of them makes the
# comparison invalid rather than merely noisy, which is why this is a check
# and not a note.
IDENTITY_KEYS = ("seed", "years", "symbols", "sessions", "bars", "rebalance",
                 "top_k", "periods")
SWEEP_KEYS = ("compare_rounds", "replay")


def load(path):
    try:
        with open(path) as f:
            return json.load(f)
    except OSError as e:
        sys.exit(f"compare_scenario1: cannot read {path}: {e}")
    except json.JSONDecodeError as e:
        sys.exit(f"compare_scenario1: {path} is not valid JSON: {e}")


def engine_of(doc, path):
    name = doc.get("meta", {}).get("engine")
    if not name:
        sys.exit(f"compare_scenario1: {path} has no meta.engine - it is not "
                 f"a scenario1 result file")
    return name


def ratio(a, b):
    """`a` relative to `b`, printed as a multiple. None where it cannot be
    computed, which is not the same as 1.00x and must not print as one."""
    if not b:
        return None
    return a / b


def fmt_ratio(value, width=10):
    return f"{value:>{width - 1},.2f}x" if value else f"{'-':>{width}}"


def fmt_qps(value, width=10):
    return f"{value:>{width},.0f}" if value is not None else f"{'-':>{width}}"


# ---- the identity check --------------------------------------------------

def check_identity(left, right, left_name, right_name):
    """Returns a list of problems. Empty means the two runs are comparable."""
    problems = []
    lm, rm = left["meta"], right["meta"]

    for key in IDENTITY_KEYS + SWEEP_KEYS:
        if key not in lm or key not in rm:
            continue
        if lm[key] != rm[key]:
            problems.append(f"{key}: {left_name}={lm[key]} "
                            f"{right_name}={rm[key]}")

    # The strong check. Both tools score the same imported models over the
    # same imported price series, so identical parameters must give
    # identical basis points - not merely a similar ranking.
    left_pnl = {r["name"]: r["total_bp"] for r in lm.get("ranking", [])}
    right_pnl = {r["name"]: r["total_bp"] for r in rm.get("ranking", [])}
    if not left_pnl or not right_pnl:
        problems.append("one of the files carries no model ranking, so the "
                        "workloads cannot be shown to be the same")
    elif left_pnl != right_pnl:
        for name in sorted(set(left_pnl) | set(right_pnl)):
            a, b = left_pnl.get(name), right_pnl.get(name)
            if a != b:
                problems.append(f"model {name}: {left_name}={a} bp "
                                f"{right_name}={b} bp")
    return problems


# ---- the QPS matrix ------------------------------------------------------

def index_shapes(doc):
    return {row["shape"]: row for row in doc["meta"].get("qps_sweep", [])}


def case_qps(row, case):
    cell = row.get(case) if row else None
    return cell["qps"] if cell else None


def print_matrix(left, right, left_name, right_name):
    left_shapes, right_shapes = index_shapes(left), index_shapes(right)
    if not left_shapes or not right_shapes:
        print("no QPS sweep in one of the files (--no-sweep?), skipping the "
              "matrix")
        return

    left_accel = left["meta"].get("accelerator", "cabin")
    right_accel = right["meta"].get("accelerator", "index")

    order = [s for s in left_shapes if s in right_shapes]
    print()
    print(f"QPS by read shape - {left_name} against {right_name}")
    print(f"  cells are queries per second; `x` columns are "
          f"{left_name}/{right_name}")
    header = (f"{'shape':<16}"
              f"{'cold ' + left_name:>13}{'cold ' + right_name:>13}{'x':>9}"
              f"{'warm ' + left_name:>13}{'warm ' + right_name:>13}{'x':>9}")
    print(header)
    print("-" * len(header))
    for shape in order:
        l, r = left_shapes[shape], right_shapes[shape]
        lc, rc = case_qps(l, "cold"), case_qps(r, "cold")
        lw, rw = case_qps(l, "warm"), case_qps(r, "warm")
        print(f"{shape:<16}{fmt_qps(lc, 13)}{fmt_qps(rc, 13)}"
              f"{fmt_ratio(ratio(lc, rc), 9)}"
              f"{fmt_qps(lw, 13)}{fmt_qps(rw, 13)}"
              f"{fmt_ratio(ratio(lw, rw), 9)}")

    # The headline. Each ratio is measured entirely within one engine, which
    # is what makes this the row of the report that carries a claim.
    print()
    print(f"the accelerator each engine offers for a non-pk equality")
    print(f"  {left_name}: {left_accel.upper()};  "
          f"{right_name}: {right_accel.upper()}")
    header = (f"{'shape':<16}"
              f"{left_name + ' warm':>13}{left_name + ' ' + left_accel:>13}"
              f"{'gain':>9}"
              f"{right_name + ' warm':>13}{right_name + ' ' + right_accel:>13}"
              f"{'gain':>9}   filter column")
    print(header)
    print("-" * (len(header) + 12))
    for shape in order:
        l, r = left_shapes[shape], right_shapes[shape]
        lw, la = case_qps(l, "warm"), case_qps(l, "cabin")
        rw, ra = case_qps(r, "warm"), case_qps(r, "cabin")
        if la is None and ra is None:
            continue        # a pk shape: neither engine has a third case
        column = l.get("cabin_on") or r.get("cabin_on") or ""
        print(f"{shape:<16}{fmt_qps(lw, 13)}{fmt_qps(la, 13)}"
              f"{fmt_ratio(ratio(la, lw), 9)}"
              f"{fmt_qps(rw, 13)}{fmt_qps(ra, 13)}"
              f"{fmt_ratio(ratio(ra, rw), 9)}   {column}")

    # Did dropping it put the number back? If it did not, the third column
    # measured something other than the accelerator - a warmed cache, an
    # autovacuum, a checkpoint - and the gain above is overstated.
    print()
    print("  drop check - warm again after dropping the accelerator, as a")
    print("  fraction of the original warm. Far from 1.00x means the third")
    print("  column measured something besides the accelerator:")
    for shape in order:
        l, r = left_shapes[shape], right_shapes[shape]
        lw, ld = case_qps(l, "warm"), case_qps(l, "dropped")
        rw, rd = case_qps(r, "warm"), case_qps(r, "dropped")
        if ld is None and rd is None:
            continue
        print(f"    {shape:<16}{left_name} {fmt_ratio(ratio(ld, lw), 8)}   "
              f"{right_name} {fmt_ratio(ratio(rd, rw), 8)}")

    # Shapes whose cold case ran short are shapes with fewer distinct
    # arguments than the cell size, and their cold/warm distinction is
    # correspondingly weaker. Reported because it is invisible in the table.
    print()
    for shape in order:
        for doc_name, shapes in ((left_name, left_shapes),
                                 (right_name, right_shapes)):
            row = shapes[shape]
            if row.get("cold_ops") and row.get("warm_ops") and \
                    row["cold_ops"] < row["warm_ops"]:
                print(f"  {shape} ({doc_name}): cold ran {row['cold_ops']} of "
                      f"{row['warm_ops']} - fewer distinct arguments exist "
                      f"than the cell size, so its cold/warm gap is "
                      f"understated")


# ---- the write sweep -----------------------------------------------------

def print_write(left, right, left_name, right_name):
    lw = {r["batch"]: r for r in left["meta"].get("write_sweep", [])}
    rw = {r["batch"]: r for r in right["meta"].get("write_sweep", [])}
    batches = [b for b in lw if b in rw]
    if not batches:
        return

    print()
    print(f"INSERT rows/s by transaction batch size")
    header = (f"{'rows/txn':<12}{left_name:>13}{right_name:>13}{'x':>9}"
              f"{left_name + ' gain':>14}{right_name + ' gain':>14}")
    print(header)
    print("-" * len(header))
    l_base = lw.get(1, lw.get(min(batches)))["qps"]
    r_base = rw.get(1, rw.get(min(batches)))["qps"]
    for batch in sorted(batches):
        l, r = lw[batch]["qps"], rw[batch]["qps"]
        label = "1 (auto)" if batch == 1 else f"{batch:,}"
        print(f"{label:<12}{fmt_qps(l, 13)}{fmt_qps(r, 13)}"
              f"{fmt_ratio(ratio(l, r), 9)}"
              f"{fmt_ratio(ratio(l, l_base), 14)}"
              f"{fmt_ratio(ratio(r, r_base), 14)}")
    print()
    print("  the `gain` columns are each engine against its own autocommit")
    print("  rate, which is the comparable half: the absolute numbers carry")
    print("  two different default durability settings (ckdbs `group`,")
    print("  PostgreSQL `synchronous_commit = on`) and the gains do not.")


# ---- the concurrency sweep -----------------------------------------------

def print_concurrency(left, right, left_name, right_name):
    lc = {r["connections"]: r for r in left["meta"].get("concurrency_sweep", [])}
    rc = {r["connections"]: r for r in right["meta"].get("concurrency_sweep", [])}
    counts = [c for c in lc if c in rc]
    if not counts:
        return

    print()
    print("the 3-relation join by connection count")
    header = (f"{'connections':<13}{left_name:>13}{right_name:>13}{'x':>9}"
              f"{left_name + ' scale':>15}{right_name + ' scale':>15}")
    print(header)
    print("-" * len(header))
    l_base = lc[min(counts)]["qps"]
    r_base = rc[min(counts)]["qps"]
    for count in sorted(counts):
        l, r = lc[count]["qps"], rc[count]["qps"]
        print(f"{count:<13}{fmt_qps(l, 13)}{fmt_qps(r, 13)}"
              f"{fmt_ratio(ratio(l, r), 9)}"
              f"{fmt_ratio(ratio(l, l_base), 15)}"
              f"{fmt_ratio(ratio(r, r_base), 15)}")
    print()
    print("  the `scale` columns are each engine against its own one-")
    print("  connection rate. This is the weakest row of the report and the")
    print("  difference is architectural: ckdbs dispatches one core's")
    print("  statements on one thread, so its curve is round-trip overlap;")
    print("  PostgreSQL runs a backend process per connection, so its curve")
    print("  is parallel execution. Both are being reported correctly.")


# ---- the phase table -----------------------------------------------------

def print_phases(left, right, left_name, right_name):
    lp = {p["phase"]: p for p in left.get("phases", [])}
    rp = {p["phase"]: p for p in right.get("phases", [])}
    shared = [p["phase"] for p in left.get("phases", [])
              if p["phase"] in rp]
    if not shared:
        return

    print()
    print("phases as the workload ran - qps, and p50 latency")
    header = (f"{'phase':<22}{left_name + ' qps':>13}{right_name + ' qps':>13}"
              f"{'x':>9}{left_name + ' p50':>13}{right_name + ' p50':>13}"
              f"{'x':>9}")
    print(header)
    print("-" * len(header))
    for name in shared:
        l, r = lp[name], rp[name]
        # p50 inverted deliberately: for latency, lower is better, so the
        # ratio is printed the same way round as the qps one - above 1.00x
        # always means the left engine did better.
        print(f"{name:<22}{fmt_qps(l['qps'], 13)}{fmt_qps(r['qps'], 13)}"
              f"{fmt_ratio(ratio(l['qps'], r['qps']), 9)}"
              f"{l['p50_us']:>12,.0f}u{r['p50_us']:>12,.0f}u"
              f"{fmt_ratio(ratio(r['p50_us'], l['p50_us']), 9)}")
    print()
    print("  every `x` column reads the same way: above 1.00x means "
          f"{left_name} did better,")
    print("  including the p50 pair, where the ratio is inverted because "
          "lower latency is better.")


def print_header(left, right, left_name, right_name):
    lm, rm = left["meta"], right["meta"]
    print()
    print(f"scenario1 comparison - {left_name} against {right_name}")
    print(f"  {lm.get('years')} years x {lm.get('symbols')} symbols = "
          f"{lm.get('bars'):,} bars + {lm.get('bars'):,} feature rows, "
          f"{lm.get('sessions'):,} sessions, seed {lm.get('seed')}")
    print(f"  {lm.get('periods')} rebalance periods, "
          f"{lm.get('rebalance')} sessions apart, "
          f"{len(lm.get('ranking', []))} models")
    print(f"  {left_name}: {lm.get('host')}:{lm.get('port')}, "
          f"daily_bars {str(lm.get('clustered')).upper()}"
          + (", cabins declared at DDL" if lm.get("cabin") else "")
          + (", foreign keys declared" if lm.get("fk") else ""))
    print(f"  {right_name}: {rm.get('host')}:{rm.get('port')}")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("left", help="the ckdbs --json file")
    parser.add_argument("right", help="the PostgreSQL --json file")
    parser.add_argument("--quiet-phases", action="store_true",
                        help="skip the per-phase table, which is long")
    parser.add_argument("--force", action="store_true",
                        help="print the comparison even when the two runs "
                             "are not of the same workload. The numbers are "
                             "then not a comparison of engines, and the "
                             "identity problems stay printed above them")
    args = parser.parse_args()

    left, right = load(args.left), load(args.right)
    left_name = engine_of(left, args.left)
    right_name = engine_of(right, args.right)
    # Short, because they are column headers in six tables below.
    left_name = "ckdbs" if left_name == "ckdbs" else left_name[:8]
    right_name = "pg" if right_name == "postgresql" else right_name[:8]

    print_header(left, right, left_name, right_name)

    problems = check_identity(left, right, left_name, right_name)
    print()
    if problems:
        print(f"identity: FAILED - the two runs did not run the same "
              f"workload")
        for line in problems:
            print(f"    {line}")
        if not args.force:
            print()
            print("  refusing to compare. Re-run both tools with the same "
                  "--seed, --years, --symbols and --rebalance, or pass "
                  "--force to print anyway.")
            return 2
        print()
        print("  --force given: the tables below compare two different "
              "workloads and license no claim about either engine.")
    else:
        print(f"identity: OK - same parameters, and all "
              f"{len(left['meta'].get('ranking', []))} models scored "
              f"identically to the basis point on both engines")

    print_matrix(left, right, left_name, right_name)
    print_write(left, right, left_name, right_name)
    print_concurrency(left, right, left_name, right_name)
    if not args.quiet_phases:
        print_phases(left, right, left_name, right_name)

    print()
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
