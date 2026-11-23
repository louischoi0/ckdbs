#!/usr/bin/env python3
"""DQ-S5's close measurement, M1 (DQ-R6): what DQ costs where it adds nothing.

`instructions/v3.2.0/workorder-dq-cabin-miss-build.md` section 5 (DQ-S5), DQ-R6.
A is the engine DQ opened from, B the closing commit, A2 a second server of
A's binary (the in-run floor). `cores = 1`, `relaxed`, `join_order = written`,
every page resident (`buffer_pool_frames = 65536`, relations made in the pool).
Three servers are up at once (one KWP connection each) on distinct physical
cores (`--cpus`, default 1,2,3; this driver on CPU 0; SMT pairs (0,4) (1,5)
(2,6) (3,7), so the siblings stay idle). The server-to-CPU map rotates per
round. Server CPU per statement is read over a timed block from
`/proc/<pid>/task/*/schedstat`, every thread, once no thread is runnable
(`settled_cpu_ns`); client wall per statement is kept for the percentile tables.

A lifetime is one start of the three servers. Per lifetime and shape the first
round is a discarded one (a lifetime's first cell reads 1-4 us dearer), then
`--rounds` timed rounds, the arm order rotated and reversed per round; every
arm runs the same statements in a round (equal work). Shapes:

    a_point    SELECT * FROM pt WHERE id = <k>            10,000-row table
    b_build    SELECT o.id, i.w FROM outb o JOIN innb i ON i.k = o.k
               16 outer rows, 1,000 inner rows, no Cabin (the build answers)
    c_cabin    the same join over outc/innc with CREATE CABIN ON innc(k),
               run until ANALYZE shows cabin_hits = 16 and no cabin_misses,
               timed only then (re-checked after every block)
    d_tuplekey SELECT o.id, i.w FROM outd o JOIN innd i ON i.id = o.k
               16 outer rows, the inner probed by its tuple key

    dq_close.py --binA A --binB B --workdir D --out OUT.json [--lifetimes 5]
                [--rounds 16] [--cpus 1,2,3] [--driver-cpu 0] [--port 15781]
                [--heartbeat F]
"""
import argparse, hashlib, json, os, random, re, shutil, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kwp  # noqa: E402
from dk_close import pin, lat_summary, host_state  # noqa: E402
from dp_close import rotated, med  # noqa: E402
from dq_s0_premise import settled_cpu_ns, quiet_gate, BUSY  # noqa: E402

ARMS = ["A", "A2", "B"]
JOIN_B = "SELECT o.id, i.w FROM outb AS o JOIN innb AS i ON i.k = o.k"
JOIN_C = "SELECT o.id, i.w FROM outc AS o JOIN innc AS i ON i.k = o.k"
JOIN_D = "SELECT o.id, i.w FROM outd AS o JOIN innd AS i ON i.id = o.k"


class Server:
    def __init__(self, binary, workdir, tag, port, cpu):
        self.dir = os.path.join(workdir, tag)
        shutil.rmtree(self.dir, ignore_errors=True)
        os.makedirs(self.dir)
        self.port = port
        conf = os.path.join(self.dir, "dq.conf")
        with open(conf, "w") as f:
            f.write(f"data_file = {self.dir}/dq.db\nport = {port}\ncores = 1\n"
                    f"durability = relaxed\nbuffer_pool_frames = 65536\n"
                    f"log_file = dq.log\nlog_dir = {self.dir}\nlog_level = warn\n"
                    f"reactor_cpus = {cpu}\njoin_order = written\n")
        self.err = open(os.path.join(self.dir, "dq.stderr"), "w")
        self.proc = subprocess.Popen([binary, "--config", conf], stdout=self.err,
                                     stderr=subprocess.STDOUT)
        deadline = time.time() + 30
        while True:
            try:
                kwp.Connection("127.0.0.1", port, timeout=600).close()
                break
            except OSError:
                if time.time() > deadline or self.proc.poll() is not None:
                    raise SystemExit(f"server {tag} did not listen on {port}")
                time.sleep(0.1)
        pin(self.proc.pid, cpu)

    def conn(self):
        return kwp.Connection("127.0.0.1", self.port, timeout=600)

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(20)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.err.close()
        shutil.rmtree(self.dir, ignore_errors=True)


