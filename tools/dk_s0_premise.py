#!/usr/bin/env python3
"""DK-S0's premise cells: how a btree descent fetches, restarts and refuses.

`instructions/v3.2.0/workorder-dk-descent-low-key.md` section 5 (DK-S0).
Needs a server built from the census spike (branch `worktree-dk-s0-spike`,
never landed), whose `sys.meta` contention block carries the `dk_*` tallies:
descents, write descents, page fetches inside a descent, the write descent's
second (exclusive) fetch of its leaf, coverage fetches of the right sibling,
restarts, and a read lookup's miss restarts - for the clustered tree (`bt`)
and the secondary tree (`ix`).

M1 (cores = 1, one connection): per statement shape, the tallies' delta over
`--m1-stmts` statements, divided by the count.

    point     SELECT * FROM m1 WHERE id = <k>            (clustered read)
    insert1   INSERT INTO m1p (v, w) VALUES (...)       (issued key, rightmost leaf)
    named     INSERT INTO m1p VALUES (<odd k>, ...)     (named key mid-chain)
    ixprobe   SELECT * FROM m1 WHERE v = <k>            (secondary probe + resolve)
    ixinsert  INSERT INTO m1 (v, w) VALUES (<new v>, ...) (both trees)

M2/M3 (cores in `--cores`, one fresh server each, `--sessions-per-core`
sessions a core, `--seconds` a cell), every cell from an empty relation, so
the window where a root grows a level under load (M3) is inside every cell;
the tallies are also read every 0.5 s, and the first second is reported
apart:

    ins1_shared  every session: issued-key INSERT into one relation
    ins1_own     every session: issued-key INSERT into a relation of its own
    named_ix     every session: named random-key INSERT into one relation with
                 an index on a random column; one reader a core doing point
                 lookups of random keys (lookup misses)

Every client error is counted by its code and the first words of its message.

    dk_s0_premise.py --bin B --workdir D --out OUT.json
"""
import argparse, hashlib, json, os, random, shutil, subprocess, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
import sys_meta  # noqa: E402

DK = ["dk_bt_descents", "dk_bt_write_descents", "dk_bt_fetches", "dk_bt_refetches",
      "dk_bt_cover_fetches", "dk_bt_restarts", "dk_bt_lookup_restarts",
      "dk_ix_descents", "dk_ix_write_descents", "dk_ix_fetches", "dk_ix_refetches",
      "dk_ix_cover_fetches", "dk_ix_restarts",
      "refused_btree_descend", "refused_btree_lookup", "refused_index_descend",
      "refused_btree_parent", "refused_btree_secure", "refused_index_parent",
      "refused_index_secure", "structural_reruns"]
TICK = os.sysconf("SC_CLK_TCK")


