#!/usr/bin/env python3
"""XG2's re-baseline: what the portal-close workaround cost, refunded.

**What this measures and why it is a client A/B rather than a server one.**
`kwp_session.cpp`'s skip-to-sync discarded every frame up to the next
`C_SYNC`, including a `C_CLOSE` a client had pipelined behind the statement
that just failed - so the portal leaked on exactly the statements that most
needed it closed, and at `kMaxSessionPortals` the session refused every
further `C_BIND` until the 60 s idle sweep freed one. XE's session worked
around it in `tools/kwp.py` by sending `C_CLOSE` as its **own frame after
the statement's `S_READY`**, which costs an extra round trip on every
*successful* statement (XE §3 measured 28.1 -> 39.2 us p50 on its host).

XG-R8 fixed it in the server: a statement error erases the portal it was
executing. The close may therefore ride the statement's own batch again,
and this probe measures what that gives back.

**Both arms speak to the same server binary and differ only in framing**,
which is why the two `execute` shapes are written out here rather than
taken from `tools/kwp.py` - the driver must not measure whichever way that
file happens to be today, and a reader must be able to see both arms side
by side:

    batched  PARSE BIND EXECUTE CLOSE SYNC   -> one round trip
    trailing PARSE BIND EXECUTE SYNC         -> read to S_READY
             CLOSE SYNC                      -> a second round trip

`--durability relaxed --cores 1` by default, following XE §3's isolation
cell: the point is the per-statement round trip, and a device sync in the
middle of it would drown the quantity being measured.

Usage:
    bench/portal_close_tax_probe.py --server build-release/kds_server \\
        --workdir ~/bench-xg --label xg2-tax --port 16400 \\
        --statements 2000 --repeats 3 --json out.json
"""

import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))

from kwp import (  # noqa: E402
    Connection, _Writer, frame,
    C_PARSE, C_BIND, C_EXECUTE, C_CLOSE, C_SYNC,
    S_PARSE_OK, S_BIND_OK, S_COMPLETE, S_ERROR, S_READY, S_NOTICE,
)


def _bind_payload(portal):
    return _Writer().s(portal).s("").u16(0).take()


def _drain_to_ready(conn):
    """Reads frames until `S_READY`, raising on an unexpected error."""
    err = None
    while True:
        ftype, _flags, payload = conn._recv_frame()
        if ftype == S_READY:
            return err
        if ftype == S_ERROR:
            err = conn._error(payload)
        elif ftype in (S_PARSE_OK, S_BIND_OK, S_COMPLETE, S_NOTICE):
            continue


def run_batched(conn, sql, portal):
    """The reverted shape: the close rides the statement's own batch."""
    b = frame(C_PARSE, _Writer().s("").text(sql).take())
    b += frame(C_BIND, _bind_payload(portal))
    b += frame(C_EXECUTE, _Writer().s(portal).u32(0).take())
    b += frame(C_CLOSE, _Writer().u8(2).s(portal).take())
    b += frame(C_SYNC)
    conn.sock.sendall(b)
    return _drain_to_ready(conn)


def run_trailing(conn, sql, portal):
    """XE's workaround: the close is its own frame, after `S_READY`."""
    b = frame(C_PARSE, _Writer().s("").text(sql).take())
    b += frame(C_BIND, _bind_payload(portal))
    b += frame(C_EXECUTE, _Writer().s(portal).u32(0).take())
    b += frame(C_SYNC)
    conn.sock.sendall(b)
    err = _drain_to_ready(conn)
    b2 = frame(C_CLOSE, _Writer().u8(2).s(portal).take())
    b2 += frame(C_SYNC)
    conn.sock.sendall(b2)
    _drain_to_ready(conn)
    return err


ARMS = {"batched": run_batched, "trailing": run_trailing}