def load(c, table, rows):
    for i in range(0, len(rows), 500):
        vals = ",".join("(" + ",".join(str(x) for x in r) + ")" for r in rows[i:i + 500])
        c.execute(f"INSERT INTO {table} VALUES {vals}")


def make_inner(c, name, n, seed):
    rng = random.Random(seed)
    c.execute(f"CREATE TABLE {name} (id int64, k int64, w int64) BTREE")
    perm = list(range(1, n + 1))
    rng.shuffle(perm)
    load(c, name, [(i + 1, perm[i], i) for i in range(n)])


def make_outer(c, name, n_outer, n_inner, seed):
    rng = random.Random(seed)
    c.execute(f"CREATE TABLE {name} (id int64, k int64) BTREE")
    keys = rng.sample(range(1, n_inner + 1), n_outer)
    load(c, name, [(i + 1, keys[i]) for i in range(n_outer)])


def prepare(c):
    rng = random.Random(11)
    c.execute("CREATE TABLE pt (id int64, v int64, w int64) BTREE")
    vs = rng.sample(range(1, 40001), 10000)
    load(c, "pt", [(2 * (i + 1), vs[i], i) for i in range(10000)])
    make_inner(c, "innb", 1000, 21)
    make_outer(c, "outb", 16, 1000, 22)
    make_inner(c, "innc", 1000, 21)
    make_outer(c, "outc", 16, 1000, 22)
    c.execute("CREATE CABIN ON innc(k)")
    make_inner(c, "innd", 1000, 21)
    make_outer(c, "outd", 16, 1000, 22)


def analyze_counters(c, sql, rel):
    rows = c.execute("ANALYZE " + sql)[1]
    text = "\n".join(" ".join((f or b"").decode() for f in r) for r in rows)
    for line in text.splitlines():
        if line.startswith("step ") and f" {rel} " in line and " opens=" in line:
            got = {"line": line.strip()}
            for n in ("cabin_hits", "cabin_misses", "cabin_recorded", "inner_built"):
                m = re.search(r"\b" + n + r"=(\d+)", line)
                got[n] = int(m.group(1)) if m else 0
            return got
    return {"line": None}


def converge(c):
    got = None
    for i in range(12):
        got = analyze_counters(c, JOIN_C, "innc")
        if got["line"] and got["cabin_hits"] == 16 and got["cabin_misses"] == 0:
            return got, i + 1
    raise SystemExit(f"cabin did not converge: {got}")


def busy_now():
    for p in os.listdir("/proc"):
        if p.isdigit():
            try:
                with open(f"/proc/{p}/comm") as f:
                    if f.read().strip() in BUSY:
                        return True
            except OSError:
                pass
    return False


def stmts_for(shape, k, rng):
    if shape == "a_point":
        return [f"SELECT * FROM pt WHERE id = {2 * rng.randrange(1, 10001)}" for _ in range(k)]
    return [{"b_build": JOIN_B, "c_cabin": JOIN_C, "d_tuplekey": JOIN_D}[shape]] * k


KS = {"a_point": 2000, "b_build": 300, "c_cabin": 1000, "d_tuplekey": 600}


def timed(c, pid, stm):
    lats = []
    rows = None
    t0 = settled_cpu_ns(pid)
    for q in stm:
        t = time.perf_counter()
        r = c.execute(q)
        lats.append(time.perf_counter() - t)
        rows = len(r[1])
    cpu = (settled_cpu_ns(pid) - t0) / 1000.0 / len(stm)
    return cpu, lats, rows


