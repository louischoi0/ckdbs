#!/usr/bin/env python3
"""DK-S4's close measurement: A/B (and A2, the same-binary control) over what DK built.

`instructions/v3.2.0/workorder-dk-descent-low-key.md` section 5 (DK-S4).
A is the engine before DK (an export of the opening commit), B the engine
after, A2 a second server started from A's binary - the noise floor from
inside the run.

M1 (cores = 1; three servers up at once, one KWP connection each, the
reactors pinned to CPUs 4, 5, 6, this driver to CPUs 0-3). Per row count N
(`--sizes`) and per shape, `--rounds` rounds; in a round every arm gets a
fresh relation preloaded with N rows (untimed) and then runs the same
`--k` statements (equal work), the arm order rotated and reversed per round.
The primary number is server CPU per statement (`/proc/<pid>/task/*/schedstat`
run time, all threads, over the timed block); client wall latency per
statement is kept for the percentile tables.

    point     SELECT * FROM t WHERE id = <even k>
    insert1   INSERT INTO t (v, w) VALUES (...)          issued key, rightmost leaf
    named     INSERT INTO t VALUES (<odd k>, ...)        named key mid-chain
    ixinsert  INSERT INTO t (v, w) VALUES (<random v>,0) a secondary index on v
    update1   UPDATE t SET w = <x> WHERE id = <even k>   point write lookup

M2 (`--cores`, one fresh server per cell, `--sessions-per-core` sessions a
core, `--writes-per-session` statements each - equal work, the wall time is
the throughput), every cell from an empty relation:

    ins1_shared  every session: issued-key INSERT into one relation
    named_ix     every session: named random-key INSERT into one relation with
                 an index on a random column, plus one reader a core doing
                 point lookups of random keys (the lookup-miss path)

The contention tallies are read before and after every cell, and every
client error is counted by its code and the first words of its message.

    dk_close.py --binA A --binB B --workdir D --out OUT.json [--heartbeat F]
"""
import argparse, hashlib, json, os, random, shutil, subprocess, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
import sys_meta  # noqa: E402

TALLIES = ["refused_btree_descend", "refused_btree_lookup", "refused_btree_parent",
           "refused_index_descend", "refused_index_parent", "structural_reruns",
           "descent_hops"]
ARMS = ["A", "A2", "B"]


