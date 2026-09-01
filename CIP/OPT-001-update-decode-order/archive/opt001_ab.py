#!/usr/bin/env python3
"""Interleaved A/B for OPT-001 (UPDATE/DELETE decode-before-predicate).

Arm A: 004da62  (baseline, has OPT-002 and OPT-004, does NOT have OPT-001)
Arm B: ea1d9d0  (OPT-001 merged in, on top of the same tree)

For one row-count size: creates tables per arm from the identical seed,
then runs interleaved rounds of five shapes:

    select      SELECT * FROM t_main WHERE id = pid                (control)
    update      UPDATE t_main SET c_int=x WHERE id = pid            (the target)
    delete      DELETE FROM t_del WHERE id = victim_id              (same defect,
                own table - see "Why t_del is separate" below)
    wide        UPDATE t_wide SET c1=x WHERE c2 = target             (20+ cols)
    subchain    UPDATE t_main SET c_int=x
                    WHERE id IN (SELECT target_id FROM t_pool
                                 WHERE target_id = target)           (control:
                any subquery forces Step::kAllColumns - see
                step_compiler.cpp's CompileWhere, `out.sub_chains.empty()`
                gates the mask - so this shape must show NO delta.)

Two passes, both interleaved and alternating which arm goes first each
round, exactly as tools/catalog_read_ab_benchmark.py does and for the same
reason:

  - latency pass: small blocks, per-statement wall-clock timing (one send +
    one recv on a loopback connection, so this already tracks server work
    closely), pooled into a bench_common.Phase per (arm, shape).
  - CPU pass: larger contiguous windows (/proc/<pid>/stat advances in whole
    10ms ticks, so a small block's +-1 tick error would swamp the signal -
    this is the *primary* instrument per the task: there is no ANALYZE for
    UPDATE), one window per (shape, arm) per round, no per-op timing inside.

One RNG stream drives the statement sequence for BOTH arms - the same
target ids, victim ids and SET values in the same order - so the only
thing that can differ between a round's A block and B block is the server.

id assignment: every INSERT in this driver omits the pk, so ids auto-assign
1..N in insertion order (invariant 11) - never a named id, which a
heap-clustered relation refuses below its own high-water mark (verified
empirically against a probe server before this driver was written).

Why t_del is separate from t_main: DELETE's CPU signal needs on the order
of a thousand ops per window to clear the 10ms tick floor at rows=200, and
that many pre-provisioned victims sitting inside t_main would itself
dominate t_main's *own* size and contaminate every other shape's
measurement (select/update/subchain all walk t_main). t_del is its own
relation, `--rows` padding rows plus its own victim pool, so contamination
is confined to the shape it belongs to. t_del's own effective scan size
therefore runs from rows+V down to rows over the delete phase rather than
holding exactly at rows - stated plainly in the results rather than hidden.

Correctness: after the run, `SELECT * FROM t_main ORDER BY id`, `... FROM
t_wide ORDER BY id` and `... FROM t_del WHERE id <= rows ORDER BY id` are
hashed (sha256 of the reply text) on both arms and compared, plus spot rows
printed verbatim. Both arms are driven by the literally identical statement
sequence, so exact equality is the correctness bar.
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
WIDE_EXTRA_COLS = 24  # id + 24 = 25 columns total
POOL_SIZE = 5         # t_pool row count - kept tiny so the sub-chain's
                       # per-outer-row re-evaluation stays affordable at N=10000

SHAPES = ["select", "update", "delete", "wide", "subchain"]


def wide_columns():
    return "id int64, " + ", ".join(f"c{i} int64" for i in range(1, WIDE_EXTRA_COLS + 1))


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
        """utime + stime of the server process, in seconds (proc(5) fields
        14+15 - the split on ') ' drops the 'pid (comm)' prefix, so index 11
        here is field 14, index 12 is field 15 - the same read
        tools/catalog_read_ab_benchmark.py's Side.cpu_seconds() does)."""
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


def setup(side, rows, del_victims, seed):
    side.must(f"CREATE TABLE t_main ({MAIN_COLUMNS})")
    side.must(f"CREATE TABLE t_wide ({wide_columns()})")
    side.must("CREATE TABLE t_pool (id int64, target_id int64)")
    side.must(f"CREATE TABLE t_del ({MAIN_COLUMNS})")

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

    for i in range(1, rows + 1):
        vals = [str(rng.randint(0, 1_000_000)) for _ in range(WIDE_EXTRA_COLS)]
        vals[1] = str(i)  # c2 (vals[0]=c1, vals[1]=c2, ...) forced to row index
        side.must(f"INSERT INTO t_wide VALUES ({', '.join(vals)})")

    pool_targets = [max(1, round(rows * (k + 1) / (POOL_SIZE + 1))) for k in range(POOL_SIZE)]
    for t in pool_targets:
        side.must(f"INSERT INTO t_pool VALUES ({t})")

    # t_del: rows core (never touched) + del_victims doomed rows, own id space
    for i in range(rows):
        side.must(f"INSERT INTO t_del VALUES ({rand_row()})")
    for i in range(del_victims):
        side.must(f"INSERT INTO t_del VALUES ({rand_row()})")

    return pool_targets, first_reply


def checksum(side, sql):
    reply = side.must(sql)
    return hashlib.sha256(reply.encode()).hexdigest(), reply


def correctness_check(sides, rows, label):
    out = {"label": label, "ok": True, "checks": []}
    for name, sql in (
        ("t_main", "SELECT * FROM t_main ORDER BY id"),
        ("t_wide", "SELECT * FROM t_wide ORDER BY id"),
        ("t_del_core", f"SELECT * FROM t_del WHERE id <= {rows} ORDER BY id"),
        ("t_del_count", "SELECT COUNT(*) FROM t_del"),
    ):
        hashes = {}
        for s in sides:
            h, _ = checksum(s, sql)
            hashes[s.label] = h
        match = len(set(hashes.values())) == 1
        out["checks"].append({"check": name, "hashes": hashes, "match": match})
        out["ok"] = out["ok"] and match
    spot_ids = sorted({1, max(1, rows // 2), rows})
    spots = {}
    for pid in spot_ids:
        row = {s.label: s.must(f"SELECT * FROM t_main WHERE id = {pid}") for s in sides}
        spots[pid] = row
        if len(set(row.values())) != 1:
            out["ok"] = False
    out["spot_rows"] = spots
    return out


def gen_round(rng, rows, pool_targets, n_select, n_update, n_del, n_wide, n_sub, victim_iter):
    return {
        "select": [rng.randint(1, rows) for _ in range(n_select)],
        "update": [(rng.randint(1, rows), rng.randint(0, 1_000_000)) for _ in range(n_update)],
        "delete": [next(victim_iter) for _ in range(n_del)],
        "wide": [(rng.randint(1, rows), rng.randint(0, 1_000_000)) for _ in range(n_wide)],
        "subchain": [(rng.choice(pool_targets), rng.randint(0, 1_000_000)) for _ in range(n_sub)],
    }


def run_shape_block(side, phase, shape, payload):
    for item in payload:
        t0 = time.perf_counter()
        if shape == "select":
            r = side(f"SELECT * FROM t_main WHERE id = {item}")
        elif shape == "update":
            pid, val = item
            r = side(f"UPDATE t_main SET c_int = {val} WHERE id = {pid}")
        elif shape == "delete":
            r = side(f"DELETE FROM t_del WHERE id = {item}")
        elif shape == "wide":
            pid, val = item
            r = side(f"UPDATE t_wide SET c1 = {val} WHERE c2 = {pid}")
        elif shape == "subchain":
            target, val = item
            r = side(f"UPDATE t_main SET c_int = {val} WHERE id IN "
                     f"(SELECT target_id FROM t_pool WHERE target_id = {target})")
        else:
            raise ValueError(shape)
        phase.record(time.perf_counter() - t0, r)


def run_shape_cpu(side, shape, payload):
    for item in payload:
        if shape == "select":
            side(f"SELECT * FROM t_main WHERE id = {item}")
        elif shape == "update":
            pid, val = item
            side(f"UPDATE t_main SET c_int = {val} WHERE id = {pid}")
        elif shape == "delete":
            side(f"DELETE FROM t_del WHERE id = {item}")
        elif shape == "wide":
            pid, val = item
            side(f"UPDATE t_wide SET c1 = {val} WHERE c2 = {pid}")
        elif shape == "subchain":
            target, val = item
            side(f"UPDATE t_main SET c_int = {val} WHERE id IN "
                 f"(SELECT target_id FROM t_pool WHERE target_id = {target})")
        else:
            raise ValueError(shape)


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
    ap.add_argument("--cpu-rounds", type=int, default=4)
    # per-shape ops/round, latency pass then cpu pass
    for shape in SHAPES:
        ap.add_argument(f"--lat-{shape}", type=int, required=True)
        ap.add_argument(f"--cpu-{shape}", type=int, required=True)
    args = ap.parse_args()

    sideA = Side("A", args.host, args.port_a, args.pid_a)
    sideB = Side("B", args.host, args.port_b, args.pid_b)
    sides = [sideA, sideB]

    lat_n = {shape: getattr(args, f"lat_{shape}") for shape in SHAPES}
    cpu_n = {shape: getattr(args, f"cpu_{shape}") for shape in SHAPES}
    del_victims = args.lat_rounds * lat_n["delete"] + args.cpu_rounds * cpu_n["delete"] + 4

    print(f"[setup] rows={args.rows} del_victims={del_victims} seed={args.seed}")
    t0 = time.perf_counter()
    pool_targets_a, first_a = setup(sideA, args.rows, del_victims, args.seed)
    pool_targets_b, first_b = setup(sideB, args.rows, del_victims, args.seed)
    assert pool_targets_a == pool_targets_b
    print(f"[setup] done in {time.perf_counter()-t0:.2f}s; first insert reply "
          f"A={first_a!r} B={first_b!r}")

    pre_check = correctness_check(sides, args.rows, "post-load")
    print(f"[correctness] post-load ok={pre_check['ok']}")
    if not pre_check["ok"]:
        print("FATAL: arms disagree immediately after identical load", file=sys.stderr)
        print(json.dumps(pre_check, indent=2), file=sys.stderr)
        sys.exit(1)

    rng = random.Random(args.seed + 1)
    victim_ids = list(range(args.rows + 1, args.rows + del_victims + 1))
    rng.shuffle(victim_ids)
    victim_iter = iter(victim_ids)

    lat_rounds = [gen_round(rng, args.rows, pool_targets_a, lat_n["select"], lat_n["update"],
                             lat_n["delete"], lat_n["wide"], lat_n["subchain"], victim_iter)
                  for _ in range(args.lat_rounds)]
    cpu_rounds = [gen_round(rng, args.rows, pool_targets_a, cpu_n["select"], cpu_n["update"],
                             cpu_n["delete"], cpu_n["wide"], cpu_n["subchain"], victim_iter)
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

    post_check = correctness_check(sides, args.rows, "post-writes")
    print(f"[correctness] post-writes ok={post_check['ok']}")

    out = {
        "rows": args.rows,
        "del_victims": del_victims,
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
