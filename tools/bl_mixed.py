#!/usr/bin/env python3
"""BL-S1's mixed cell: the priority premise (BL §5.1).

`instructions/v3.0.0/workorder-bl-session-balancer-priority-affinity.md`.
One core is shared by a walker (an unindexed one-row SELECT over
`--walk-rows` wide rows, so the walk spans hundreds of pages and reaches its
slice boundary), an inserter (a `--batch`-row multi-row INSERT, which does not
yield), and `--readers` pk point readers driven open-loop at `--rate`
statements per second in total. The isolated arm runs the readers alone.

A reader's latency is taken two ways: from the send (service time) and from
the instant the open-loop schedule said to send (so a reader held behind a
non-yielding statement is charged for the wait it could not start in,
rather than quietly sending later - no coordinated omission).

Arms (`--arm`): `isolated` (readers only), `walk` (readers + walker),
`insert` (readers + inserter), `mixed` (all three). `--placement` decides
each session's core (`placement_targets`); a session reaches its core by
reconnecting until `SHOW META` says so.

`--slice` needs a measurement build that reads `KDS_BL_PAGES_PER_SLICE`; the
override is never committed (`bench/v3.0.0/results-bl-s1-*.md` names the
patch), and a build without it ignores the variable.

Every client is its own process. Ports have no default (bench/README.md
rule 5); each run starts a fresh server from one copy of `--bin` (rule 3)
and records the load and the data device (rules 2, 4).

    tools/bl_mixed.py --bin build-release/kds_server --port P --cores 1 \\
        --arm mixed --runs 5 --out bench/v3.0.0/archive/...
"""

import argparse
import hashlib
import json
import os
import random
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_common import nearest_rank  # noqa: E402
from bb_overhead_benchmark import host_state, meta_all, wait_for_load  # noqa: E402
from ba_census import Kds, data_device, is_gauge, start_kds  # noqa: E402

ARMS = ("isolated", "walk", "insert", "mixed")
PAD = "x" * 200


def connect_on(port, core):
    be = Kds(port)
    tries = 0
    while core >= 0 and meta_all(be.conn).get("core") != core:
        be.conn.close()
        be = Kds(port)
        tries += 1
        if tries > 2000:
            raise SystemExit(f"core {core} not reached")
    return be


def must(be, sql):
    r, kind, err = be.run(sql)
    if kind:
        raise SystemExit(f"setup: {sql[:80]!r} -> {err!r}")
    return r


def setup(args, be):
    must(be, "CREATE TABLE bl_point (id int64, c0 int64, c1 int64) BTREE")
    for at in range(0, args.point_rows, 200):
        must(be, "INSERT INTO bl_point VALUES " + ", ".join(
            f"({i}, {i})" for i in range(at, min(at + 200, args.point_rows))))
    must(be, "CREATE TABLE bl_walk (id int64, c0 int64, pad char(200)) BTREE")
    for at in range(0, args.walk_rows, 100):
        must(be, "INSERT INTO bl_walk VALUES " + ", ".join(
            f"({i}, '{PAD}')" for i in range(at, min(at + 100, args.walk_rows))))
    must(be, "CREATE TABLE bl_ins (id int64, c0 int64, c1 int64) BTREE")


def client(args):
    be = connect_on(args.port, args.core)
    out = {"role": args.role, "core": meta_all(be.conn).get("core"), "lat": [], "late": [],
           "errors": 0, "first_error": None}
    while time.time() < args.start_at:
        time.sleep(0.0005)
    measure_from = args.start_at + args.warmup
    stop_at = measure_from + args.seconds
    rng = random.Random(args.index * 7919 + 1)
    if args.role == "reader":
        interval = 1.0 / args.rate
        # Readers are offset within one interval so their sends interleave.
        due = args.start_at + interval * args.index / max(1, args.readers)
        while True:
            now = time.time()
            if now >= stop_at:
                break
            if now < due:
                time.sleep(due - now)
            t0 = time.time()
            _r, kind, err = be.run(f"SELECT * FROM bl_point WHERE id = {rng.randint(1, args.point_rows)}")
            t1 = time.time()
            if due >= measure_from:
                out["lat"].append(t1 - t0)
                out["late"].append(t1 - due)
                if kind:
                    out["errors"] += 1
                    out["first_error"] = out["first_error"] or err
            due += interval
    else:
        n = 0
        while time.time() < stop_at:
            if args.role == "walker":
                # Unindexed and not aggregated, so its outermost walk is the
                # one that reaches the slice boundary (an aggregate does not).
                sql = f"SELECT * FROM bl_walk WHERE c0 = {rng.randrange(args.walk_rows)}"
            else:
                n += 1
                sql = "INSERT INTO bl_ins VALUES " + ", ".join(f"({n}, {j})" for j in range(args.batch))
            t0 = time.time()
            _r, kind, err = be.run(sql)
            t1 = time.time()
            if t0 >= measure_from and t1 <= stop_at:
                out["lat"].append(t1 - t0)
                if kind:
                    out["errors"] += 1
                    out["first_error"] = out["first_error"] or err
    print(json.dumps(out))


