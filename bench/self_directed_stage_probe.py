#!/usr/bin/env python3
"""S-a (CS1) — what a self-directed stage costs, against a real remote hop
and against no stage at all.

`workplan-insert-spreading.md` §10b decided a self-directed run gets **no**
message-free path — the ring hop is a send to itself, going through the
same STEP_OPEN/STEP_BATCH protocol a remote stage does — on the argument
that a second spelling of the fan-in protocol is a second place to forget
one of them (RB4's lesson, §10b). The number that would justify revisiting
that decision is what a self-send costs against a genuine remote hop and
against no stage at all, and this driver measures exactly that at the
smallest shape that still separates all three.

One server, `cores = 2`. Two relations, equal row counts:

- `spread` — written round-robin from **both** cores' sessions, so under
  `placement = creating` it keeps its anchor range `[0, X)` on core 0
  (CC9) and takes a second range, on core 1. `ServableBy(core)` is false
  on both cores for a relation with two owners (`schema.hpp`), so every
  `SELECT *` against it takes the fan-in route regardless of which core
  asks.
- `twin` — written from core 0's session alone, so it never splits;
  `ServableBy(0)` is true and a core-0 read never leaves the local walk.

Four arms, `SELECT * FROM <relation>` from a session pinned to a chosen
core (`tools/multicore_benchmark.collect_connections` is the only way to
choose a core under `SO_REUSEPORT`):

    B  twin   from core 0   0 stages — ServableBy(0), a plain local walk
    A  twin   from core 1   1 stage,  remote to core 0
    C  spread from core 1   2 stages — one remote (core 0's anchor
                             range), one self-directed (core 1 asking
                             itself for its own range)
    D  spread from core 0   2 stages — the same two stages, opposite roles

    A - B   what one *remote* stage costs over no stage at all.
    C - A   what a *self-directed* stage adds on top of one remote stage.

If `C - A` is close to `A - B`, a self-send costs about what a real hop
costs and §10b's decision has a price worth revisiting; if it is much
smaller, the self-send is close to free. This driver reports the two
differences and leaves the reading to the results file — it measures, it
does not decide.

**Correctness checks riding along for free**: `A` and `B` read the same
relation over two routes and must return byte-identical replies; so must
`C` and `D`, since the fan-in's stage order is `lo`-ascending and does not
depend on which core opened it. A mismatch here is a correctness finding,
not a performance one, and is reported as such.

Usage:
    bench/self_directed_stage_probe.py --server build-release/kds_server \\
        --workdir ~/selfdirected --reps 300
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from bench_common import Phase  # noqa: E402
from multicore_benchmark import (  # noqa: E402
    check_host, collect_connections, field, stop_server, wait_for_port,
)

SPREAD, TWIN = "spread", "twin"
# name -> (relation, core the session must be pinned to, expected stages)
ARMS = (
    ("B", TWIN, 0, 0),
    ("A", TWIN, 1, 1),
    ("C", SPREAD, 1, 2),
    ("D", SPREAD, 0, 2),
)


def start_server(binary, workdir, tag, port, range_size_ids):
    """One server: `cores = 2`, `placement = creating`, `peer_listeners =
    on`, `durability = relaxed` — the rig CS1 asks for, the 2-core
    equivalent of the 4-core original (`spread-realation.md`): the anchor
    range stays on core 0 and the rest lands on core 1, which is enough to
    separate all three arms."""
    conf = os.path.join(workdir, f"{tag}.conf")
    data = os.path.join(workdir, f"{tag}.db")
    stderr_path = os.path.join(workdir, f"{tag}.stderr")
    with open(conf, "w") as f:
        f.write(
            f"data_file = {data}\nport = {port}\ncores = 2\n"
            "placement = creating\npeer_listeners = on\n"
            f"range_size_ids = {range_size_ids}\ndurability = relaxed\n"
            f"log_file = {tag}.log\nlog_dir = {workdir}\nlog_level = warn\n")
    subprocess.run(["rm", "-rf", data, data + ".wal"], check=False)
    with open(stderr_path, "w") as err:
        proc = subprocess.Popen([binary, "--config", conf], stdout=err,
                                stderr=subprocess.STDOUT)
    wait_for_port(port, stderr_path)
    return proc, stderr_path


def insert_retrying(conn, sql, max_attempts=400, sleep_s=0.002):
    """One INSERT, retried while the engine's answer is the transient pump
    refusal a foreign core's first write into a relation it holds no range
    for costs (R4/IS1 — the demand is recorded on the refusal and the
    drain tick turns it into a range). Same idiom as
    `bench/spread_read_surface.py`'s load loop."""
    last = ""
    for _ in range(max_attempts):
        reply = conn.cmd(sql)
        if not reply.startswith("ERR"):
            return reply
        last = reply
        time.sleep(sleep_s)
    raise RuntimeError(f"insert never landed after {max_attempts} attempts: {last[:200]}")


