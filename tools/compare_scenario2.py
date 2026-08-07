#!/usr/bin/env python3
"""Diffs a ckdbs scenario2 run against its PostgreSQL baseline.

Reads the two `--json` files written by

    tools/scenario2_freight.py       (ckdbs)
    tools/pg_scenario2_freight.py    (PostgreSQL)

and prints them side by side: the three-way outcome accounting that is this
scenario's shape, and every statement the booking and the reporter issue,
grouped by *read case* — point lookup, non-pk filter scan, grouped
aggregate, join aggregate, insert, update, and the commit — so "which engine
is faster at what kind of access" is a table rather than an impression.

## What is comparable, and what is not

**The commit row is the strongest.** Both engines fsync a write-ahead log to
the same device under the same durability promise, so that row compares two
flush paths doing one job.

**The statement rows are real but protocol-loaded.** Both drivers send one
statement per round trip with inline literals and decode every returned row,
but ckdbs speaks its newline text protocol and PostgreSQL its v3 wire — a
lighter round trip on one side. Read the per-case rows as "what a client
pays", and read *ratios between cases within one engine* as the engine
comparison: an engine whose join costs 10× its point lookup has a different
shape than one where it costs 3×, whatever the absolute numbers say.

**The reporter rows carry a placement difference.** The ckdbs reporter runs
in its own process on its own connection, contending with the bookers;
PostgreSQL's twin has one connection, so the same reads interleave between
bookings and displace them instead. Both read a freight ledger growing under
them. The rows are printed and the difference is labelled.

## The identity check

Before any table, the tool verifies the two runs did the same work: same
seed, sizes, headrooms, capacity mode, transaction mode, booker count — and
the loaded reference-relation counts, which both drivers derive from the same
imported generator, so identical parameters must load identical counts. A
mismatch makes every number below meaningless; the tool says so and exits
non-zero unless --force.

Outcome *counts* are deliberately not identity: a time-boxed run's mix of
committed/rejected depends on how far the run got, and comparing the mixes is
part of the comparison, not a precondition of it.

## Run them one at a time

Both drivers saturate what they point at and both clients are Python on the
same box; run concurrently, each measures the other and nothing records that
it happened. Sequentially, and with matching parameters:

    python3 tools/scenario2_freight.py    --port 15599 --bookings 1500 \\
        --verify 25 --json kds.json
    python3 tools/pg_scenario2_freight.py --port 15433 --bookings 1500 \\
        --verify 25 --json pg.json
    python3 tools/compare_scenario2.py kds.json pg.json
"""

import argparse
import json
import sys

# The run parameters that must agree for the two files to describe one
# workload: what was loaded, and what a booking does. `bookers` is identity
# because the PostgreSQL twin runs one booker — a 4-booker ckdbs file against
# it compares a contended run to an uncontended one, which is a different
# question than "which engine".
IDENTITY_KEYS = ("seed", "capacity_mode", "txn", "max_fees", "bookings",
                 "organizations", "ships", "operations", "cargos",
                 "capacity_headroom", "credit_headroom", "hot_routes",
                 "bookers")
# The reference relations both loaders fill from the same imported generator.
LOADED_KEYS = ("organizations", "ships", "operations", "fees", "recipes",
               "cargos")

# The statements, grouped by the access case each one exercises. Labels for
# `capacity-read` depend on --capacity-mode and are patched in main().
CASES = (
    ("point lookup (pk)", ("cargo-lookup", "credit-lookup")),
    ("capacity read", ("capacity-read",)),
    ("non-pk filter scan", ("recipe-read", "manifest-scan")),
    ("grouped aggregate", ("voyage-rollup",)),
    ("join + aggregate", ("customer-statement",)),
    ("insert", ("freight-insert", "charge-insert")),
    ("update (pk)", ("operation-update", "org-update")),
    ("durability", ("commit",)),
    ("whole booking", ("booking",)),
)

OUTCOME_ORDER = ("committed", "rejected-capacity", "rejected-credit",
                 "conflicted", "failed")


