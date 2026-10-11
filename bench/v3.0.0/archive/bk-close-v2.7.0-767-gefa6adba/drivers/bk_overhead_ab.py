#!/usr/bin/env python3
"""BK close overhead A/B: scratch driver (a measurement-stage copy; tools/ is untouched), shaped like
bench/v3.0.0/archive/bh-close-v2.7.0-711-g04649133/drivers/bh_overhead_ab.py and
bench/v3.0.0/archive/bj-close-v2.7.0-757-ge458e272/drivers/bj_close_ab.py (host-state gate and per-CPU busy fraction).

Per (cores, rows, run): a fresh server and data file for A and for B, both started from hashed copies of their
binaries; relations `be_<t> (id int64, v int64, w int64) BTREE` loaded with named ascending keys 1..rows in 500-row
INSERTs; then, arm by arm, 12 blocks of ops/12 statements, A and B interleaved with the first server of a block
alternating:

  insert        INSERT INTO be_a VALUES (<n>, <n>)                  pk omitted (the engine issues it)
  insert-again  the same shape on be_b                               noise floor (same binary, second relation)
  insert-named  INSERT INTO be_n VALUES (rows+i, i, i)               pk named, ascending above the preload, keys < 2^40
                (both servers: the one named-pk shape A accepts)
  named-pair    B ONLY, blocks alternate lo/hi first:
                  named-lo    INSERT INTO be_m VALUES (rows+i, i, i)   keys < 2^40
                  named-hi    INSERT INTO be_h VALUES (2^40+i, i, i)   keys in the upper eight bits of the 48-bit id space
                (A refuses a key >= 2^40 OutOfRange, so A runs nothing in this arm; evidence of that is the
                 refusal probe at the end of the run)
  select-pk     SELECT * FROM be_r WHERE id = k
  update-pk     UPDATE be_u SET v = q WHERE id = k                   k spread over the preload
  update-hot    UPDATE be_o SET v = q WHERE id = 1                   one row, so its version chain grows
  delete-pk     DELETE FROM be_d WHERE id = k                        keys inserted untimed, before each block
  range100      SELECT COUNT(*), MIN(id), MAX(id) FROM be_r WHERE id >= lo AND id < lo+100
  ping          SHOW META                                            control no Keystone path can touch

Per block and server the driver also reads the server's on-CPU time (sum of /proc/<pid>/task/*/schedstat, ns) around
the timed statements, so the document can give server CPU per statement.

At the end of each run (untimed): row counts of every relation, a read-back of the last hot-row write and of the first
and last upper-bits key on B, and a refusal probe on both servers (INSERT of 2^40, 2^48 and 2^48 - 1 into a fresh
relation), all recorded in the JSON.

Host state: /proc/loadavg and the real cc1plus/cmake/ctest/make/ninja list before and after every run, the other
kds_server processes on the host, and a per-CPU busy fraction over the whole run from /proc/stat.
"""
import argparse, gzip, json, os, shutil, subprocess, sys, time

TOOLS = "/home/cdkbs/ckdbs/.claude/worktrees/bk-keystone-48/tools"
sys.path.insert(0, TOOLS)
from bench_common import Phase  # noqa
from ckdbs_cli import ServerConnection  # noqa
import bb_overhead_benchmark as bb  # noqa  (host_state, meta_all, must)

BATCH = 500
HI = 1 << 40
RELS = ("a", "b", "r", "u", "o", "d", "n", "m", "h")


def cpu_times():
    out = {}
    with open("/proc/stat") as f:
        for l in f:
            if l.startswith("cpu") and l[3].isdigit():
                p = l.split()
                v = [int(x) for x in p[1:]]
                out[int(p[0][3:])] = (sum(v), v[3] + v[4])
    return out


def busy_frac(t0, t1):
    r = {}
    for c in t0:
        dt = t1[c][0] - t0[c][0]
        di = t1[c][1] - t0[c][1]
        r[c] = round(1 - di / dt, 3) if dt > 0 else 0.0
    return r


