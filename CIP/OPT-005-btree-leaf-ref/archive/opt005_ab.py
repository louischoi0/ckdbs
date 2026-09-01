#!/usr/bin/env python3
"""Interleaved A/B for OPT-005 (`BtreeLookupHeld` carries the descent's leaf
`PageRef` out instead of dropping it and letting the caller re-fetch).

Five shapes, chosen to reach every converted call site plus one structural
control that never reaches `BtreeLookup*` at all:

    point        SELECT * FROM t_pt WHERE id = pid          (BTREE relation
                 - step_vm.cpp:590's RunPointStep, the point-step site)
    point_heap   SELECT * FROM t_pt_heap WHERE id = pid      (HEAP relation -
                 RunPointStep's `access.clustered_type == kBtree` gate is
                 false, so this never reaches BtreeLookup* at all. TRUE
                 structural control: must not move on any pair.)
    range        SELECT id, val FROM t_rng WHERE cust_id BETWEEN lo AND hi
                 (a secondary-index range scan - step_vm.cpp's index-range
                 phase 2, once **per resolved row**, the largest instance
                 of the pattern named in the proposal)
    join         SELECT c.id, p.val FROM t_join_child c
                     JOIN t_join_parent p ON c.parent_id = p.id
                     WHERE c.id = pid
                 (child: point step; parent: pk join probe, also a point
                 step by step_compiler's access-kind assignment - two
                 converted-site hits in one statement)
    fk_insert    INSERT INTO t_fk_child VALUES (pid, val)   -- pk omitted:
                 2 values for 3 columns is the omitted-pk form (sql.md's
                 INSERT grammar), so this sets parent_id=pid, val=val
                 (t_fk_child.parent_id REFERENCES t_fk_parent - fk_check.cpp's
                 parent descent, converted only at 1495016. On every earlier
                 arm this is itself a control: identical code runs on both
                 sides of a pair that predates the conversion.)
    plain_insert INSERT INTO t_plain_child VALUES (pid, val)   -- same omitted-pk form
                 (same shape, no REFERENCES - isolates "cost of INSERT" from
                 "cost of the FK check" so fk_insert's delta is attributable)

point_heap and plain_insert are the two controls that must hold flat on
every pair; fk_insert must hold flat on 31bc482-vs-c578e29 (fk_check not
touched yet) and move only from c578e29 (before) to a 1495016-line arm
(after).

Methodology follows CIP/OPT-001-update-decode-order/archive/opt001_ab.py:
one RNG stream drives an identical statement sequence on both arms; a
latency pass times each statement on the wire (bench_common.Phase per
arm/shape); a CPU pass brackets larger blocks with /proc/<pid>/stat
(utime+stime, 10ms ticks) because OPT-005's proposal predicted a counter
(`pages=`) that its own review then retracted (btree.hpp's amended comment:
`pages_fetched` is incremented explicitly beside the descent and never
counted the removed re-fetch) - so CPU is the only instrument, exactly as
OPT-006's. Both passes alternate which arm goes first each round. The
floor is established the same way opt001 does: arm A's own latency and CPU
series are split in half and reported as two "phases" of the identical
configuration.

ANALYZE is still run once per read shape after the load, on both arms, and
`pages=`/`index_resolved=` are compared - not because they are expected to
move (they should not, per the retraction above) but because printing them
is what makes "no counter shows this" a checked claim rather than an
assertion.

Correctness: full-table hashes (sha256 of `SELECT * ... ORDER BY id`) on
every relation, before and after the run, on both arms.
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

TXT_LEN = 12
SHAPES = ["point", "point_heap", "range", "join", "fk_insert", "plain_insert"]
CONTROLS = {"point_heap", "plain_insert"}

PAGES_RE = re.compile(r"\bpages=(\d+)")
INDEX_RESOLVED_RE = re.compile(r"\bindex_resolved=(\d+)")


class Side:
    def __init__(self, label, host, port, pid, timeout=120.0):
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


def rand_text(rng, n=TXT_LEN):
    return "".join(rng.choices("abcdefghijklmnopqrstuvwxyz", k=n))


def setup(side, rows, matches, seed):
    side.must("CREATE TABLE t_pt (id int64, val int64, txt varchar) BTREE")
    side.must("CREATE TABLE t_pt_heap (id int64, val int64, txt varchar)")
    side.must("CREATE TABLE t_rng (id int64, cust_id int64, val int64, txt varchar) BTREE")
    side.must("CREATE INDEX ix_rng_cust ON t_rng(cust_id)")
    side.must("CREATE TABLE t_join_parent (id int64, val int64, txt varchar) BTREE")
    side.must("CREATE TABLE t_join_child (id int64, parent_id int64, val int64) BTREE")
    side.must("CREATE TABLE t_fk_parent (id int64, val int64) BTREE")
    side.must("CREATE TABLE t_fk_child (id int64, parent_id int64 REFERENCES t_fk_parent, "
              "val int64) BTREE")
    side.must("CREATE TABLE t_plain_child (id int64, other_id int64, val int64) BTREE")

    rng = random.Random(seed)
    customers = max(rows // max(matches, 1), 4)

    for i in range(1, rows + 1):
        side.must(f"INSERT INTO t_pt VALUES ({rng.randint(0, 1_000_000)}, '{rand_text(rng)}')")
    for i in range(1, rows + 1):
        side.must(f"INSERT INTO t_pt_heap VALUES "
                  f"({rng.randint(0, 1_000_000)}, '{rand_text(rng)}')")
    for i in range(1, rows + 1):
        cust = i % customers
        side.must(f"INSERT INTO t_rng VALUES "
                  f"({cust}, {rng.randint(0, 1_000_000)}, '{rand_text(rng)}')")
    for i in range(1, rows + 1):
        side.must(f"INSERT INTO t_join_parent VALUES "
                  f"({rng.randint(0, 1_000_000)}, '{rand_text(rng)}')")
    for i in range(1, rows + 1):
        # parent_id = i: t_join_parent's i-th insert gets id i (ids assign
        # 1..rows in insertion order - invariant 11 - so this is a real
        # pk on the parent side, one per child, never a miss.
        side.must(f"INSERT INTO t_join_child VALUES ({i}, {rng.randint(0, 1_000_000)})")
    for i in range(1, rows + 1):
        side.must(f"INSERT INTO t_fk_parent VALUES ({rng.randint(0, 1_000_000)})")
    for i in range(1, rows + 1):
        side.must(f"INSERT INTO t_fk_child VALUES ({i}, {rng.randint(0, 1_000_000)})")
    for i in range(1, rows + 1):
        side.must(f"INSERT INTO t_plain_child VALUES ({i}, {rng.randint(0, 1_000_000)})")

    return customers


def checksum(side, sql):
    reply = side.must(sql)
    return hashlib.sha256(reply.encode()).hexdigest()


def correctness_check(sides, label):
    out = {"label": label, "ok": True, "checks": []}
    for name, sql in (
        ("t_pt", "SELECT * FROM t_pt ORDER BY id"),
        ("t_pt_heap", "SELECT * FROM t_pt_heap ORDER BY id"),
        ("t_rng", "SELECT * FROM t_rng ORDER BY id"),
        ("t_join_parent", "SELECT * FROM t_join_parent ORDER BY id"),
        ("t_join_child", "SELECT * FROM t_join_child ORDER BY id"),
        ("t_fk_parent", "SELECT * FROM t_fk_parent ORDER BY id"),
        ("t_fk_child", "SELECT * FROM t_fk_child ORDER BY id"),
        ("t_plain_child", "SELECT * FROM t_plain_child ORDER BY id"),
    ):
        hashes = {s.label: checksum(s, sql) for s in sides}
        match = len(set(hashes.values())) == 1
        out["checks"].append({"check": name, "hashes": hashes, "match": match})
        out["ok"] = out["ok"] and match
    return out


def gen_round(rng, rows, customers, range_span, n):
    return {
        "point": [rng.randint(1, rows) for _ in range(n["point"])],
        "point_heap": [rng.randint(1, rows) for _ in range(n["point_heap"])],
        "range": [rng.randint(0, customers - 1) for _ in range(n["range"])],
        "join": [rng.randint(1, rows) for _ in range(n["join"])],
        "fk_insert": [(rng.randint(1, rows), rng.randint(0, 1_000_000))
                      for _ in range(n["fk_insert"])],
        "plain_insert": [(rng.randint(1, rows), rng.randint(0, 1_000_000))
                         for _ in range(n["plain_insert"])],
    }


def run_shape(side, shape, item, range_span):
    if shape == "point":
        return side(f"SELECT * FROM t_pt WHERE id = {item}")
    if shape == "point_heap":
        return side(f"SELECT * FROM t_pt_heap WHERE id = {item}")
    if shape == "range":
        lo, hi = item, item + range_span
        return side(f"SELECT id, val FROM t_rng WHERE cust_id BETWEEN {lo} AND {hi}")
    if shape == "join":
        return side(f"SELECT c.id, p.val FROM t_join_child AS c "
                    f"JOIN t_join_parent AS p ON c.parent_id = p.id WHERE c.id = {item}")
    if shape == "fk_insert":
        pid, val = item
        return side(f"INSERT INTO t_fk_child VALUES ({pid}, {val})")
    if shape == "plain_insert":
        pid, val = item
        return side(f"INSERT INTO t_plain_child VALUES ({pid}, {val})")
    raise ValueError(shape)


def analyze_counters(side, sql):
    reply = side.must("ANALYZE " + sql)
    head = reply.split("\\n")[0]
    pages = PAGES_RE.findall(reply)
    resolved = INDEX_RESOLVED_RE.findall(reply)
    return {
        "pages": [int(x) for x in pages],
        "index_resolved": [int(x) for x in resolved],
        "raw_head": head,
    }


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
    ap.add_argument("--matches", type=int, default=6)
    ap.add_argument("--range-span", type=int, default=10)
    ap.add_argument("--seed", type=int, default=20260901)
    ap.add_argument("--json", required=True)
    ap.add_argument("--lat-rounds", type=int, default=16)
    ap.add_argument("--cpu-rounds", type=int, default=16)
    for shape in SHAPES:
        ap.add_argument(f"--lat-{shape}", type=int, required=True)
        ap.add_argument(f"--cpu-{shape}", type=int, required=True)
    args = ap.parse_args()

    sideA = Side(args.label_a, args.host, args.port_a, args.pid_a)
    sideB = Side(args.label_b, args.host, args.port_b, args.pid_b)
    sides = [sideA, sideB]

    lat_n = {shape: getattr(args, f"lat_{shape}") for shape in SHAPES}
    cpu_n = {shape: getattr(args, f"cpu_{shape}") for shape in SHAPES}

    print(f"[setup] rows={args.rows} matches={args.matches} seed={args.seed}")
    t0 = time.perf_counter()
    custA = setup(sideA, args.rows, args.matches, args.seed)
    custB = setup(sideB, args.rows, args.matches, args.seed)
    assert custA == custB
    print(f"[setup] done in {time.perf_counter()-t0:.2f}s, customers={custA}")

    pre_check = correctness_check(sides, "post-load")
    print(f"[correctness] post-load ok={pre_check['ok']}")
    if not pre_check["ok"]:
        print("FATAL: arms disagree immediately after identical load", file=sys.stderr)
        print(json.dumps(pre_check, indent=2), file=sys.stderr)
        sys.exit(1)

    # ANALYZE snapshot on the read shapes, taken once against a fixed probe
    # value so both arms run the identical statement.
    analyze_probes = {
        "point": "SELECT * FROM t_pt WHERE id = 1",
        "point_heap": "SELECT * FROM t_pt_heap WHERE id = 1",
        "range": f"SELECT id, val FROM t_rng WHERE cust_id BETWEEN 0 AND {args.range_span}",
        "join": "SELECT c.id, p.val FROM t_join_child AS c "
                "JOIN t_join_parent AS p ON c.parent_id = p.id WHERE c.id = 1",
    }
    analyze_out = {}
    for shape, sql in analyze_probes.items():
        analyze_out[shape] = {s.label: analyze_counters(s, sql) for s in sides}

    rng = random.Random(args.seed + 1)
    lat_rounds = [gen_round(rng, args.rows, custA, args.range_span, lat_n)
                  for _ in range(args.lat_rounds)]
    cpu_rounds = [gen_round(rng, args.rows, custA, args.range_span, cpu_n)
                  for _ in range(args.cpu_rounds)]

    phases = {(tag, shape): Phase(f"{tag}-{shape}") for tag in (args.label_a, args.label_b)
              for shape in SHAPES}
    lat_t0 = time.perf_counter()
    for r, round_payload in enumerate(lat_rounds):
        for side in ordered(sides, r):
            for shape in SHAPES:
                for item in round_payload[shape]:
                    t0i = time.perf_counter()
                    reply = run_shape(side, shape, item, args.range_span)
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
    for r, round_payload in enumerate(cpu_rounds):
        for side in ordered(sides, r):
            for shape in SHAPES:
                payload = round_payload[shape]
                if not payload:
                    continue
                before = side.cpu_seconds()
                for item in payload:
                    run_shape(side, shape, item, args.range_span)
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
        for tagname, rrange in ((f"{args.label_a}floor1", range(0, half)),
                                 (f"{args.label_a}floor2", range(half, args.cpu_rounds))):
            tot_s = sum(cpu_per_round.get((args.label_a, shape, r), (0.0, 0))[0] for r in rrange)
            tot_ops = sum(cpu_per_round.get((args.label_a, shape, r), (0.0, 0))[1] for r in rrange)
            cpu_floor[(tagname, shape)] = (tot_s, tot_ops)

    post_check = correctness_check(sides, "post-writes")
    print(f"[correctness] post-writes ok={post_check['ok']}")

    out = {
        "rows": args.rows, "matches": args.matches, "range_span": args.range_span,
        "seed": args.seed, "customers": custA,
        "lat_rounds": args.lat_rounds, "cpu_rounds": args.cpu_rounds,
        "lat_n": lat_n, "cpu_n": cpu_n,
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
    print(f"{'arm:shape':<24}{'ops':>7}{'qps':>9}{'mean us':>10}{'p0':>8}{'p25':>8}"
          f"{'p50':>8}{'p95':>8}{'p99':>9}{'err':>5}")
    for key, p in list(phases.items()) + list(floor_phases.items()):
        s = p.summary()
        print(f"{s['phase']:<24}{s['ops']:>7}{s['qps']:>9,.0f}{s['mean_us']:>10.1f}"
              f"{s['p0_us']:>8.1f}{s['p25_us']:>8.1f}{s['p50_us']:>8.1f}"
              f"{s['p95_us']:>8.1f}{s['p99_us']:>9.1f}{s['errors']:>5}")
    print()
    print(f"{'arm:shape (cpu)':<24}{'ops':>7}{'us/op':>10}")
    for key, (s, n) in list(cpu.items()) + list(cpu_floor.items()):
        label = key if isinstance(key, str) else f"{key[0]}:{key[1]}"
        us = (s / n * 1e6) if n else 0.0
        print(f"{label:<24}{n:>7}{us:>10.2f}")
    print()
    print("ANALYZE pages=/index_resolved=:")
    for shape, byarm in analyze_out.items():
        for label, c in byarm.items():
            print(f"  {shape:<12}{label:<4} pages={c['pages']} index_resolved={c['index_resolved']}")

    for s in sides:
        s.close()


if __name__ == "__main__":
    main()
