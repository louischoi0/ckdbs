#!/usr/bin/env python3
"""DC close, barred cells: point SELECT and point UPDATE by tuple key, autocommit, cores = 1,
server CPU per statement (server threads' schedstat over the block), arms interleaved, one
server per arm, ONE server lifetime per invocation (the caller restarts it).
Tables t200, t1k, t10k (id int64, v int64), BTREE.  --arm LABEL=BINARY repeated.
Method is the CX close bisect's (arms_bench.py / cx_close.py): warm-up of a tenth untimed, arm
order rotated and reversed on alternate rounds, a block is `ops` statements."""
import argparse, json, os, random, shutil, statistics, subprocess, sys, time

sys.path.insert(0, "/home/louis/ckdbs/.claude/worktrees/dc-select-for-update/tools")
import kwp
from bn_census import sched_ns
from bb_overhead_benchmark import host_state

SIZES = {"200": 200, "1k": 1000, "10k": 10000}


def sel(r, n): return f"SELECT * FROM t{n} WHERE id = {r.randint(1, SIZES[n])}"
def upd(r, n): return f"UPDATE t{n} SET v = {r.randint(1, 1000000)} WHERE id = {r.randint(1, SIZES[n])}"


class Srv:
    def __init__(self, label, binary, port, wd, cpu):
        self.label, self.port = label, port
        self.data = os.path.join(wd, f"{label}.db")
        shutil.rmtree(self.data + ".wal", ignore_errors=True)
        if os.path.exists(self.data): os.remove(self.data)
        conf = os.path.join(wd, f"{label}.conf")
        with open(conf, "w") as f:
            f.write(f"data_file = {self.data}\nport = {port}\ncores = 1\ndurability = group\n"
                    f"buffer_pool_frames = 65536\nlog_file = {label}.log\nlog_dir = {wd}\nlog_level = warn\n")
        self.err = open(os.path.join(wd, f"{label}.stderr"), "w")
        self.proc = subprocess.Popen(["taskset", "-c", str(cpu), binary, "--config", conf],
                                     stdout=self.err, stderr=subprocess.STDOUT)
        t = time.time() + 30
        while True:
            try:
                self.conn = kwp.Connection("127.0.0.1", port, timeout=300, client_name="dc_close"); break
            except OSError:
                if time.time() > t or self.proc.poll() is not None: sys.exit(f"{label} did not start")
                time.sleep(0.1)

    def must(self, sql):
        r = self.conn.query(sql)
        if r[3] is not None: sys.exit(f"{self.label}: {sql[:80]} -> {r[3]}")
        return r

    def load(self):
        for n, rows in SIZES.items():
            self.must(f"CREATE TABLE t{n} (id int64, v int64) BTREE")
            for i in range(1, rows + 1, 500):
                vals = ", ".join(f"({k}, {k})" for k in range(i, min(rows, i + 499) + 1))
                self.must(f"INSERT INTO t{n} (id, v) VALUES {vals}")

    def stop(self):
        try: self.conn.close()
        except OSError: pass
        self.proc.terminate()
        try: self.proc.wait(30)
        except subprocess.TimeoutExpired: self.proc.kill(); self.proc.wait()


def run_cell(s, kind, n, ops, rnd, life):
    make = sel if kind == "sel" else upd
    warm = random.Random(f"{life}-{kind}-{n}-{rnd}-warm")
    for _ in range(max(1, ops // 10)): s.must(make(warm, n))
    rng = random.Random(f"{life}-{kind}-{n}-{rnd}")
    stmts = [make(rng, n) for _ in range(ops)]
    lat = []
    cpu0 = sched_ns(s.proc.pid); w0 = time.perf_counter()
    for q in stmts:
        t0 = time.perf_counter()
        r = s.conn.query(q)
        lat.append(time.perf_counter() - t0)
        if r[3] is not None: sys.exit(f"{s.label} {kind}{n}: {r[3]}")
    wall = time.perf_counter() - w0
    cpu = (sched_ns(s.proc.pid) - cpu0) / 1e3 / ops
    lat.sort()
    at = lambda q: lat[min(len(lat) - 1, int(len(lat) * q))] * 1e6
    return {"engine": s.label, "cell": f"{kind}-{n}", "round": rnd, "ops": ops, "qps": ops / wall,
            "mean_us": statistics.fmean(lat) * 1e6, "p0": at(0), "p25": at(.25), "p50": at(.5),
            "p95": at(.95), "p99": at(.99), "server_cpu_us": cpu}


def status(path, line):
    with open(path + ".tmp", "w") as f: f.write(f"{time.strftime('%FT%TZ', time.gmtime())} {line}\n")
    os.replace(path + ".tmp", path)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--arm", action="append", required=True)
    p.add_argument("--wd", required=True); p.add_argument("--json", required=True); p.add_argument("--status", required=True)
    p.add_argument("--life", default="1"); p.add_argument("--rounds", type=int, default=10)
    p.add_argument("--ops-sel", type=int, default=10000); p.add_argument("--ops-upd", type=int, default=10000)
    p.add_argument("--server-cpu", default="2"); p.add_argument("--base-port", type=int, default=27701)
    a = p.parse_args()
    os.makedirs(a.wd, exist_ok=True)
    srvs = []
    for i, spec in enumerate(a.arm):
        label, binary = spec.split("=", 1)
        srvs.append(Srv(label, binary, a.base_port + i, a.wd, a.server_cpu))
    for s in srvs:
        status(a.status, f"load {s.label}"); s.load()
    cells = [(k, n) for n in SIZES for k in ("sel", "upd")]
    out = []; t0 = time.time(); m = len(srvs)
    for rnd in range(1, a.rounds + 1):
        for kind, n in cells:
            order = srvs[(rnd - 1) % m:] + srvs[:(rnd - 1) % m]
            if rnd % 2 == 0: order = order[::-1]
            for s in order:
                status(a.status, f"life {a.life} round {rnd}/{a.rounds} {kind}{n} {s.label} elapsed {time.time() - t0:.0f}s")
                r = run_cell(s, kind, n, a.ops_sel if kind == "sel" else a.ops_upd, rnd, a.life)
                r["host"] = host_state(); r["life"] = a.life
                out.append(r)
        json.dump(out, open(a.json + ".tmp", "w")); os.replace(a.json + ".tmp", a.json)
    for s in srvs: s.stop()
    status(a.status, "DONE")


if __name__ == "__main__":
    main()