def write_conf(workdir, port, cores, durability):
    conf = os.path.join(workdir, "kds.conf")
    with open(conf, "w") as f:
        f.write("data_file = %s\nport = %d\ncores = %d\ndurability = %s\n"
                "log_file = s.log\nlog_dir = %s\nlog_level = info\n"
                % (os.path.join(workdir, "s.db"), port, cores, durability, workdir))
    return conf


def wait_up(port, proc, deadline_s=20.0):
    end = time.time() + deadline_s
    while time.time() < end:
        if proc.poll() is not None:
            return False
        try:
            c = Connection("127.0.0.1", port, timeout=2.0)
            c.close()
            return True
        except OSError:
            time.sleep(0.05)
    return False


def measure(port, arm, statements, start_id):
    """One arm's per-statement round trip, in microseconds."""
    conn = Connection("127.0.0.1", port, timeout=30.0)
    run = ARMS[arm]
    latencies = []
    try:
        for i in range(statements):
            sql = "INSERT INTO tax VALUES (%d, %d)" % (start_id + i, i)
            t0 = time.perf_counter()
            err = run(conn, sql, "p%d" % i)
            latencies.append((time.perf_counter() - t0) * 1e6)
            if err is not None:
                raise RuntimeError("statement %d failed: %s" % (i, err))
    finally:
        conn.close()
    return latencies


def summarize(latencies):
    s = sorted(latencies)
    def pct(q):
        return s[min(len(s) - 1, int(q * len(s)))]
    return {
        "ops": len(s),
        "p0_us": round(s[0], 1),
        "p25_us": round(pct(0.25), 1),
        "p50_us": round(pct(0.50), 1),
        "p95_us": round(pct(0.95), 1),
        "p99_us": round(pct(0.99), 1),
        "max_us": round(s[-1], 1),
        "mean_us": round(statistics.fmean(s), 1),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--label", default="xg2-tax")
    ap.add_argument("--port", type=int, default=16400)
    ap.add_argument("--cores", type=int, default=1)
    ap.add_argument("--durability", default="relaxed")
    ap.add_argument("--statements", type=int, default=2000)
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    workdir = os.path.join(args.workdir, args.label)
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir, exist_ok=True)
    conf = write_conf(workdir, args.port, args.cores, args.durability)

    out = {"label": args.label, "cores": args.cores, "durability": args.durability,
           "statements": args.statements, "repeats": args.repeats, "arms": {}}

    proc = subprocess.Popen([args.server, "--config", conf],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_up(args.port, proc):
            out["error"] = "server did not listen"
            print(json.dumps(out, indent=2))
            return 1

        setup = Connection("127.0.0.1", args.port, timeout=10.0)
        setup.execute("CREATE TABLE tax (id int64, v int64)")
        setup.close()

        # **Interleaved**, per the measurement rule: one repeat of each arm
        # in turn, never all of one then all of the other, so a drift in the
        # machine lands on both.
        per_arm = {arm: [] for arm in ARMS}
        next_id = 1
        for r in range(args.repeats):
            for arm in ("batched", "trailing"):
                lat = measure(args.port, arm, args.statements, next_id)
                next_id += args.statements
                per_arm[arm].append(summarize(lat))

        for arm, runs in per_arm.items():
            out["arms"][arm] = {
                "runs": runs,
                "p50_us": round(statistics.median(r["p50_us"] for r in runs), 1),
                "mean_us": round(statistics.median(r["mean_us"] for r in runs), 1),
                "p50_floor_pct": round(
                    100 * (max(r["p50_us"] for r in runs) - min(r["p50_us"] for r in runs))
                    / statistics.median(r["p50_us"] for r in runs), 2),
            }
        b, t = out["arms"]["batched"], out["arms"]["trailing"]
        out["refund_p50_us"] = round(t["p50_us"] - b["p50_us"], 1)
        out["refund_mean_us"] = round(t["mean_us"] - b["mean_us"], 1)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

    print(json.dumps(out, indent=2))
    if args.json:
        with open(args.json, "w") as f:
            json.dump(out, f, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
