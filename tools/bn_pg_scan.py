#!/usr/bin/env python3
"""BN's close: a scan row's server cost on PostgreSQL, measured as
`tools/bn_census.py` measures KDS's (`workorder-bn-opt-read-update-path.md`
§5: "a 10,000-row unindexed equality against BA-S3's PostgreSQL twin").

    bn_pg_scan.py --port 15433 [--database bench] [--rows 10000] [--ops 200]
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

    bn_pg_scan.py --shapes bm --port 15434 [--runs 8] [--scan-ops 192]
                  [--point-ops 3000] [--blocks 12] --out OUT.json

`--shapes bm` is BM-S1's twin (`workorder-bm-scan-row-kernel.md` §5.1): the
cells of `tools/bm_scan.py census` - rows (1,000 / 10,000 / 100,000),
selectivity (`w < ?` at 0 / 1 / 10 / 100 %), the 13-column width relation
(`h = ?`, `h = ? AND k = ?`, one equality per type) and the nullable pair -
over the same row values (`bm_scan.wide_row_sql` and `nul_row_sql`), each
with its relation's primary-key `point` arm, over the simple query protocol.
Type map: int32 -> integer, int64 -> bigint, uint64 -> bigint (an 8-byte
integer compare; `numeric(20,0)` would be dearer), bool -> boolean, date ->
date, timestamp -> timestamp, decimal(12,2) / decimal(30,4) -> numeric(12,2)
/ numeric(30,4), char(16) -> char(16) (blank-padded), varchar -> varchar
(the 300-byte value stays in the heap row: PostgreSQL moves a value out of
line only past about 2 KB, so there is no spilled column here). Each
relation is `VACUUM (ANALYZE)`d after its load (hint bits and the visibility
map are set, so a scan skips the per-tuple visibility check on an all-visible
page; autovacuum would do it within a minute) - `--no-vacuum` runs ANALYZE
alone, the state `--shapes bn` measures. The first `EXPLAIN` of every
cell is recorded so a parallel plan - whose workers this driver's backend
CPU would not see - is flagged.
"""
import argparse, json, os, re, subprocess, sys, time

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


PG_COLS = {
    "s200": "id bigint PRIMARY KEY, v bigint, w bigint",
    "s1k": "id bigint PRIMARY KEY, v bigint, w bigint",
    "s": "id bigint PRIMARY KEY, v bigint, w bigint",
    "s100k": "id bigint PRIMARY KEY, v bigint, w bigint",
    "wd": "id bigint PRIMARY KEY, a integer, b bigint, c bigint, d boolean, e date, "
          "f timestamp, g numeric(12,2), h numeric(30,4), k char(16), m varchar, n bigint, "
          "p varchar",
    "nul0": "id bigint PRIMARY KEY, n bigint, w bigint",
    "nul50": "id bigint PRIMARY KEY, n bigint, w bigint",
}


def pg_load(conn, bm, rel, vacuum=True):
    conn.send_command(f"DROP TABLE IF EXISTS bm_{rel}")
    must_ok(conn, f"CREATE TABLE bm_{rel} ({PG_COLS[rel]})")
    _, rows, rowfn = bm.RELS[rel][0], bm.RELS[rel][1], bm.RELS[rel][2]
    if rel.startswith("s"):
        must_ok(conn, f"INSERT INTO bm_{rel} SELECT i, i, i FROM generate_series(1, {rows}) i")
    else:
        for lo in range(1, rows + 1, 200):
            hi = min(rows, lo + 199)
            if rel == "wd":
                vals = ",".join(bm.wide_row_sql(i, "pg") for i in range(lo, hi + 1))
            else:
                vals = ",".join(bm.nul_row_sql(i, 0 if rel == "nul0" else 50)
                                for i in range(lo, hi + 1))
            must_ok(conn, f"INSERT INTO bm_{rel} VALUES {vals}")
    must_ok(conn, f"VACUUM (ANALYZE) bm_{rel}" if vacuum else f"ANALYZE bm_{rel}")


def must_ok(conn, sql):
    reply = conn.send_command(sql)
    if reply.startswith("ERR"):
        raise SystemExit(f"setup: {sql[:80]!r} -> {reply[:200]}")