def host_state():
    try:
        load = open("/proc/loadavg").read().split()[:3]
    except OSError:
        load = ["?"]
    try:
        busy = subprocess.run(["pgrep", "-c", "cc1plus"], capture_output=True, text=True,
                              timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        busy = "?"
    return {"loadavg": " ".join(load), "cc1plus": busy}


def tallies(conn):
    fields, rows, tag, _ = conn.execute("SHOW META")
    meta = sys_meta.from_rows(rows, tag)
    out = {}
    for name in TALLIES:
        v = meta.get("contention_" + name, meta.get(name))
        out[name] = int(v) if v not in (None, "") else None
    return out


def delta(a, b):
    return {k: (b[k] - a[k]) if a[k] is not None and b[k] is not None else None for k in a}


def cpu_ns(pid):
    tot = 0
    for t in os.listdir(f"/proc/{pid}/task"):
        try:
            with open(f"/proc/{pid}/task/{t}/schedstat") as f:
                tot += int(f.read().split()[0])
        except (OSError, ValueError):
            pass
    return tot


def pin(pid, cpu):
    """Move every thread of a server onto one CPU (the arms' CPUs rotate per round)."""
    for t in os.listdir(f"/proc/{pid}/task"):
        try:
            os.sched_setaffinity(int(t), {cpu})
        except OSError:
            pass


def pct(xs, q):
    xs = sorted(xs)
    if not xs:
        return None
    return xs[min(len(xs) - 1, int(q / 100.0 * len(xs)))] if q > 0 else xs[0]


def lat_summary(xs):
    return {"ops": len(xs), **{f"p{q}": round(pct(xs, q) * 1e6, 1) for q in (0, 25, 50, 95, 99)}}


class Server:
    def __init__(self, binary, workdir, tag, port, cores, cpus, frames=65536):
        self.dir = os.path.join(workdir, tag)
        shutil.rmtree(self.dir, ignore_errors=True)
        os.makedirs(self.dir)
        self.port = port
        conf = os.path.join(self.dir, "dk.conf")
        with open(conf, "w") as f:
            f.write(f"data_file = {self.dir}/dk.db\nport = {port}\ncores = {cores}\n"
                    f"durability = relaxed\nbuffer_pool_frames = {frames}\n"
                    f"log_file = dk.log\nlog_dir = {self.dir}\nlog_level = warn\n"
                    f"reactor_cpus = {','.join(str(c) for c in cpus)}\n")
        self.err = open(os.path.join(self.dir, "dk.stderr"), "w")
        self.proc = subprocess.Popen([binary, "--config", conf], stdout=self.err,
                                     stderr=subprocess.STDOUT)
        deadline = time.time() + 30
        while True:
            try:
                kwp.Connection("127.0.0.1", port, timeout=300).close()
                return
            except OSError:
                if time.time() > deadline or self.proc.poll() is not None:
                    raise SystemExit(f"server {tag} did not listen on {port}")
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
        shutil.rmtree(self.dir, ignore_errors=True)


def load(conn, table, rows):
    for i in range(0, len(rows), 500):
        vals = ",".join("(%d,%d,%d)" % r for r in rows[i:i + 500])
        conn.execute(f"INSERT INTO {table} VALUES {vals}")


def prepare(conn, shape, table, n, rng_seed):
    """Create and preload one relation; return the timed statements."""
    rng = random.Random(rng_seed)
    conn.execute(f"CREATE TABLE {table} (id int64, v int64, w int64) BTREE")
    vs = rng.sample(range(1, 4 * n + 1), n)
    load(conn, table, [(2 * (i + 1), vs[i], i) for i in range(n)])
    if shape == "ixinsert":
        conn.execute(f"CREATE INDEX {table}_ix ON {table} (v)")
    return vs


def statements(shape, table, n, k, seed):
    rng = random.Random(seed)
    if shape == "point":
        return [f"SELECT * FROM {table} WHERE id = {2 * rng.randrange(1, n + 1)}" for _ in range(k)]
    if shape == "insert1":
        return [f"INSERT INTO {table} (v, w) VALUES ({i}, {i})" for i in range(k)]
    if shape == "named":
        return [f"INSERT INTO {table} VALUES ({x}, 0, 0)"
                for x in rng.sample(range(1, 2 * n, 2), min(k, n))]
    if shape == "ixinsert":
        return [f"INSERT INTO {table} (v, w) VALUES ({x}, 0)"
                for x in rng.sample(range(4 * n + 1, 8 * n + 1), k)]
    if shape == "update1":
        return [f"UPDATE {table} SET w = {i} WHERE id = {2 * rng.randrange(1, n + 1)}"
                for i in range(k)]
    raise ValueError(shape)


def m1(a, beat):
    ports = {"A": 15761, "A2": 15762, "B": 15763}
    binary = {"A": a.binA, "A2": a.binA, "B": a.binB}
    cpus = {"A": 4, "A2": 5, "B": 6}
    srv = {x: Server(binary[x], a.workdir, f"m1{x}", ports[x], 1, [cpus[x]]) for x in ARMS}
    conns = {x: srv[x].conn() for x in ARMS}
    out = {}
    try:
        for n in [int(x) for x in a.sizes.split(",")]:
            for shape in ["point", "insert1", "named", "ixinsert", "update1"]:
                k = a.k_point if shape == "point" else a.k
                cpu = {x: [] for x in ARMS}
                lat = {x: [] for x in ARMS}
                nst = None
                host = host_state()
                for r in range(a.rounds):
                    order = ARMS[r % 3:] + ARMS[:r % 3]
                    if r % 2:
                        order = order[::-1]
                    stm = {}
                    for i, x in enumerate(ARMS):
                        pin(srv[x].proc.pid, 4 + (i + r) % 3)
                    for x in order:
                        table = f"t_{shape}_{n}_{r}"
                        prepare(conns[x], shape, table, n, 7000 + r)
                        stm[x] = statements(shape, table, n, k, 9000 + r)
                    for x in order:
                        c = conns[x]
                        s = stm[x]
                        nst = len(s)
                        lats = []
                        t0 = cpu_ns(srv[x].proc.pid)
                        for q in s:
                            t = time.perf_counter()
                            try:
                                c.execute(q)
                            except kwp.KwpError as e:
                                raise SystemExit(f"m1 {x} {shape} refused: {e.code} {e.message}")
                            lats.append(time.perf_counter() - t)
                        t1 = cpu_ns(srv[x].proc.pid)
                        cpu[x].append((t1 - t0) / 1000.0 / len(s))
                        lat[x].extend(lats)
                out.setdefault(str(n), {})[shape] = {
                    "stmts_per_block": nst, "rounds": a.rounds, "host_before": host,
                    "host_after": host_state(),
                    "cpu_us": cpu, "lat": {x: lat_summary(lat[x]) for x in ARMS}}
                beat(f"m1 n={n} {shape} done")
        return out
    finally:
        for c in conns.values():
            c.close()
        for s in srv.values():
            s.stop()


def m2_cell(a, binary, arm, cores, cell, rnd, beat):
    cpus = list(range(4, 4 + cores))
    srv = Server(binary, a.workdir, f"m2{arm}{cores}{cell}{rnd}", 15764, cores, cpus, frames=131072)
    sessions = cores * a.sessions_per_core
    try:
        admin = srv.conn()
        t = cell
        admin.execute(f"CREATE TABLE {t} (id int64, v int64, w int64) BTREE")
        if cell == "named_ix":
            admin.execute(f"CREATE INDEX {t}_v ON {t} (v)")
        stop = threading.Event()
        counts = [0] * sessions
        errors = {}
        lock = threading.Lock()
        readers = cores if cell == "named_ix" else 0
        reads = [0] * max(readers, 1)
        ready = threading.Barrier(sessions + readers + 1)

        def note(prefix, e):
            key = f"{prefix}{e.code:#x}:{' '.join(str(e.message).split()[:4])}"
            with lock:
                errors[key] = errors.get(key, 0) + 1

        def writer(i):
            c = srv.conn()
            rng = random.Random(1000 + i)
            ready.wait()
            for _ in range(a.writes_per_session):
                if cell == "named_ix":
                    s = f"INSERT INTO {t} VALUES ({rng.randrange(1, 1 << 40)}, {rng.randrange(1 << 40)}, 0)"
                else:
                    s = f"INSERT INTO {t} (v, w) VALUES ({i}, 0)"
                try:
                    c.execute(s)
                    counts[i] += 1
                except kwp.KwpError as e:
                    note("", e)
            c.close()

        def reader(i):
            c = srv.conn()
            rng = random.Random(5000 + i)
            ready.wait()
            while not stop.is_set():
                try:
                    c.execute(f"SELECT * FROM {t} WHERE id = {rng.randrange(1, 1 << 40)}")
                    reads[i] += 1
                except kwp.KwpError as e:
                    note("read ", e)
            c.close()

        ws = [threading.Thread(target=writer, args=(i,)) for i in range(sessions)]
        rs = [threading.Thread(target=reader, args=(i,)) for i in range(readers)]
        for th in ws + rs:
            th.start()
        host = host_state()
        t0 = tallies(admin)
        c0 = cpu_ns(srv.proc.pid)
        ready.wait()
        start = time.perf_counter()
        for th in ws:
            th.join()
        wall = time.perf_counter() - start
        c1 = cpu_ns(srv.proc.pid)
        stop.set()
        for th in rs:
            th.join()
        d = delta(t0, tallies(admin))
        w = sum(counts)
        res = {"arm": arm, "cores": cores, "cell": cell, "round": rnd, "sessions": sessions,
               "readers": readers, "writes_ok": w, "reads": sum(reads[:readers]),
               "wall_s": round(wall, 3), "tps": round(w / wall, 1),
               "server_cpu_us_per_write": round((c1 - c0) / 1000.0 / max(w, 1), 2),
               "errors": errors, "tallies": d, "host": host}
        beat(f"m2 {arm} cores={cores} {cell} r{rnd} tps={res['tps']} errors={sum(errors.values())}")
        admin.close()
        return res
    finally:
        srv.stop()


def m2(a, beat):
    binary = {"A": a.binA, "A2": a.binA, "B": a.binB}
    cells = []
    for cores in [int(x) for x in a.cores.split(",")]:
        for cell in ("ins1_shared", "named_ix"):
            for r in range(a.m2_rounds):
                order = ARMS[r % 3:] + ARMS[:r % 3]
                if r % 2:
                    order = order[::-1]
                for arm in order:
                    cells.append(m2_cell(a, binary[arm], arm, cores, cell, r, beat))
    return cells


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--binA", required=True)
    p.add_argument("--binB", required=True)
    p.add_argument("--workdir", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--sizes", default="200,1000,10000")
    p.add_argument("--rounds", type=int, default=12)
    p.add_argument("--k", type=int, default=200)
    p.add_argument("--k-point", type=int, default=1000)
    p.add_argument("--cores", default="2,4")
    p.add_argument("--sessions-per-core", type=int, default=4)
    p.add_argument("--writes-per-session", type=int, default=2000)
    p.add_argument("--m2-rounds", type=int, default=4)
    p.add_argument("--skip", default="", help="m1 or m2")
    p.add_argument("--heartbeat", default=None)
    a = p.parse_args()
    os.makedirs(a.workdir, exist_ok=True)
    try:
        os.sched_setaffinity(0, {0, 1, 2, 3})
    except OSError:
        pass

    def beat(line):
        msg = f"{time.strftime('%H:%M:%S', time.gmtime())} {line}"
        print(msg, flush=True)
        if a.heartbeat:
            with open(a.heartbeat, "a") as f:
                f.write(msg + "\n")

    rep = {"binA": a.binA, "binB": a.binB,
           "sha256": {k: hashlib.sha256(open(v, "rb").read()).hexdigest()
                      for k, v in (("A", a.binA), ("B", a.binB))},
           "started": time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime()),
           "host_before": host_state(), "args": vars(a)}
    beat("start")
    if "m1" not in a.skip:
        rep["m1"] = m1(a, beat)
    if "m2" not in a.skip:
        rep["m2"] = m2(a, beat)
    rep["host_after"] = host_state()
    rep["ended"] = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime())
    with open(a.out, "w") as f:
        json.dump(rep, f, indent=1)
    beat("end")


if __name__ == "__main__":
    main()