def load(path):
    try:
        with open(path) as f:
            return json.load(f)
    except OSError as e:
        sys.exit(f"compare_scenario2: cannot read {path}: {e}")
    except json.JSONDecodeError as e:
        sys.exit(f"compare_scenario2: {path} is not valid JSON: {e}")


def engine_of(doc, path):
    name = doc.get("meta", {}).get("engine")
    if not name:
        sys.exit(f"compare_scenario2: {path} has no meta.engine - it is not "
                 f"a scenario2 result file")
    if doc["meta"].get("scenario") != "freight":
        sys.exit(f"compare_scenario2: {path} is not a scenario2 (freight) "
                 f"result file")
    return name


def ratio(a, b):
    """`a` relative to `b` as a multiple; None where it cannot be computed,
    which is not the same as 1.00x and must not print as one."""
    if not b:
        return None
    return a / b


def fmt_ratio(value, width=9):
    return f"{value:>{width - 1},.2f}x" if value else f"{'-':>{width}}"


def fmt_us(value, width=10):
    return f"{value:>{width - 1},.0f}u" if value is not None else f"{'-':>{width}}"


def fmt_count(value, width=10):
    return f"{value:>{width},}" if value is not None else f"{'-':>{width}}"


# ---- the identity check --------------------------------------------------

def check_identity(left, right, left_name, right_name):
    """Returns a list of problems. Empty means the two runs are comparable."""
    problems = []
    lm, rm = left["meta"], right["meta"]

    for key in IDENTITY_KEYS:
        if key not in lm or key not in rm:
            continue
        if lm[key] != rm[key]:
            problems.append(f"{key}: {left_name}={lm[key]} "
                            f"{right_name}={rm[key]}")

    # The strong check: both loaders are the same imported generator driven
    # by the same seed, so the reference relations must have loaded the same
    # counts row for row. freights/charges are outcomes, not identity.
    l_loaded, r_loaded = lm.get("loaded", {}), rm.get("loaded", {})
    for key in LOADED_KEYS:
        if key not in l_loaded or key not in r_loaded:
            problems.append(f"loaded.{key}: missing from one file")
        elif l_loaded[key] != r_loaded[key]:
            problems.append(f"loaded.{key}: {left_name}={l_loaded[key]} "
                            f"{right_name}={r_loaded[key]}")
    return problems


# ---- the outcome accounting ----------------------------------------------

def print_outcomes(left, right, left_name, right_name):
    lo, ro = left["meta"].get("outcomes"), right["meta"].get("outcomes")
    if not lo or not ro:
        print("no outcome accounting in one of the files (--load-only?), "
              "skipping")
        return

    def attempts(counts):
        return sum(counts.values()) - counts.get("conflicted", 0)

    print()
    print(f"the three-way outcome accounting (S2-6) - {left_name} against "
          f"{right_name}")
    header = (f"{'outcome':<20}{left_name:>10}{'share':>9}"
              f"{right_name:>10}{'share':>9}")
    print(header)
    print("-" * len(header))
    la, ra = attempts(lo), attempts(ro)
    for name in OUTCOME_ORDER:
        l, r = lo.get(name, 0), ro.get(name, 0)
        ls = f"{100.0 * l / la:.1f}%" if la else "-"
        rs = f"{100.0 * r / ra:.1f}%" if ra else "-"
        print(f"{name:<20}{fmt_count(l)}{ls:>9}{fmt_count(r)}{rs:>9}")
    print(f"{'retries':<20}{fmt_count(left['meta'].get('retries'))}{'':>9}"
          f"{fmt_count(right['meta'].get('retries'))}{'':>9}")
    l_tps, r_tps = left["meta"].get("tps"), right["meta"].get("tps")
    print(f"{'TPS (committed)':<20}{l_tps:>10,.1f}{'':>9}{r_tps:>10,.1f}"
          f"{fmt_ratio(ratio(l_tps, r_tps))}")
    print()
    print("  shares are of attempts (conflicts are re-driven, not attempts of")
    print("  their own). A rejection is the business saying no, and a rejection")
    print("  share that differs across engines on identical parameters means")
    print("  the runs stopped at different depths of the same cargo pool.")


