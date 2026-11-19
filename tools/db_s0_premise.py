#!/usr/bin/env python3
"""DB-S0's census: pages written per second against log bytes per second.

    db_s0_premise.py --bin BINARY --workdir DIR --port P --cores N
                     [--durability group] [--clients 4] [--secs 25] [--warm 4]
                     [--cells select-pk:10000,update-lit:200,...] --out OUT.json

`instructions/v3.2.0/workorder-db-torn-page-healing.md` §5 (DB-S0) and DB-Q5 (a).
Per cell a fresh server and a fresh data file (`cores`, `durability`, the
default `checkpoint_interval_ms`, `buffer_pool_frames = 65536` unless the cell
names its own), the cell's relation loaded, `--warm` seconds of the workload,
then `--secs` seconds of steady state across `--clients` closed-loop
connections. `SHOW META` and `sys.checkpoints` / `sys.statement_stats` are read
around the steady window and the differences written to the JSON line:

  statements, window seconds            (client side, tag-checked)
  wal_append_lsn / wal_durable_lsn      (the log's bytes: an LSN is a position)
  contention_checkpoint_pages_written   (pages the checkpoint's steps and publish wrote)
  pool_dirty_drained, pool_dirty_queued (pages the eviction drain wrote)
  contention_checkpoint_runs            (checkpoints finished)
  sys.checkpoints rows with seq above the window's first (pages_flushed, dirty_at_start, duration_us)
  sys.statement_stats log_bytes / log_records of the cell's statement shape

A cell is `shape:rows` or `shape:rows:pool_frames`. Shapes: select-pk,
update-lit (UPDATE t SET v = ? WHERE id = ?, uniform keys), update-spill-assign
(a 300-byte varchar assigned, so a var-heap value is written), insert (named
keys from one shared counter, so the appends meet one tail leaf).
"""
import argparse, json, os, random, shutil, subprocess, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
import sys_meta  # noqa: E402

SPILL = "x" * 300
FIELDS = ["wal_append_lsn", "wal_durable_lsn", "contention_checkpoint_pages_written",
          "contention_checkpoint_runs", "contention_data_syncs", "pool_dirty_drained",
          "pool_dirty_queued", "pool_reclaimed_inline", "pool_reclaimed_background",
          "pool_resident", "pool_misses", "pages_allocated", "pages_reused",
          "undo_pages_live", "undo_pages_recycled", "wal_syncs", "wal_ring_full",
          "wal_group_commits", "wal_group_batches"]


def must(conn, sql, tag):
    _, _, got, err = conn.execute(sql)
    if not got.startswith(tag):
        raise SystemExit(f"setup: {sql!r} -> {got!r} {err}")


def load(conn, name, rows, cols, row):
    must(conn, f"CREATE TABLE {name} ({cols}) BTREE", "CREATE")
    for lo in range(1, rows + 1, 500):
        hi = min(rows, lo + 499)
        must(conn, f"INSERT INTO {name} VALUES " + ",".join(row(i) for i in range(lo, hi + 1)),
             "INSERT")


def meta(conn):
    _, rows, tag, _ = conn.execute("SHOW META")
    d = sys_meta.numeric(sys_meta.from_rows(rows, tag))
    return {k: d.get(k) for k in FIELDS}


def checkpoints(conn):
    _, rows, _, _ = conn.execute(
        "SELECT seq, core, duration_us, steps, pages_flushed, dirty_at_start, log_wait_us, "
        "data_wait_us, resumed, outcome FROM sys.checkpoints")
    out = []
    for r in rows:
        c = [x for x in r]

        def num(x):
            if x is None:
                return None
            return int.from_bytes(x, "little", signed=True) if len(x) == 8 else int(x.decode())
        out.append({"seq": num(c[0]), "core": num(c[1]), "duration_us": num(c[2]),
                    "steps": num(c[3]), "pages_flushed": num(c[4]),
                    "dirty_at_start": num(c[5]), "log_wait_us": num(c[6]),
                    "data_wait_us": num(c[7]), "resumed": num(c[8]),
                    "outcome": (c[9] or b"").decode()})
    return out


