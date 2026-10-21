#!/usr/bin/env python3
"""A1 milestone close: statement-path overhead and the build-yield benefit.

Two modes against one running server (start it with the config the results
file names; this driver never starts or stops a server).

    --mode stmt    Statement-path cost. For each --sizes row count, a fresh
                   BTREE table (id int64, c_int int64, c_small int32,
                   c_flag bool, c_text varchar) is loaded with that many rows
                   (untimed), then four phases are timed from one connection:
                   insert (--ops single-row INSERTs), point-select
                   (WHERE id = <random>), update (single-row UPDATE of the
                   fixed-width column by id) and full-scan (SELECT * FROM t,
                   --scan-ops times, unsorted).

    --mode build   The benefit. A wide BTREE table `big` (--rows rows, a
                   --note-len character varchar) and a small BTREE table
                   `small` (1000 rows) are loaded. Connection P loops tuple-key
                   point SELECTs on `small` in a thread; connection B runs
                   CREATE INDEX (and then CREATE ASSERTION ... GROUP BY ...
                   CHECK COUNT(*) <= N) on `big`. P's latencies are split into
                   idle (before the first build), during each build, and the
                   build's own wall time is reported. With cores=1 both
                   sessions share the reactor core.

Both print one JSON object on stdout (--json PATH also writes it). Latencies
are seconds on the wire; the summary is microseconds, nearest-rank p0 p25 p50
p90 p99 max (bench_common.nearest_rank).

    tools/a1_close_benchmark.py --port 15801 --mode stmt --sizes 200,1000,10000
    tools/a1_close_benchmark.py --port 15801 --mode build --rows 400000
"""

import argparse
import json
import random
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench_common import nearest_rank  # noqa: E402
from ckdbs_cli import ServerConnection  # noqa: E402

COLS = "id int64, c_int int64, c_small int32, c_flag bool, c_text varchar"


def must(conn, stmt):
    r = conn.send_command(stmt)
    if r.startswith("ERR"):
        raise RuntimeError(f"{stmt[:80]} -> {r[:200]}")
    return r


def dist(lat):
    """Microsecond summary of a list of second-latencies."""
    if not lat:
        return {"n": 0}
    o = sorted(lat)
    f = lambda p: round(nearest_rank(o, p) * 1e6, 1)  # noqa: E731
    return {"n": len(o), "p0": round(o[0] * 1e6, 1), "p25": f(25), "p50": f(50),
            "p90": f(90), "p99": f(99), "max": round(o[-1] * 1e6, 1),
            "mean": round(sum(o) / len(o) * 1e6, 1)}


def timed(conn, stmt):
    t = time.perf_counter()
    r = conn.send_command(stmt)
    d = time.perf_counter() - t
    if r.startswith("ERR"):
        raise RuntimeError(f"{stmt[:80]} -> {r[:200]}")
    return d


def load_rows(conn, table, rows, rng, batch=500):
    must(conn, "BEGIN")
    for i in range(rows):
        t = "".join(rng.choices("abcdefghijklmnopqrstuvwxyz", k=16))
        must(conn, f"INSERT INTO {table} VALUES ({rng.randint(0, 10**6)}, "
                   f"{rng.randint(0, 30000)}, {rng.randint(0, 1)}, '{t}')")
        if (i + 1) % batch == 0:
            must(conn, "COMMIT")
            must(conn, "BEGIN")
    must(conn, "COMMIT")


def mode_stmt(args):
    rng = random.Random(args.seed)
    conn = ServerConnection("127.0.0.1", args.port, timeout=120)
    out = {}
    for size in [int(s) for s in args.sizes.split(",")]:
        table = f"st_{size}_{args.suffix}"
        must(conn, f"CREATE TABLE {table} ({COLS}) BTREE")
        load_rows(conn, table, size, rng)
        hi = size
        for _ in range(50):
            conn.send_command(f"SELECT * FROM {table} WHERE id = {rng.randint(1, hi)}")
        res = {}
        lat = []
        for _ in range(args.ops):
            t = "".join(rng.choices("abcdefghijklmnopqrstuvwxyz", k=16))
            lat.append(timed(conn, f"INSERT INTO {table} VALUES ({rng.randint(0, 10**6)}, "
                                   f"{rng.randint(0, 30000)}, {rng.randint(0, 1)}, '{t}')"))
        res["insert"] = dist(lat)
        lat = [timed(conn, f"SELECT * FROM {table} WHERE id = {rng.randint(1, hi)}")
               for _ in range(args.ops)]
        res["point-select"] = dist(lat)
        lat = [timed(conn, f"UPDATE {table} SET c_int = {rng.randint(0, 10**6)} "
                           f"WHERE id = {rng.randint(1, hi)}") for _ in range(args.ops)]
        res["update"] = dist(lat)
        lat = [timed(conn, f"SELECT * FROM {table}") for _ in range(args.scan_ops)]
        res["full-scan"] = dist(lat)
        res["full-scan"]["rows"] = size + args.ops
        out[str(size)] = res
    conn.close()
    return out


