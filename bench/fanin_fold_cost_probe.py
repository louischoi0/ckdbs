#!/usr/bin/env python3
"""AG3 — what a fold over a fan-in costs, and against which alternative.

`workplan-insert-spreading.md` §12d built the widening and stated its cost
without a number: **a fold over a fan-in ships every row it folds.** The
session opens one stage per run of same-owner ranges, every stage streams
the relation's whole rows back, and the fold runs at the session — so
`SELECT COUNT(*)` moves the whole relation across the ring to count it.
`aggregate.hpp`'s `Merge` (AG-M) is the reserved answer and is unbuilt.

This driver measures that, and it measures the decision that rides on it.
§12d's routing rule sends a widened shape to the fan-in **only when no
single core owns the relation whole**; where one does, the statement ships
as text and is folded on the owner, which returns one row instead of every
row. That rule was taken on the argument above, with no number. Here is
the number.

Five arms on one `cores = 2` server, over the fixture
`self_directed_stage_probe.py` already defines — imported rather than
copied, so the two files measure the same rig by construction:

    Lstar  twin   @core0  SELECT *          0 stages, whole rows
    Lfold  twin   @core0  SELECT SUM(v)     0 stages, folded locally
    Ship   twin   @core1  SELECT SUM(v)     shipped as text, folded on the
                                            owner, one-row reply
    Fstar  spread @core0  SELECT *          2 stages, whole rows
    Ffold  spread @core0  SELECT SUM(v)     2 stages, folded at the session

Four differences, each answering something §12d had to assert:

    Ffold - Fstar   the **fold alone**, over rows the fan-in already
                    fetched. This is what AG3 added to a route that
                    existed; if it is small, the widening is nearly free
                    *given* the fetch.
    Ffold - Lfold   what the fan-in costs a fold, against the same fold on
                    a local walk.
    Ffold - Ship    **the routing rule's price.** Folding at the session
                    over every row, against folding on the owner over the
                    same rows and shipping one. This bounds what AG-M's
                    partial aggregates would be worth, because a partial
                    aggregate is exactly the owner-side fold with the
                    fan-in's stage count.
    Lfold - Lstar   the fold's cost with no wire at all — the control for
                    the first difference.

**A correctness check rides along, and it is exact.** The fixture loads
`spread` with `r*2+c` for r in [0, rows/2) and c in {0, 1}, and `twin` with
`r` for r in [0, rows) — the same multiset of `v`. So `SELECT SUM(v)` over
the two relations must return a **byte-identical** reply, over three
different routes: local walk, statement shipping, and a two-stage fan-in
folded at the session. A mismatch is a correctness finding and is reported
as one, not as a slow arm.

Usage:
    bench/fanin_fold_cost_probe.py --server build-release/kds_server \\
        --workdir ~/foldcost --reps 300
"""

import argparse
import hashlib
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, HERE)
from bench_common import Phase  # noqa: E402
from multicore_benchmark import (  # noqa: E402
    check_host, collect_connections, stop_server, wait_for_port,
)
# The rig, imported rather than restated: same server config, same load,
# same two relations. Two drivers with two copies of a fixture is two
# fixtures that agree today.
from self_directed_stage_probe import (  # noqa: E402
    SPREAD, TWIN, load, start_server,
)

STAR = "SELECT * FROM {t}"
FOLD = "SELECT SUM(v) FROM {t}"