def stmt_stats(conn, needle):
    _, rows, _, _ = conn.execute(
        "SELECT kind, calls, log_bytes, log_records, shape_text FROM sys.statement_stats")
    calls = lb = lr = 0

    def num(x):
        if x is None:
            return 0
        return int.from_bytes(x, "little", signed=True) if len(x) == 8 else int(x.decode())
    for r in rows:
        if needle in (r[4] or b"").decode():
            calls += num(r[1]); lb += num(r[2]); lr += num(r[3])
    return {"calls": calls, "log_bytes": lb, "log_records": lr}


class Server:
    def __init__(self, a, tag, frames):
        os.makedirs(a.workdir, exist_ok=True)
        self.data = os.path.join(a.workdir, f"db-{tag}.db")
        conf = os.path.join(a.workdir, f"db-{tag}.conf")
        if os.path.exists(self.data):
            os.remove(self.data)
        shutil.rmtree(self.data + ".wal", ignore_errors=True)
        with open(conf, "w") as f:
            f.write(f"data_file = {self.data}\nport = {a.port}\ncores = {a.cores}\n"
                    f"durability = {a.durability}\nbuffer_pool_frames = {frames}\n"
                    f"log_file = db-{tag}.log\nlog_dir = {a.workdir}\nlog_level = warn\n")
        self.err = open(os.path.join(a.workdir, f"db-{tag}.stderr"), "w")
        self.proc = subprocess.Popen([a.bin, "--config", conf], stdout=self.err,
                                     stderr=subprocess.STDOUT)
        deadline = time.time() + 30
        while True:
            try:
                self.conn = kwp.Connection("127.0.0.1", a.port, timeout=120)
                break
            except OSError:
                if time.time() > deadline or self.proc.poll() is not None:
                    raise SystemExit("server did not listen")
                time.sleep(0.1)

    def stop(self):
        try:
            self.conn.close()
        except OSError:
            pass
        self.proc.terminate()
        try:
            self.proc.wait(60)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.err.close()


def workload(shape, rows, a):
    """(setup(conn), needle, make(tid) -> callable(conn) running one statement)."""
    counter = [rows]
    lock = threading.Lock()
    if shape == "select-pk":
        def setup(c):
            load(c, "t", rows, "id int64, v int64, w int64", lambda i: f"({i}, {i}, {i})")
        def make(tid):
            rnd = random.Random(1000 + tid)
            return lambda c: c.query(f"SELECT * FROM t WHERE id = {rnd.randint(1, rows)}"), "SELECT"
        return setup, "FROM t WHERE id", make
    if shape == "update-lit":
        def setup(c):
            load(c, "t", rows, "id int64, v int64, w int64", lambda i: f"({i}, {i}, {i})")
        def make(tid):
            rnd = random.Random(2000 + tid)
            return (lambda c: c.query(f"UPDATE t SET v = {rnd.randint(1, 10**9)} "
                                      f"WHERE id = {rnd.randint(1, rows)}")), "UPDATED 1"
        return setup, "UPDATE t SET v", make
    if shape == "update-spill-assign":
        def setup(c):
            load(c, "sa", rows, "id int64, v int64, txt varchar",
                 lambda i: f"({i}, {i}, '{SPILL}')")
        def make(tid):
            rnd = random.Random(3000 + tid)
            def one(c):
                body = SPILL[:-9] + "%09d" % rnd.randint(0, 10**9 - 1)
                return c.query(f"UPDATE sa SET txt = '{body}' WHERE id = {rnd.randint(1, rows)}")
            return one, "UPDATED 1"
        return setup, "UPDATE sa SET txt", make
    if shape == "insert":
        def setup(c):
            must(c, "CREATE TABLE i (id int64, v int64, w int64) BTREE", "CREATE")
        def make(tid):
            def one(c):
                with lock:
                    counter[0] += 1
                    k = counter[0]
                return c.query(f"INSERT INTO i VALUES ({k}, {k}, {k})")
            return one, "INSERT"
        return setup, "INSERT INTO i", make
    raise SystemExit("unknown shape " + shape)