def pct(xs):
    xs = sorted(xs)
    if not xs:
        return {}
    return {**{f"p{q}_us": round(nearest_rank(xs, q) * 1e6, 1) for q in (0, 25, 50, 95, 99)},
            "max_us": round(xs[-1] * 1e6, 1), "n": len(xs)}


def placement_targets(args, roles):
    """Each client's core, -1 for wherever the kernel put it.
    `same`: every session on `--core` (the one-core mixed cell). `kernel`: as
    accepted. `segregated`: the walker and inserter on the highest core, the
    readers spread over the others - BL-R2's partition, forced by the driver.
    `spread`: every session spread round-robin over all cores, roles mixed -
    a balanced placement that ignores what each session does."""
    if args.placement == "same":
        return [args.core] * len(roles)
    if args.placement == "kernel":
        return [-1] * len(roles)
    if args.placement == "spread":
        return [i % args.cores for i in range(len(roles))]
    # `segregated-no0` also keeps the readers off core 0, the log's owner;
    # `batch0` puts the batch sessions on core 0 and the readers on the rest.
    if args.placement == "batch0":
        batch, first, span = 0, 1, max(1, args.cores - 1)
    else:
        batch = args.cores - 1
        first = 1 if args.placement == "segregated-no0" else 0
        span = max(1, batch - first)
    out, r = [], 0
    for role in roles:
        if role == "reader":
            out.append(first + r % span)
            r += 1
        else:
            out.append(batch)
    return out