def host_state():
    try:
        load = open("/proc/loadavg").read().strip()
    except OSError:
        load = "unavailable"
    try:
        busy = subprocess.run(["pgrep", "-c", "cc1plus"], capture_output=True, text=True,
                              timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        busy = "?"
    return {"loadavg": load, "cc1plus": busy}


def tallies(conn):
    fields, rows, tag, _ = conn.execute("SHOW META")
    meta = sys_meta.from_rows(rows, tag)
    out = {}
    for name in DK:
        v = meta.get(name, meta.get("contention_" + name))
        out[name] = int(v) if v not in (None, "") else None
    return out


def delta(a, b):
    return {k: (b[k] - a[k]) if a[k] is not None and b[k] is not None else None for k in a}


class Server:
    def __init__(self, a, cores, tag):
        self.dir = os.path.join(a.workdir, tag)
        shutil.rmtree(self.dir, ignore_errors=True)
        os.makedirs(self.dir)
        self.port = a.port
        conf = os.path.join(self.dir, "dk.conf")
        cpus = ",".join(str(4 + i) for i in range(cores)) if cores <= 4 else None
        with open(conf, "w") as f:
            f.write(f"data_file = {self.dir}/dk.db\nport = {a.port}\ncores = {cores}\n"
                    f"durability = relaxed\nbuffer_pool_frames = 131072\n"
                    f"log_file = dk.log\nlog_dir = {self.dir}\nlog_level = warn\n")
            if cpus:
                f.write(f"reactor_cpus = {cpus}\n")
        self.err = open(os.path.join(self.dir, "dk.stderr"), "w")
        self.proc = subprocess.Popen([a.bin, "--config", conf], stdout=self.err,
                                     stderr=subprocess.STDOUT)
        deadline = time.time() + 30
        while True:
            try:
                kwp.Connection("127.0.0.1", self.port, timeout=300).close()
                return
            except OSError:
                if time.time() > deadline or self.proc.poll() is not None:
                    raise SystemExit(f"server did not listen on {self.port}")
                time.sleep(0.1)

    def conn(self):
        return kwp.Connection("127.0.0.1", self.port, timeout=300)

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(20)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.err.close()


def load(conn, table, rows):
    for i in range(0, len(rows), 500):
        vals = ",".join("(%d,%d,%d)" % r for r in rows[i:i + 500])
        conn.execute(f"INSERT INTO {table} VALUES {vals}")


def m1(a, beat):
    srv = Server(a, 1, "m1")
    try:
        c = srv.conn()
        c.execute("CREATE TABLE m1 (id int64, v int64, w int64) BTREE")
        c.execute("CREATE TABLE m1p (id int64, v int64, w int64) BTREE")
        n = a.m1_rows
        rng = random.Random(20261121)
        vs = rng.sample(range(1, 4 * n), n)
        load(c, "m1", [(2 * (i + 1), vs[i], i) for i in range(n)])
        load(c, "m1p", [(2 * (i + 1), i, i) for i in range(n)])
        c.execute("CREATE INDEX ix ON m1 (v)")
        k = a.m1_stmts
        keys = [2 * rng.randrange(1, n + 1) for _ in range(k)]
        odd = rng.sample(range(1, 2 * n, 2), k)
        newv = rng.sample(range(4 * n, 8 * n), k)
        probe = [vs[rng.randrange(n)] for _ in range(k)]
        shapes = {
            "point": [f"SELECT * FROM m1 WHERE id = {x}" for x in keys],
            "insert1": [f"INSERT INTO m1p (v, w) VALUES ({i}, {i})" for i in range(k)],
            "named": [f"INSERT INTO m1p VALUES ({x}, 0, 0)" for x in odd],
            "ixprobe": [f"SELECT * FROM m1 WHERE v = {x}" for x in probe],
            "ixinsert": [f"INSERT INTO m1 (v, w) VALUES ({x}, 0)" for x in newv],
        }
        out = {}
        for name, stmts in shapes.items():
            t0 = tallies(c)
            errs = 0
            for s in stmts:
                try:
                    c.execute(s)
                except kwp.KwpError:
                    errs += 1
            t1 = tallies(c)
            d = delta(t0, t1)
            out[name] = {"stmts": len(stmts), "errors": errs,
                         "per_stmt": {kk: (round(v / len(stmts), 4) if v is not None else None)
                                      for kk, v in d.items()}}
            beat(f"m1 {name} done")
        c.close()
        return out
    finally:
        srv.stop()


def m2(a, cores, beat):
    srv = Server(a, cores, f"m2c{cores}")
    sessions = cores * a.sessions_per_core
    results = {}
    try:
        admin = srv.conn()
        for cell in ("ins1_shared", "ins1_own", "named_ix"):
            tables = []
            if cell == "ins1_own":
                tables = [f"{cell}_{i}" for i in range(sessions)]
            else:
                tables = [cell]
            for t in tables:
                admin.execute(f"CREATE TABLE {t} (id int64, v int64, w int64) BTREE")
            if cell == "named_ix":
                admin.execute(f"CREATE INDEX {cell}_v ON {cell} (v)")
            stop = threading.Event()
            counts = [0] * sessions
            errors = {}
            lock = threading.Lock()
            readers = cores if cell == "named_ix" else 0
            reads = [0] * max(readers, 1)

            def writer(i):
                c = srv.conn()
                rng = random.Random(1000 + i)
                t = tables[i] if cell == "ins1_own" else tables[0]
                while not stop.is_set():
                    if cell == "named_ix":
                        k = rng.randrange(1, 1 << 40)
                        s = f"INSERT INTO {t} VALUES ({k}, {rng.randrange(1 << 40)}, 0)"
                    else:
                        s = f"INSERT INTO {t} (v, w) VALUES ({i}, 0)"
                    try:
                        c.execute(s)
                        counts[i] += 1
                    except kwp.KwpError as e:
                        key = f"{e.code}:{' '.join(str(e.message).split()[:4])}"
                        with lock:
                            errors[key] = errors.get(key, 0) + 1
                c.close()

            def reader(i):
                c = srv.conn()
                rng = random.Random(5000 + i)
                while not stop.is_set():
                    try:
                        c.execute(f"SELECT * FROM {tables[0]} WHERE id = {rng.randrange(1, 1 << 40)}")
                        reads[i] += 1
                    except kwp.KwpError as e:
                        key = f"read {e.code}:{' '.join(str(e.message).split()[:4])}"
                        with lock:
                            errors[key] = errors.get(key, 0) + 1
                c.close()

            t0 = tallies(admin)
            threads = [threading.Thread(target=writer, args=(i,)) for i in range(sessions)]
            threads += [threading.Thread(target=reader, args=(i,)) for i in range(readers)]
            start = time.time()
            for th in threads:
                th.start()
            series = []
            while time.time() - start < a.seconds:
                time.sleep(0.5)
                series.append((round(time.time() - start, 2), delta(t0, tallies(admin))))
            stop.set()
            for th in threads:
                th.join()
            t1 = tallies(admin)
            d = delta(t0, t1)
            first = next((s for t_, s in series if t_ >= 1.0), series[0][1] if series else d)
            results[cell] = {"cores": cores, "sessions": sessions, "readers": readers,
                             "seconds": round(time.time() - start, 2),
                             "writes": sum(counts), "reads": sum(reads[:readers]),
                             "errors": errors, "total": d, "first_second": first,
                             "host": host_state()}
            beat(f"m2 cores={cores} {cell} writes={sum(counts)} errors={sum(errors.values())}")
        admin.close()
        return results
    finally:
        srv.stop()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--bin", required=True)
    p.add_argument("--workdir", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--port", type=int, default=15741)
    p.add_argument("--m1-rows", type=int, default=50_000)
    p.add_argument("--m1-stmts", type=int, default=5_000)
    p.add_argument("--cores", default="1,2,4")
    p.add_argument("--sessions-per-core", type=int, default=4)
    p.add_argument("--seconds", type=float, default=8.0)
    p.add_argument("--heartbeat", default=None)
    a = p.parse_args()
    os.makedirs(a.workdir, exist_ok=True)

    def beat(line):
        msg = f"{time.strftime('%H:%M:%S', time.gmtime())} {line}"
        print(msg, flush=True)
        if a.heartbeat:
            with open(a.heartbeat, "a") as f:
                f.write(msg + "\n")

    sha = hashlib.sha256(open(a.bin, "rb").read()).hexdigest()
    report = {"bin": a.bin, "sha256": sha, "started": time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime()),
              "host_before": host_state(), "args": vars(a)}
    beat("start")
    report["m1"] = m1(a, beat)
    report["m2"] = {}
    for cores in [int(x) for x in a.cores.split(",")]:
        report["m2"][str(cores)] = m2(a, cores, beat)
    report["host_after"] = host_state()
    report["ended"] = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime())
    with open(a.out, "w") as f:
        json.dump(report, f, indent=1)
    beat("end")


if __name__ == "__main__":
    main()