def mode_build(args):
    rng = random.Random(args.seed)
    conn = ServerConnection("127.0.0.1", args.port, timeout=900)
    must(conn, "CREATE TABLE small (id int64, v int64) BTREE")
    must(conn, "BEGIN")
    for i in range(1000):
        must(conn, f"INSERT INTO small VALUES ({i})")
    must(conn, "COMMIT")
    must(conn, "CREATE TABLE big (id int64, grp int64, amount int64, note varchar) BTREE")
    note = "x" * args.note_len
    t0 = time.perf_counter()
    must(conn, "BEGIN")
    for i in range(args.rows):
        must(conn, f"INSERT INTO big VALUES ({i % 1000}, {(i * 7) % 100000}, '{note}')")
        if (i + 1) % 1000 == 0:
            must(conn, "COMMIT")
            must(conn, "BEGIN")
    must(conn, "COMMIT")
    load_s = time.perf_counter() - t0

    # (start, end, latency) of every probe, appended by the thread.
    probe = []
    stop = threading.Event()

    def probe_loop():
        pc = ServerConnection("127.0.0.1", args.port, timeout=900)
        prng = random.Random(7)
        while not stop.is_set():
            s = time.perf_counter()
            r = pc.send_command(f"SELECT * FROM small WHERE id = {prng.randint(1, 1000)}")
            e = time.perf_counter()
            if r.startswith("ERR"):
                raise RuntimeError(r)
            probe.append((s, e))
        pc.close()

    th = threading.Thread(target=probe_loop)
    th.start()
    time.sleep(args.idle)
    builds = [("CREATE INDEX", f"CREATE INDEX ix_big_grp ON big (grp)")]
    if not args.no_assertion:
        builds.append(("CREATE ASSERTION",
                       f"CREATE ASSERTION big_cap ON big GROUP BY (grp) CHECK COUNT(*) <= {args.rows}"))
    marks = []
    import sys_meta
    walmeta = {}

    def wal_fields():
        m = sys_meta.from_text(conn.send_command("SHOW META"))
        return {k: v for k, v in m.items() if k.startswith("wal_")}

    for name, stmt in builds:
        before = wal_fields()
        s = time.perf_counter()
        w0 = time.time()
        r = conn.send_command(stmt)
        e = time.perf_counter()
        w1 = time.time()
        if r.startswith("ERR"):
            raise RuntimeError(f"{stmt} -> {r[:200]}")
        walmeta[name] = {"epoch_start": w0, "epoch_end": w1, "wal_before": before,
                         "wal_after": wal_fields()}
        marks.append((name, s, e))
        time.sleep(args.idle)
    stop.set()
    th.join()
    conn.close()

    first = marks[0][1]
    res = {"load_s": round(load_s, 2), "idle": dist([e - s for s, e in probe if e < first])}
    for name, s, e in marks:
        # A probe is "during" when its request overlapped the build window.
        during = [(ps, pe) for ps, pe in probe if pe > s and ps < e]
        res[name] = {"build_wall_s": round(e - s, 3),
                     "probes_during": dist([pe - ps for ps, pe in during]),
                     # Probes that *completed* inside the window: a stalled
                     # core completes none until the build ends.
                     "wal": walmeta[name],
                     "completed_inside": sum(1 for ps, pe in probe if s < pe < e),
                     # Where the long probes fall in the build window:
                     # (ms after build start, probe latency ms), > 20 ms.
                     "slow_probes": [(round((ps - s) * 1e3, 1), round((pe - ps) * 1e3, 1))
                                     for ps, pe in during if pe - ps > 0.020]}
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--mode", choices=("stmt", "build"), required=True)
    ap.add_argument("--sizes", default="200,1000,10000")
    ap.add_argument("--ops", type=int, default=1000)
    ap.add_argument("--scan-ops", type=int, default=20)
    ap.add_argument("--rows", type=int, default=400000)
    ap.add_argument("--note-len", type=int, default=200)
    ap.add_argument("--idle", type=float, default=1.0)
    ap.add_argument("--no-assertion", action="store_true")
    ap.add_argument("--suffix", default="r")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--json")
    args = ap.parse_args()
    res = mode_stmt(args) if args.mode == "stmt" else mode_build(args)
    text = json.dumps(res, indent=1)
    print(text)
    if args.json:
        Path(args.json).write_text(text)


if __name__ == "__main__":
    main()
