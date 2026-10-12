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

`--writers` adds open-loop writers beside the readers in every arm (BQ-S1's
C2, `instructions/v3.0.0/workorder-bq-wal-segment-roll.md` §5.1): each sends
an autocommit one-row pk `UPDATE` of `bl_point` at `--writer-rate /
--writers` statements per second, its keys taken from its own stripe so no
two writers touch one row and no row lock confounds what is measured. The
statement latency includes the commit, since an autocommit session's one
reply follows it. Placed like a reader. BQ's cells:

    C1          --arm mixed --readers 6 --rate 400 --placement P
    C2          --arm insert --readers 0 --writers 8 --placement segregated
    C2 isolated --arm isolated --readers 0 --writers 8 --placement segregated
    C3          --arm insert --readers 0 --writers 0
    every cell  --cores 4 --log-level info --ctl-core 0 --durability relaxed|group
                (the per-roll Info line, "[wal] roll seg=...", is written only at info)

Since BQ-S1, `errors` and `first_error` exclude retryable TXN_CONFLICT
refusals, which are counted as `refusals`, and a reader's service time
(`read_service`) no longer includes building its SQL text. Neither figure is
comparable with a results file produced by the driver before BQ-S1.

For lining stalls up with the server's per-roll log lines, every client keeps
the `time.monotonic_ns()` stamp (CLOCK_MONOTONIC, the clock of the engine's
steady_clock on Linux) of each measured sample slower than `SLOW_NS` - its
due instant for an open-loop role, its send for a closed-loop one - and the
run records the stamps of the measured window's edges. The server log is
copied to `--out` as `<tag>.s.log` before the run's directory is removed;
`--log-level info` is what keeps Info-level lines in it.

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
OPEN_LOOP = ("reader", "writer")
PAD = "x" * 200
# A measured sample at least this slow keeps its monotonic stamp; a client
# keeps at most SLOW_CAP of them and counts the rest as dropped.
SLOW_NS = 1_000_000
SLOW_CAP = 20_000
# SHOW META keys that describe the answering core rather than the instance:
# a delta between two reads answered on different cores is meaningless.
PER_CORE_PREFIXES = ("sched_", "wal_group_", "wal_ring_full", "checkpoint_")
PER_CORE_KEYS = ("wal_mean_group_batch", "undo_pages_live", "undo_pages_recycled", "wal_syncs", "wal_interval_syncs", "wal_sync_failures", "wal_first_segment",
                 "wal_segments_removed", "wal_remove_failures")


def is_per_core(key):
    return key.startswith(PER_CORE_PREFIXES) or key in PER_CORE_KEYS


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


