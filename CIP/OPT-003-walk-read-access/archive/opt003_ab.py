#!/usr/bin/env python3
"""Interleaved A/B for OPT-003 (UPDATE/DELETE walk `kRead` instead of `kWrite`).

Arm A: 40c5e86  (v2.7.0-40-g40c5e86, baseline - VisitRelation called with
                 storage::PageAccess::kWrite for UPDATE and DELETE)
Arm B: 31bc482  (v2.7.0-41-g31bc482, OPT-003 - the same two call sites take
                 storage::PageAccess::kRead)

**This is a write-amplification claim, not a CPU claim** (the task's own
framing, and the proposal's own: "No CPU change is predicted - this is an
I/O claim"). `kWrite` routes ChainVisitOnePage to `store.Get`, which marks
every frame it hands out dirty; `kRead` takes `GetForRead`, which does not.
Neither UPDATE's `apply` nor DELETE's `mark` writes through the walk's own
page - both re-fetch with `page_store_.Get` before mutating - so under the
baseline, a point UPDATE/DELETE on a heap relation (LocateByPk answers
`kScan` for every heap relation - it has no pk index, so PkSpan pruning
never applies to an unsplit table either: VisitRelation's kHeap branch
takes `access.ranges.empty()`'s plain `ChainVisit` over the whole chain)
dirties every page the walk touches, i.e. the whole relation, every
statement.

**Two instruments, two passes, two kinds of server** (see below for why):

  1. **Raw (unsynced) latency pass** - interleaved rounds, no SYNC in the
     loop, per-statement wall time only. Answers the CPU-claim question
     directly: OPT-003 predicts this shows nothing, because neither arm's
     per-row work changed, only which store call the walk's own fetch
     makes. Run against a **plain** (untraced) server pair, so the numbers
     carry no tracing tax.

  2. **Checkpoint/bytes pass** - round size 1 (one statement, then one
     explicit SYNC), repeated `--ckpt-reps` times per op-type
     (select/update/delete). `checkpoint_interval_ms = 0` (this proposal's
     server config disables the timer cadence entirely - see
     run_ab_server_opt003.sh / run_ab_server_strace.sh - so the *only*
     thing that ever calls `page_store_.Sync()` is this driver's own
     explicit SYNC). At round size 1, a round's SYNC writes back exactly
     what became dirty since the previous round's SYNC: under arm A, the
     whole relation (the walk re-dirties every page it touches regardless
     of the predicate, every single statement); under arm B, at most the
     one page the mutating re-fetch touched.

     **Why this pass needs its statement's own server, launched under
     `strace`, rather than `/proc/<pid>/io`.** `/proc/<pid>/io`'s
     `write_bytes` is process-wide and cannot separate the data file's own
     writeback from the WAL's - and on this workload the WAL dominates it
     by two orders of magnitude: every statement's redo record gets its
     own `pwrite`+`fdatasync` on a *separate* fd under `durability=group`
     with one statement in flight (confirmed empirically before this
     driver's final form: a single point UPDATE on a 200-row table read
     back ~1.1MB of `/proc/<pid>/io` delta on *both* arms, while the data
     file itself is a few hundred KB total - the WAL segment's own
     per-record fdatasync, not page writeback, was what that counter was
     reading). SHOW META carries no page-store write-back counter either
     (surveyed - `dirty`/writeback fields exist on `DevicePageStore` and
     `FilePageDevice` but none is surfaced through `HandleShowMeta`; the
     WAL sync counters and refill/commit-leg stats it does carry answer a
     different question). ptrace-attaching a *running* server is refused
     in this sandbox (`PTRACE_SEIZE: Operation not permitted`), so tracing
     has to start at exec - `run_ab_server_strace.sh` launches the server
     under `strace -f -ttt -e trace=openat,pwrite64,fsync,fdatasync`, and
     this driver resolves the data file's own fd from the `openat` record
     and sums `pwrite64(<that fd>, ...)` bytes whose `-ttt` (epoch,
     directly comparable to Python's own `time.time()`) timestamp falls
     inside each round's `[statement-start, sync-done]` window.

     Because tracing adds a per-syscall tax, this pass's own **latency**
     numbers (`sync_lat_us`, `stmt_lat_us`) are reported but marked
     `measured_under_strace=true` in the JSON and are not the latency this
     driver's write-up treats as authoritative - a **third, untraced**
     checkpoint-latency-only pass (statement + SYNC, timed, no byte
     attempt) runs against the plain server instead, immediately after the
     raw latency pass, and supplies the clean SYNC-latency reading.

A `select` op-type runs through the identical round-with-SYNC harness as
the control the task asks for: `SELECT` dirties nothing on either arm, so
its bytes and sync-latency numbers should sit at both arms' shared floor -
proving the instrument attributes bytes to the write, not to the SYNC
call's own fixed cost or to the per-statement access-stats bookkeeping
every statement (including SELECT) pays.

**id assignment**: every INSERT in this driver omits the pk, so ids
auto-assign 1..N in insertion order (invariant 11) - same convention
OPT-001's driver used, verified there against a probe server.

**t_del is separate from t_upd**, same reason as OPT-001's driver: `t_del`
carries `rows` core rows (never touched) plus a victim pool sized for every
delete this run performs, so a delete's own id space never collides with
`t_upd`'s.

Correctness: after the full run, `SELECT * FROM t_upd ORDER BY id` and
`SELECT * FROM t_del WHERE id <= rows ORDER BY id` plus `SELECT COUNT(*)
FROM t_del` are hashed (sha256 of the reply text) on both arms and
compared. Both arms are driven by the literally identical statement
sequence (one RNG stream feeds both), so exact equality is the correctness
bar.

Two invocations per (arm-pair, row count) - see run_sweep_opt003.sh:

    opt003_ab.py --mode plain  ...    # raw latency + checkpoint-latency-only
    opt003_ab.py --mode strace ...    # checkpoint/bytes, against traced servers
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

MAIN_COLUMNS = "id int64, c_int int64, c_small int32, c_flag bool, c_text varchar"
TEXT_LEN = 16
SMALL_RANGE = 30000


class Side:
    def __init__(self, label, host, port, timeout=120.0):
        self.label = label
        self.host = host
        self.port = port
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

    def close(self):
        self.conn.close()


def ordered(sides, i):
    return sides if i % 2 == 0 else list(reversed(sides))


def rand_row(rng):
    text = "".join(rng.choices("abcdefghijklmnopqrstuvwxyz", k=TEXT_LEN))
    return (f"{rng.randint(0, 1_000_000)}, {rng.randint(0, SMALL_RANGE)}, "
            f"{rng.randint(0, 1)}, '{text}'")


def setup(side, rows, del_victims, seed):
    side.must(f"CREATE TABLE t_upd ({MAIN_COLUMNS})")
    side.must(f"CREATE TABLE t_del ({MAIN_COLUMNS})")

    rng = random.Random(seed)
    first_reply = None
    for i in range(rows):
        reply = side.must(f"INSERT INTO t_upd VALUES ({rand_row(rng)})")
        if first_reply is None:
            first_reply = reply
    for i in range(rows):
        side.must(f"INSERT INTO t_del VALUES ({rand_row(rng)})")
    for i in range(del_victims):
        side.must(f"INSERT INTO t_del VALUES ({rand_row(rng)})")
    return first_reply


def checksum(side, sql):
    reply = side.must(sql)
    return hashlib.sha256(reply.encode()).hexdigest(), reply


def correctness_check(sides, rows, label):
    out = {"label": label, "ok": True, "checks": []}
    for name, sql in (
        ("t_upd", "SELECT * FROM t_upd ORDER BY id"),
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
        row = {s.label: s.must(f"SELECT * FROM t_upd WHERE id = {pid}") for s in sides}
        spots[pid] = row
        if len(set(row.values())) != 1:
            out["ok"] = False
    out["spot_rows"] = spots
    return out


# ---- raw (unsynced) latency pass -----------------------------------------

def gen_lat_round(rng, rows, n_select, n_update, n_delete, victim_iter):
    return {
        "select": [rng.randint(1, rows) for _ in range(n_select)],
        "update": [(rng.randint(1, rows), rng.randint(0, 1_000_000)) for _ in range(n_update)],
        "delete": [next(victim_iter) for _ in range(n_delete)],
    }


def run_lat_block(side, phase, shape, payload):
    for item in payload:
        t0 = time.perf_counter()
        if shape == "select":
            r = side(f"SELECT * FROM t_upd WHERE id = {item}")
        elif shape == "update":
            pid, val = item
            r = side(f"UPDATE t_upd SET c_int = {val} WHERE id = {pid}")
        elif shape == "delete":
            r = side(f"DELETE FROM t_del WHERE id = {item}")
        else:
            raise ValueError(shape)
        phase.record(time.perf_counter() - t0, r)


# ---- checkpoint pass (round size 1: statement, SYNC) ----------------------

def run_ckpt_pass(sides, rng, rows, op, reps, victim_iter):
    """Returns per-side lists: stmt_lat[], sync_lat[], windows[] (epoch
    time.time() (before_stmt, after_sync) pairs, for byte attribution)."""
    out = {s.label: {"stmt_lat": [], "sync_lat": [], "windows": []} for s in sides}
    for rep in range(reps):
        if op == "select":
            target = rng.randint(1, rows)
        elif op == "update":
            target = (rng.randint(1, rows), rng.randint(0, 1_000_000))
        elif op == "delete":
            target = next(victim_iter)
        else:
            raise ValueError(op)
        for side in ordered(sides, rep):
            before = time.time()
            t0 = time.perf_counter()
            if op == "select":
                side(f"SELECT * FROM t_upd WHERE id = {target}")
            elif op == "update":
                pid, val = target
                side(f"UPDATE t_upd SET c_int = {val} WHERE id = {pid}")
            elif op == "delete":
                side(f"DELETE FROM t_del WHERE id = {target}")
            stmt_lat = time.perf_counter() - t0
            t0 = time.perf_counter()
            side.must("SYNC")
            sync_lat = time.perf_counter() - t0
            after = time.time()
            rec = out[side.label]
            rec["stmt_lat"].append(stmt_lat)
            rec["sync_lat"].append(sync_lat)
            rec["windows"].append((before, after))
    return out


def summarize_ckpt(vals):
    if not vals:
        return {"n": 0, "mean": 0.0, "min": 0, "max": 0, "stdev": 0.0}
    n = len(vals)
    mean = sum(vals) / n
    var = sum((x - mean) ** 2 for x in vals) / n if n > 1 else 0.0
    return {"n": n, "mean": mean, "min": min(vals), "max": max(vals), "stdev": var ** 0.5}


# ---- strace log parsing (bytes attribution) -------------------------------

OPENAT_RE = re.compile(r'^\d+\s+(\d+\.\d+)\s+openat\(AT_FDCWD,\s*"([^"]*)".*\)\s*=\s*(-?\d+)')
PWRITE_RE = re.compile(r'^\d+\s+(\d+\.\d+)\s+pwrite64\((\d+),.*,\s*(\d+),\s*(\d+)\)\s*=\s*(\d+)\s*$')


def resolve_data_fd(strace_path, data_file_suffix="/kds.db"):
    with open(strace_path, "r", errors="replace") as f:
        for line in f:
            m = OPENAT_RE.match(line)
            if m and m.group(2).endswith(data_file_suffix) and int(m.group(3)) >= 0:
                return int(m.group(3))
    return None


def parse_pwrites(strace_path, fd):
    """Returns a sorted list of (epoch_ts, bytes_written) for pwrite64(fd,...)
    calls in the trace."""
    events = []
    with open(strace_path, "r", errors="replace") as f:
        for line in f:
            m = PWRITE_RE.match(line)
            if not m:
                continue
            ts, wfd, _count, _off, ret = m.groups()
            if int(wfd) != fd:
                continue
            events.append((float(ts), int(ret)))
    events.sort()
    return events


def bucket_bytes(events, windows):
    """Sums event bytes whose timestamp falls in each [start, end] window.
    Windows must be non-overlapping and given in chronological order (which
    run_ckpt_pass's round-by-round construction guarantees)."""
    out = []
    ei = 0
    n = len(events)
    for (start, end) in windows:
        total = 0
        while ei < n and events[ei][0] < start:
            ei += 1
        j = ei
        while j < n and events[j][0] <= end:
            total += events[j][1]
            j += 1
        out.append(total)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["plain", "strace"], required=True)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port-a", type=int, required=True)
    ap.add_argument("--port-b", type=int, required=True)
    ap.add_argument("--strace-a", default=None, help="arm A's strace.out (--mode strace only)")
    ap.add_argument("--strace-b", default=None, help="arm B's strace.out (--mode strace only)")
    ap.add_argument("--rows", type=int, required=True)
    ap.add_argument("--seed", type=int, default=20260901)
    ap.add_argument("--json", required=True)
    ap.add_argument("--lat-rounds", type=int, default=25)
    ap.add_argument("--lat-select", type=int, default=15)
    ap.add_argument("--lat-update", type=int, default=15)
    ap.add_argument("--lat-delete", type=int, default=4)
    ap.add_argument("--ckpt-reps", type=int, default=25)
    args = ap.parse_args()

    sideA = Side("A", args.host, args.port_a)
    sideB = Side("B", args.host, args.port_b)
    sides = [sideA, sideB]

    # Victim budget: the raw pass's deletes (skipped entirely in --mode
    # strace, so 0 there) plus the checkpoint pass's delete reps.
    lat_deletes = 0 if args.mode == "strace" else args.lat_rounds * args.lat_delete
    del_victims = lat_deletes + args.ckpt_reps + 4

    print(f"[setup] mode={args.mode} rows={args.rows} del_victims={del_victims} "
          f"seed={args.seed}")
    t0 = time.perf_counter()
    first_a = setup(sideA, args.rows, del_victims, args.seed)
    first_b = setup(sideB, args.rows, del_victims, args.seed)
    print(f"[setup] done in {time.perf_counter()-t0:.2f}s; first insert reply "
          f"A={first_a!r} B={first_b!r}")

    pre_check = correctness_check(sides, args.rows, "post-load")
    print(f"[correctness] post-load ok={pre_check['ok']}")
    if not pre_check["ok"]:
        print("FATAL: arms disagree immediately after identical load", file=sys.stderr)
        print(json.dumps(pre_check, indent=2), file=sys.stderr)
        sys.exit(1)

    for s in sides:
        s.must("SYNC")  # baseline clean, untimed hygiene

    rng = random.Random(args.seed + 1)
    victim_ids = list(range(args.rows + 1, args.rows + del_victims + 1))
    rng.shuffle(victim_ids)
    victim_iter = iter(victim_ids)

    lat_phases = {}
    lat_floor = {}
    if args.mode == "plain":
        # ---- raw (unsynced) latency pass, interleaved ---------------------
        lat_rounds = [gen_lat_round(rng, args.rows, args.lat_select, args.lat_update,
                                     args.lat_delete, victim_iter)
                      for _ in range(args.lat_rounds)]
        lat_phases = {(tag, shape): Phase(f"{tag}-{shape}")
                      for tag in ("A", "B") for shape in ("select", "update", "delete")}
        lat_t0 = time.perf_counter()
        for r, round_payload in enumerate(lat_rounds):
            for side in ordered(sides, r):
                for shape in ("select", "update", "delete"):
                    run_lat_block(side, lat_phases[(side.label, shape)], shape,
                                  round_payload[shape])
        print(f"[raw latency pass] {args.lat_rounds} rounds done in "
              f"{time.perf_counter()-lat_t0:.2f}s")
        for p in lat_phases.values():
            p.elapsed = sum(p.latencies) if p.latencies else 0.0

        for shape in ("select", "update", "delete"):
            lat = lat_phases[("A", shape)].latencies
            mid = len(lat) // 2
            p1, p2 = Phase(f"Afloor1-{shape}"), Phase(f"Afloor2-{shape}")
            for x in lat[:mid]:
                p1.record(x, "OK")
            for x in lat[mid:]:
                p2.record(x, "OK")
            p1.elapsed = sum(p1.latencies) if p1.latencies else 0.0
            p2.elapsed = sum(p2.latencies) if p2.latencies else 0.0
            lat_floor[("Afloor1", shape)] = p1
            lat_floor[("Afloor2", shape)] = p2

        for s in sides:
            s.must("SYNC")  # reset to clean before the checkpoint-latency pass

    # ---- checkpoint pass, round size 1 ------------------------------------
    ckpt_t0 = time.perf_counter()
    ckpt = {}
    for op in ("select", "update", "delete"):
        ckpt[op] = run_ckpt_pass(sides, rng, args.rows, op, args.ckpt_reps, victim_iter)
    print(f"[checkpoint pass] done in {time.perf_counter()-ckpt_t0:.2f}s")

    ckpt_bytes = {op: {s.label: None for s in sides} for op in ("select", "update", "delete")}
    if args.mode == "strace" and args.strace_a and args.strace_b:
        strace_paths = {"A": args.strace_a, "B": args.strace_b}
        for label, path in strace_paths.items():
            fd = resolve_data_fd(path)
            if fd is None:
                print(f"WARNING: could not resolve data-file fd in {path}", file=sys.stderr)
                continue
            events = parse_pwrites(path, fd)
            for op in ("select", "update", "delete"):
                windows = ckpt[op][label]["windows"]
                ckpt_bytes[op][label] = bucket_bytes(events, windows)
        print(f"[bytes] resolved from strace logs: "
              f"A fd={resolve_data_fd(args.strace_a)} B fd={resolve_data_fd(args.strace_b)}")

    ckpt_floor = {}
    for op in ("select", "update", "delete"):
        if ckpt_bytes[op]["A"] is not None:
            vals = ckpt_bytes[op]["A"]
            mid = len(vals) // 2
            ckpt_floor[op] = {
                "half1_bytes": summarize_ckpt(vals[:mid]),
                "half2_bytes": summarize_ckpt(vals[mid:]),
            }
        sync_vals = ckpt[op]["A"]["sync_lat"]
        mid = len(sync_vals) // 2
        ckpt_floor.setdefault(op, {})
        ckpt_floor[op]["half1_sync_us"] = summarize_ckpt([x * 1e6 for x in sync_vals[:mid]])
        ckpt_floor[op]["half2_sync_us"] = summarize_ckpt([x * 1e6 for x in sync_vals[mid:]])

    post_check = correctness_check(sides, args.rows, "post-writes")
    print(f"[correctness] post-writes ok={post_check['ok']}")

    out = {
        "mode": args.mode,
        "rows": args.rows,
        "del_victims": del_victims,
        "seed": args.seed,
        "lat_rounds": args.lat_rounds if args.mode == "plain" else 0,
        "lat_n": {"select": args.lat_select, "update": args.lat_update,
                  "delete": args.lat_delete} if args.mode == "plain" else {},
        "ckpt_reps": args.ckpt_reps,
        "pre_check": pre_check, "post_check": post_check,
        "lat_phases": {f"{tag}:{shape}": p.summary() for (tag, shape), p in lat_phases.items()},
        "lat_floor": {f"{tag}:{shape}": p.summary() for (tag, shape), p in lat_floor.items()},
        "ckpt": {
            op: {
                side.label: {
                    "bytes": (summarize_ckpt(ckpt_bytes[op][side.label])
                              if ckpt_bytes[op][side.label] is not None else None),
                    "raw_bytes": ckpt_bytes[op][side.label],
                    "stmt_lat_us": summarize_ckpt(
                        [x * 1e6 for x in ckpt[op][side.label]["stmt_lat"]]),
                    "sync_lat_us": summarize_ckpt(
                        [x * 1e6 for x in ckpt[op][side.label]["sync_lat"]]),
                    "measured_under_strace": args.mode == "strace",
                }
                for side in sides
            }
            for op in ("select", "update", "delete")
        },
        "ckpt_floor": ckpt_floor,
        "errors": {s.label: {"count": s.errors, "first": s.first_error} for s in sides},
    }
    with open(args.json, "w") as f:
        json.dump(out, f, indent=2)
    print(f"wrote {args.json}")

    if lat_phases:
        print()
        print(f"{'arm:shape (raw lat)':<20}{'ops':>7}{'qps':>9}{'mean us':>10}{'p0':>8}{'p25':>8}"
              f"{'p50':>8}{'p95':>8}{'p99':>9}{'err':>5}")
        for key, p in list(lat_phases.items()) + list(lat_floor.items()):
            s = p.summary()
            print(f"{s['phase']:<20}{s['ops']:>7}{s['qps']:>9,.0f}{s['mean_us']:>10.1f}"
                  f"{s['p0_us']:>8.1f}{s['p25_us']:>8.1f}{s['p50_us']:>8.1f}"
                  f"{s['p95_us']:>8.1f}{s['p99_us']:>9.1f}{s['errors']:>5}")
    print()
    print(f"{'op:side (ckpt)':<16}{'n':>4}{'bytes mean':>12}{'bytes min':>11}{'bytes max':>11}"
          f"{'sync us mean':>13}")
    for op in ("select", "update", "delete"):
        for side in sides:
            b = ckpt_bytes[op][side.label]
            bsum = summarize_ckpt(b) if b is not None else {"n": 0, "mean": -1, "min": -1,
                                                             "max": -1}
            sy = summarize_ckpt([x * 1e6 for x in ckpt[op][side.label]["sync_lat"]])
            print(f"{op+':'+side.label:<16}{bsum['n']:>4}{bsum['mean']:>12.0f}"
                  f"{bsum['min']:>11}{bsum['max']:>11}{sy['mean']:>13.1f}")

    for s in sides:
        s.close()


if __name__ == "__main__":
    main()