# ---- the read-case table -------------------------------------------------

def phases_of(doc):
    return {p["phase"]: p for p in doc.get("phases", [])}


def print_cases(left, right, left_name, right_name, capacity_mode):
    lp, rp = phases_of(left), phases_of(right)
    print()
    print(f"every statement, by access case - mean and p99 are client round")
    print(f"trips; `x` is {right_name}/{left_name}, so above 1.00x means "
          f"{left_name} was faster")
    header = (f"{'case / statement':<34}{'ops':>7}"
              f"{left_name + ' mean':>12}{right_name + ' mean':>12}{'x':>9}"
              f"{left_name + ' p99':>12}{right_name + ' p99':>12}{'x':>9}")
    print(header)
    print("-" * len(header))
    for case, names in CASES:
        if case == "capacity read":
            case = ("capacity read (pk, derived column)"
                    if capacity_mode == "cached"
                    else "capacity read (SUM over filter scan)")
        rows = [(n, lp.get(n), rp.get(n)) for n in names]
        rows = [(n, l, r) for n, l, r in rows if l or r]
        if not rows:
            continue
        print(case)
        for name, l, r in rows:
            lm = l["mean_us"] if l else None
            rm = r["mean_us"] if r else None
            l99 = l["p99_us"] if l else None
            r99 = r["p99_us"] if r else None
            ops = (l or r)["ops"]
            print(f"  {name:<32}{ops:>7,}"
                  f"{fmt_us(lm, 12)}{fmt_us(rm, 12)}"
                  f"{fmt_ratio(ratio(rm, lm))}"
                  f"{fmt_us(l99, 12)}{fmt_us(r99, 12)}"
                  f"{fmt_ratio(ratio(r99, l99))}")
    print()
    print("  the commit row is the strongest comparison here: one fsync to the")
    print("  same device on both sides. The statement rows carry two different")
    print("  wire protocols, so read them as what a client pays - the engine")
    print("  claim is in each engine's *shape*, printed next.")


def print_shape(left, right, left_name, right_name):
    """Each engine's statement costs as multiples of its own point lookup.

    This is the within-engine tier: the protocol and client costs divide
    out, so the two columns are comparable in a way the absolute rows above
    are not."""
    baseline_of = {}
    for name, doc in ((left_name, left), (right_name, right)):
        p = phases_of(doc).get("cargo-lookup")
        baseline_of[name] = p["mean_us"] if p else None
    if not all(baseline_of.values()):
        return

    print()
    print("each engine's costs as multiples of its own pk point lookup")
    print("  (cargo-lookup = 1.00x; the protocol divides out of these columns)")
    header = f"{'statement':<24}{left_name:>10}{right_name:>10}"
    print(header)
    print("-" * len(header))
    for name in ("recipe-read", "manifest-scan", "voyage-rollup",
                 "customer-statement", "freight-insert", "charge-insert",
                 "operation-update", "org-update", "commit"):
        l = phases_of(left).get(name)
        r = phases_of(right).get(name)
        if not l and not r:
            continue
        lx = ratio(l["mean_us"], baseline_of[left_name]) if l else None
        rx = ratio(r["mean_us"], baseline_of[right_name]) if r else None
        print(f"{name:<24}{fmt_ratio(lx, 10)}{fmt_ratio(rx, 10)}")


# ---- verify and the reporter ---------------------------------------------

def print_verify(left, right, left_name, right_name):
    lv, rv = left["meta"].get("verify"), right["meta"].get("verify")
    if not lv and not rv:
        return
    print()
    print("the §4 invariants (I1-I4)")
    for name, v in ((left_name, lv), (right_name, rv)):
        if not v:
            print(f"  {name:<8} not verified (--verify 0)")
        elif v["failures"]:
            print(f"  {name:<8} {v['checks']} checks, "
                  f"{v['failures']} FAILURE(S) - first: {v['first']}")
        else:
            print(f"  {name:<8} {v['checks']} checks, 0 failures")


