#!/usr/bin/env python3
"""Prices a two-relation join on ckdbs, and checks what declaring it as a
pattern is worth today.

Two tables, 10000 rows each by default, and three join shapes that differ
in exactly one thing: **which access kind the step compiler assigns the
inner relation**. That choice is not an executor implementation note - it
is simultaneously the probe strategy and Waystone's lookup/search line
(docs/spec/parser-v2.md section 1, include/kds/exec/step_chain.hpp), so the gap
between these phases is the same gap between what a recorded trail may
one day *replace* and what it may only prefetch for.

    join-point     child pk lookup, then a pk probe into parent.
                   Both steps lookup-class, so the whole chain is
                   trail-replayable. This is the shape a pattern is for.
    join-driven    child scanned under a non-pk filter, then a pk probe
                   into parent per surviving row. Step 0 is a search and
                   can never be replayed; step 1 could be.
    join-nonpk     joined on a non-pk column, so the inner relation is
                   walked per outer row. Nothing here is replayable, and
                   it is quadratic - see --nonpk-ops.

---- What the pattern half of this tool does and does not measure --------

Recording and replay both work now (workplan P08-P13), so a declared
pattern *can* make a query faster here - but **this tool's key
distribution is the one case where it will not**, and that is worth
understanding before quoting any number it prints.

join-point picks a random id per query. Over --rows distinct ids and
--point-ops executions, each instance is seen roughly
`point-ops / rows` times; at the defaults that is under two, so most
instances never reach the n=2 recording threshold and never get a trail.
The workload pays Waystone's identification cost on every statement and
replays almost nothing. That is Waystone behaving correctly - declining
to record one-shot work - and it is exactly the shape a random-key
benchmark measures.

**For the favourable case, measure a hot instance**: one id, repeated.
bench/results-waystone-v2.md does that, and reports 22-33x on relations
with no pk index against a few percent of overhead on B+ tree ones.

What this tool still measures well:

1. **The join access-kind price** - join-point vs join-driven vs
   join-nonpk - which is independent of Waystone.

2. **That a declaration actually matches the traffic.** The tool declares
   the join-point shape, then runs `ANALYZE` on the live inline statement
   and compares the two `pattern_id`s. That equality is the whole
   load-bearing claim of CREATE PATTERN
   (docs/spec/create-pattern-user-defined-patterns-v1.md section 3.2). If
   it ever stops holding, every declared pattern silently matches nothing
   - no error, no failed query, just a feature that quietly does not
   work. This tool fails loudly on it.

It also runs join-point twice, before and after the declaration. The
declared form records from the first execution rather than the second, so
a small improvement there is expected; a large one is not, for the
distribution reason above.

Timing is per-request wall clock around a send+recv pair on a single
connection, so every latency includes Python's own socket cost - the same
convention benchmark.py uses, and the same floor: treat sub-10us
differences as noise.

Usage:
    # start a Release-build server on a scratch data file first:
    #   cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
    #   cmake --build build-release -j && ./build-release/kds_server /tmp/join.db
    python3 tools/join_benchmark.py
    python3 tools/join_benchmark.py --rows 10000 --port 15432
    python3 tools/join_benchmark.py --clustered heap    # see the probe collapse
    python3 tools/join_benchmark.py --json join.json

Each run creates its own pair of tables (`jb_<pid>_<epoch>_parent` /
`_child`) so a persistent data file can be benchmarked repeatedly without
earlier runs' rows inflating the scan phases.
"""

import argparse
import os
import random
import re
import sys
import time

from bench_common import report, run_phase, write_json
from ckdbs_cli import DEFAULT_HOST, DEFAULT_PORT, ServerConnection

# The inner relation - the one every join probes into. Its pk is what the
# equi-join binds, which is what makes those steps lookup-class.
PARENT_COLUMNS = "id int64, grp int64, c_text varchar"

