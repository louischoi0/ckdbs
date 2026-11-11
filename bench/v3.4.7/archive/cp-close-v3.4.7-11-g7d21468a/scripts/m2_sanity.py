#!/usr/bin/env python3
"""CP-S0 cell M2, the pre-flight sanity: both builds answer the same scan, and
no engine counter moves with the slice `IS`.

    m2_sanity.py --base BIN --noslice BIN --workdir DIR --port P
                 [--pin-server 2] [--pin-driver 3] [--scans 40] --out OUT.json

For each binary in turn (one server at a time, `cores = 1`, `relaxed`, the
config tools/bn_census.py's `Server` writes): load `s100k` with
tools/bm_scan.py's `load_rel`, wait for the server to go idle, warm the scan
`SELECT * FROM s100k WHERE w = ?` three times, then read

  - every integer field of `sys.meta` (`SHOW META`),
  - `sys.wait_stats`' total rows (`core` NULL) for `app.lock_wait` and
    `app.lock_conflict`,
  - the row count of `sys.lock_waits`,

run `--scans` literal scans (wall per statement, the server's on-CPU time
from /proc/<pid>/task/*/schedstat), read the three again and report every
field that moved. The lock-side keys are named so a reader sees them even
when they did not move:

  contention_fence_walks           a `FenceCoversKey` walk; a slice `IS` never
                                   triggers one (lock_table.cpp:340-356 runs
                                   it for a tuple ask under a nonzero fence
                                   counter, and `IsFence` (:26) does not count
                                   an intention mode)
  contention_lock_partition_waits  the contended branch of a partition latch;
  contention_lock_partition_wait_us  null latches at cores = 1 (lock_table.hpp:511-512)
  contention_lock_wait_for_waits   the wait-for graph latch
  lock_waits_recorded              parked waits (`sys.lock_waits`); a granted
                                   `TryAcquire` never parks
  app.lock_wait / app.lock_conflict  (sys.wait_stats) a wait on a holder / a
                                   refused ask (wait-events.md:208-245)

No key of `sys.meta` counts a granted `TryAcquire` (contention.hpp's `Tally`
has no lock-ask tally), so the expected reading is an all-zero delta on BOTH
arms: the cell is fence-walk-free, and the slice's presence or absence is
invisible to every counter - its price is on-CPU time alone, which the census
measures. The sanity also records each arm's `ANALYZE` of the scan (its
`pages=`) and a digest of the scan's rows; the two arms must agree on both,
and exit status 2 says they did not (the census must not run on two
workloads). Exit 1 is a server that would not start or answer.

Outputs: the JSON at --out, one line per arm on stdout.
"""
import argparse, hashlib, json, os, re, sys, time

TOOLS = "/home/louis/ckdbs/.claude/worktrees/worktree-cp-lock-model-exploration/tools"
sys.path.insert(0, TOOLS)
import kwp  # noqa: E402
import sys_meta  # noqa: E402
import bn_census as bc  # noqa: E402
import bm_scan  # noqa: E402

SCAN_SQL = "SELECT * FROM s100k WHERE w = %d"
ROWS = 100000
LOCK_KEYS = ("contention_fence_walks", "contention_lock_partition_waits",
             "contention_lock_partition_wait_us", "contention_lock_wait_for_waits",
             "contention_lock_wait_for_wait_us", "lock_waits_recorded",
             "contention_inflight_asks", "contention_visibility_window_lookups",
             "pool_misses")
WAIT_EVENTS = ("app.lock_wait", "app.lock_conflict")


def meta_all(conn):
    _, rows, tag, _ = conn.execute("SHOW META")
    return sys_meta.numeric(sys_meta.from_rows(rows, tag))


def view_rows(conn, sql):
    """A `sys` view as a list of {column: text}, rendered as the CLI would."""
    fields, rows, tag, _ = conn.execute(sql)
    if not tag.startswith("SELECT"):
        raise RuntimeError("%s -> %s" % (sql, tag))
    render = [kwp.renderer_for(f) for f in fields]
    names = [f["name"] for f in fields]
    out = []
    for r in rows:
        out.append({n: ("NULL" if c is None else render[i](c)) for i, (n, c) in
                    enumerate(zip(names, r))})
    return out


def wait_totals(conn):
    """{event: (count, total_us)} of sys.wait_stats' total rows, for WAIT_EVENTS."""
    out = {}
    for r in view_rows(conn, "SELECT * FROM sys.wait_stats"):
        if r.get("core") == "NULL" and r.get("event") in WAIT_EVENTS:
            out[r["event"]] = (int(r["count"]), int(r["total_us"]))
    return out


def lock_wait_rows(conn):
    return len(view_rows(conn, "SELECT * FROM sys.lock_waits"))


def digest(rows):
    h = hashlib.sha256()
    for rw in rows:
        h.update(b"\x1e".join(c if c is not None else b"\xff\xff" for c in rw))
        h.update(b"\x1f")
    return "%d:%s" % (len(rows), h.hexdigest()[:16])


