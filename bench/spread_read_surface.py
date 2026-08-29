#!/usr/bin/env python3
"""R4-R / RR3 — what a spread relation can be read as, from which core.

R4-M measured this surface and found it was one shape from one core
(`bench/v2.6.0/results-k-sweep-and-read-ceiling-v2.4.0-52-g5b37fec.md`
§6a). RR1 and RR2 changed it, and **CR2 asks for the new surface stated
the same way: by enumeration, with each shape either reachable or refused
with its reason** — not claimed.

So this driver spreads one relation across k cores, then asks every shape
from **every** core's session and prints the grid. It also asks the same
shapes of an *unspread* twin on the same server, because a shape refused
on both is refused for a reason that has nothing to do with ranges and
saying so is the difference between a surface and a list of errors.

The shapes are IM0's nine and H5's four, the equivalence suite's own
inventory (`tests/range_chain_test.cpp`), so a shape that answers here is
a shape that suite has a byte-identical claim about.

Usage:
    bench/spread_read_surface.py --server build-release/kds_server \\
        --workdir ~/surface --cores 4 --placement creating
"""

import argparse
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from multicore_benchmark import (  # noqa: E402
    check_host, collect_connections, field, stop_server, wait_for_port,
)

SPREAD, WHOLE = "spread", "whole"

# Every shape the equivalence suite makes a claim about, plus the three
# whole-relation shapes a client actually issues. `{t}` is the relation.
SHAPES = [
    ("star", "SELECT * FROM {t}"),
    ("star+where-pk", "SELECT * FROM {t} WHERE id = 1"),
    ("star+where-nonpk", "SELECT * FROM {t} WHERE v = 1"),
    ("star+between", "SELECT * FROM {t} WHERE id BETWEEN 1 AND 100000"),
    ("star+order-pk-asc", "SELECT * FROM {t} ORDER BY id ASC"),
    ("star+order-pk-desc", "SELECT * FROM {t} ORDER BY id DESC"),
    ("star+order-nonpk", "SELECT * FROM {t} ORDER BY v ASC"),
    ("projection", "SELECT id FROM {t}"),
    ("projection-multi", "SELECT v, id FROM {t}"),
    ("limit", "SELECT * FROM {t} LIMIT 2"),
    ("limit+offset", "SELECT * FROM {t} LIMIT 2 OFFSET 2"),
    ("order+limit", "SELECT * FROM {t} ORDER BY id ASC LIMIT 3"),
    ("count", "SELECT COUNT(*) FROM {t}"),
    ("sum", "SELECT SUM(v) FROM {t}"),
    ("count-distinct", "SELECT COUNT(DISTINCT v) FROM {t}"),
    ("group-by", "SELECT v, COUNT(*) FROM {t} GROUP BY v"),
]


