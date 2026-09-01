#!/usr/bin/env python3
"""Interleaved A/B for OPT-004 (`DecodeRowInto`'s preconditions through
`CheckDecodeInputs`, the same fix AP02 already gave its sibling
`DecodeColumnsInto`).

Two pairs share this one driver (different `--port-a/--port-b/--pid-a/--pid-b`
per invocation, orchestrated by run_pair.sh):

  Pair 1 - the change in isolation, on the tree it was written against:
    A = 55d2c0b (v2.7.0-30-g55d2c0b, OPT-002 present, OPT-004 absent)
    B = b05925f (v2.7.0-31-gb05925f, OPT-004 added - `git diff --stat` is
        one file, src/exec/row_codec.cpp, +8/-17)
    This pair predates OPT-001, so `update` still decodes every scanned row
    twice (DecodeRow *and* DecodeRowInto, matched or not) - the shape where
    DecodeRowInto's call volume is largest.

  Pair 2 - what OPT-004 is worth today, after OPT-001 cut UPDATE's call
  volume to "once per scanned row for the mask, once more only for a match":
    A = ea1d9d0 with DecodeRowInto's CheckDecodeInputs call reverted to the
        pre-OPT-004 two-checker form (see CIP/OPT-004-.../results-*.md for
        exactly what was edited and why) - a *modified* tree, never a bare
        commit
    B = ea1d9d0 unmodified (v2.7.0-38-gea1d9d0)

Four shapes, chosen because they are the ones that actually reach
`DecodeRowInto` (the kAllColumns/whole-row path - `DecodeAndResolve` calls
`DecodeColumnsInto` instead whenever a step's `filter_columns` narrows past
kAllColumns, which is what OPT-001 gave a plain point UPDATE):

    full-scan    SELECT * FROM t_main                    render+wire-bound;
                 OPT-002's own run showed this shape cannot see a decode
                 saving (96.9% of its latency at 10,000 rows is render/wire/
                 client-parse) - included as the control that should stay
                 flat, not as the instrument.
    analyze-scan ANALYZE SELECT * FROM t_main             decode-only: same
                 compiled chain as full-scan, no render, no wire payload -
                 this is the real instrument for a decode-path fix.
    update       UPDATE t_main SET c_int=x WHERE id=pid   pair 1: DecodeRowInto
                 runs 2x per scanned row, matched or not (no OPT-001 yet).
                 pair 2: DecodeRowInto runs for the masked pre-check only
                 when filter_columns is still kAllColumns, and OPT-001 masks
                 a point UPDATE down to just the WHERE's own column - so
                 DecodeRowInto now runs on at most the ~1 matched row per
                 statement, not the whole scanned prefix. Predicted near-zero
                 here in pair 2, which is itself the finding this pair is
                 for.
    subchain     UPDATE t_main SET c_int=x WHERE id IN
                     (SELECT target_id FROM t_pool WHERE target_id=target)
                 any subquery forces filter_columns=kAllColumns
                 (step_compiler.cpp's CompileWhere, `maskable = sub_chains
                 .empty() && ...`) - so DecodeRowInto keeps running once per
                 scanned row in *both* pairs, matched or not. This is the
                 shape OPT-001's own run named as still paying the full
                 per-row decode cost regardless of OPT-001 being present.

Methodology is opt001_ab.py's, reused near-verbatim (same role, same
harness): one RNG stream drives an identical statement sequence for both
arms; a latency pass times each statement on the wire
(bench_common.Phase); a CPU pass brackets larger contiguous blocks with
/proc/<pid>/stat (utime+stime, 10ms ticks) because a fix this small needs
volume to clear tick-quantization noise; both passes alternate which arm
goes first each round. The noise floor is arm A's own pooled samples split
at the run's midpoint, same table, same shape, same server - not a second
"repeat" run, because a repeat run is not what the floor is *for*: it has
to be measurable from data this run already produced, on the arm that
cannot possibly show the effect being hunted (an already-fixed vs.
not-yet-fixed comparison would leak the comparison into its own floor).

id assignment: every INSERT in this driver omits the pk, so ids
auto-assign 1..N in insertion order (invariant 11).

Correctness: after the run, `SELECT * FROM t_main ORDER BY id` (sha256 of
the reply text) and `ANALYZE SELECT * FROM t_main` (sha256 of the *whole*
reply, plan included - deterministic, since `plan_printer.cpp`'s per-step
stats carry no wall-clock field) are compared between arms. Both arms are
driven by the literally identical statement sequence, so exact equality is
the correctness bar for both - the ANALYZE hash is what proves
`examined=`/`pages=`/`opens=`/`pattern_id=` didn't move, without needing to
parse each field out by hand.
"""
import argparse
import hashlib
import json
import os
import random
import sys
import time

sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/path-optimizer/tools")
from ckdbs_cli import ServerConnection, format_reply  # noqa: E402
from bench_common import Phase  # noqa: E402

MAIN_COLUMNS = "id int64, c_int int64, c_small int32, c_flag bool, c_text varchar"
TEXT_LEN = 16
SMALL_RANGE = 30000
POOL_SIZE = 5  # t_pool row count, tiny so the sub-chain's per-outer-row
               # re-evaluation stays affordable at N=10000

SHAPES = ["full-scan", "analyze-scan", "update", "subchain"]


class Side:
    def __init__(self, label, host, port, pid, timeout=180.0):
        self.label = label
        self.host = host
        self.port = port
        self.pid = pid
        self.conn = ServerConnection(host, port, timeout=timeout)
        self.errors = 0
        self.first_error = None

    def __call__(self, cmd):
        reply = format_reply(self.conn.send_command(cmd))
        if reply.startswith("ERR"):
            self.errors += 1
            if self.first_error is None:
                self.first_error = f"{cmd} -> {reply}"
        return reply

    def must(self, cmd):
        reply = self(cmd)
        if reply.startswith("ERR"):
            print(f"FATAL[{self.label}]: {cmd} -> {reply}", file=sys.stderr)
            sys.exit(1)
        return reply

    def cpu_seconds(self):
        """utime + stime of the server process, in seconds (proc(5) fields
        14+15 - split on ') ' drops the 'pid (comm)' prefix)."""
        if self.pid is None:
            return None
        try:
            with open(f"/proc/{self.pid}/stat") as f:
                fields = f.read().rsplit(") ", 1)[1].split()
        except OSError:
            return None
        ticks = int(fields[11]) + int(fields[12])
        return ticks / os.sysconf("SC_CLK_TCK")

    def close(self):
        self.conn.close()


def ordered(sides, i):
    return sides if i % 2 == 0 else list(reversed(sides))


def setup(side, rows, seed):
    side.must(f"CREATE TABLE t_main ({MAIN_COLUMNS})")
    side.must("CREATE TABLE t_pool (id int64, target_id int64)")

    rng = random.Random(seed)

    def rand_row():
        text = "".join(rng.choices("abcdefghijklmnopqrstuvwxyz", k=TEXT_LEN))
        return (f"{rng.randint(0, 1_000_000)}, {rng.randint(0, SMALL_RANGE)}, "
                f"{rng.randint(0, 1)}, '{text}'")

    first_reply = None
    for i in range(rows):
        reply = side.must(f"INSERT INTO t_main VALUES ({rand_row()})")
        if first_reply is None:
            first_reply = reply

    pool_targets = [max(1, round(rows * (k + 1) / (POOL_SIZE + 1))) for k in range(POOL_SIZE)]
    for t in pool_targets:
        side.must(f"INSERT INTO t_pool VALUES ({t})")

    return pool_targets, first_reply


def checksum(side, sql):
    reply = side.must(sql)
    return hashlib.sha256(reply.encode()).hexdigest(), reply


def correctness_check(sides, label):
    out = {"label": label, "ok": True, "checks": []}
    for name, sql in (
        ("t_main", "SELECT * FROM t_main ORDER BY id"),
        ("t_main_count", "SELECT COUNT(*) FROM t_main"),
        ("analyze_full", "ANALYZE SELECT * FROM t_main"),
    ):
        hashes = {}
        replies = {}
        for s in sides:
            h, r = checksum(s, sql)
            hashes[s.label] = h
            replies[s.label] = r
        match = len(set(hashes.values())) == 1
        entry = {"check": name, "hashes": hashes, "match": match}
        if not match:
            entry["replies"] = replies
        out["checks"].append(entry)
        out["ok"] = out["ok"] and match
    return out


def gen_round(rng, rows, pool_targets, n_full, n_analyze, n_update, n_sub):
    return {
        "full-scan": [None for _ in range(n_full)],
        "analyze-scan": [None for _ in range(n_analyze)],
        "update": [(rng.randint(1, rows), rng.randint(0, 1_000_000)) for _ in range(n_update)],
        "subchain": [(rng.choice(pool_targets), rng.randint(0, 1_000_000)) for _ in range(n_sub)],
    }