def quiet_gate(self_cpus, thr, max_wait):
    """Wait until the mean busy fraction of the CPUs outside `self_cpus`, over a 3 s window, is at or below `thr`."""
    t0 = time.time()
    while True:
        a = cpu_times(); time.sleep(3); b = cpu_times()
        bf = busy_frac(a, b)
        others = [v for c, v in bf.items() if c not in self_cpus]
        m = sum(others) / len(others)
        if m <= thr or time.time() - t0 > max_wait:
            return round(time.time() - t0, 1), round(m, 3)


def real_busy():
    out = []
    for name in ("cc1plus", "cmake", "ctest", "make", "ninja", "ld", "collect2", "cc1"):
        r = subprocess.run(["pgrep", "-x", name], capture_output=True, text=True)
        if r.stdout.strip():
            out.append(f"{name}:{','.join(r.stdout.split())}")
    return out


def other_servers(own_dir):
    r = subprocess.run(["pgrep", "-a", "-f", "kds_server"], capture_output=True, text=True)
    out = []
    for l in r.stdout.splitlines():
        if "pgrep" in l or own_dir in l or "/bin/bash" in l or "python" in l:
            continue
        out.append(l[:140])
    return out


def cpu_ns(pid):
    tot = 0
    try:
        for t in os.listdir(f"/proc/{pid}/task"):
            try:
                with open(f"/proc/{pid}/task/{t}/schedstat") as f:
                    tot += int(f.read().split()[0])
            except OSError:
                pass
    except OSError:
        pass
    return tot


class Srv:
    def __init__(self, label, binary, workdir, port, pin, tag, frames, cores, durability):
        self.label = label
        os.makedirs(workdir, exist_ok=True)
        data = os.path.join(workdir, f"{label}-{tag}.db")
        conf = os.path.join(workdir, f"{label}-{tag}.conf")
        self.files = [data, os.path.join(workdir, f"{label}-{tag}.log")]
        with open(conf, "w") as f:
            f.write(f"data_file = {data}\nport = {port}\ncores = {cores}\ndurability = {durability}\n"
                    f"buffer_pool_frames = {frames}\n"
                    f"log_file = {label}-{tag}.log\nlog_dir = {workdir}\nlog_level = warn\n")
        self.err = os.path.join(workdir, f"{label}-{tag}.stderr")
        cmd = (["taskset", "-c", str(pin)] if pin is not None else []) + [binary, "--config", conf]
        with open(self.err, "w") as e:
            self.proc = subprocess.Popen(cmd, stdout=e, stderr=subprocess.STDOUT)
        self.pid = self.proc.pid
        deadline = time.time() + 30
        while True:
            try:
                self.conn = ServerConnection("127.0.0.1", port, timeout=120)
                break
            except OSError:
                if time.time() > deadline:
                    raise SystemExit(f"server {label} did not listen on {port}")
                time.sleep(0.1)
        self.mount = bb.meta_all(self.conn)
        self.dq = []

    def rss_kb(self):
        with open(f"/proc/{self.pid}/status") as f:
            for l in f:
                if l.startswith("VmHWM"):
                    return int(l.split()[1])
        return 0

    def stop(self):
        try:
            self.conn.close()
        except Exception:
            pass
        self.proc.terminate()
        try:
            self.proc.wait(20)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        for f in self.files + [self.err]:
            try:
                os.remove(f)
            except OSError:
                pass


