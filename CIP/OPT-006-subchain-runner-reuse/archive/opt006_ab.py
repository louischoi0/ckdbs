#!/usr/bin/env python3
"""Interleaved A/B for OPT-006 (a correlated sub-chain's `ChainRunner` and
its `RowSink` cached across outer rows instead of rebuilt per row).

Schema: `t_outer(id, target, target2, flag)` and `t_inner(id, outer_id, val,
rank)`, `t_inner` BTREE with a secondary index on `outer_id` so every
correlated equality below compiles to a correlated index probe rather than
a per-outer-row scan of `t_inner` (step_compiler.cpp's correlated-index-probe
rule) - the point is to isolate the runner-rebuild cost this entry is about,
not the cost of a linear inner scan. Each outer row owns exactly `--fanout`
inner rows (ranks 0..fanout-1), so EXISTS is always true and the scalar
subquery (keyed on rank=0) always has exactly one qualifying row - both
deterministic, which is what makes the correctness hash meaningful rather
than incidental.

Five shapes, one full-relation statement each - not a per-row point query -
because the proposal's own claim is that the win **scales with the number
of outer rows** a single statement's walk evaluates the sub-chain against;
holding op count low and sweeping rows (the required 200/1000/10000) is
what shows that scaling rather than asserting it:

    exists          SELECT id FROM t_outer WHERE EXISTS
                         (SELECT t_inner.id FROM t_inner
                          WHERE t_inner.outer_id = t_outer.id)
    in_             SELECT id FROM t_outer WHERE target IN
                         (SELECT t_inner.val FROM t_inner
                          WHERE t_inner.outer_id = t_outer.id)
    scalar          SELECT id FROM t_outer WHERE target2 =
                         (SELECT t_inner.val FROM t_inner
                          WHERE t_inner.outer_id = t_outer.id
                            AND t_inner.rank = 0)
    control_hoisted SELECT id FROM t_outer WHERE EXISTS
                         (SELECT t_inner.id FROM t_inner
                          WHERE t_inner.rank = 0 AND t_inner.val < 5)
                    -- NO outer reference: step_compiler.cpp's placement
                    pass only hoists a value-less EXISTS/NOT EXISTS with no
                    outer column (`!sub.correlated && !sub.has_value`) into
                    `chain.hoisted`, evaluated exactly once regardless of
                    outer row count (ExecuteAsync's hoisting block) - the
                    one shape genuinely unaffected by outer row count
                    before or after OPT-006, and this driver's real control.
    control_update  UPDATE t_outer SET flag = flag + 1 WHERE target IN
                         (SELECT t_inner.val FROM t_inner
                          WHERE t_inner.outer_id = t_outer.id)
                    -- the shape the review found ff27662 regressed:
                    `EvaluateConjuncts` (command_dispatcher.cpp's UPDATE
                    walk) builds a *fresh* ChainRunner per scanned row, so
                    every row's sub-chain evaluation is a "first" evaluation
                    on that runner. ff27662 cached unconditionally from the
                    first evaluation - turning a stack ChainRunner into a
                    heap-allocated one on every scanned row of every UPDATE.
                    1495016 keeps the first evaluation on the stack and
                    caches only from the second sighting of a sub-chain on
                    one runner instance - which this shape's per-row fresh
                    runners never reach - so it should read flat against a
                    pre-OPT-006 baseline and *regressed* only if `ff27662`
                    is in the arm set.

`target`/`target2` are drawn so roughly 60% of outer rows match one of
their own inner rows and 40% do not - real true/false traffic through
`kInSubquery`/`kCompareSubquery`, not a constant-true predicate.

Methodology follows opt005_ab.py / opt001_ab.py: one RNG stream, an
interleaved latency pass (bench_common.Phase) and a CPU pass
(/proc/<pid>/stat utime+stime), both alternating which arm leads each
round, plus a floor split of arm A's own series. ANALYZE is run once per
read shape on both arms after the load, and `sub_runs=`/`examined=` are
required to match while `memo_hits=`/`replays=`/`trail_misses=` are
reported but **excluded from the match requirement** - the V19 probe memo
surviving across outer rows on a cached runner is documented, expected
behaviour on an OPT-006 arm, not a discrepancy.
"""
import argparse
import hashlib
import json
import os
import random
import re
import sys
import time

sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/path-optimizer/tools")
from ckdbs_cli import ServerConnection, format_reply  # noqa: E402
from bench_common import Phase  # noqa: E402

SHAPES = ["exists", "in_", "scalar", "control_hoisted", "control_update"]
CONTROLS = {"control_hoisted"}

SUB_RUNS_RE = re.compile(r"\bsub_runs=(\d+)")
MEMO_HITS_RE = re.compile(r"\bmemo_hits=(\d+)")
REPLAYS_RE = re.compile(r"\breplays=(\d+)")
TRAIL_MISSES_RE = re.compile(r"\btrail_misses=(\d+)")
EXAMINED_RE = re.compile(r"\bexamined=(\d+)")

QUERIES = {
    "exists": "SELECT id FROM t_outer WHERE EXISTS "
              "(SELECT t_inner.id FROM t_inner WHERE t_inner.outer_id = t_outer.id)",
    "in_": "SELECT id FROM t_outer WHERE target IN "
           "(SELECT t_inner.val FROM t_inner WHERE t_inner.outer_id = t_outer.id)",
    "scalar": "SELECT id FROM t_outer WHERE target2 = "
              "(SELECT t_inner.val FROM t_inner WHERE t_inner.outer_id = t_outer.id "
              "AND t_inner.rank = 0)",
    "control_hoisted": "SELECT id FROM t_outer WHERE EXISTS "
                       "(SELECT t_inner.id FROM t_inner WHERE t_inner.rank = 0 "
                       "AND t_inner.val < 5)",
    # SET takes a literal, not an expression (`flag = flag + 1` is refused
    # - manual/sql/sql.md's UPDATE grammar is `SET <col> = <val>`) - so the
    # driver supplies a fresh literal per call instead, via {flag}.
    "control_update": "UPDATE t_outer SET flag = {flag} WHERE target IN "
                      "(SELECT t_inner.val FROM t_inner WHERE t_inner.outer_id = t_outer.id)",
}


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


def setup(side, rows, fanout, seed):
    side.must("CREATE TABLE t_outer (id int64, target int64, target2 int64, flag int64) BTREE")
    side.must("CREATE TABLE t_inner (id int64, outer_id int64, val int64, rank int32) BTREE")
    side.must("CREATE INDEX ix_inner_outer ON t_inner(outer_id)")

    rng = random.Random(seed)
    # inner rows for outer id `o` (1..rows): vals are `o*1000 + rank`, so
    # every inner val is unique and traceable back to its owner - useful
    # when debugging a mismatch, not load-bearing for the measurement.
    for o in range(1, rows + 1):
        vals = [o * 1000 + r for r in range(fanout)]
        rank0_val = vals[0]
        if rng.random() < 0.6:
            target = rng.choice(vals)
        else:
            target = -o  # never equals any val (vals are all positive)
        if rng.random() < 0.6:
            target2 = rank0_val
        else:
            target2 = -o - 1
        side.must(f"INSERT INTO t_outer VALUES ({target}, {target2}, 0)")
        for r, v in enumerate(vals):
            side.must(f"INSERT INTO t_inner VALUES ({o}, {v}, {r})")

    return rng


def checksum(side, sql):
    reply = side.must(sql)
    return hashlib.sha256(reply.encode()).hexdigest()


def correctness_check(sides, label):
    out = {"label": label, "ok": True, "checks": []}
    for name, sql in (
        ("t_outer", "SELECT * FROM t_outer ORDER BY id"),
        ("t_inner", "SELECT * FROM t_inner ORDER BY id"),
        ("exists_result", "SELECT id FROM t_outer WHERE EXISTS "
                          "(SELECT t_inner.id FROM t_inner WHERE t_inner.outer_id = t_outer.id) "
                          "ORDER BY id"),
        ("in_result", "SELECT id FROM t_outer WHERE target IN "
                      "(SELECT t_inner.val FROM t_inner WHERE t_inner.outer_id = t_outer.id) "
                      "ORDER BY id"),
        ("scalar_result", "SELECT id FROM t_outer WHERE target2 = "
                          "(SELECT t_inner.val FROM t_inner WHERE t_inner.outer_id = t_outer.id "
                          "AND t_inner.rank = 0) ORDER BY id"),
    ):
        hashes = {s.label: checksum(s, sql) for s in sides}
        match = len(set(hashes.values())) == 1
        out["checks"].append({"check": name, "hashes": hashes, "match": match})
        out["ok"] = out["ok"] and match
    return out