def meta_on(port, core):
    """A SHOW META answered on `core`: each read is a transaction head the
    placer (BL-S3) may move the session at, so a session placed by
    `connect_on` is asked again until the reply itself names `core`.
    Returns the session and the reply."""
    for _ in range(2000):
        be = connect_on(port, core)
        m = meta_all(be.conn)
        if m.get("core") == core:
            return be, m
        be.conn.close()
    raise SystemExit(f"SHOW META not answered on core {core}")


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
    out = {"role": args.role, "index": args.index, "core": meta_all(be.conn).get("core"),
           "lat": [], "late": [], "ok": 0, "errors": 0, "refusals": 0, "first_error": None,
           "slow": [], "slow_dropped": 0, "first_t0": None, "last_t1": None}

    def keep(t0, t1, kind, err, late=None):
        """One measured sample. `slow` keeps [monotonic ns of the due instant
        (open loop) or of the send (closed loop), latency ns] - the latency
        from the schedule where there is one."""
        mono1 = time.monotonic_ns()
        out["lat"].append(t1 - t0)
        # The kept samples' own span, which a rate over them divides by.
        if out["first_t0"] is None:
            out["first_t0"] = t0
        out["last_t1"] = t1
        if late is not None:
            out["late"].append(late)
        ns = round((t1 - t0 if late is None else late) * 1e9)
        if ns >= SLOW_NS:
            if len(out["slow"]) < SLOW_CAP:
                out["slow"].append([mono1 - ns, ns])
            else:
                out["slow_dropped"] += 1
        if kind == "refused":
            out["refusals"] += 1
        elif kind:
            out["errors"] += 1
            out["first_error"] = out["first_error"] or err
        else:
            out["ok"] += 1

    while time.time() < args.start_at:
        time.sleep(0.0005)
    measure_from = args.start_at + args.warmup
    stop_at = measure_from + args.seconds
    rng = random.Random(args.index * 7919 + 1)
    if args.role in OPEN_LOOP:
        interval = 1.0 / args.rate
        # Sessions of one role are offset within one interval so their sends
        # interleave. Writers follow the readers in the role list, so a
        # writer's own index is its place after them; its keys are its own
        # stripe (w + 1 modulo `--writers`), so no two writers share a row.
        if args.role == "reader":
            slot, peers = args.index, args.readers
        else:
            slot, peers = args.index - args.readers, args.writers
            stripe = args.point_rows // args.writers
        due = args.start_at + interval * slot / max(1, peers)
        while True:
            now = time.time()
            if now >= stop_at:
                break
            if now < due:
                time.sleep(due - now)
            if args.role == "reader":
                sql = f"SELECT * FROM bl_point WHERE id = {rng.randint(1, args.point_rows)}"
            else:
                sql = (f"UPDATE bl_point SET c1 = c1 + 1 WHERE id = "
                       f"{slot + 1 + args.writers * rng.randrange(stripe)}")
            t0 = time.time()
            _r, kind, err = be.run(sql)
            t1 = time.time()
            if due >= measure_from:
                keep(t0, t1, kind, err, late=t1 - due)
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
                keep(t0, t1, kind, err)
    # The core that answered the closing SHOW META, itself a transaction head
    # the placer (BL-S3) may move the session at - not a detection of moves
    # during the run, which the run's `moves` counts instead.
    out["core_end"] = meta_all(be.conn).get("core")
    print(json.dumps(out))


def pct(xs):
    xs = sorted(xs)
    if not xs:
        return {}
    return {**{f"p{q}_us": round(nearest_rank(xs, q) * 1e6, 1) for q in (0, 25, 50, 95, 99)},
            "p99.9_us": round(nearest_rank(xs, 99.9) * 1e6, 1),
            "max_us": round(xs[-1] * 1e6, 1), "n": len(xs)}


