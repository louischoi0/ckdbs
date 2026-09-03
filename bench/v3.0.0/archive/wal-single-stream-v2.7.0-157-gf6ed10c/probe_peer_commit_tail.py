#!/usr/bin/env python3
"""AL-S8 cell B1: a peer's commit tail under a stated durability class.

Not a tools/ driver - a standalone probe built on the unmodified
tools/ckdbs_cli.ServerConnection. Design: two relations on one server,
placement=namespace (the default) so an undeclared-namespace table is
core-0-owned and a declared-namespace table's first relation rotates onto
a peer (NS10 rule 3). One pinned session per relation, each writing only
the relation ITS core owns (so its statement executes locally - no
statement shipping crosses in either direction), while background filler
connections on both relations supply the concurrent committers group
durability needs to batch at all. Records client round-trip latency per
INSERT on the two pinned sessions and reports percentiles.

Usage: peer_commit_tail.py <port> <n_inserts> <n_fillers_per_side> [seconds_of_fill]
"""
import json
import sys
import threading
import time

sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/v3.0.0-arch-revision/tools")
from ckdbs_cli import ServerConnection  # noqa: E402


def field(reply, key):
    for tok in reply.split():
        if tok.startswith(key + "="):
            return int(tok[len(key) + 1:])
    raise RuntimeError(f"reply carries no {key}= field: {reply}")


def session_core(conn):
    return field(conn.send_command("SHOW META"), "core")


def pin(port, target_core, max_attempts=500):
    for _ in range(max_attempts):
        c = ServerConnection("127.0.0.1", port, timeout=30)
        if session_core(c) == target_core:
            return c
        c.close()
    raise RuntimeError(f"never landed on core {target_core} in {max_attempts} attempts")


def percentiles(latencies_s):
    if not latencies_s:
        return {}
    xs = sorted(latencies_s)
    n = len(xs)
    def p(pct):
        rank = max(1, -(-int(pct) * n // 100)) - 1
        return xs[rank]
    return {
        "ops": n,
        "p0_us": round(xs[0] * 1e6, 1),
        "p25_us": round(p(25) * 1e6, 1),
        "p50_us": round(p(50) * 1e6, 1),
        "p95_us": round(p(95) * 1e6, 1),
        "p99_us": round(p(99) * 1e6, 1),
        "max_us": round(xs[-1] * 1e6, 1),
        "mean_us": round(sum(xs) / n * 1e6, 1),
    }


def filler_loop_valid(port, table, stop_evt, base):
    conn = ServerConnection("127.0.0.1", port, timeout=30)
    i = base
    while not stop_evt.is_set():
        conn.send_command(f"INSERT INTO {table} (val) VALUES ({i})")
        i += 1
    conn.close()


def measured_loop(conn, table, n):
    lat = []
    for i in range(n):
        t0 = time.perf_counter()
        reply = conn.send_command(f"INSERT INTO {table} (val) VALUES ({i})")
        lat.append(time.perf_counter() - t0)
        if reply.startswith("ERR"):
            pass
    return lat


def main():
    port = int(sys.argv[1])
    n_inserts = int(sys.argv[2])
    n_fillers = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    suffix = sys.argv[4] if len(sys.argv) > 4 else ""
    t0name = "probe0" + suffix
    nsname = "probe_ns" + suffix
    t1name = f"{nsname}.probe1{suffix}"

    setup = ServerConnection("127.0.0.1", port, timeout=30)
    print(setup.send_command(f"CREATE TABLE {t0name} (id int64, val int64)"), file=sys.stderr)
    print(setup.send_command(f"CREATE NAMESPACE {nsname}"), file=sys.stderr)
    print(setup.send_command(f"CREATE TABLE {t1name} (id int64, val int64)"), file=sys.stderr)
    d0 = setup.send_command(f"DESCRIBE {t0name}")
    d1 = setup.send_command(f"DESCRIBE {t1name}")
    core0_owner = field(d0, "owner_core")
    core1_owner = field(d1, "owner_core")
    print(f"probe0 owner_core={core0_owner}  probe_ns.probe1 owner_core={core1_owner}",
          file=sys.stderr)
    setup.close()

    conn_a = pin(port, core0_owner)   # measured session on the table's own owner
    conn_b = pin(port, core1_owner)

    stop_evt = threading.Event()
    fillers = []
    for i in range(n_fillers):
        t = threading.Thread(target=filler_loop_valid, args=(port, t0name, stop_evt, i * 1000000))
        t.start()
        fillers.append(t)
    for i in range(n_fillers):
        t = threading.Thread(target=filler_loop_valid,
                             args=(port, t1name, stop_evt, i * 1000000))
        t.start()
        fillers.append(t)

    time.sleep(0.5)  # let fillers ramp up so batching is already happening

    results = {}
    lat_a = measured_loop(conn_a, t0name, n_inserts)
    results["core0_owner"] = {"core": core0_owner, "table": t0name,
                              "latency": percentiles(lat_a)}
    lat_b = measured_loop(conn_b, t1name, n_inserts)
    results["peer_owner"] = {"core": core1_owner, "table": t1name,
                             "latency": percentiles(lat_b)}

    stop_evt.set()
    for t in fillers:
        t.join(timeout=5)

    conn_a.close()
    conn_b.close()

    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