def analyze_counters(side, sql):
    reply = side.must("ANALYZE " + sql)
    head = reply.split("\\n")[0]
    return {
        "sub_runs": [int(x) for x in SUB_RUNS_RE.findall(reply)],
        "memo_hits": [int(x) for x in MEMO_HITS_RE.findall(reply)],
        "replays": [int(x) for x in REPLAYS_RE.findall(reply)],
        "trail_misses": [int(x) for x in TRAIL_MISSES_RE.findall(reply)],
        "examined": [int(x) for x in EXAMINED_RE.findall(reply)],
        "raw_head": head,
    }


def run_shape(side, shape, flag_val=0):
    return side(QUERIES[shape].format(flag=flag_val))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port-a", type=int, required=True)
    ap.add_argument("--port-b", type=int, required=True)
    ap.add_argument("--pid-a", type=int, required=True)
    ap.add_argument("--pid-b", type=int, required=True)
    ap.add_argument("--label-a", default="A")
    ap.add_argument("--label-b", default="B")
    ap.add_argument("--rows", type=int, required=True)
    ap.add_argument("--fanout", type=int, default=3)
    ap.add_argument("--seed", type=int, default=20260901)
    ap.add_argument("--json", required=True)
    ap.add_argument("--lat-rounds", type=int, required=True)
    ap.add_argument("--cpu-rounds", type=int, required=True)
    ap.add_argument("--lat-ops", type=int, required=True,
                     help="statements per shape per round, latency pass")
    ap.add_argument("--cpu-ops", type=int, required=True,
                     help="statements per shape per round, CPU pass")
    args = ap.parse_args()

    sideA = Side(args.label_a, args.host, args.port_a, args.pid_a)
    sideB = Side(args.label_b, args.host, args.port_b, args.pid_b)
    sides = [sideA, sideB]

    print(f"[setup] rows={args.rows} fanout={args.fanout} seed={args.seed}")
    t0 = time.perf_counter()
    setup(sideA, args.rows, args.fanout, args.seed)
    setup(sideB, args.rows, args.fanout, args.seed)
    print(f"[setup] done in {time.perf_counter()-t0:.2f}s")

    pre_check = correctness_check(sides, "post-load")
    print(f"[correctness] post-load ok={pre_check['ok']}")
    if not pre_check["ok"]:
        print("FATAL: arms disagree immediately after identical load", file=sys.stderr)
        print(json.dumps(pre_check, indent=2), file=sys.stderr)
        sys.exit(1)

    analyze_out = {}
    for shape in ("exists", "in_", "scalar", "control_hoisted"):
        analyze_out[shape] = {s.label: analyze_counters(s, QUERIES[shape]) for s in sides}

    phases = {(tag, shape): Phase(f"{tag}-{shape}") for tag in (args.label_a, args.label_b)
              for shape in SHAPES}
    lat_t0 = time.perf_counter()
    for r in range(args.lat_rounds):
        for side in ordered(sides, r):
            for shape in SHAPES:
                for i in range(args.lat_ops):
                    # A fresh literal per control_update call (SET takes no
                    # expression) - the same value on both arms since it is
                    # a pure function of (round, op index), not of which
                    # side ran first.
                    flag_val = r * args.lat_ops + i
                    t0i = time.perf_counter()
                    reply = run_shape(side, shape, flag_val)
                    phases[(side.label, shape)].record(time.perf_counter() - t0i, reply)
    print(f"[latency pass] {args.lat_rounds} rounds done in {time.perf_counter()-lat_t0:.2f}s")

    for p in phases.values():
        p.elapsed = sum(p.latencies) if p.latencies else 0.0

    floor_phases = {}
    for shape in SHAPES:
        lat = phases[(args.label_a, shape)].latencies
        mid = len(lat) // 2
        p1, p2 = Phase(f"{args.label_a}floor1-{shape}"), Phase(f"{args.label_a}floor2-{shape}")
        for x in lat[:mid]:
            p1.record(x, "OK")
        for x in lat[mid:]:
            p2.record(x, "OK")
        p1.elapsed = sum(p1.latencies) if p1.latencies else 0.0
        p2.elapsed = sum(p2.latencies) if p2.latencies else 0.0
        floor_phases[(f"{args.label_a}floor1", shape)] = p1
        floor_phases[(f"{args.label_a}floor2", shape)] = p2

    cpu = {}
    cpu_per_round = {}
    cpu_t0 = time.perf_counter()
    for r in range(args.cpu_rounds):
        for side in ordered(sides, r):
            for shape in SHAPES:
                before = side.cpu_seconds()
                for i in range(args.cpu_ops):
                    flag_val = 1_000_000 + r * args.cpu_ops + i
                    run_shape(side, shape, flag_val)
                after = side.cpu_seconds()
                if before is not None and after is not None:
                    key = (side.label, shape)
                    tot_s, tot_ops = cpu.get(key, (0.0, 0))
                    cpu[key] = (tot_s + (after - before), tot_ops + args.cpu_ops)
                    cpu_per_round[(side.label, shape, r)] = (after - before, args.cpu_ops)
    print(f"[cpu pass] {args.cpu_rounds} rounds done in {time.perf_counter()-cpu_t0:.2f}s")

    cpu_floor = {}
    half = max(1, args.cpu_rounds // 2)
    for shape in SHAPES:
        for tagname, rrange in ((f"{args.label_a}floor1", range(0, half)),
                                 (f"{args.label_a}floor2", range(half, args.cpu_rounds))):
            tot_s = sum(cpu_per_round.get((args.label_a, shape, r), (0.0, 0))[0] for r in rrange)
            tot_ops = sum(cpu_per_round.get((args.label_a, shape, r), (0.0, 0))[1] for r in rrange)
            cpu_floor[(tagname, shape)] = (tot_s, tot_ops)

    post_check = correctness_check(sides, "post-writes")
    print(f"[correctness] post-writes ok={post_check['ok']}")

    out = {
        "rows": args.rows, "fanout": args.fanout, "seed": args.seed,
        "lat_rounds": args.lat_rounds, "cpu_rounds": args.cpu_rounds,
        "lat_ops": args.lat_ops, "cpu_ops": args.cpu_ops,
        "pre_check": pre_check, "post_check": post_check,
        "analyze": analyze_out,
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
    print(f"{'arm:shape':<28}{'ops':>7}{'qps':>9}{'mean us':>10}{'p0':>8}{'p25':>8}"
          f"{'p50':>8}{'p95':>8}{'p99':>9}{'err':>5}")
    for key, p in list(phases.items()) + list(floor_phases.items()):
        s = p.summary()
        print(f"{s['phase']:<28}{s['ops']:>7}{s['qps']:>9,.0f}{s['mean_us']:>10.1f}"
              f"{s['p0_us']:>8.1f}{s['p25_us']:>8.1f}{s['p50_us']:>8.1f}"
              f"{s['p95_us']:>8.1f}{s['p99_us']:>9.1f}{s['errors']:>5}")
    print()
    print(f"{'arm:shape (cpu)':<28}{'ops':>7}{'us/op':>12}{'us/1k-outer-rows':>20}")
    for key, (s, n) in list(cpu.items()) + list(cpu_floor.items()):
        label = key if isinstance(key, str) else f"{key[0]}:{key[1]}"
        us = (s / n * 1e6) if n else 0.0
        us_per_1k = us / args.rows * 1000 if args.rows else 0.0
        print(f"{label:<28}{n:>7}{us:>12.2f}{us_per_1k:>20.2f}")
    print()
    print("ANALYZE sub_runs=/examined= (memo_hits=/replays=/trail_misses= informational only):")
    for shape, byarm in analyze_out.items():
        for label, c in byarm.items():
            print(f"  {shape:<18}{label:<4} sub_runs={c['sub_runs']} examined={c['examined']} "
                  f"memo_hits={c['memo_hits']} replays={c['replays']} "
                  f"trail_misses={c['trail_misses']}")

    for s in sides:
        s.close()


if __name__ == "__main__":
    main()