def placement_targets(args, roles):
    """Each client's core, -1 for wherever the kernel put it.
    `same`: every session on `--core` (the one-core mixed cell). `kernel`: as
    accepted. `segregated`: the walker and inserter on the highest core, the
    readers and writers spread over the others - BL-R2's partition, forced by
    the driver.
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
        if role in OPEN_LOOP:
            out.append(first + r % span)
            r += 1
        else:
            out.append(batch)
    return out


def run_one(args, run):
    tag = (f"bl-mixed-k{args.cores}-{args.placement}-{args.arm}-{args.durability}-w{args.writers}"
           f"-slice{args.slice or 'dflt'}-r{run}")
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
            proc, err, ctl = start_kds(args, wd, args.port, log_level=args.log_level)
        finally:
            if old is None:
                os.environ.pop("KDS_BL_PAGES_PER_SLICE", None)
            else:
                os.environ["KDS_BL_PAGES_PER_SLICE"] = old
        setup(args, ctl)
        # `--ctl-core` fixes the core each control read is answered on, so the
        # per-core fields of m0/m1 (`sched_*`, `wal_syncs`, core 0's
        # `wal_first_segment`) come from one core in every run. Placing the
        # session at connect is not enough: every SHOW META is a transaction
        # head the placer (BL-S3) may move the session at, so a read answered
        # elsewhere is read again on `--ctl-core` (`meta_on`).
        if args.ctl_core >= 0:
            ctl.conn.close()
            ctl = connect_on(args.port, args.ctl_core)
        elif args.placement == "same" and args.cores > 1:
            ctl.conn.close()
            ctl = connect_on(args.port, args.core)
        roles = ["reader"] * args.readers + ["writer"] * args.writers
        if args.arm in ("walk", "mixed"):
            roles.append("walker")
        if args.arm in ("insert", "mixed"):
            roles.append("inserter")
        targets = placement_targets(args, roles)
        start_at = time.time() + 3.0 + 0.1 * len(roles) * args.cores
        # The monotonic equivalent of a wall instant the clients schedule by,
        # for lining the window up with the server's log.
        wall0, mono0 = time.time(), time.monotonic_ns()

        def mono_of(t):
            return mono0 + round((t - wall0) * 1e9)

        rates = {"reader": args.rate / max(1, args.readers),
                 "writer": args.writer_rate / max(1, args.writers)}
        for i, role in enumerate(roles):
            cl = [sys.executable, os.path.abspath(__file__), "--client", "--role", role,
                  "--port", str(args.port), "--index", str(i), "--readers", str(args.readers),
                  "--writers", str(args.writers),
                  "--core", str(targets[i]), "--rate", str(rates.get(role, 0.0)),
                  "--point-rows", str(args.point_rows), "--batch", str(args.batch),
                  "--walk-rows", str(args.walk_rows),
                  "--start-at", repr(start_at), "--warmup", str(args.warmup),
                  "--seconds", str(args.seconds)]
            if args.client_cpus:
                cpus = args.client_cpus.split(",")
                cl = ["taskset", "-c", cpus[i % len(cpus)]] + cl
            procs.append(subprocess.Popen(cl, stdout=subprocess.PIPE, text=True))
        rereads = 0

        def ctl_meta():
            nonlocal ctl, rereads
            m = meta_all(ctl.conn)
            if args.ctl_core >= 0 and m.get("core") != args.ctl_core:
                ctl.conn.close()
                ctl, m = meta_on(args.port, args.ctl_core)
                rereads += 1
            return m

        time.sleep(max(0.0, start_at + args.warmup - time.time()))
        m0 = ctl_meta()
        # Stamped after the read that counts: a re-read moves the snapshot.
        m0_mono = time.monotonic_ns()
        time.sleep(max(0.0, start_at + args.warmup + args.seconds / 2 - time.time()))
        m_mid = ctl_meta()
        outs = []
        for p in procs:
            o, _ = p.communicate(timeout=args.warmup + args.seconds + 600)
            outs.append(json.loads(o.strip().splitlines()[-1]))
        m1 = ctl_meta()
        m1_mono = time.monotonic_ns()
        # Without `--ctl-core` the placer may have answered the three reads on
        # different cores; the delta then keeps only the instance-wide keys.
        ctl_cores = (m0.get("core"), m_mid.get("core"), m1.get("core"))
        ctl_moved = len(set(ctl_cores)) > 1
        meta_delta = {k: m1[k] - m0[k] for k in m1
                      if k in m0 and not k.endswith("_longest_us") and not is_gauge(k)
                      and not (ctl_moved and is_per_core(k))}
        # Balancer moves (BL-S3) per core, summed: `moves` raw from the
        # server's start (setup and warmup included), `moves_in_window` over
        # m0..m1. A move makes a session's connect core a wrong attribution.
        moves = sum(int(m1.get(f"load_c{k}_moves_out", 0)) for k in range(args.cores))
        moves_in_window = sum(int(meta_delta.get(f"load_c{k}_moves_out", 0))
                              for k in range(args.cores))
        # Each inserter's kept rows over its kept samples' own span, summed:
        # statements straddling the window's edges are dropped, so `--seconds`
        # would understate the rate.
        ins_rate = 0.0
        for o in outs:
            if o["role"] == "inserter" and o["first_t0"] is not None:
                span = o["last_t1"] - o["first_t0"]
                if span > 0:
                    ins_rate += o["ok"] * args.batch / span
        # The log's byte rate over m0..m1 - a window that differs from the
        # inserters' (theirs is their kept samples' span).
        log_bps = None
        if "wal_append_lsn" in meta_delta and m1_mono > m0_mono:
            log_bps = round(meta_delta["wal_append_lsn"] / ((m1_mono - m0_mono) / 1e9), 1)
        after = host_state()
        readers = [o for o in outs if o["role"] == "reader"]
        writers = [o for o in outs if o["role"] == "writer"]
        res = {
            "arm": args.arm, "cores": args.cores, "placement": args.placement,
            "targets": targets, "roles": roles, "slice": args.slice,
            "run": run, "durability": args.durability, "rate": args.rate, "readers": args.readers,
            "writers": args.writers, "writer_rate": args.writer_rate, "batch": args.batch,
            "walk_rows": args.walk_rows, "seconds": args.seconds, "warmup": args.warmup,
            "log_level": args.log_level, "ctl_core": args.ctl_core,
            "wait_s": round(wait, 1), "host_before": before, "host_after": after,
            "read_service": pct([x for o in readers for x in o["lat"]]),
            "read_from_schedule": pct([x for o in readers for x in o["late"]]),
            # `read_by_core` and `writer_by_core` group by each session's
            # connect core; sound only while `per_core_valid` (no move).
            "by_core_attribution": "connect_core", "per_core_valid": moves - rereads == 0,  # the control session's own moves excluded
            "moves": moves, "moves_in_window": moves_in_window,
            "read_by_core": {str(c): pct([x for o in readers if o["core"] == c for x in o["late"]])
                             for c in sorted({o["core"] for o in readers})},
            "walker": pct([x for o in outs if o["role"] == "walker" for x in o["lat"]]),
            "writer_service": pct([x for o in writers for x in o["lat"]]),
            "writer_from_schedule": pct([x for o in writers for x in o["late"]]),
            "writer_by_session": [{"index": o["index"], "core": o["core"], "core_end": o["core_end"],
                                   "service": pct(o["lat"]), "from_schedule": pct(o["late"]),
                                   "errors": o["errors"], "refusals": o["refusals"]} for o in writers],
            "writer_by_core": {str(c): pct([x for o in writers if o["core"] == c for x in o["late"]])
                               for c in sorted({o["core"] for o in writers})},
            "inserter": pct([x for o in outs if o["role"] == "inserter" for x in o["lat"]]),
            "inserter_rows_per_s": round(ins_rate, 1),
            "log_bytes_per_s": log_bps,
            "cores_seen": [o["core"] for o in outs],
            "cores_end": [o["core_end"] for o in outs],
            # `errors` excludes retryable TXN_CONFLICT refusals, counted apart.
            "errors": sum(o["errors"] for o in outs),
            "refusals": sum(o["refusals"] for o in outs),
            "first_error": next((o["first_error"] for o in outs if o["first_error"]), None),
            # CLOCK_MONOTONIC ns: the measured window's edges, the two meta
            # reads, and each client's measured samples slower than
            # `slow_threshold_us` as [due or send ns, latency ns] - the
            # latency from the schedule for an open-loop role. A client keeps
            # at most `slow_cap_per_client`; `slow_dropped` counts the rest.
            "measure_from_mono_ns": mono_of(start_at + args.warmup),
            "stop_at_mono_ns": mono_of(start_at + args.warmup + args.seconds),
            "m0_mono_ns": m0_mono, "m1_mono_ns": m1_mono,
            "slow_threshold_us": SLOW_NS / 1000, "slow_cap_per_client": SLOW_CAP,
            "slow_dropped": sum(o["slow_dropped"] for o in outs),
            "clients": [{k: o[k] for k in ("role", "index", "core", "core_end", "ok", "errors",
                                           "refusals", "slow_dropped", "slow", "first_t0",
                                           "last_t1")} for o in outs],
            # The per-core keys (`is_per_core`) describe the core that
            # answered each control read, recorded here; `load_c<k>_*`
            # describes each core. With `meta_ctl_moved` the per-core keys
            # are left out of `meta_delta`. `meta_core` is m1's, as before.
            "meta_core": m1.get("core"),
            "meta_core_m0": ctl_cores[0], "meta_core_mid": ctl_cores[1], "meta_core_m1": ctl_cores[2],
            "meta_ctl_moved": ctl_moved, "meta_rereads": rereads,
            "meta_delta": meta_delta,
            "meta_gauges_mid": {k: v for k, v in m_mid.items() if is_gauge(k)},
            "meta_longest": {k: v for k, v in m1.items() if k.endswith("_longest_us")},
            # Raw, so values `meta_delta` differences away (`wal_*` positions,
            # `recovery_checkpoint_us`) stay readable.
            "meta_m0": m0, "meta_m1": m1,
            "bin_sha256": sha, "data_device": data_device(wd),
        }
        if args.out:
            os.makedirs(args.out, exist_ok=True)
            with open(os.path.join(args.out, f"{tag}.json"), "w") as f:
                json.dump(res, f, indent=1)
        rs, rl = res["read_from_schedule"], res["read_service"]
        ws, wl = res["writer_from_schedule"], res["writer_service"]
        print(f"{tag} load {before['loadavg'].split()[0]}->{after['loadavg'].split()[0]} "
              f"errors={res['errors']} refusals={res['refusals']} "
              f"read p50={rl.get('p50_us')} p99={rl.get('p99_us')} p99.9={rl.get('p99.9_us')} "
              f"sched p99={rs.get('p99_us')} p99.9={rs.get('p99.9_us')} max={rs.get('max_us')} "
              f"write p50={wl.get('p50_us')} p99={wl.get('p99_us')} p99.9={wl.get('p99.9_us')} "
              f"sched p99={ws.get('p99_us')} p99.9={ws.get('p99.9_us')} max={ws.get('max_us')} "
              f"walker p50={res['walker'].get('p50_us')} "
              f"ins p50={res['inserter'].get('p50_us')} rows/s={res['inserter_rows_per_s']} "
              f"log B/s={res['log_bytes_per_s']} moves={moves} moves_in_window={moves_in_window} "
              f"cores={sorted(set(map(str, res['cores_seen'])))}",
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
        # The server log carries the engine's own lines (BQ-S1's per-roll
        # line among them) and `wd` is about to go, so it is kept beside the
        # JSON - after the server has exited and flushed it.
        log = os.path.join(wd, "s.log")
        if args.out and os.path.exists(log):
            os.makedirs(args.out, exist_ok=True)
            shutil.copy2(log, os.path.join(args.out, f"{tag}.s.log"))
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
    p.add_argument("--writers", type=int, default=0, help="open-loop one-row pk UPDATE sessions")
    p.add_argument("--writer-rate", type=float, default=800.0,
                   help="UPDATEs per second, all writers together")
    p.add_argument("--point-rows", type=int, default=10_000)
    p.add_argument("--walk-rows", type=int, default=10_000)
    p.add_argument("--batch", type=int, default=1024)
    p.add_argument("--slice", type=int, default=0, help="pages per walk slice (0: the build's)")
    p.add_argument("--seconds", type=float, default=10.0)
    p.add_argument("--warmup", type=float, default=2.0)
    p.add_argument("--runs", type=int, default=5)
    p.add_argument("--durability", default="relaxed")
    p.add_argument("--log-level", default="warn", help="the server's `log_level` (BQ runs `info`)")
    p.add_argument("--ctl-core", type=int, default=-1,
                   help="the control session's core (-1: as accepted, or `--core` under `same`)")
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
    if args.writers > args.point_rows:
        p.error("--writers above --point-rows leaves a writer an empty stripe")
    os.makedirs(args.workdir, exist_ok=True)
    copy = os.path.join(args.workdir, "kds_server.blmixed")
    shutil.copy2(args.bin, copy)
    args.bin = copy
    for run in range(args.runs):
        run_one(args, run)
    return 0


if __name__ == "__main__":
    sys.exit(main())