# The driving relation, always step 0.
#
# **It takes the same --clustered as the parent, and it has to.** A
# `Lookup` step on a heap relation is a lookup in name only: there is no pk
# index to descend, so it falls through to the same chain walk a Scan does
# (step_vm.cpp's RunPointStep). With a heap child, join-point examines every
# child row and measures exactly what join-driven measures - which is how
# the first version of this tool reported the two phases as equal and
# looked plausible doing it.
#
#   parent_id   a real parent id, so the pk join finds exactly one row per
#               child row rather than measuring the cost of finding nothing.
#   grp         drawn from the same range as the parent's `grp`, so the
#               non-pk join has real matches for the same reason.
CHILD_COLUMNS = "id int64, parent_id int64, grp int64"

# How many distinct values `grp` takes on both sides. Shared, so the
# non-pk join's selectivity is a property of this constant rather than an
# accident of two separate literals. 64 groups over 10000 rows is ~156
# matches per value - enough that the join produces rows, few enough that
# the reply does not dominate the timing.
GROUP_COUNT = 64

TEXT_LEN = 16

# The declared pattern's name. Dropped and re-declared per run so a
# persistent data file does not accumulate one per invocation.
PATTERN_NAME = "jb_point"


def fail(message, reply=None):
    print(f"join_benchmark aborted: {message}", file=sys.stderr)
    if reply:
        print(f"  server said: {reply}", file=sys.stderr)
    sys.exit(1)


def check_phase(phase):
    """A phase where nothing succeeded measured nothing, so the run stops
    rather than reporting a qps for a column of errors."""
    if phase.errors == 0:
        return phase
    if phase.errors == phase.ops:
        fail(f"every {phase.name} failed - there is nothing to measure",
             phase.first_error)
    print(f"warning: {phase.errors} of {phase.ops} {phase.name} operations "
          f"failed; first: {phase.first_error}", file=sys.stderr)
    return phase


# ---- Pattern helpers ------------------------------------------------------

# `pattern_id=0x<hex>` as it appears in a CREATE PATTERN reply and in an
# ANALYZE header. One regex for both, deliberately: the whole point of the
# comparison is that the two numbers come from the same field.
PATTERN_ID_RE = re.compile(r"pattern_id=0x([0-9a-f]+)")


def pattern_id_of(reply):
    match = PATTERN_ID_RE.search(reply)
    return match.group(1) if match else None


def point_join_sql(parent, child, child_id):
    """The join-point statement, with an inline literal.

    Kept beside declared_pattern_sql() below because the two must stay the
    same *shape*: same relations, same written order, same predicate
    positions. Only the value differs, and a value is not shape - which is
    exactly the property the tool checks.
    """
    return (f"SELECT c.id, p.c_text FROM {child} AS c "
            f"JOIN {parent} AS p ON c.parent_id = p.id "
            f"WHERE c.id = {child_id}")


def declared_pattern_sql(parent, child):
    """The same statement with the literal written as a typed parameter."""
    return (f"CREATE PATTERN {PATTERN_NAME}($cid int64) "
            f"OF SELECT c.id, p.c_text FROM {child} AS c "
            f"JOIN {parent} AS p ON c.parent_id = p.id "
            f"WHERE c.id = $cid")


def declare_pattern(execute, parent, child):
    """Declares the join-point shape and returns
    (pattern_id, dir_depth, warnings, elapsed_s).

    Drops any leftover of the same name first: a persistent data file may
    carry one from an earlier run, and check 9 refuses a duplicate name.
    """
    execute(f"DROP PATTERN {PATTERN_NAME}")  # NotFound is fine and expected

    t0 = time.perf_counter()
    reply = execute(declared_pattern_sql(parent, child))
    elapsed = time.perf_counter() - t0

    if reply.startswith("ERR"):
        fail("could not declare the join pattern", reply)

    sections = reply.split("\\n")
    head = sections[0]
    warnings = [s[len("WARN "):] for s in sections[1:] if s.startswith("WARN ")]

    depth = re.search(r"dir_depth=(\d+)", head)
    return (pattern_id_of(head), int(depth.group(1)) if depth else 0, warnings,
            elapsed)