def lifetime(a, lt, beat, out):
    ports = {x: a.port + i for i, x in enumerate(ARMS)}
    binary = {"A": a.binA, "A2": a.binA, "B": a.binB, "B2": a.binB2}
    cpus = [int(x) for x in a.cpus.split(",")]
    srv = {x: Server(binary[x], a.workdir, f"m1{x}{lt}", ports[x], cpus[i])
           for i, x in enumerate(ARMS)}
    conns = {x: srv[x].conn() for x in ARMS}
    res = {}
    try:
        for x in ARMS:
            prepare(conns[x])
        conv = {}
        for x in ARMS:
            conv[x] = converge(conns[x])
        for shape in ["a_point", "b_build", "c_cabin", "d_tuplekey"]:
            k = KS[shape]
            held = quiet_gate(beat)
            for attempt in range(3):
                cpu = {x: [] for x in ARMS}
                lat = {x: [] for x in ARMS}
                rows = {}
                host = host_state()
                dirty = False
                for r in range(-1, a.rounds):
                    for i, x in enumerate(ARMS):
                        pin(srv[x].proc.pid, cpus[(i + max(r, 0)) % len(cpus)])
                    stm = stmts_for(shape, k, random.Random(31 + r))
                    for x in rotated(ARMS, max(r, 0)):
                        cc, l, nrows = timed(conns[x], srv[x].proc.pid, stm)
                        rows[x] = nrows
                        if r >= 0:
                            cpu[x].append(cc)
                            lat[x].extend(l)
                    if busy_now():
                        dirty = True
                        break
                if not dirty:
                    break
                beat(f"lt{lt} {shape} attempt {attempt}: a build or suite started; repeating")
                held += quiet_gate(beat)
            if dirty:
                # A block cut short by a busy host is neither whole nor quiet.
                raise SystemExit(f"lt{lt} {shape}: a build or suite ran in all 3 attempts")
            if len({rows[x] for x in ARMS}) != 1:
                raise SystemExit(f"row counts differ across arms in {shape}: {rows}")
            post = None
            if shape == "c_cabin":
                post = {x: analyze_counters(conns[x], JOIN_C, "innc") for x in ARMS}
                for x, g in post.items():
                    if g["cabin_hits"] != 16 or g["cabin_misses"]:
                        raise SystemExit(f"cabin left converged in {x}: {g}")
            res[shape] = {"k": k, "rounds": a.rounds, "rows_per_stmt": rows["A"],
                          "host_before": host, "host_after": host_state(),
                          "quiet_hold_s": round(held, 1), "post": post,
                          "cpu_us": cpu, "lat": {x: lat_summary(lat[x]) for x in ARMS},
                          "lat_sample_us": {x: [round(v * 1e6, 1) for v in lat[x][::7]] for x in ARMS}}
            bd = [b - c0 for b, c0 in zip(cpu["B"], cpu["A"])]
            fd = [b - c0 for b, c0 in zip(cpu["A2"], cpu["A"])]
            beat(f"lt{lt} {shape} k={k} A={med(cpu['A']):.3f} A2={med(cpu['A2']):.3f} "
                 f"B={med(cpu['B']):.3f} us/stmt  B-A={med(bd):+.3f} A2-A={med(fd):+.3f}")
        res["converge"] = {x: {"statements": conv[x][1], **conv[x][0]} for x in ARMS}
        out["m1"][str(lt)] = res
    finally:
        for c in conns.values():
            c.close()
        for s in srv.values():
            s.stop()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--binA", required=True)
    p.add_argument("--binB", required=True)
    p.add_argument("--binB2", default=None,
                   help="a fourth arm, B2, started from this binary (then --cpus names four CPUs)")
    p.add_argument("--workdir", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--lifetimes", type=int, default=5)
    p.add_argument("--rounds", type=int, default=16)
    p.add_argument("--cpus", default="1,2,3")
    p.add_argument("--driver-cpu", type=int, default=0)
    p.add_argument("--port", type=int, default=15781)
    p.add_argument("--heartbeat", default=None)
    a = p.parse_args()
    os.makedirs(a.workdir, exist_ok=True)
    global ARMS
    if a.binB2:
        ARMS = ["A", "A2", "B", "B2"]
    os.sched_setaffinity(0, {a.driver_cpu})

    def beat(line):
        msg = f"{time.strftime('%H:%M:%S', time.gmtime())} {line}"
        print(msg, flush=True)
        if a.heartbeat:
            with open(a.heartbeat, "a") as f:
                f.write(msg + "\n")

    out = {"binA": a.binA, "binB": a.binB,
           "sha256": {k: hashlib.sha256(open(v, "rb").read()).hexdigest()
                      for k, v in (("A", a.binA), ("B", a.binB), ("B2", a.binB2)) if v},
           "started": time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime()),
           "host_before": host_state(), "args": vars(a), "m1": {}}
    beat("start")
    for lt in range(a.lifetimes):
        lifetime(a, lt, beat, out)
        with open(a.out, "w") as f:
            json.dump(out, f, indent=1)
    out["ended"] = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime())
    with open(a.out, "w") as f:
        json.dump(out, f, indent=1)
    beat("end")


if __name__ == "__main__":
    main()
