#!/usr/bin/env python3
"""BH close overhead A/B: scratch driver shaped like tools/bb_overhead_benchmark.py
(and bench/v3.0.0/archive/be-close-*/drivers/be_overhead_ab.py, which it extends).

Differences from that driver (a measurement-stage copy, tools/ is untouched):
  * both arms get `buffer_pool_frames = --frames` (both refuse to boot without it)
  * `--cores N` sets the server's cores (the session stays one connection)
  * new arm delete-pk: untimed INSERT of a block's fresh keys, then timed DELETE by pk
  * arms: insert (omitted pk), insert-again (noise floor), select-pk,
    update-pk, range (COUNT/MIN/MAX over a 100-id window), ping (control)
  * raw per-statement latencies are written gzipped for pooled percentiles
"""
import argparse, gzip, json, os, shutil, subprocess, sys, time

TOOLS = "/home/cdkbs/ckdbs/.claude/worktrees/ba-open-marks/tools"
sys.path.insert(0, TOOLS)
from bench_common import Phase  # noqa
from ckdbs_cli import ServerConnection  # noqa
import bb_overhead_benchmark as bb  # noqa  (host_state, meta_all, must, wait_for_load)

BATCH = 500


class Srv:
    def __init__(self, label, binary, workdir, port, pin, tag, frames, cores, dur="relaxed"):
        self.label = label
        os.makedirs(workdir, exist_ok=True)
        data = os.path.join(workdir, f"{label}-{tag}.db")
        conf = os.path.join(workdir, f"{label}-{tag}.conf")
        self.files = [data, os.path.join(workdir, f"{label}-{tag}.log")]
        with open(conf, "w") as f:
            f.write(f"data_file = {data}\nport = {port}\ncores = {cores}\ndurability = {dur}\n"
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
                self.conn = ServerConnection("127.0.0.1", port)
                break
            except OSError:
                if time.time() > deadline:
                    raise SystemExit(f"server {label} did not listen on {port}")
                time.sleep(0.1)
        self.mount = bb.meta_all(self.conn)

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


def run_one(args, rows, run):
    tag = f"n{rows}-r{run}"
    wait = bb.wait_for_load(args.max_load, args.max_wait)
    before = bb.host_state()
    wd = os.path.join(args.workdir, tag)
    mk = {"B": (args.bin_b, args.port_b), "A": (args.bin_a, args.port_a)}
    labels = ["B", "A"] if run % 2 == 0 else ["A", "B"]
    servers = []
    try:
        for l in labels:
            servers.append(Srv(l, mk[l][0], wd, mk[l][1], args.pin_server, tag, args.frames, args.cores, args.durability))
        servers.sort(key=lambda s: s.label)
        for s in servers:
            for t in ("a", "b", "r", "u", "d", "k"):
                bb.must(s.conn, f"CREATE TABLE be_{t} (id int64, v int64, w int64) BTREE", "CREATED")
                for lo in range(1, rows + 1, BATCH):
                    hi = min(rows, lo + BATCH - 1)
                    bb.must(s.conn, f"INSERT INTO be_{t} VALUES " +
                            ",".join(f"({i}, {i}, {i})" for i in range(lo, hi + 1)), "INSERTED")
        per_block = max(1, args.ops // args.blocks)
        ctr = {s.label: {"n": 10_000_000, "q": 0} for s in servers}

        def ins(s, tbl, ph):
            c = ctr[s.label]; c["n"] += 1
            sql = f"INSERT INTO {tbl} VALUES ({c['n']}, {c['n']})"
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

        def bulk(s, tbl, ph):
            c = ctr[s.label]; base = c["n"] + 1; c["n"] += 20
            sql = f"INSERT INTO {tbl} VALUES " + ",".join(f"({i}, {i}, {i})" for i in range(base, base + 20))
            t0 = time.perf_counter(); r = s.conn.send_command(sql); ph.record(time.perf_counter() - t0, r)

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

        def ping2(s, tbl, ph):
            t0 = time.perf_counter(); r = s.conn.send_command("SHOW NAMESPACES"); ph.record(time.perf_counter() - t0, r)

        def iso(s, tbl, ph):
            t0 = time.perf_counter(); r = s.conn.send_command("SET ISOLATION LEVEL READ COMMITTED"); ph.record(time.perf_counter() - t0, r)

        arms = [("ping2", None, ping2), ("iso", None, iso), ("insert", "be_a", ins), ("insert-again", "be_b", ins), ("select-pk", "be_r", sel),
                ("update-pk", "be_u", upd), ("delete-pk", "be_d", dele), ("bulk20", "be_k", bulk), ("range100", "be_r", rng), ("ping", None, ping)]
        if args.arms:
            arms = [a for a in arms if a[0] in args.arms.split(",")]
        res = {s.label: {} for s in servers}
        for name, tbl, fn in arms:
            for s in servers:
                res[s.label][name] = Phase(name)
            for block in range(args.blocks):
                order = servers if block % 2 == 0 else list(reversed(servers))
                for s in order:
                    ph = res[s.label][name]
                    if name == "delete-pk":
                        c = ctr[s.label]; s.dq = []
                        for _ in range(per_block):
                            c["n"] += 1; s.dq.append(c["n"])
                            bb.must(s.conn, f"INSERT INTO {tbl} VALUES ({c['n']}, {c['n']}, {c['n']})", "INSERTED")
                    t0 = time.perf_counter()
                    for _ in range(per_block):
                        fn(s, tbl, ph)
                    ph.elapsed += time.perf_counter() - t0
        counts = {s.label: s.conn.send_command("SELECT COUNT(*) FROM be_a").replace("\\n", " ") for s in servers}
        meta_end = {s.label: bb.meta_all(s.conn) for s in servers}
        rss = {s.label: s.rss_kb() for s in servers}
        after = bb.host_state()
        summ = {}
        raw = {}
        for l, r in res.items():
            summ[l] = {}
            raw[l] = {}
            for n, p in r.items():
                d = p.summary()
                sl = sorted(p.latencies)
                d["p90_us"] = round(pct(sl, 90) * 1e6, 1)
                d["p999_us"] = round(pct(sl, 99.9) * 1e6, 1)
                summ[l][n] = d
                raw[l][n] = [round(x * 1e6, 1) for x in p.latencies]
        payload = {"durability": args.durability, "rows": rows, "run": run, "tag": tag, "wait_s": round(wait, 1), "order_first": labels[0],
                   "frames": args.frames, "host_before": before, "host_after": after,
                   "mount_ckpt_us": {s.label: s.mount.get("recovery_checkpoint_us") for s in servers},
                   "count_be_a": counts, "rss_hwm_kb": rss,
                   "pool_end": {l: {k: v for k, v in m.items() if k.startswith("pool_")} for l, m in meta_end.items()},
                   "arms": summ}
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, f"{tag}.json"), "w") as f:
            json.dump(payload, f, indent=1)
        with gzip.open(os.path.join(args.out, f"{tag}.raw.json.gz"), "wt") as f:
            json.dump(raw, f)
        errs = sum(p["errors"] for l in summ.values() for p in l.values())
        line = " ".join(f"{n}:A={summ['A'][n]['p50_us']}/B={summ['B'][n]['p50_us']}" for n in summ["A"])
        print(f"{tag} load {before['loadavg'].split()[0]}->{after['loadavg'].split()[0]} errs={errs} {line}", flush=True)
    finally:
        for s in servers:
            s.stop()
        shutil.rmtree(wd, ignore_errors=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--rows", default="200,1000,10000")
    p.add_argument("--runs", type=int, default=12)
    p.add_argument("--ops", type=int, default=3000)
    p.add_argument("--blocks", type=int, default=12)
    p.add_argument("--frames", type=int, default=65536)
    p.add_argument("--bin-a", required=True)
    p.add_argument("--bin-b", required=True)
    p.add_argument("--port-a", type=int, required=True)
    p.add_argument("--port-b", type=int, required=True)
    p.add_argument("--pin-server", default=None)
    p.add_argument("--cores", type=int, default=1)
    p.add_argument("--durability", default="relaxed")
    p.add_argument("--arms", default=None)
    p.add_argument("--workdir", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--max-load", type=float, default=1.0)
    p.add_argument("--max-wait", type=float, default=300)
    args = p.parse_args()
    for rows in [int(x) for x in args.rows.split(",")]:
        for run in range(args.runs):
            run_one(args, rows, run)


if __name__ == "__main__":
    sys.exit(main())