def pg_arm(conn, pid, sql_of, expect, ops, blocks):
    """`ops` statements in `blocks` blocks; wall times, backend CPU, bad replies."""
    lat, bad, cpu_ns_total, block_us = [], 0, 0, []
    per = max(1, ops // blocks)
    q = 0
    for _ in range(blocks):
        s0 = cpu_ns(pid)
        for _ in range(per):
            q += 1
            sql = sql_of(q)
            t0 = time.perf_counter()
            tag, rows, _, error = conn.query(sql)
            lat.append(time.perf_counter() - t0)
            if error is not None or rows != expect(q):
                bad += 1
        d = cpu_ns(pid) - s0
        cpu_ns_total += d
        block_us.append(round(d / per / 1e3, 2))
    n = per * blocks
    us = [x * 1e6 for x in lat]
    return {"ops": n, "bad": bad, "server_cpu_us": cpu_ns_total / n / 1e3,
            "block_server_us": block_us, "lat_us": [round(x, 1) for x in us],
            "p_p0": min(us), "p_p25": pct(us, 25), "p_p50": pct(us, 50), "p_p90": pct(us, 90),
            "p_p95": pct(us, 95), "p_p99": pct(us, 99), "p_mean": sum(us) / n}


def bm_main(a):
    import bm_scan as bm
    from bn_census import literal_of
    os.sched_setaffinity(0, {int(a.pin_driver)})
    cells = bm.make_cells(a)
    if a.cells:
        cells = [c for c in cells if c.name in a.cells.split(",")]
    cells = [c for c in cells if c.rel != "srec"]  # a KDS visibility-floor cell: no twin
    rels = [r for r in PG_COLS if any(c.rel == r for c in cells)]
    out = {"args": vars(a), "runs": []}
    for r in range(a.runs):
        conn = PgConnection(port=a.port, database=a.database, application_name="bn_pg_scan.py")
        pid = conn.backend_pid
        subprocess.run(["taskset", "-p", "-c", a.pin_backend, str(pid)], check=True,
                       capture_output=True)
        t0 = time.time()
        for rel in rels:
            pg_load(conn, bm, rel, not a.no_vacuum)
        run = {"run": r, "host_before": host_state(), "load_s": round(time.time() - t0, 1),
               "arms": [], "plans": {}}
        pg_text = lambda c, q: re.sub(r"FROM (\w+) ", lambda m: f"FROM bm_{m.group(1)} ",
                                      literal_of(c.sql, c.params(q)), count=1)
        for cell in cells:
            sql_of = lambda q, c=cell: pg_text(c, q).replace(" d = 1", " d = true")
            if cell.rel not in {x["rel"] for x in run["arms"] if x["kind"] == "point"}:
                for _ in range(3):
                    conn.query(f"SELECT * FROM bm_{cell.rel} WHERE id = 1")
                host = host_state()
                arm = pg_arm(conn, pid,
                             lambda q, rel=cell.rel: f"SELECT * FROM bm_{rel} WHERE id = "
                             f"{bm.TARGET_ID(q, bm.RELS[rel][1])}", lambda q: 1, a.point_ops,
                             a.blocks)
                arm.update({"cell": "point:" + cell.rel, "kind": "point", "rel": cell.rel,
                            "path": "literal", "rows": 1, "host": host})
                run["arms"].append(arm)
            if r == 0:
                rows, error = conn.fetch("EXPLAIN " + sql_of(5))
                plan = " | ".join(x[0].decode() for x in rows) if not error else "ERR " + error
                run["plans"][cell.name] = plan
            for _ in range(2):  # warm: hint bits and the cache, untimed
                conn.query(sql_of(1))
            host = host_state()
            arm = pg_arm(conn, pid, sql_of, cell.expect, cell.ops, a.blocks)
            arm.update({"cell": cell.name, "kind": "scan", "rel": cell.rel, "path": "literal",
                        "rows": cell.rows, "host": host})
            run["arms"].append(arm)
            print(json.dumps({k: (round(v, 2) if isinstance(v, float) else v)
                              for k, v in arm.items()
                              if k not in ("host", "block_server_us", "lat_us")}), flush=True)
        run["host_after"] = host_state()
        out["runs"].append(run)
        for rel in rels:
            conn.send_command(f"DROP TABLE bm_{rel}")
        conn.close()
        with open(a.out, "w") as f:
            json.dump(out, f, indent=1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shapes", choices=["bn", "bm"], default="bn")
    ap.add_argument("--scan-ops", type=int, default=192, help="--shapes bm: bm_scan's flag")
    ap.add_argument("--blocks", type=int, default=12, help="--shapes bm: blocks per arm")
    ap.add_argument("--cells", help="--shapes bm: comma-separated cell names (default: all)")
    ap.add_argument("--no-vacuum", action="store_true",
                    help="--shapes bm: ANALYZE only, as --shapes bn does (pages not marked "
                    "all-visible, so a scan checks each tuple's visibility)")
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
    if a.shapes == "bm":
        return bm_main(a)
    os.sched_setaffinity(0, {int(a.pin_driver)})
    out = {"args": vars(a), "runs": []}
    for r in range(a.runs):
        conn = PgConnection(port=a.port, database=a.database, application_name="bn_pg_scan.py")
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
