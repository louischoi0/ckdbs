#!/usr/bin/env python3
"""CD close, cells 4-6 (binary B only: A has no PURGE DROPPED COLUMNS).

    cd_pass.py --bin B --workdir DIR --port P --out OUT.json --progress FILE
               [--sizes 200,1000,10000,100000,1000000] [--stream-rows N]
               [--reps 1] [--add-trials 40] [--cells 4,5,6]

cell 4  per size, a fresh server and data file: table p (id, v, w) BTREE SPARE 2,
        ADD note varchar(16) NULL, N rows each with a 20-byte note (spills), DROP
        note; then PURGE DROPPED COLUMNS FROM p twice. Records wall, server CPU,
        the WAL append position delta (sys.meta wal_append_lsn) and the reply tag
        of each pass: first all dirty, then all clean.
cell 5  on a fresh server with the same table at --stream-rows: a serial
        autocommit stream `UPDATE p SET v = ? WHERE id = ?` (random keys, one
        connection) runs --window-s alone (pre), through the pass (during), then
        --window-s alone (post). Every statement's start and latency is recorded.
cell 6  ADD COLUMN latency: --add-trials of a vacant-slot ADD and of a reusing ADD
        (the slot of a dropped, cleaned column), interleaved, on 3-row tables.
        One reusing ADD is also timed on the cleaned N-row table of cell 4.
Progress lines go to --progress (a file the watchdog polls).
"""
import argparse, json, os, random, struct, sys, threading, time
sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/cd-column-slot-cleaning/tools")
import bn_census as bc  # noqa: E402
import kwp  # noqa: E402
import sys_meta  # noqa: E402
from bb_overhead_benchmark import host_state  # noqa: E402

NOTE = "n" * 20  # longer than 13 bytes: spills from a varchar(16) slot


def stamp(a, msg):
    with open(a.progress, "a") as f:
        f.write(f"{time.strftime('%H:%M:%S', time.gmtime())} {msg}\n")


def meta_num(conn):
    _, rows, tag, _ = conn.execute("SHOW META")
    return sys_meta.numeric(sys_meta.from_rows(rows, tag))