def drive(a, make, seconds, bad, stop_at=None):
    done = [0] * a.clients
    conns = [kwp.Connection("127.0.0.1", a.port, timeout=120) for _ in range(a.clients)]
    end = time.time() + seconds

    def run(tid):
        one, tag = make(tid)
        c = conns[tid]
        n = 0
        while time.time() < end:
            t, _, _, err = one(c)
            if not t.startswith(tag):
                bad[0] += 1
            n += 1
        done[tid] = n
    ths = [threading.Thread(target=run, args=(t,)) for t in range(a.clients)]
    t0 = time.time()
    for t in ths:
        t.start()
    return conns, ths, done, t0


def finish(conns, ths):
    for t in ths:
        t.join()
    for c in conns:
        c.close()


def cell(a, spec, progress):
    parts = spec.split(":")
    shape, rows = parts[0], int(parts[1])
    frames = int(parts[2]) if len(parts) > 2 else 65536
    tag = f"{shape}-{rows}-c{a.cores}"
    srv = Server(a, tag, frames)
    try:
        setup, needle, make = workload(shape, rows, a)
        setup(srv.conn)
        loadavg = open("/proc/loadavg").read().split()[0]
        bad = [0]
        progress(f"{tag}: warm {a.warm}s")
        conns, ths, done, _ = drive(a, make, a.warm, bad)
        finish(conns, ths)
        warm_ops = sum(done)
        # Steady window. The reading connection is idle during it.
        ck0 = max([r["seq"] for r in checkpoints(srv.conn)] or [0])
        m0, s0 = meta(srv.conn), stmt_stats(srv.conn, needle)
        w0 = time.time()
        conns, ths, done, t0 = drive(a, make, a.secs, bad)
        finish(conns, ths)
        w1 = time.time()
        m1, s1 = meta(srv.conn), stmt_stats(srv.conn, needle)
        ck = [r for r in checkpoints(srv.conn) if r["seq"] > ck0]
        win = w1 - w0
        d = {k: (m1[k] - m0[k]) if m1[k] is not None and m0[k] is not None else None
             for k in FIELDS}
        d.update({"stmt_" + k: s1[k] - s0[k] for k in s1})
        res = {"cell": tag, "shape": shape, "rows": rows, "cores": a.cores,
               "durability": a.durability, "clients": a.clients, "frames": frames,
               "window_s": win, "statements": sum(done), "bad": bad[0], "warm_ops": warm_ops,
               "loadavg_before": loadavg, "loadavg_after": open("/proc/loadavg").read().split()[0],
               "delta": d, "meta_end": m1, "checkpoints": ck}
        progress(f"{tag}: done {sum(done)} stmts in {win:.1f}s, bad={bad[0]}, "
                 f"ckpt_runs={d['contention_checkpoint_runs']}")
        return res
    finally:
        srv.stop()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--cores", type=int, default=1)
    ap.add_argument("--durability", default="group")
    ap.add_argument("--clients", type=int, default=4)
    ap.add_argument("--secs", type=float, default=25)
    ap.add_argument("--warm", type=float, default=4)
    ap.add_argument("--cells", default="select-pk:10000,update-lit:200,update-lit:1000,"
                    "update-lit:10000,update-lit:100000,update-spill-assign:10000,insert:0")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    plog = os.path.join(a.workdir, f"progress-c{a.cores}.log")
    os.makedirs(a.workdir, exist_ok=True)

    def progress(msg):
        with open(plog, "a") as f:
            f.write(time.strftime("%H:%M:%S ") + msg + "\n")
    with open(a.out, "w") as out:
        for spec in a.cells.split(","):
            r = cell(a, spec, progress)
            out.write(json.dumps(r) + "\n")
            out.flush()
    progress("ALL DONE")


if __name__ == "__main__":
    main()