def load(conns, rows):
    """`spread` round-robin from both cores' sessions (300 + 300 by
    default); `twin` from core 0 alone. Both omit the pk, which is what
    makes the id the engine's own high-water mark or lease block — the
    condition R4's routing (`command_dispatcher.cpp`'s `heap_omitting_pk`)
    keys on."""
    rows_per_core = rows // 2
    placed_spread = 0
    for r in range(rows_per_core):
        for c in (0, 1):
            insert_retrying(conns[c][0], f"INSERT INTO {SPREAD} VALUES ({r * 2 + c})")
            placed_spread += 1
    placed_twin = 0
    for r in range(rows):
        insert_retrying(conns[0][0], f"INSERT INTO {TWIN} VALUES ({r})")
        placed_twin += 1
    return placed_spread, placed_twin


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--port", type=int, default=17700)
    ap.add_argument("--rows", type=int, default=600,
                    help="total rows per relation (spread: split evenly over "
                         "both cores; twin: all from core 0)")
    ap.add_argument("--range-size-ids", type=int, default=512)
    ap.add_argument("--reps", type=int, default=300,
                    help="rep-interleaved repetitions of the four arms")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--json-out", default="")
    args = ap.parse_args()
    os.makedirs(args.workdir, exist_ok=True)
    check_host(args.workdir, args.force)

    tag = "self-directed"
    proc, stderr_path = start_server(args.server, args.workdir, tag, args.port,
                                     args.range_size_ids)
    out = {"arms": {}, "checks": {}}
    try:
        wait_for_port(args.port, stderr_path)
        conns, connect_attempts = collect_connections(
            args.port, {0: 1, 1: 1}, max_attempts=400)

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

        # **Rep-interleaved** (the standing rule since RD9(a)): one rep
        # touches all four arms once, so drift arriving mid-run — a page
        # fault, a GC pause, the kernel scheduling the other client
        # process — lands on every arm rather than on whichever ran last.
        phases = {name: Phase(f"{name} {rel}@core{core}")
                  for name, rel, core, _ in ARMS}
        rows_returned = {name: set() for name, _, _, _ in ARMS}
        reply_hash = {name: [] for name, _, _, _ in ARMS}
        errors = {name: [] for name, _, _, _ in ARMS}
        for rep in range(args.reps):
            for name, rel, core, _ in ARMS:
                t0 = time.perf_counter()
                reply = conns[core][0].cmd(f"SELECT * FROM {rel}")
                dt = time.perf_counter() - t0
                phases[name].record(dt, reply)
                if reply.startswith("ERR"):
                    if len(errors[name]) < 3:
                        errors[name].append(reply[:200])
                    continue
                rows_returned[name].add(reply.count("\\n"))
                reply_hash[name].append(hashlib.sha256(reply.encode()).hexdigest())
        for phase in phases.values():
            phase.elapsed = sum(phase.latencies)

        out["arms"] = {}
        for name, rel, core, expect_stages in ARMS:
            s = phases[name].summary()
            s["p90_us"] = round(phases[name].percentile(90) * 1e6, 1)
            s["relation"] = rel
            s["core"] = core
            s["expected_stages"] = expect_stages
            s["rows_returned"] = sorted(rows_returned[name])
            s["errors_sample"] = errors[name]
            out["arms"][name] = s

        # **The correctness check.** A and B read `twin` over two routes
        # (remote fan-in, local walk); C and D read `spread` over the same
        # two stages in the opposite roles. Both pairs must be
        # byte-identical, because the fan-in's concatenation order is
        # `lo`-ascending and does not depend on who opened it
        # (`command_dispatcher.cpp`'s stage-building comment).
        def agree(x, y):
            hx, hy = set(reply_hash[x]), set(reply_hash[y])
            return len(hx) == 1 and hx == hy

        out["checks"]["A_B_byte_identical"] = agree("A", "B")
        out["checks"]["C_D_byte_identical"] = agree("C", "D")
        out["checks"]["A_distinct_replies"] = len(set(reply_hash["A"]))
        out["checks"]["C_distinct_replies"] = len(set(reply_hash["C"]))

        a_p50 = out["arms"]["A"]["p50_us"]
        b_p50 = out["arms"]["B"]["p50_us"]
        c_p50 = out["arms"]["C"]["p50_us"]
        d_p50 = out["arms"]["D"]["p50_us"]
        out["remote_over_none_p50_us"] = round(a_p50 - b_p50, 1)
        out["self_directed_over_remote_p50_us"] = round(c_p50 - a_p50, 1)
        out["c_d_gap_p50_us"] = round(c_p50 - d_p50, 1)
        a_mean = out["arms"]["A"]["mean_us"]
        b_mean = out["arms"]["B"]["mean_us"]
        c_mean = out["arms"]["C"]["mean_us"]
        out["remote_over_none_mean_us"] = round(a_mean - b_mean, 1)
        out["self_directed_over_remote_mean_us"] = round(c_mean - a_mean, 1)

        print(f"\nplaced spread={placed_spread} twin={placed_twin} "
              f"split_relation_detail={out['split_relation_detail']} "
              f"reps={args.reps} range_size_ids={args.range_size_ids}")
        print(f"\n{'arm':<4}{'relation':<10}{'core':>5}{'stages':>8}{'ops':>7}"
              f"{'rows':>7}{'mean us':>10}{'p0':>9}{'p25':>9}{'p50':>9}"
              f"{'p90':>9}{'p99':>9}{'err':>5}")
        print("-" * 100)
        for name, rel, core, expect_stages in ARMS:
            s = out["arms"][name]
            rows = "/".join(str(r) for r in s["rows_returned"]) or "-"
            print(f"{name:<4}{rel:<10}{core:>5}{expect_stages:>8}{s['ops']:>7}"
                  f"{rows:>7}{s['mean_us']:>10.1f}{s['p0_us']:>9.1f}"
                  f"{s['p25_us']:>9.1f}{s['p50_us']:>9.1f}{s['p90_us']:>9.1f}"
                  f"{s['p99_us']:>9.1f}{s['errors']:>5}")
        print(f"\nA - B (remote over none),      p50: {out['remote_over_none_p50_us']:>8.1f} us"
              f"   mean: {out['remote_over_none_mean_us']:>8.1f} us")
        print(f"C - A (self-directed over remote), p50: "
              f"{out['self_directed_over_remote_p50_us']:>8.1f} us"
              f"   mean: {out['self_directed_over_remote_mean_us']:>8.1f} us")
        print(f"C - D (same two stages, opposite roles), p50: "
              f"{out['c_d_gap_p50_us']:>8.1f} us")
        print(f"\nA/B byte-identical: {out['checks']['A_B_byte_identical']}   "
              f"C/D byte-identical: {out['checks']['C_D_byte_identical']}")
        for name in ("A", "B", "C", "D"):
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