def run_one(args, run):
    tag = f"bl-mixed-k{args.cores}-{args.placement}-{args.arm}-slice{args.slice or 'dflt'}-r{run}"
    wait = wait_for_load(args.max_load, args.max_wait)
    before = host_state()
    wd = os.path.join(args.workdir, tag)
    os.makedirs(wd, exist_ok=True)
    procs, proc, err = [], None, None
    env = dict(os.environ)
    if args.slice:
        env["KDS_BL_PAGES_PER_SLICE"] = str(args.slice)  # the measurement build's override
    try:
        with open(args.bin, "rb") as f:
            sha = hashlib.sha256(f.read()).hexdigest()
        old = os.environ.get("KDS_BL_PAGES_PER_SLICE")
        os.environ.update(env)
        try:
            proc, err, ctl = start_kds(args, wd, args.port)
        finally:
            if old is None:
                os.environ.pop("KDS_BL_PAGES_PER_SLICE", None)
            else:
                os.environ["KDS_BL_PAGES_PER_SLICE"] = old
        setup(args, ctl)
        if args.placement == "same" and args.cores > 1:
            ctl.conn.close()
            ctl = connect_on(args.port, args.core)
        roles = ["reader"] * args.readers
        if args.arm in ("walk", "mixed"):
            roles.append("walker")
        if args.arm in ("insert", "mixed"):
            roles.append("inserter")
        targets = placement_targets(args, roles)
        start_at = time.time() + 3.0 + 0.1 * len(roles) * args.cores
        for i, role in enumerate(roles):
            cl = [sys.executable, os.path.abspath(__file__), "--client", "--role", role,
                  "--port", str(args.port), "--index", str(i), "--readers", str(args.readers),
                  "--core", str(targets[i]), "--rate", str(args.rate / args.readers),
                  "--point-rows", str(args.point_rows), "--batch", str(args.batch),
                  "--walk-rows", str(args.walk_rows),
                  "--start-at", repr(start_at), "--warmup", str(args.warmup),
                  "--seconds", str(args.seconds)]
            if args.client_cpus:
                cpus = args.client_cpus.split(",")
                cl = ["taskset", "-c", cpus[i % len(cpus)]] + cl
            procs.append(subprocess.Popen(cl, stdout=subprocess.PIPE, text=True))
        time.sleep(max(0.0, start_at + args.warmup - time.time()))
        m0 = meta_all(ctl.conn)
        time.sleep(max(0.0, start_at + args.warmup + args.seconds / 2 - time.time()))
        m_mid = meta_all(ctl.conn)
        outs = []
        for p in procs:
            o, _ = p.communicate(timeout=args.warmup + args.seconds + 600)
            outs.append(json.loads(o.strip().splitlines()[-1]))
        m1 = meta_all(ctl.conn)
        after = host_state()
        readers = [o for o in outs if o["role"] == "reader"]
        res = {
            "arm": args.arm, "cores": args.cores, "placement": args.placement,
            "targets": targets, "roles": roles, "slice": args.slice,
            "run": run, "rate": args.rate, "readers": args.readers, "batch": args.batch,
            "walk_rows": args.walk_rows, "seconds": args.seconds,
            "wait_s": round(wait, 1), "host_before": before, "host_after": after,
            "read_service": pct([x for o in readers for x in o["lat"]]),
            "read_from_schedule": pct([x for o in readers for x in o["late"]]),
            "read_by_core": {str(c): pct([x for o in readers if o["core"] == c for x in o["late"]])
                             for c in sorted({o["core"] for o in readers})},
            "walker": pct([x for o in outs if o["role"] == "walker" for x in o["lat"]]),
            "inserter": pct([x for o in outs if o["role"] == "inserter" for x in o["lat"]]),
            "cores_seen": [o["core"] for o in outs],
            "errors": sum(o["errors"] for o in outs),
            "first_error": next((o["first_error"] for o in outs if o["first_error"]), None),
            # `sched_*` describes the control session's core only (`ctl`,
            # placed on `--core` for `same`); `load_c<k>_*` describes each.
            "meta_core": m1.get("core"),
            "meta_delta": {k: m1[k] - m0[k] for k in m1
                           if k in m0 and not k.endswith("_longest_us") and not is_gauge(k)},
            "meta_gauges_mid": {k: v for k, v in m_mid.items() if is_gauge(k)},
            "meta_longest": {k: v for k, v in m1.items() if k.endswith("_longest_us")},
            "bin_sha256": sha, "data_device": data_device(wd),
        }
        if args.out:
            os.makedirs(args.out, exist_ok=True)
            with open(os.path.join(args.out, f"{tag}.json"), "w") as f:
                json.dump(res, f, indent=1)
        rs, rl = res["read_from_schedule"], res["read_service"]
        print(f"{tag} load {before['loadavg'].split()[0]}->{after['loadavg'].split()[0]} "
              f"errors={res['errors']} read p50={rl.get('p50_us')} p99={rl.get('p99_us')} "
              f"sched p99={rs.get('p99_us')} max={rs.get('max_us')} walker p50={res['walker'].get('p50_us')} "
              f"ins p50={res['inserter'].get('p50_us')} cores={sorted(set(map(str, res['cores_seen'])))}",
              flush=True)
        return res
    finally:
        for p in procs:
            if p.poll() is None:
                p.kill()
                p.wait()
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(30)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        if err is not None:
            err.close()
        shutil.rmtree(wd, ignore_errors=True)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--arm", choices=ARMS, default="mixed")
    p.add_argument("--cores", type=int, default=1)
    p.add_argument("--core", type=int, default=0, help="the core of `--placement same`")
    p.add_argument("--placement", choices=("same", "kernel", "segregated", "segregated-no0", "batch0", "spread"),
                   default="same")
    p.add_argument("--extra-conf", action="append", default=[],
                   help="a `key = value` line appended to the KDS config (repeatable)")
    p.add_argument("--readers", type=int, default=4)
    p.add_argument("--rate", type=float, default=2000.0, help="point reads per second, all readers together")
    p.add_argument("--point-rows", type=int, default=10_000)
    p.add_argument("--walk-rows", type=int, default=10_000)
    p.add_argument("--batch", type=int, default=1024)
    p.add_argument("--slice", type=int, default=0, help="pages per walk slice (0: the build's)")
    p.add_argument("--seconds", type=float, default=10.0)
    p.add_argument("--warmup", type=float, default=2.0)
    p.add_argument("--runs", type=int, default=5)
    p.add_argument("--durability", default="relaxed")
    p.add_argument("--frames", type=int, default=65536)
    p.add_argument("--server-cpus", default=None)
    p.add_argument("--client-cpus", default=None)
    p.add_argument("--bin")
    p.add_argument("--port", type=int)
    p.add_argument("--workdir", default=os.path.expanduser("~/bench-runs/bl/work"))
    p.add_argument("--out", default=None)
    p.add_argument("--max-load", type=float, default=1.5)
    p.add_argument("--max-wait", type=float, default=120)
    p.add_argument("--client", action="store_true", help=argparse.SUPPRESS)
    p.add_argument("--role", default="reader", help=argparse.SUPPRESS)
    p.add_argument("--index", type=int, default=0, help=argparse.SUPPRESS)
    p.add_argument("--start-at", type=float, default=0.0, help=argparse.SUPPRESS)
    args = p.parse_args()
    if args.client:
        client(args)
        return 0
    if not (args.bin and args.port):
        p.error("needs --bin and --port")
    os.makedirs(args.workdir, exist_ok=True)
    copy = os.path.join(args.workdir, "kds_server.blmixed")
    shutil.copy2(args.bin, copy)
    args.bin = copy
    for run in range(args.runs):
        run_one(args, run)
    return 0


if __name__ == "__main__":
    sys.exit(main())