def one_arm(a, label, binary):
    ns = argparse.Namespace(workdir=a.workdir, pin_server=a.pin_server)
    srv = bc.Server(ns, label, binary, a.port, "san")
    rec = {"arm": label, "binary": binary, "port": a.port, "pid": srv.proc.pid}
    try:
        conn = srv.conn
        t0 = time.time()
        bm_scan.load_rel(conn, "s100k")
        rec["load_s"] = round(time.time() - t0, 1)
        rec["idle_wait"] = bc.wait_idle(srv, limit_s=30.0)
        for q in range(1, 4):
            conn.execute(SCAN_SQL % bm_scan.TARGET_ID(q, ROWS))
        # The scan's plan and its row digest, before the counted burst.
        _, rows, tag, _ = conn.execute("ANALYZE " + SCAN_SQL % bm_scan.TARGET_ID(7, ROWS))
        text = tag + " | " + " | ".join(b" ".join(c or b"" for c in rw).decode(errors="replace")
                                       for rw in rows[:4])
        m = re.search(r"pages=(\d+)", text)
        rec["analyze"] = text[:400]
        rec["pages"] = int(m.group(1)) if m else None
        _, rows, tag, _ = conn.execute(SCAN_SQL % bm_scan.TARGET_ID(7, ROWS))
        rec["digest"] = digest(rows)
        rec["scan_tag"] = tag
        # Two SHOW META back to back: what one read moves on its own.
        s0 = meta_all(conn)
        s1 = meta_all(conn)
        rec["meta_self_delta"] = {k: s1[k] - s0[k] for k in s1 if k in s0 and s1[k] != s0[k]}
        m0, w0, l0 = meta_all(conn), wait_totals(conn), lock_wait_rows(conn)
        cpu0 = bc.sched_ns(srv.proc.pid)
        lat, bad = [], 0
        for q in range(100, 100 + a.scans):
            t = time.perf_counter()
            _, rows, tag, _ = conn.execute(SCAN_SQL % bm_scan.TARGET_ID(q, ROWS))
            lat.append((time.perf_counter() - t) * 1e6)
            if not tag.startswith("SELECT") or len(rows) != 1:
                bad += 1
        cpu1 = bc.sched_ns(srv.proc.pid)
        m1, w1, l1 = meta_all(conn), wait_totals(conn), lock_wait_rows(conn)
        rec["scans"], rec["bad"] = a.scans, bad
        rec["server_cpu_us_per_scan"] = round((cpu1 - cpu0) / a.scans / 1e3, 1)
        rec["wall_us"] = {"p50": round(bc.pct(lat, 50), 1), "p90": round(bc.pct(lat, 90), 1),
                          "p99": round(bc.pct(lat, 99), 1)}
        moved = {k: m1[k] - m0[k] for k in m1 if k in m0 and m1[k] != m0[k]}
        rec["meta_moved"] = moved
        rec["lock_keys"] = {k: (m1[k] - m0[k] if k in m1 and k in m0 else None) for k in LOCK_KEYS}
        rec["wait_events"] = {e: {"count": w1.get(e, (0, 0))[0] - w0.get(e, (0, 0))[0],
                                  "total_us": w1.get(e, (0, 0))[1] - w0.get(e, (0, 0))[1],
                                  "present": e in w1} for e in WAIT_EVENTS}
        rec["lock_waits_rows_delta"] = l1 - l0
        zero_keys = ("contention_fence_walks", "contention_lock_partition_waits",
                     "contention_lock_wait_for_waits", "lock_waits_recorded")
        # A key this engine does not print is None, and None is not zero: a
        # renamed counter must read as "not shown fence-free", never as free.
        rec["fence_free"] = all(rec["lock_keys"].get(k) == 0 for k in zero_keys) and \
            all(rec["wait_events"][e]["count"] == 0 for e in WAIT_EVENTS) and \
            rec["lock_waits_rows_delta"] == 0
        rec["missing_keys"] = [k for k in LOCK_KEYS if rec["lock_keys"][k] is None]
    finally:
        srv.stop()
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--noslice", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--pin-server", default="2")
    ap.add_argument("--pin-driver", default="3")
    ap.add_argument("--scans", type=int, default=40)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.sched_setaffinity(0, {int(a.pin_driver)})
    os.makedirs(a.workdir, exist_ok=True)
    out = {"host_before": bc.host_state(), "arms": []}
    rc = 0
    for label, binary in (("base", a.base), ("noslice", a.noslice)):
        try:
            rec = one_arm(a, label, binary)
        except (SystemExit, OSError, RuntimeError, kwp.KwpError) as e:  # noqa: PERF203
            rec = {"arm": label, "binary": binary, "error": str(e)[:300]}
            rc = max(rc, 1)
        out["arms"].append(rec)
        print(json.dumps({k: v for k, v in rec.items() if k not in ("analyze",)}), flush=True)
    ok = [r for r in out["arms"] if "error" not in r]
    if len(ok) == 2:
        same_pages = ok[0]["pages"] == ok[1]["pages"]
        same_rows = ok[0]["digest"] == ok[1]["digest"]
        out["same_pages"], out["same_rows"] = same_pages, same_rows
        out["fence_free_both"] = all(r["fence_free"] for r in ok)
        if not (same_pages and same_rows) or any(r["bad"] for r in ok):
            rc = 2
        print("sanity: pages %s (%s), rows %s, fence-free %s, bad %s" % (
            "same" if same_pages else "DIFFER", ok[0]["pages"], "same" if same_rows else "DIFFER",
            out["fence_free_both"], [r["bad"] for r in ok]), flush=True)
    out["host_after"] = bc.host_state()
    out["rc"] = rc
    with open(a.out, "w") as f:
        json.dump(out, f, indent=1)
    sys.exit(rc)


if __name__ == "__main__":
    main()