def pct(sorted_l, p):
    if not sorted_l:
        return 0.0
    k = max(0, min(len(sorted_l) - 1, int(-(-p * len(sorted_l) // 100)) - 1))
    return sorted_l[k]


def scalar(conn, sql):
    r = conn.send_command(sql)
    parts = r.split("\\n")
    return parts[1].split(",") if len(parts) > 1 else [r]


def run_one(args, rows, run):
    tag = f"c{args.cores}-n{rows}-r{run}"
    if os.path.exists(os.path.join(args.out, f"{tag}.json")):
        print(tag, "exists, skipped", flush=True)
        return
    self_cpus = {int(x) for x in args.pin_server.split(",")} | {int(args.pin_driver)}
    gate_wait, gate_other = quiet_gate(self_cpus, args.max_other_busy, args.max_wait)
    before = bb.host_state()
    before["real"] = real_busy()
    before["others"] = other_servers(args.workdir)
    cpu0 = cpu_times()
    tstart = time.time()
    wd = os.path.join(args.workdir, tag)
    mk = {"B": (args.bin_b, args.port_b), "A": (args.bin_a, args.port_a)}
    labels = ["B", "A"] if run % 2 == 0 else ["A", "B"]
    servers = []
    try:
        for l in labels:
            servers.append(Srv(l, mk[l][0], wd, mk[l][1], args.pin_server, tag, args.frames, args.cores, args.durability))
        servers.sort(key=lambda s: s.label)
        S = {s.label: s for s in servers}
        for s in servers:
            for t in RELS:
                bb.must(s.conn, f"CREATE TABLE be_{t} (id int64, v int64, w int64) BTREE", "CREATED")
                for lo in range(1, rows + 1, BATCH):
                    hi = min(rows, lo + BATCH - 1)
                    bb.must(s.conn, f"INSERT INTO be_{t} VALUES " +
                            ",".join(f"({i}, {i}, {i})" for i in range(lo, hi + 1)), "INSERTED")
        per_block = max(1, args.ops // args.blocks)
        n_ops = per_block * args.blocks
        ctr = {s.label: {"n": 10_000_000, "q": 0, "nn": 0, "m": 0, "h": 0, "o": 0} for s in servers}

        def ins(s, tbl, ph):
            c = ctr[s.label]; c["n"] += 1
            sql = f"INSERT INTO {tbl} VALUES ({c['n']}, {c['n']})"
            t0 = time.perf_counter(); r = s.conn.send_command(sql); ph.record(time.perf_counter() - t0, r)

        def ins_named(key):
            def fn(s, tbl, ph):
                c = ctr[s.label]; c[key] += 1
                k = rows + c[key]
                sql = f"INSERT INTO {tbl} VALUES ({k}, {c[key]}, {c[key]})"
                t0 = time.perf_counter(); r = s.conn.send_command(sql); ph.record(time.perf_counter() - t0, r)
            return fn

        def ins_hi(s, tbl, ph):
            c = ctr[s.label]; c["h"] += 1
            sql = f"INSERT INTO {tbl} VALUES ({HI + c['h']}, {c['h']}, {c['h']})"
            t0 = time.perf_counter(); r = s.conn.send_command(sql); ph.record(time.perf_counter() - t0, r)

        def sel(s, tbl, ph):
            c = ctr[s.label]; c["q"] += 1
            k = 1 + (c["q"] * 7919) % rows
            t0 = time.perf_counter(); r = s.conn.send_command(f"SELECT * FROM {tbl} WHERE id = {k}")
            ph.record(time.perf_counter() - t0, r)

        def upd(s, tbl, ph):
            c = ctr[s.label]; c["q"] += 1
            k = 1 + (c["q"] * 7919) % rows
            t0 = time.perf_counter(); r = s.conn.send_command(f"UPDATE {tbl} SET v = {c['q']} WHERE id = {k}")
            ph.record(time.perf_counter() - t0, r)

        def upd_hot(s, tbl, ph):
            c = ctr[s.label]; c["o"] += 1
            t0 = time.perf_counter(); r = s.conn.send_command(f"UPDATE {tbl} SET v = {c['o']} WHERE id = 1")
            ph.record(time.perf_counter() - t0, r)

        def dele(s, tbl, ph):
            k = s.dq.pop(0)
            t0 = time.perf_counter(); r = s.conn.send_command(f"DELETE FROM {tbl} WHERE id = {k}")
            ph.record(time.perf_counter() - t0, r)

        def rng(s, tbl, ph):
            c = ctr[s.label]; c["q"] += 1
            lo = 1 + (c["q"] * 7919) % max(1, rows - 100)
            t0 = time.perf_counter()
            r = s.conn.send_command(
                f"SELECT COUNT(*), MIN(id), MAX(id) FROM {tbl} WHERE id >= {lo} AND id < {lo + 100}")
            ph.record(time.perf_counter() - t0, r)

        def ping(s, tbl, ph):
            t0 = time.perf_counter(); r = s.conn.send_command("SHOW META"); ph.record(time.perf_counter() - t0, r)

        # (arm, [(phase, table, fn, only-label-or-None)])
        arms = [
            ("insert", [("insert", "be_a", ins, None)]),
            ("insert-again", [("insert-again", "be_b", ins, None)]),
            ("insert-named", [("insert-named", "be_n", ins_named("nn"), None)]),
            ("named-pair", [("named-lo", "be_m", ins_named("m"), "B"), ("named-hi", "be_h", ins_hi, "B")]),
            ("select-pk", [("select-pk", "be_r", sel, None)]),
            ("update-pk", [("update-pk", "be_u", upd, None)]),
            ("update-hot", [("update-hot", "be_o", upd_hot, None)]),
            ("delete-pk", [("delete-pk", "be_d", dele, None)]),
            ("range100", [("range100", "be_r", rng, None)]),
            ("ping", [("ping", None, ping, None)]),
        ]
        res = {s.label: {} for s in servers}
        cpu_acc = {s.label: {} for s in servers}
        for arm, phases in arms:
            for s in servers:
                for pname, tbl, fn, only in phases:
                    if only is None or only == s.label:
                        res[s.label][pname] = Phase(pname)
                        cpu_acc[s.label][pname] = 0
            for block in range(args.blocks):
                order = servers if block % 2 == 0 else list(reversed(servers))
                for s in order:
                    segs = [p for p in phases if p[3] is None or p[3] == s.label]
                    if block % 2 == 1:
                        segs = list(reversed(segs))
                    for pname, tbl, fn, only in segs:
                        ph = res[s.label][pname]
                        if pname == "delete-pk":
                            c = ctr[s.label]; s.dq = []
                            for _ in range(per_block):
                                c["n"] += 1; s.dq.append(c["n"])
                                bb.must(s.conn, f"INSERT INTO {tbl} VALUES ({c['n']}, {c['n']}, {c['n']})", "INSERTED")
                        c0 = cpu_ns(s.pid)
                        t0 = time.perf_counter()
                        for _ in range(per_block):
                            fn(s, tbl, ph)
                        ph.elapsed += time.perf_counter() - t0
                        cpu_acc[s.label][pname] += cpu_ns(s.pid) - c0

        # ---- verification and the refusal probe (untimed) ----
        verify = {}
        for s in servers:
            v = {}
            for t in RELS:
                v[f"count_be_{t}"] = scalar(s.conn, f"SELECT COUNT(*) FROM be_{t}")[0]
            v["hot_last_v"] = scalar(s.conn, "SELECT v FROM be_o WHERE id = 1")[0]
            if s.label == "B":
                v["hi_first"] = s.conn.send_command(f"SELECT * FROM be_h WHERE id = {HI + 1}")
                v["hi_last"] = s.conn.send_command(f"SELECT * FROM be_h WHERE id = {HI + n_ops}")
            bb.must(s.conn, "CREATE TABLE be_x (id int64, v int64, w int64) BTREE", "CREATED")
            v["probe_2_40"] = s.conn.send_command(f"INSERT INTO be_x VALUES ({1 << 40}, 1, 1)")
            v["probe_2_48"] = s.conn.send_command(f"INSERT INTO be_x VALUES ({1 << 48}, 1, 1)")
            v["probe_2_48_minus_1"] = s.conn.send_command(f"INSERT INTO be_x VALUES ({(1 << 48) - 1}, 2, 2)")
            v["probe_2_48_minus_1_read"] = s.conn.send_command(f"SELECT * FROM be_x WHERE id = {(1 << 48) - 1}")
            verify[s.label] = v
        meta_end = {s.label: bb.meta_all(s.conn) for s in servers}
        rss = {s.label: s.rss_kb() for s in servers}
        after = bb.host_state()
        after["real"] = real_busy()
        after["others"] = other_servers(args.workdir)
        cpu1 = cpu_times()
        bf = busy_frac(cpu0, cpu1)
        others = [v for c, v in bf.items() if c not in self_cpus]
        summ, raw = {}, {}
        for l, r in res.items():
            summ[l], raw[l] = {}, {}
            for n, p in r.items():
                d = p.summary()
                sl = sorted(p.latencies)
                d["p75_us"] = round(pct(sl, 75) * 1e6, 1)
                d["p90_us"] = round(pct(sl, 90) * 1e6, 1)
                d["p999_us"] = round(pct(sl, 99.9) * 1e6, 1)
                d["server_cpu_us_per_stmt"] = round(cpu_acc[l][n] / 1000.0 / max(1, p.ops), 3)
                summ[l][n] = d
                raw[l][n] = [round(x * 1e6, 1) for x in p.latencies]
        payload = {"cores": args.cores, "rows": rows, "run": run, "tag": tag, "durability": args.durability,
                   "gate_wait_s": gate_wait, "gate_other_busy": gate_other, "order_first": labels[0],
                   "frames": args.frames, "ops": n_ops, "blocks": args.blocks,
                   "t_start": tstart, "t_end": time.time(),
                   "host_before": before, "host_after": after,
                   "cpu_busy": {str(c): v for c, v in bf.items()},
                   "other_cpu_busy_mean": round(sum(others) / len(others), 3),
                   "mount_ckpt_us": {s.label: s.mount.get("recovery_checkpoint_us") for s in servers},
                   "verify": verify, "rss_hwm_kb": rss,
                   "pool_end": {l: {k: v for k, v in m.items() if k.startswith("pool_")} for l, m in meta_end.items()},
                   "arms": summ}
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, f"{tag}.json"), "w") as f:
            json.dump(payload, f, indent=1)
        with gzip.open(os.path.join(args.out, f"{tag}.raw.json.gz"), "wt") as f:
            json.dump(raw, f)
        errs = sum(p["errors"] for l in summ.values() for p in l.values())
        line = " ".join(f"{n}:A={summ['A'].get(n, {}).get('p50_us')}/B={summ['B'].get(n, {}).get('p50_us')}"
                        for n in ("insert", "insert-named", "update-pk", "select-pk", "ping"))
        print(f"{tag} load {before['loadavg'].split()[0]}->{after['loadavg'].split()[0]} other_busy={payload['other_cpu_busy_mean']} "
              f"gate={gate_wait}s errs={errs} {line}", flush=True)
    finally:
        for s in servers:
            s.stop()
        shutil.rmtree(wd, ignore_errors=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--rows", default="200,1000,10000")
    p.add_argument("--runs", type=int, default=16)
    p.add_argument("--run-start", type=int, default=0)
    p.add_argument("--ops", type=int, default=3000)
    p.add_argument("--blocks", type=int, default=12)
    p.add_argument("--frames", type=int, default=65536)
    p.add_argument("--bin-a", required=True)
    p.add_argument("--bin-b", required=True)
    p.add_argument("--port-a", type=int, required=True)
    p.add_argument("--port-b", type=int, required=True)
    p.add_argument("--pin-server", required=True)
    p.add_argument("--pin-driver", default="4")
    p.add_argument("--cores", type=int, default=1)
    p.add_argument("--durability", default="relaxed")
    p.add_argument("--workdir", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--max-other-busy", type=float, default=0.10)
    p.add_argument("--max-wait", type=float, default=30)
    args = p.parse_args()
    for rows in [int(x) for x in args.rows.split(",")]:
        for run in range(args.run_start, args.run_start + args.runs):
            run_one(args, rows, run)


if __name__ == "__main__":
    sys.exit(main())