def pcts(xs):
    xs = sorted(xs)
    if not xs:
        return {"n": 0}
    f = lambda p: xs[max(0, min(len(xs) - 1, -(-p * len(xs) // 100) - 1))] * 1e6
    return {"n": len(xs), "p0": xs[0] * 1e6, "p25": f(25), "p50": f(50), "p95": f(95),
            "p99": f(99), "max": xs[-1] * 1e6, "mean": sum(xs) / len(xs) * 1e6}


def make_table(conn, name, n, spare=2):
    bc.must(conn, f"CREATE TABLE {name} (id int64, v int64, w int64) BTREE SPARE {spare}", "CREATE")
    bc.must(conn, f"ALTER TABLE {name} ADD COLUMN note varchar(16) NULL", "ADDED")
    for lo in range(1, n + 1, 500):
        hi = min(n, lo + 499)
        bc.must(conn, f"INSERT INTO {name} (id, v, w, note) VALUES " +
                ",".join(f"({i}, {i}, {i}, '{NOTE}{i % 1000:03d}')" for i in range(lo, hi + 1)), "INSERT")
    bc.must(conn, f"ALTER TABLE {name} DROP COLUMN note", "DROPPED")


def start(a, tag):
    bc.quiet_gate(a)
    return bc.Server(a, "B", a.bin, a.port, tag)


def timed_pass(a, srv, table):
    """One PURGE DROPPED COLUMNS; returns a record."""
    def fn():
        m0 = meta_num(srv.conn)
        c0 = bc.sched_ns(srv.proc.pid)
        t0 = time.perf_counter()
        _, _, tag, _ = srv.conn.execute(f"PURGE DROPPED COLUMNS FROM {table}")
        wall = time.perf_counter() - t0
        cpu = bc.sched_ns(srv.proc.pid) - c0
        m1 = meta_num(srv.conn)
        delta = {k: m1[k] - m0[k] for k in m1 if k in m0 and isinstance(m1[k], (int, float))
                 and isinstance(m0[k], (int, float)) and m1[k] != m0[k]}
        return {"tag": tag, "wall_s": wall, "server_cpu_s": cpu / 1e9,
                "wal_bytes": m1.get("wal_append_lsn", 0) - m0.get("wal_append_lsn", 0),
                "meta_delta": delta}
    res, host = bc.clean_cell(a, fn, False, [srv.proc.pid])
    res["host"] = host
    return res


def cell4(a, out):
    for n in [int(x) for x in a.sizes.split(",")]:
        for rep in range(a.reps):
            stamp(a, f"cell4 size={n} rep={rep} load")
            srv = start(a, f"c4-{n}-{rep}")
            try:
                t0 = time.time()
                make_table(srv.conn, "p", n)
                load_s = time.time() - t0
                stamp(a, f"cell4 size={n} loaded in {load_s:.1f}s; idle wait")
                bc.wait_idle(srv)
                p1 = timed_pass(a, srv, "p")
                stamp(a, f"cell4 size={n} dirty pass {p1['wall_s']:.3f}s {p1['tag']}")
                bc.wait_idle(srv, limit_s=30)
                p2 = timed_pass(a, srv, "p")
                stamp(a, f"cell4 size={n} clean pass {p2['wall_s']:.3f}s {p2['tag']}")
                t0 = time.perf_counter()
                _, _, tag, _ = srv.conn.execute("ALTER TABLE p ADD COLUMN again int64 NULL")
                add_s = time.perf_counter() - t0
                _, rows, _, _ = srv.conn.execute("SELECT count(*) FROM p WHERE again IS NULL")
                nulls = struct.unpack("<q", rows[0][0])[0] if rows else None
                out["cell4"].append({"cell": 4, "rows": n, "rep": rep, "load_s": load_s,
                                     "dirty": p1, "clean": p2, "reusing_add_us": add_s * 1e6,
                                     "add_tag": tag, "rows_null_in_new_col": nulls})
                with open(a.out, "w") as f:
                    json.dump(out, f, indent=1)
            finally:
                srv.stop()


def cell5(a, out):
    n = a.stream_rows
    for rep in range(a.reps):
        stamp(a, f"cell5 rows={n} rep={rep} load")
        srv = start(a, f"c5-{n}-{rep}")
        try:
            make_table(srv.conn, "p", n)
            bc.wait_idle(srv)
            sconn = kwp.Connection("127.0.0.1", a.port, timeout=300)
            samples = []  # (t_start, latency, ok)
            stop = threading.Event()
            rnd = random.Random(7 + rep)

            def stream():
                k = 0
                while not stop.is_set():
                    key = 1 + rnd.randrange(n)
                    t0 = time.perf_counter()
                    _, _, got, _ = sconn.execute(f"UPDATE p SET v = {k} WHERE id = {key}")
                    samples.append((t0, time.perf_counter() - t0, got.startswith("UPDATED 1")))
                    k += 1
            th = threading.Thread(target=stream)
            host0 = host_state()
            th.start()
            time.sleep(a.window_s)
            tp0 = time.perf_counter()
            m_a = meta_num(srv.conn)
            stamp(a, f"cell5 pass starts (stream has {len(samples)} samples) control={a.control}")
            c0 = bc.sched_ns(srv.proc.pid)
            if a.control:
                time.sleep(a.control_gap_s)
                tag = "CONTROL: no pass"
            else:
                _, _, tag, _ = srv.conn.execute("PURGE DROPPED COLUMNS FROM p")
            tp1 = time.perf_counter()
            pass_cpu = (bc.sched_ns(srv.proc.pid) - c0) / 1e9
            stamp(a, f"cell5 pass ended {tp1 - tp0:.3f}s {tag}")
            m_b = meta_num(srv.conn)
            time.sleep(a.window_s)
            stop.set()
            th.join()
            sconn.close()
            m_c = meta_num(srv.conn)
            dl = lambda x, y: {k: y[k] - x[k] for k in y if k in x and isinstance(y[k], (int, float))
                               and isinstance(x[k], (int, float)) and y[k] != x[k]}
            stalls = [[round(t - tp0, 3), round(l * 1e3, 1)] for (t, l, _) in samples if l > 0.005]
            bad = sum(1 for s in samples if not s[2])
            # "pre": finished before the pass began; "post": began after it
            # ended; "during": everything else (the statement in flight when
            # the pass began, and all until it ended).
            pre = [l for (t, l, _) in samples if t + l < tp0]
            post = [l for (t, l, _) in samples if t > tp1]
            during = [l for (t, l, _) in samples if t + l >= tp0 and t <= tp1]
            _, _, tag2, _ = srv.conn.execute("PURGE DROPPED COLUMNS FROM p")
            top = [round(x * 1e3, 3) for x in sorted(l for (_, l, _) in samples)[-5:]]
            out["cell5"].append({
                "cell": 5, "rows": n, "rep": rep, "pass_wall_s": tp1 - tp0, "pass_tag": tag,
                "pass_server_cpu_s": pass_cpu, "rerun_tag": tag2, "bad": bad,
                "pre": pcts(pre), "during": pcts(during), "post": pcts(post),
                "whole": pcts([l for (_, l, _) in samples]),
                "pre_stmts_per_s": len(pre) / a.window_s, "post_stmts_per_s": len(post) / a.window_s,
                "top5_latencies_ms": top, "host": host0, "control": a.control,
                "stalls_over_5ms_[offset_from_pass_start_s, latency_ms]": stalls[:60],
                "meta_delta_pass": dl(m_a, m_b), "meta_delta_post": dl(m_b, m_c)})
            with open(a.out, "w") as f:
                json.dump(out, f, indent=1)
        finally:
            srv.stop()


def cell6(a, out):
    stamp(a, "cell6 start")
    srv = start(a, "c6")
    try:
        van, reu, van_cpu, reu_cpu = [], [], [], []
        c = srv.conn
        for k in range(a.add_trials):
            bc.must(c, f"CREATE TABLE va{k} (id int64, v int64) BTREE SPARE 1", "CREATE")
            bc.must(c, f"CREATE TABLE re{k} (id int64, v int64) BTREE SPARE 1", "CREATE")
            bc.must(c, f"INSERT INTO re{k} VALUES (1, 1),(2, 2),(3, 3)", "INSERT")
            bc.must(c, f"ALTER TABLE re{k} ADD COLUMN note varchar(16) NULL", "ADDED")
            bc.must(c, f"UPDATE re{k} SET note = '{NOTE}' WHERE id >= 1", "UPDATED")
            bc.must(c, f"ALTER TABLE re{k} DROP COLUMN note", "DROPPED")
            bc.must(c, f"PURGE DROPPED COLUMNS FROM re{k}", "PURGED")
            order = [("va", van, van_cpu), ("re", reu, reu_cpu)]
            if k % 2:
                order.reverse()
            for kind, acc, cacc in order:
                s0 = bc.sched_ns(srv.proc.pid)
                t0 = time.perf_counter()
                _, _, tag, _ = c.execute(f"ALTER TABLE {kind}{k} ADD COLUMN extra int64 NULL")
                acc.append(time.perf_counter() - t0)
                cacc.append((bc.sched_ns(srv.proc.pid) - s0) / 1e9)
                if not tag.startswith("ADDED"):
                    raise SystemExit(f"cell6: {kind}{k}: {tag}")
            time.sleep(0.05)
        out["cell6"].append({
            "cell": 6, "trials": a.add_trials, "vacant": pcts(van), "reusing": pcts(reu),
            "raw_us": {"vacant": [round(x * 1e6, 1) for x in van], "reusing": [round(x * 1e6, 1) for x in reu]},
            "vacant_server_cpu_us_median": sorted(van_cpu)[len(van_cpu) // 2] * 1e6,
            "reusing_server_cpu_us_median": sorted(reu_cpu)[len(reu_cpu) // 2] * 1e6})
        with open(a.out, "w") as f:
            json.dump(out, f, indent=1)
    finally:
        srv.stop()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--progress", required=True)
    ap.add_argument("--sizes", default="200,1000,10000,100000,1000000")
    ap.add_argument("--stream-rows", type=int, default=1000000)
    ap.add_argument("--window-s", type=float, default=3.0)
    ap.add_argument("--reps", type=int, default=1)
    ap.add_argument("--add-trials", type=int, default=40)
    ap.add_argument("--cells", default="4,5,6")
    ap.add_argument("--control", action="store_true",
                    help="cell 5 without the pass: the same stream, windows and idle gap, PURGE not sent")
    ap.add_argument("--control-gap-s", type=float, default=2.0)
    ap.add_argument("--durability", default="relaxed", help="the server's durability key (the close runs relaxed; one extra cell strict)")
    ap.add_argument("--pin-server", default="2")
    ap.add_argument("--pin-driver", default="4")
    ap.add_argument("--quiet-load", type=float, default=2.0)
    ap.add_argument("--quiet-wait", type=float, default=30.0)
    ap.add_argument("--cell-quiet-pct", type=float, default=10.0)
    ap.add_argument("--cell-quiet-wait", type=float, default=30.0)
    ap.add_argument("--cell-clean-pct", type=float, default=20.0)
    ap.add_argument("--cell-foreign-pct", type=float, default=15.0)
    ap.add_argument("--cell-retries", type=int, default=1)
    a = ap.parse_args()
    os.sched_setaffinity(0, {int(a.pin_driver)})
    os.makedirs(a.workdir, exist_ok=True)
    out = {"args": vars(a), "cell4": [], "cell5": [], "cell6": []}
    for c in a.cells.split(","):
        {"4": cell4, "5": cell5, "6": cell6}[c](a, out)
    stamp(a, "cd_pass done")


if __name__ == "__main__":
    main()