def classify(reply):
    """`ok`, or a short tag for why it was refused. The tags are the
    engine's own distinctions and not this driver's: which refusal arrives
    is the finding, because *"cannot fan in"* means the route declined the
    shape and anything else means the shape never reached the route."""
    if not reply.startswith("ERR"):
        return "ok"
    if "cannot fan in over them" in reply:
        return "no-route"
    if "stages, above the fan-in ceiling" in reply:
        return "ceiling"
    if "UNKNOWN_OUTCOME" in reply:
        return "reply-lost"
    if "writes are bound to core" in reply or "owned by core" in reply:
        return "affinity"
    return "other"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--cores", type=int, default=4)
    ap.add_argument("--range-size-ids", type=int, default=512)
    ap.add_argument("--rounds", type=int, default=300, help="rows per core")
    ap.add_argument("--placement", default="creating", choices=("creating", "rotate"))
    ap.add_argument("--port", type=int, default=16800)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    os.makedirs(args.workdir, exist_ok=True)
    check_host(args.workdir, args.force)

    tag = f"surface-{args.placement}-k{args.cores}"
    conf = os.path.join(args.workdir, f"{tag}.conf")
    stderr_path = os.path.join(args.workdir, f"{tag}.stderr")
    with open(conf, "w") as f:
        f.write(f"data_file = {args.workdir}/{tag}.db\nport = {args.port}\n"
                f"cores = {args.cores}\nplacement = {args.placement}\n"
                "peer_listeners = on\n"
                f"range_size_ids = {args.range_size_ids}\ndurability = relaxed\n"
                f"log_dir = {args.workdir}\nlog_file = {tag}.log\nlog_level = warn\n")
    subprocess.run(["rm", "-rf", f"{args.workdir}/{tag}.db",
                    f"{args.workdir}/{tag}.db.wal"], check=False)
    with open(stderr_path, "w") as err:
        proc = subprocess.Popen([args.server, "--config", conf], stdout=err,
                                stderr=subprocess.STDOUT)
    out = {}
    try:
        wait_for_port(args.port, stderr_path)
        conns, _ = collect_connections(args.port, {c: 1 for c in range(args.cores)},
                                       max_attempts=200 * args.cores)
        for rel in (SPREAD, WHOLE):
            reply = conns[0][0].cmd(f"CREATE TABLE {rel} (id int64, v int64)")
            if reply.startswith("ERR"):
                raise RuntimeError(f"CREATE {rel}: {reply}")

        # `spread` is written from every core, so it takes a range per
        # peer; `whole` is written from core 0 alone, so it never does.
        # Same rows, so a shape refused on one and not the other is the
        # split doing it.
        placed = 0
        for r in range(args.rounds):
            for c in range(args.cores):
                for _ in range(300):
                    if not conns[c][0].cmd(
                            f"INSERT INTO {SPREAD} VALUES ({r * args.cores + c})"
                    ).startswith("ERR"):
                        placed += 1
                        break
                    time.sleep(0.002)
                conns[0][0].cmd(f"INSERT INTO {WHOLE} VALUES ({r * args.cores + c})")
        meta = conns[0][0].cmd("SHOW META")
        split_detail = ""
        if "split_relation_detail=" in meta:
            split_detail = meta.split("split_relation_detail=")[1].split()[0]

        for name, sql in SHAPES:
            row = {"shape": name, "sql": sql.format(t="<rel>"), "cores": {}}
            for c in range(args.cores):
                spread_reply = conns[c][0].cmd(sql.format(t=SPREAD))
                whole_reply = conns[c][0].cmd(sql.format(t=WHOLE))
                row["cores"][c] = {
                    "spread": classify(spread_reply),
                    "whole": classify(whole_reply),
                    "detail": spread_reply[:150] if spread_reply.startswith("ERR") else "",
                }
            out[name] = row

        print(f"\nplaced={placed} placement={args.placement} cores={args.cores} "
              f"range_size_ids={args.range_size_ids}")
        print(f"split_relation_detail={split_detail or '(absent)'}\n")
        # **The control says *why*, not *whether the rows match*.** The two
        # relations cannot have identical ids - `spread`'s come from
        # per-core leased blocks and `whole`'s from core 0's dense mark -
        # so a row-level comparison here would report a difference that is
        # the id allocator's and not the split's. Row equivalence is
        # `CoreRuntimeTest.ACoreReadsARelationItOwnsButDoesNotWhollyHold`'s,
        # where the fixture places identical ids on both sides. What this
        # column adds is the distinction that matters for a *surface*: a
        # shape refused on the unsplit twin too is refused for a reason
        # that has nothing to do with ranges.
        head = "  ".join(f"c{c}" for c in range(args.cores))
        print(f"{'shape':<20} {'spread: ' + head:<28} {'whole (control): ' + head:<28} verdict")
        print("-" * 100)
        for name, _ in SHAPES:
            row = out[name]
            s = "  ".join(f"{row['cores'][c]['spread']:<4}"[:4] for c in range(args.cores))
            w = "  ".join(f"{row['cores'][c]['whole']:<4}"[:4] for c in range(args.cores))
            spread_ok = [row["cores"][c]["spread"] == "ok" for c in range(args.cores)]
            # **The verdict is read off core 0**, because that is the one
            # core where the control is clean: on a peer, an unsplit
            # core-0-owned relation is reached by *statement shipping*, and
            # a large reply is lost there (`reply-lost` below) for reasons
            # that are neither the shape's nor the split's. Judging the
            # split by a control that is itself failing would have called
            # three shapes "not the split's" that are exactly the split's.
            if all(spread_ok):
                verdict = "reachable everywhere"
            elif any(spread_ok):
                verdict = "reachable on some cores"
            elif row["cores"][0]["whole"] == "ok":
                verdict = "refused BY THE SPLIT"
            else:
                verdict = "refused, and not by the split"
            print(f"{name:<20} {s:<28} {w:<28} {verdict}")
        lost = sorted({name for name, _ in SHAPES
                       for c in range(1, args.cores)
                       if out[name]["cores"][c]["whole"] == "reply-lost"})
        if lost:
            print(f"\n**Separately**: on a peer, these shapes lose their reply against the "
                  f"*unsplit* control and answer UNKNOWN_OUTCOME - statement shipping, not "
                  f"ranges: {', '.join(lost)}")
    finally:
        stop_server(args.port)
        proc.wait(timeout=30)
    print()
    print(json.dumps(out, indent=2))


if __name__ == "__main__":
    main()
