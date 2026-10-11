#!/usr/bin/env python3
"""BM's close: a scan row's server cost on PostgreSQL, measured as
`tools/bm_census.py` measures KDS's (`workorder-bm-opt-read-update-path.md`
§5: "a 10,000-row unindexed equality against BA-S3's PostgreSQL twin").

    bm_pg_scan.py --port 15433 [--database bench] [--rows 10000] [--ops 200]
                  [--runs 5] [--pin-backend 2] [--pin-driver 4] --out OUT.json

Expects the scratch cluster `tools/pg_setup.sh` manages, already started.
Per run it (re)creates `bk_s (id bigint primary key, v bigint, w bigint)`
with `--rows` rows `(i, i, i)`, ANALYZEs it, pins this connection's backend
to `--pin-backend`, warms the table with one scan, then runs `--ops`
`SELECT * FROM bk_s WHERE w = <k>` (one row matches, no index on `w`) over
the simple query protocol - a literal statement per execution, the shape of
KDS's `literal` path - recording each statement's wall time and, over the
arm, the backend's on-CPU time from `/proc/<pid>/schedstat`. The same
`point` arm (`WHERE id = <k>`, the primary key) gives the fixed per-statement
cost the scan's per-row figure is taken net of.
"""
import argparse, json, os, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pg_wire import PgConnection  # noqa: E402
from bb_overhead_benchmark import host_state  # noqa: E402


def cpu_ns(pid):
    with open(f"/proc/{pid}/schedstat") as f:
        return int(f.read().split()[0])


def pct(xs, p):
    xs = sorted(xs)
    return xs[max(0, min(len(xs) - 1, -(-p * len(xs) // 100) - 1))]


def arm(conn, sql_of, ops, rows, pid):
    lat = []
    s0 = cpu_ns(pid)
    for q in range(ops):
        sql = sql_of(1 + (q * 7919) % rows)
        t0 = time.perf_counter()
        got, error = conn.fetch(sql)
        lat.append(time.perf_counter() - t0)
        if error is not None or len(got) != 1:
            raise SystemExit(f"{sql!r} answered {len(got)} rows, error {error}")
    cpu = (cpu_ns(pid) - s0) / ops / 1e3
    return {"ops": ops, "p0_us": min(lat) * 1e6, "p25_us": pct(lat, 25) * 1e6,
            "p50_us": pct(lat, 50) * 1e6, "p90_us": pct(lat, 90) * 1e6,
            "p99_us": pct(lat, 99) * 1e6, "server_cpu_us": cpu}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--database", default="bench")
    ap.add_argument("--rows", type=int, default=10000)
    ap.add_argument("--ops", type=int, default=200)
    ap.add_argument("--point-ops", type=int, default=3000)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--pin-backend", default="2")
    ap.add_argument("--pin-driver", default="4")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.sched_setaffinity(0, {int(a.pin_driver)})
    out = {"args": vars(a), "runs": []}
    for r in range(a.runs):
        conn = PgConnection(port=a.port, database=a.database, application_name="bm_pg_scan.py")
        pid = conn.backend_pid
        subprocess.run(["taskset", "-p", "-c", a.pin_backend, str(pid)], check=True,
                       capture_output=True)
        conn.send_command("DROP TABLE IF EXISTS bk_s")
        conn.send_command("CREATE TABLE bk_s (id bigint PRIMARY KEY, v bigint, w bigint)")
        conn.send_command(f"INSERT INTO bk_s SELECT i, i, i FROM generate_series(1, {a.rows}) i")
        conn.send_command("ANALYZE bk_s")
        conn.fetch("SELECT * FROM bk_s WHERE w = 1")
        before = host_state()
        res = {"run": r, "host_before": before,
               "scan": arm(conn, lambda k: f"SELECT * FROM bk_s WHERE w = {k}", a.ops, a.rows, pid),
               "point": arm(conn, lambda k: f"SELECT * FROM bk_s WHERE id = {k}", a.point_ops,
                            a.rows, pid)}
        res["host_after"] = host_state()
        res["scan_row_ns"] = (res["scan"]["server_cpu_us"] - res["point"]["server_cpu_us"]) \
            * 1e3 / a.rows
        print(json.dumps({"run": r, "scan_cpu_us": round(res["scan"]["server_cpu_us"], 1),
                          "point_cpu_us": round(res["point"]["server_cpu_us"], 1),
                          "scan_row_ns": round(res["scan_row_ns"], 1)}), flush=True)
        out["runs"].append(res)
        conn.send_command("DROP TABLE bk_s")
        conn.close()
    with open(a.out, "w") as f:
        json.dump(out, f, indent=1)


if __name__ == "__main__":
    main()