def plan_of(execute, sql):
    """The access kinds and row counts ANALYZE reports for one statement.

    Reported beside the timings because a phase can measure the wrong thing
    while looking entirely plausible: a `Lookup` step on a heap relation
    walks the whole chain, so join-point collapses onto join-driven and the
    table shows two believable, equal numbers. Printing `examined` next to
    each phase is what makes that visible instead of assumed.

    Returns (steps, examined) where `steps` is e.g. "Lookup+Probe".
    """
    reply = execute("ANALYZE " + sql)
    if reply.startswith("ERR"):
        return ("?", 0)
    sections = reply.split("\\n")
    examined = re.search(r"examined=(\d+)", sections[0])
    kinds = re.findall(r"^step \d+ (\w+) ", "\n".join(sections[1:]), re.MULTILINE)
    # findall over the plan body picks up each step once; the per-step stats
    # block below it repeats them, so only the first half is the plan.
    unique = kinds[:len(kinds) // 2] if len(kinds) > 1 else kinds
    return ("+".join(unique) if unique else "?",
            int(examined.group(1)) if examined else 0)


def live_pattern_id(execute, sql):
    """The pattern_id the engine computes for a statement it actually ran.

    Taken from ANALYZE rather than recomputed in Python on purpose: a
    reimplementation of the fingerprint here could agree with itself and
    disagree with the engine, which is the one failure this check exists
    to catch.
    """
    reply = execute("ANALYZE " + sql)
    if reply.startswith("ERR"):
        fail("could not ANALYZE the join statement", reply)
    return pattern_id_of(reply.split("\\n")[0])


# ---- Setup ----------------------------------------------------------------

def create_tables(execute, parent, child, clustered):
    reply = execute(f"CREATE TABLE {parent} ({PARENT_COLUMNS}) {clustered.upper()}")
    if reply.startswith("ERR"):
        fail(f"could not create {parent}", reply)
    reply = execute(f"CREATE TABLE {child} ({CHILD_COLUMNS}) {clustered.upper()}")
    if reply.startswith("ERR"):
        fail(f"could not create {child}", reply)


def parent_rows(rows, rng):
    for i in range(rows):
        text = "".join(rng.choice("abcdefghijklmnopqrstuvwxyz") for _ in range(TEXT_LEN))
        yield i % GROUP_COUNT, text


def main():
    ap = argparse.ArgumentParser(
        description="Join benchmark for ckdbs, with a CREATE PATTERN control.")
    ap.add_argument("--host", default=DEFAULT_HOST, help=f"default: {DEFAULT_HOST}")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT,
                    help=f"default: {DEFAULT_PORT}")
    ap.add_argument("--rows", type=int, default=10000,
                    help="rows in EACH of the two tables (default: 10000)")
    ap.add_argument("--point-ops", type=int, default=2000,
                    help="join-point queries (default: 2000)")
    ap.add_argument("--driven-ops", type=int, default=200,
                    help="join-driven queries; each scans the child relation "
                         "end to end (default: 200)")
    ap.add_argument("--nonpk-ops", type=int, default=10,
                    help="join-nonpk queries. Quadratic - each one is "
                         "--rows x --rows tuple decodes - so this is "
                         "deliberately tiny (default: 10)")
    ap.add_argument("--clustered", choices=("heap", "btree"), default="btree",
                    help="storage for BOTH relations. btree by default, "
                         "unlike benchmark.py: a lookup or probe is only a "
                         "descent on a tree, and on a heap relation it falls "
                         "through to the same chain walk a scan does - which "
                         "collapses join-point onto join-driven and makes "
                         "the headline phase measure the wrong thing "
                         "(default: btree)")
    ap.add_argument("--seed", type=int, default=20260802,
                    help="RNG seed, so a run is reproducible")
    ap.add_argument("--json", help="also write the full results here")
    ap.add_argument("--keep", action="store_true",
                    help="leave the declared pattern in place after the run")
    args = ap.parse_args()

    stamp = f"jb_{os.getpid()}_{int(time.time())}"
    parent, child = f"{stamp}_parent", f"{stamp}_child"
    rng = random.Random(args.seed)

    try:
        conn = ServerConnection(args.host, args.port)
    except OSError as exc:
        fail(f"could not connect to {args.host}:{args.port}: {exc}")
    execute = conn.send_command

    create_tables(execute, parent, child, args.clustered)

    phases = []

    # ---- Load ------------------------------------------------------------
    #
    # Timed and reported, but not the point of the tool: it is here so a
    # surprising join number can be checked against whether the load itself
    # was surprising.
    phases.append(check_phase(run_phase(
        execute, "insert-parent",
        (f"INSERT INTO {parent} VALUES ({grp}, '{text}')"
         for grp, text in parent_rows(args.rows, rng)),
        detail=f"{args.rows} rows, {args.clustered}-clustered")))

    # Child rows reference real parent ids. Parent ids are 1..rows (the
    # Keystone sequence starts at 1), so parent_id is drawn from that range
    # and every pk join finds exactly one row.
    phases.append(check_phase(run_phase(
        execute, "insert-child",
        (f"INSERT INTO {child} VALUES ({rng.randint(1, args.rows)}, "
         f"{rng.randint(0, GROUP_COUNT - 1)})"
         for _ in range(args.rows)),
        detail=f"{args.rows} rows, {args.clustered}-clustered, "
               f"parent_id in 1..{args.rows}")))

    # ---- The three join shapes -------------------------------------------

    def point_ids():
        return (rng.randint(1, args.rows) for _ in range(args.point_ops))

    phases.append(check_phase(run_phase(
        execute, "join-point",
        (point_join_sql(parent, child, i) for i in point_ids()),
        detail="child pk lookup + pk probe into parent; both steps "
               "lookup-class, so the whole chain is trail-replayable")))

    phases.append(check_phase(run_phase(
        execute, "join-driven",
        (f"SELECT c.id, p.c_text FROM {child} AS c "
         f"JOIN {parent} AS p ON c.parent_id = p.id "
         f"WHERE c.grp = {rng.randint(0, GROUP_COUNT - 1)}"
         for _ in range(args.driven_ops)),
        detail=f"child scanned ({args.rows} rows) under a non-pk filter, "
               f"then one pk probe per surviving row (~{args.rows // GROUP_COUNT})")))

    phases.append(check_phase(run_phase(
        execute, "join-nonpk",
        (f"SELECT c.id, p.c_text FROM {child} AS c "
         f"JOIN {parent} AS p ON c.grp = p.grp "
         f"WHERE c.id = {rng.randint(1, args.rows)}"
         for _ in range(args.nonpk_ops)),
        detail=f"joined on a non-pk column, so parent is walked per outer "
               f"row: ~{args.rows:,} decodes per query, never replayable")))

    # ---- The pattern control ---------------------------------------------

    # What each shape actually compiled to, taken once per shape outside
    # any timed region. See plan_of() for why this is worth printing.
    plans = {
        "join-point": plan_of(execute, point_join_sql(parent, child, 1)),
        "join-driven": plan_of(
            execute,
            f"SELECT c.id, p.c_text FROM {child} AS c "
            f"JOIN {parent} AS p ON c.parent_id = p.id WHERE c.grp = 0"),
        "join-nonpk": plan_of(
            execute,
            f"SELECT c.id, p.c_text FROM {child} AS c "
            f"JOIN {parent} AS p ON c.grp = p.grp WHERE c.id = 1"),
    }

    declared_id, dir_depth, warnings, declare_s = declare_pattern(
        execute, parent, child)

    probe_sql = point_join_sql(parent, child, 1)
    observed_id = live_pattern_id(execute, probe_sql)

    # The same phase again, with the pattern now declared. Expected to be
    # noise - see this file's header - and printed so that the day it is
    # not, it is visible rather than assumed.
    phases.append(check_phase(run_phase(
        execute, "join-point[declared]",
        (point_join_sql(parent, child, i) for i in point_ids()),
        detail="identical statements, pattern now declared - so instances "
               "record on their first execution rather than their second")))

    before = next(p for p in phases if p.name == "join-point")
    after = next(p for p in phases if p.name == "join-point[declared]")
    delta_pct = ((after.percentile(50) - before.percentile(50)) /
                 before.percentile(50) * 100.0) if before.percentile(50) else 0.0

    matched = declared_id is not None and declared_id == observed_id

    meta = {
        "engine": "ckdbs",
        "columns": f"{len(PARENT_COLUMNS.split(','))} parent / "
                   f"{len(CHILD_COLUMNS.split(','))} child",
        "rows": args.rows,
        "host": args.host,
        "port": args.port,
        "table": f"{parent} + {child}",
        "clustered": args.clustered,
        "pattern": {
            "name": PATTERN_NAME,
            "declared_pattern_id": declared_id,
            "observed_pattern_id": observed_id,
            "matched": matched,
            "dir_depth": dir_depth,
            "declare_ms": round(declare_s * 1e3, 3),
            "warnings": warnings,
        },
        "join_point_p50_delta_pct": round(delta_pct, 2),
        "plans": {k: {"steps": v[0], "examined": v[1]} for k, v in plans.items()},
    }

    footer = [
        "",
        "what each shape compiled to (from ANALYZE, one run per shape):",
    ]
    for name, (kinds, examined) in plans.items():
        footer.append(f"  {name:<14} {kinds:<16} examined={examined:,} rows/query")
    footer += [
        "",
        "CREATE PATTERN:",
        f"  declared  pattern_id=0x{declared_id}  dir_depth={dir_depth}  "
        f"in {declare_s * 1e3:.2f} ms",
        f"  observed  pattern_id=0x{observed_id}   (from ANALYZE of the live "
        f"inline statement)",
        f"  match     {'YES - a declared pattern will match this traffic'if matched else 'NO'}",
    ]
    for warning in warnings:
        footer.append(f"  warning   {warning}")
    footer += [
        "",
        f"  join-point p50 moved {delta_pct:+.1f}% once declared.",
        "  A declared pattern records from its first execution instead of its",
        "  second, which helps a little here and not much: this tool picks a",
        "  random id per query, so most instances are executed too few times",
        "  to be worth a trail either way. See the module docstring, and",
        "  bench/results-waystone-v2.md for the hot-instance measurement.",
        "",
        "  join-point vs join-driven vs join-nonpk is the access kind's price:",
        "  a pk equi-join probes, anything else walks the inner relation per",
        "  outer row. That is the same line Waystone draws between what a",
        "  trail may replace and what it may only prefetch for (invariant 9).",
    ]
    if args.clustered == "heap":
        footer += [
            "",
            "  --clustered heap: the parent has no pk index, so join-point's",
            "  'probe' falls through to the same chain walk join-nonpk does.",
            "  Run with --clustered btree to see the phases separate.",
        ]

    report(phases, meta, footer)

    if args.json:
        write_json(args.json, meta, phases)

    if not args.keep:
        execute(f"DROP PATTERN {PATTERN_NAME}")

    conn.close()

    # A mismatch is a failure, not a footnote: it means every declared
    # pattern silently matches nothing, which produces no error anywhere
    # else in the system.
    if not matched:
        print("join_benchmark: FAILED - the declared pattern_id does not match "
              "the one live traffic produces", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