def run_one(side, shape, item):
    if shape == "full-scan":
        return side("SELECT * FROM t_main")
    if shape == "analyze-scan":
        return side("ANALYZE SELECT * FROM t_main")
    if shape == "update":
        pid, val = item
        return side(f"UPDATE t_main SET c_int = {val} WHERE id = {pid}")
    if shape == "subchain":
        target, val = item
        return side(f"UPDATE t_main SET c_int = {val} WHERE id IN "
                    f"(SELECT target_id FROM t_pool WHERE target_id = {target})")
    raise ValueError(shape)


def run_shape_block(side, phase, shape, payload):
    for item in payload:
        t0 = time.perf_counter()
        r = run_one(side, shape, item)
        phase.record(time.perf_counter() - t0, r)


def run_shape_cpu(side, shape, payload):
    for item in payload:
        run_one(side, shape, item)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port-a", type=int, required=True)
    ap.add_argument("--port-b", type=int, required=True)
    ap.add_argument("--pid-a", type=int, required=True)
    ap.add_argument("--pid-b", type=int, required=True)
    ap.add_argument("--rows", type=int, required=True)
    ap.add_argument("--seed", type=int, default=20260901)
    ap.add_argument("--json", required=True)
    ap.add_argument("--lat-rounds", type=int, default=15)
    ap.add_argument("--cpu-rounds", type=int, default=20)
    for shape in SHAPES:
        ap.add_argument(f"--lat-{shape}", type=int, required=True)
        ap.add_argument(f"--cpu-{shape}", type=int, required=True)
    args = ap.parse_args()

    sideA = Side("A", args.host, args.port_a, args.pid_a)
    sideB = Side("B", args.host, args.port_b, args.pid_b)
    sides = [sideA, sideB]

    lat_n = {shape: getattr(args, f"lat_{shape.replace('-', '_')}") for shape in SHAPES}
    cpu_n = {shape: getattr(args, f"cpu_{shape.replace('-', '_')}") for shape in SHAPES}

    print(f"[setup] rows={args.rows} seed={args.seed}")
    t0 = time.perf_counter()
    pool_targets_a, first_a = setup(sideA, args.rows, args.seed)
    pool_targets_b, first_b = setup(sideB, args.rows, args.seed)
    assert pool_targets_a == pool_targets_b
    print(f"[setup] done in {time.perf_counter()-t0:.2f}s; first insert reply "
          f"A={first_a!r} B={first_b!r}")

    pre_check = correctness_check(sides, "post-load")
    print(f"[correctness] post-load ok={pre_check['ok']}")
    if not pre_check["ok"]:
        print("FATAL: arms disagree immediately after identical load", file=sys.stderr)
        print(json.dumps(pre_check, indent=2), file=sys.stderr)
        sys.exit(1)

    rng = random.Random(args.seed + 1)

    lat_rounds = [gen_round(rng, args.rows, pool_targets_a, lat_n["full-scan"], lat_n["analyze-scan"],
                             lat_n["update"], lat_n["subchain"])
                  for _ in range(args.lat_rounds)]
    cpu_rounds = [gen_round(rng, args.rows, pool_targets_a, cpu_n["full-scan"], cpu_n["analyze-scan"],
                             cpu_n["update"], cpu_n["subchain"])
                  for _ in range(args.cpu_rounds)]

    # ---- latency pass -------------------------------------------------
    phases = {(tag, shape): Phase(f"{tag}-{shape}") for tag in ("A", "B") for shape in SHAPES}
    lat_t0 = time.perf_counter()
    for r, round_payload in enumerate(lat_rounds):
        for side in ordered(sides, r):
            for shape in SHAPES:
                run_shape_block(side, phases[(side.label, shape)], shape, round_payload[shape])
    print(f"[latency pass] {args.lat_rounds} rounds done in {time.perf_counter()-lat_t0:.2f}s")

    for p in phases.values():
        p.elapsed = sum(p.latencies) if p.latencies else 0.0

    floor_phases = {}
    for shape in SHAPES:
        lat = phases[("A", shape)].latencies
        mid = len(lat) // 2
        p1, p2 = Phase(f"Afloor1-{shape}"), Phase(f"Afloor2-{shape}")
        for x in lat[:mid]:
            p1.record(x, "OK")
        for x in lat[mid:]:
            p2.record(x, "OK")
        p1.elapsed = sum(p1.latencies) if p1.latencies else 0.0
        p2.elapsed = sum(p2.latencies) if p2.latencies else 0.0
        floor_phases[("Afloor1", shape)] = p1
        floor_phases[("Afloor2", shape)] = p2

    # ---- CPU pass -------------------------------------------------------
    cpu = {}
    cpu_per_round = {}
    cpu_t0 = time.perf_counter()
    for r, round_payload in enumerate(cpu_rounds):
        for side in ordered(sides, r):
            for shape in SHAPES:
                payload = round_payload[shape]
                if not payload:
                    continue
                before = side.cpu_seconds()
                run_shape_cpu(side, shape, payload)
                after = side.cpu_seconds()
                if before is not None and after is not None:
                    key = (side.label, shape)
                    tot_s, tot_ops = cpu.get(key, (0.0, 0))
                    cpu[key] = (tot_s + (after - before), tot_ops + len(payload))
                    cpu_per_round[(side.label, shape, r)] = (after - before, len(payload))
    print(f"[cpu pass] {args.cpu_rounds} rounds done in {time.perf_counter()-cpu_t0:.2f}s")

    cpu_floor = {}
    half = max(1, args.cpu_rounds // 2)
    for shape in SHAPES:
        for tagname, rrange in (("Afloor1", range(0, half)), ("Afloor2", range(half, args.cpu_rounds))):
            tot_s = sum(cpu_per_round.get(("A", shape, r), (0.0, 0))[0] for r in rrange)
            tot_ops = sum(cpu_per_round.get(("A", shape, r), (0.0, 0))[1] for r in rrange)
            cpu_floor[(tagname, shape)] = (tot_s, tot_ops)

    post_check = correctness_check(sides, "post-writes")
    print(f"[correctness] post-writes ok={post_check['ok']}")

    out = {
        "rows": args.rows,
        "seed": args.seed,
        "lat_rounds": args.lat_rounds, "cpu_rounds": args.cpu_rounds,
        "lat_n": lat_n, "cpu_n": cpu_n,
        "pool_targets": pool_targets_a,
        "pre_check": pre_check, "post_check": post_check,
        "phases": {f"{tag}:{shape}": p.summary() for (tag, shape), p in phases.items()},
        "floor": {f"{tag}:{shape}": p.summary() for (tag, shape), p in floor_phases.items()},
        "cpu": {f"{label}:{shape}": {"cpu_s": s, "ops": n, "us_per_op": (s / n * 1e6) if n else 0.0}
                for (label, shape), (s, n) in cpu.items()},
        "cpu_floor": {f"{tag}:{shape}": {"cpu_s": s, "ops": n, "us_per_op": (s / n * 1e6) if n else 0.0}
                      for (tag, shape), (s, n) in cpu_floor.items()},
        "errors": {s.label: {"count": s.errors, "first": s.first_error} for s in sides},
    }
    with open(args.json, "w") as f:
        json.dump(out, f, indent=2)
    print(f"wrote {args.json}")

    print()
    print(f"{'arm:shape':<20}{'ops':>7}{'qps':>9}{'mean us':>10}{'p0':>8}{'p25':>8}"
          f"{'p50':>8}{'p95':>8}{'p99':>9}{'err':>5}")
    for key, p in list(phases.items()) + list(floor_phases.items()):
        s = p.summary()
        print(f"{s['phase']:<20}{s['ops']:>7}{s['qps']:>9,.0f}{s['mean_us']:>10.1f}"
              f"{s['p0_us']:>8.1f}{s['p25_us']:>8.1f}{s['p50_us']:>8.1f}"
              f"{s['p95_us']:>8.1f}{s['p99_us']:>9.1f}{s['errors']:>5}")
    print()
    print(f"{'arm:shape (cpu)':<20}{'ops':>7}{'us/op':>10}")
    for key, (s, n) in list(cpu.items()) + list(cpu_floor.items()):
        label = key if isinstance(key, str) else f"{key[0]}:{key[1]}"
        us = (s / n * 1e6) if n else 0.0
        print(f"{label:<20}{n:>7}{us:>10.2f}")

    for s in sides:
        s.close()


if __name__ == "__main__":
    main()