# name -> (relation, core, statement, expected stages, what it is)
ARMS = (
    ("Lstar", TWIN, 0, STAR, 0, "local walk, whole rows"),
    ("Lfold", TWIN, 0, FOLD, 0, "local walk, folded locally"),
    ("Ship", TWIN, 1, FOLD, 0, "shipped as text, folded on the owner"),
    ("Fstar", SPREAD, 0, STAR, 2, "fan-in, whole rows"),
    ("Ffold", SPREAD, 0, FOLD, 2, "fan-in, folded at the session"),
)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--port", type=int, default=17900)
    ap.add_argument("--rows", type=int, default=600)
    ap.add_argument("--range-size-ids", type=int, default=512)
    ap.add_argument("--reps", type=int, default=300)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--json-out", default="")
    args = ap.parse_args()
    os.makedirs(args.workdir, exist_ok=True)
    check_host(args.workdir, args.force)

    tag = "fanin-fold"
    proc, stderr_path = start_server(args.server, args.workdir, tag, args.port,
                                     args.range_size_ids)
    out = {"arms": {}, "checks": {}}
    try:
        wait_for_port(args.port, stderr_path)
        conns, _ = collect_connections(args.port, {0: 1, 1: 1}, max_attempts=400)

        for rel in (SPREAD, TWIN):
            reply = conns[0][0].cmd(f"CREATE TABLE {rel} (id int64, v int64)")
            if reply.startswith("ERR"):
                raise RuntimeError(f"CREATE {rel}: {reply}")

        placed_spread, placed_twin = load(conns, args.rows)
        meta = conns[0][0].cmd("SHOW META")
        split_detail = ""
        if "split_relation_detail=" in meta:
            split_detail = meta.split("split_relation_detail=")[1].split()[0]
        out["placed_spread"] = placed_spread
        out["placed_twin"] = placed_twin
        out["split_relation_detail"] = split_detail or "(absent)"
        out["reps"] = args.reps

        # Rep-interleaved, the standing rule since RD9(a): one rep touches
        # every arm once, so drift arriving mid-run lands on all of them
        # rather than on whichever ran last.
        phases = {name: Phase(f"{name} {rel}@core{core}")
                  for name, rel, core, _, _, _ in ARMS}
        reply_hash = {name: set() for name, _, _, _, _, _ in ARMS}
        reply_text = {}
        errors = {name: [] for name, _, _, _, _, _ in ARMS}
        for _ in range(args.reps):
            for name, rel, core, sql, _, _ in ARMS:
                t0 = time.perf_counter()
                reply = conns[core][0].cmd(sql.format(t=rel))
                dt = time.perf_counter() - t0
                phases[name].record(dt, reply)
                if reply.startswith("ERR"):
                    if len(errors[name]) < 3:
                        errors[name].append(reply[:200])
                    continue
                reply_hash[name].add(hashlib.sha256(reply.encode()).hexdigest())
                reply_text.setdefault(name, reply[:120])
        for phase in phases.values():
            phase.elapsed = sum(phase.latencies)

        for name, rel, core, sql, stages, what in ARMS:
            s = phases[name].summary()
            s["p90_us"] = round(phases[name].percentile(90) * 1e6, 1)
            s["relation"] = rel
            s["core"] = core
            s["sql"] = sql.format(t=rel)
            s["expected_stages"] = stages
            s["what"] = what
            s["distinct_replies"] = len(reply_hash[name])
            s["reply_sample"] = reply_text.get(name, "")
            s["errors_sample"] = errors[name]
            out["arms"][name] = s

        # **The exact check.** The three fold arms read the same multiset
        # of `v` over three routes and must answer byte-identically.
        folds = ("Lfold", "Ship", "Ffold")
        hashes = [next(iter(reply_hash[n])) if len(reply_hash[n]) == 1 else None
                  for n in folds]
        out["checks"]["each_fold_arm_stable"] = {
            n: len(reply_hash[n]) for n in folds}
        out["checks"]["three_routes_byte_identical"] = (
            all(h is not None for h in hashes) and len(set(hashes)) == 1)
        out["checks"]["fold_reply"] = {n: reply_text.get(n, "") for n in folds}

        def p50(name):
            return out["arms"][name]["p50_us"]

        out["fold_alone_over_fanin_p50_us"] = round(p50("Ffold") - p50("Fstar"), 1)
        out["fanin_over_local_fold_p50_us"] = round(p50("Ffold") - p50("Lfold"), 1)
        out["session_fold_over_owner_fold_p50_us"] = round(p50("Ffold") - p50("Ship"), 1)
        out["fold_alone_local_p50_us"] = round(p50("Lfold") - p50("Lstar"), 1)

        print(f"\nplaced spread={placed_spread} twin={placed_twin} "
              f"split_relation_detail={out['split_relation_detail']} "
              f"reps={args.reps} range_size_ids={args.range_size_ids}")
        print(f"\n{'arm':<7}{'relation':<9}{'core':>5}{'stages':>7}{'ops':>6}"
              f"{'mean us':>10}{'p0':>9}{'p25':>9}{'p50':>9}{'p90':>9}"
              f"{'p99':>9}{'err':>5}  what")
        print("-" * 118)
        for name, rel, core, sql, stages, what in ARMS:
            s = out["arms"][name]
            print(f"{name:<7}{rel:<9}{core:>5}{stages:>7}{s['ops']:>6}"
                  f"{s['mean_us']:>10.1f}{s['p0_us']:>9.1f}{s['p25_us']:>9.1f}"
                  f"{s['p50_us']:>9.1f}{s['p90_us']:>9.1f}{s['p99_us']:>9.1f}"
                  f"{s['errors']:>5}  {what}")
        print(f"\nFfold - Fstar (the fold alone, rows already fetched), p50: "
              f"{out['fold_alone_over_fanin_p50_us']:>9.1f} us")
        print(f"Lfold - Lstar (the fold alone, no wire at all),       p50: "
              f"{out['fold_alone_local_p50_us']:>9.1f} us")
        print(f"Ffold - Lfold (what the fan-in costs a fold),         p50: "
              f"{out['fanin_over_local_fold_p50_us']:>9.1f} us")
        print(f"Ffold - Ship  (the routing rule's price),             p50: "
              f"{out['session_fold_over_owner_fold_p50_us']:>9.1f} us")
        print(f"\nthree fold routes byte-identical: "
              f"{out['checks']['three_routes_byte_identical']}   "
              f"replies: {out['checks']['fold_reply']}")
        for name, _, _, _, _, _ in ARMS:
            if out["arms"][name]["errors"]:
                print(f"  {name}: {out['arms'][name]['errors']} errors, "
                      f"sample: {out['arms'][name]['errors_sample']}")
    finally:
        stop_server(args.port)
        proc.wait(timeout=30)

    print()
    print(json.dumps(out, indent=2))
    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump(out, f, indent=2)
        print(f"  wrote {args.json_out}")


if __name__ == "__main__":
    main()