def print_reporter_note(left, right, left_name, right_name):
    lm, rm = left["meta"], right["meta"]
    if "manifest_passes" not in lm and "manifest_passes" not in rm:
        return
    print()
    print("the reporter, and why its rows are the weakest comparison:")
    for name, m, how in (
            (left_name, lm, "its own process and connection, contending "
                            "with the bookers"),
            (right_name, rm, "interleaved between bookings on the one "
                             "connection, displacing them")):
        if "manifest_passes" in m:
            print(f"  {name:<8} {m['manifest_passes']} passes, "
                  f"{m['manifest_rows_read']:,} freight rows scanned - {how}")
        else:
            print(f"  {name:<8} ran without the reporter (--no-manifest)")


def print_header(left, right, left_name, right_name):
    lm, rm = left["meta"], right["meta"]
    print()
    print(f"scenario2 (freight) comparison - {left_name} against {right_name}")
    print(f"  {lm.get('organizations'):,} organizations, {lm.get('ships'):,} "
          f"ships, {lm.get('operations'):,} voyages, {lm.get('cargos'):,} "
          f"cargos, seed {lm.get('seed')}")
    print(f"  {'BEGIN/COMMIT' if lm.get('txn') else 'autocommit'}, "
          f"capacity={lm.get('capacity_mode')}, "
          f"{lm.get('bookers', 1)} booker(s)"
          + (", foreign keys declared" if lm.get("fk") else "")
          + (", cabin declared" if lm.get("cabin") else ""))
    print(f"  {left_name}: {lm.get('host')}:{lm.get('port')}, "
          f"isolation {lm.get('isolation', 'server default')}"
          + (f", durability {lm['durability']}" if lm.get("durability") else ""))
    print(f"  {right_name}: {rm.get('host')}:{rm.get('port')}, "
          f"synchronous_commit {rm.get('synchronous_commit')}")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("left", help="the ckdbs --json file")
    parser.add_argument("right", help="the PostgreSQL --json file")
    parser.add_argument("--force", action="store_true",
                        help="print the comparison even when the two runs are "
                             "not of the same workload. The numbers are then "
                             "not a comparison of engines, and the identity "
                             "problems stay printed above them")
    args = parser.parse_args()

    left, right = load(args.left), load(args.right)
    left_name = engine_of(left, args.left)
    right_name = engine_of(right, args.right)
    # Short, because they are column headers in the tables below.
    left_name = "ckdbs" if left_name == "ckdbs" else left_name[:8]
    right_name = "pg" if right_name == "postgresql" else right_name[:8]

    print_header(left, right, left_name, right_name)

    problems = check_identity(left, right, left_name, right_name)
    print()
    if problems:
        print("identity: FAILED - the two runs did not run the same workload")
        for line in problems:
            print(f"    {line}")
        if not args.force:
            print()
            print("  refusing to compare. Re-run both tools with the same "
                  "--seed and sizes")
            print("  (and the ckdbs side with --bookers 1, which is the twin's "
                  "shape), or")
            print("  pass --force to print anyway.")
            return 2
        print()
        print("  --force given: the tables below compare two different "
              "workloads and license no claim about either engine.")
    else:
        loaded = left["meta"].get("loaded", {})
        print(f"identity: OK - same parameters, and all "
              f"{sum(loaded.get(k, 0) for k in LOADED_KEYS):,} reference rows "
              f"loaded identically on both engines")

    capacity_mode = left["meta"].get("capacity_mode", "cached")
    print_outcomes(left, right, left_name, right_name)
    print_cases(left, right, left_name, right_name, capacity_mode)
    print_shape(left, right, left_name, right_name)
    print_verify(left, right, left_name, right_name)
    print_reporter_note(left, right, left_name, right_name)

    print()
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
