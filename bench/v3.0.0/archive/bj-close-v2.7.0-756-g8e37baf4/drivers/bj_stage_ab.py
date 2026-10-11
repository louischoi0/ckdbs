#!/usr/bin/env python3
"""BJ close: the cost of an expression SET value, and of the literal path A -> B.

Scratch driver (a measurement-stage driver; tools/ is untouched), shaped like
bench/v3.0.0/archive/bh-close-v2.7.0-712-g4015736d/drivers/bh_overhead_ab.py.

Per (cores, rows, run): a fresh server and data file for A and for B, both
started from hashed copies of their binaries, relations loaded with named
ascending keys in 500-row INSERTs (`t (id int64, v int64, w int64) BTREE`),
then

  PART 1  (A and B, interleaved in 12 blocks per arm, the first server of a
           block alternating): the literal path and the controls.
             lit-point        UPDATE t SET v = <lit> WHERE id = k
             lit-point-again  the same shape on a second relation (replicate)
             lit2-point       UPDATE t SET v = <lit>, w = <lit> WHERE id = k
             lit-range        UPDATE t SET v = <lit> WHERE id >= 1   (every row)
             lit-range-again  the same on a second relation (replicate)
             insert           INSERT, pk named, ascending above the preload
             select-pk        SELECT * FROM t WHERE id = k
             delete-pk        DELETE FROM t WHERE id = k (keys inserted untimed)
             ping             SHOW META (control no BJ path can touch)
  PART 2  (B only; arms of a group interleaved block by block on one server,
           one relation per arm): the expression's own price.
             point group (WHERE id = k): lit, expr (v = v + 1), swap
               (v = w, w = v), case (CASE WHEN v % 2 = 0 ...)
             range group (WHERE id >= 1): lit, expr, swap, case, subq
               (v = (SELECT COUNT(*) FROM <t>), run once)
  PART 3  (B only, one session): the client's former round trip.
             rt-pair   SELECT v ... WHERE id = k, then UPDATE ... SET v = <v+1 read>
             rt-one    UPDATE ... SET v = v + 1 WHERE id = k
             rt-txn    BEGIN; SELECT; UPDATE; COMMIT

Every reply is checked: `UPDATED 1` / `UPDATED <rows>` / `INSERTED` / `DELETED`
prefixes are counted as `bad` when missing, and the end state of every
updating arm is verified by a SELECT against the value the arm must have left.

Host state: /proc/loadavg and the cc1plus|cmake --build|ctest list before and
after every run, plus a per-CPU busy fraction over the whole run from
/proc/stat, so another session's load on a CPU outside the pinned set shows.
"""
import argparse, gzip, json, os, shutil, subprocess, sys, time

TOOLS = "/home/cdkbs/ckdbs/.claude/worktrees/bj-expression-update/tools"
sys.path.insert(0, TOOLS)
from bench_common import Phase  # noqa
from ckdbs_cli import ServerConnection  # noqa
import bb_overhead_benchmark as bb  # noqa  (host_state, meta_all, must)

BATCH = 500
SEP = "\\n"  # the reply's literal two-character row separator


def cpu_times():
    out = {}
    with open("/proc/stat") as f:
        for l in f:
            if l.startswith("cpu") and l[3].isdigit():
                p = l.split()
                v = [int(x) for x in p[1:]]
                idle = v[3] + v[4]
                out[int(p[0][3:])] = (sum(v), idle)
    return out


def busy_frac(t0, t1):
    r = {}
    for c in t0:
        dt = t1[c][0] - t0[c][0]
        di = t1[c][1] - t0[c][1]
        r[c] = round(1 - di / dt, 3) if dt > 0 else 0.0
    return r


def quiet_gate(self_cpus, thr, max_wait):
    """Wait until the mean busy fraction of the CPUs outside `self_cpus`, over
    a 3 s window, is below `thr`. Returns (waited_s, last_other_busy)."""
    t0 = time.time()
    while True:
        a = cpu_times(); time.sleep(3); b = cpu_times()
        bf = busy_frac(a, b)
        others = [v for c, v in bf.items() if c not in self_cpus]
        m = sum(others) / len(others)
        if m <= thr or time.time() - t0 > max_wait:
            return time.time() - t0, round(m, 3)


class Srv:
    def __init__(self, label, binary, workdir, port, pin, tag, frames, cores):
        self.label = label
        os.makedirs(workdir, exist_ok=True)
        data = os.path.join(workdir, f"{label}-{tag}.db")
        conf = os.path.join(workdir, f"{label}-{tag}.conf")
        self.files = [data, os.path.join(workdir, f"{label}-{tag}.log")]
        with open(conf, "w") as f:
            f.write(f"data_file = {data}\nport = {port}\ncores = {cores}\ndurability = relaxed\n"
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


def load_table(conn, name, rows, wofs=0):
    bb.must(conn, f"CREATE TABLE {name} (id int64, v int64, w int64) BTREE", "CREATED")
    for lo in range(1, rows + 1, BATCH):
        hi = min(rows, lo + BATCH - 1)
        bb.must(conn, f"INSERT INTO {name} VALUES " +
                ",".join(f"({i}, {i}, {i + wofs})" for i in range(lo, hi + 1)), "INSERTED")


def scalar(conn, sql):
    """First data cell of a one-row reply."""
    r = conn.send_command(sql)
    parts = r.split(SEP)
    return parts[1].split(",") if len(parts) > 1 else [r]


def run_one(args, rows, run):
    tag = f"{args.tagpre}c{args.cores}-n{rows}-r{run}"
    if os.path.exists(os.path.join(args.out, f"{tag}.json")):
        print(tag, "exists, skipped", flush=True)
        return
    self_cpus = {int(x) for x in args.pin_server.split(",")} | {int(args.pin_driver)}
    gate_wait, gate_other = quiet_gate(self_cpus, args.max_other_busy, args.max_wait)
    before = bb.host_state()
    cpu0 = cpu_times()
    tstart = time.time()
    wd = os.path.join(args.workdir, tag)
    mk = {"B": (args.bin_b, args.port_b), "A": (args.bin_a, args.port_a)}
    labels = ["B", "A"] if run % 2 == 0 else ["A", "B"]
    servers = []
    try:
        for l in labels:
            servers.append(Srv(l, mk[l][0], wd, mk[l][1], args.pin_server, tag, args.frames, args.cores))
        servers.sort(key=lambda s: s.label)
        S = {s.label: s for s in servers}
        for s in servers:
            for t in ("u1", "u2", "u3", "r1", "r2", "s", "i", "d"):
                load_table(s.conn, "p_" + t, rows)
        sB = S["B"]
        xp = ("lit", "expr", "swap", "case")
        xr = ("lit", "expr", "swap", "case", "subq")
        if not args.p1_only:
            for g in xp:
                load_table(sB.conn, f"xp_{g}", rows, 7)
            for g in xr:
                load_table(sB.conn, f"xr_{g}", rows, 7)
            load_table(sB.conn, "rt", rows)

        per_block = max(1, args.ops // args.blocks)
        per_block_r = max(1, args.range_ops // args.blocks)
        ctr = {s.label: {"n": 10_000_000, "q": 0} for s in servers}
        bad = {}

        def key(c):
            c["q"] += 1
            return 1 + (c["q"] * 7919) % rows

        def tm(s, ph, sql, expect, name):
            t0 = time.perf_counter(); r = s.conn.send_command(sql); dt = time.perf_counter() - t0
            ph.record(dt, r)
            if not r.startswith(expect):
                bad[(s.label, name)] = bad.get((s.label, name), 0) + 1
                if len(bad) < 5:
                    print("BAD", s.label, name, sql[:80], "->", r[:100], flush=True)
            return r

        # ---- PART 1 ------------------------------------------------------------
        def lit_point(tbl, name, cols=1):
            def fn(s, ph):
                c = ctr[s.label]; k = key(c)
                sets = f"v = {c['q']}" + (f", w = {c['q'] + 3}" if cols == 2 else "")
                tm(s, ph, f"UPDATE {tbl} SET {sets} WHERE id = {k}", "UPDATED 1", name)
            return fn

        def lit_range(tbl, name):
            def fn(s, ph):
                c = ctr[s.label]; c["q"] += 1
                tm(s, ph, f"UPDATE {tbl} SET v = {c['q']} WHERE id >= 1", f"UPDATED {rows}", name)
            return fn

        def ins(s, ph):
            c = ctr[s.label]; c["n"] += 1
            tm(s, ph, f"INSERT INTO p_i VALUES ({c['n']}, {c['n']}, {c['n']})", "INSERTED", "insert")

        def sel(s, ph):
            c = ctr[s.label]; k = key(c)
            tm(s, ph, f"SELECT * FROM p_s WHERE id = {k}", "id", "select-pk")

        def dele(s, ph):
            k = s.dq.pop(0)
            tm(s, ph, f"DELETE FROM p_d WHERE id = {k}", "DELETED", "delete-pk")

        def ping(s, ph):
            tm(s, ph, "SHOW META", "", "ping")

        arms1 = [("lit-point", per_block, lit_point("p_u1", "lit-point")),
                 ("lit-point-again", per_block, lit_point("p_u2", "lit-point-again")),
                 ("lit2-point", per_block, lit_point("p_u3", "lit2-point", 2)),
                 ("lit-range", per_block_r, lit_range("p_r1", "lit-range")),
                 ("lit-range-again", per_block_r, lit_range("p_r2", "lit-range-again")),
                 ("insert", per_block, ins), ("select-pk", per_block, sel),
                 ("delete-pk", per_block, dele), ("ping", per_block, ping)]
        res = {s.label: {} for s in servers}
        for name, n_in_block, fn in arms1:
            for s in servers:
                res[s.label][name] = Phase(name)
            for block in range(args.blocks):
                order = servers if block % 2 == 0 else list(reversed(servers))
                for s in order:
                    ph = res[s.label][name]
                    if name == "delete-pk":
                        c = ctr[s.label]; s.dq = []
                        for _ in range(n_in_block):
                            c["n"] += 1; s.dq.append(c["n"])
                            bb.must(s.conn, f"INSERT INTO p_d VALUES ({c['n']}, {c['n']}, {c['n']})", "INSERTED")
                    t0 = time.perf_counter()
                    for _ in range(n_in_block):
                        fn(s, ph)
                    ph.elapsed += time.perf_counter() - t0

        res2 = {}
        qBr = {}
        verify = {}
        tot_i = rows * (rows + 1) // 2
        if not args.p1_only:
            # ---- PART 2 (B only) ------------------------------------------------------
            sqlp = {"lit": lambda q, k: f"UPDATE xp_lit SET v = {q} WHERE id = {k}",
                    "expr": lambda q, k: f"UPDATE xp_expr SET v = v + 1 WHERE id = {k}",
                    "swap": lambda q, k: f"UPDATE xp_swap SET v = w, w = v WHERE id = {k}",
                    "case": lambda q, k: f"UPDATE xp_case SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END WHERE id = {k}"}
            sqlr = {"lit": lambda q: f"UPDATE xr_lit SET v = {q} WHERE id >= 1",
                    "expr": lambda q: "UPDATE xr_expr SET v = v + 1 WHERE id >= 1",
                    "swap": lambda q: "UPDATE xr_swap SET v = w, w = v WHERE id >= 1",
                    "case": lambda q: "UPDATE xr_case SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END WHERE id >= 1",
                    "subq": lambda q: "UPDATE xr_subq SET v = (SELECT COUNT(*) FROM xr_subq) WHERE id >= 1"}
            res2 = {}
            # model of each point table: v, w per key
            model = {g: {i: [i, i + 7] for i in range(1, rows + 1)} for g in xp}
            qB = {g: 0 for g in xp}
            qBr = {g: 0 for g in xr}
            for grp, names, sqls, nb in (("xp", xp, sqlp, per_block), ("xr", xr, sqlr, per_block_r)):
                for g in names:
                    res2[f"{grp}-{g}"] = Phase(f"{grp}-{g}")
                for block in range(args.blocks):
                    rot = block % len(names)
                    for g in names[rot:] + names[:rot]:
                        ph = res2[f"{grp}-{g}"]
                        t0 = time.perf_counter()
                        for _ in range(nb):
                            if grp == "xp":
                                qB[g] += 1
                                k = 1 + (qB[g] * 7919) % rows
                                tm(sB, ph, sqls[g](qB[g], k), "UPDATED 1", f"{grp}-{g}")
                                m = model[g][k]
                                if g == "lit": m[0] = qB[g]
                                elif g == "expr": m[0] += 1
                                elif g == "swap": m[0], m[1] = m[1], m[0]
                                else: m[0] = m[0] + 1 if m[0] % 2 == 0 else m[0] - 1
                            else:
                                qBr[g] += 1
                                tm(sB, ph, sqls[g](qBr[g]), f"UPDATED {rows}", f"{grp}-{g}")
                        ph.elapsed += time.perf_counter() - t0

            # ---- PART 3 (B only) ------------------------------------------------------
            for n_ in ("rt-pair", "rt-pair-select", "rt-pair-update", "rt-one", "rt-txn"):
                res2[n_] = Phase(n_)
            qrt = 0
            rt_ops = 0
            names3 = ["rt-pair", "rt-one", "rt-txn"]

            def rt_select(ph, k):
                t0 = time.perf_counter(); r = sB.conn.send_command(f"SELECT v FROM rt WHERE id = {k}"); dt = time.perf_counter() - t0
                ph.record(dt, r)
                try:
                    return int(r.split(SEP)[1].strip().split(",")[0]), dt
                except Exception:
                    bad[("B", "rt-select-parse")] = bad.get(("B", "rt-select-parse"), 0) + 1
                    return 0, dt

            for block in range(args.blocks):
                rot = block % 3
                for g in names3[rot:] + names3[:rot]:
                    t0 = time.perf_counter()
                    for _ in range(per_block):
                        qrt += 1
                        k = 1 + (qrt * 7919) % rows
                        rt_ops += 1
                        if g == "rt-pair":
                            ta = time.perf_counter()
                            v, _ = rt_select(res2["rt-pair-select"], k)
                            tb = time.perf_counter()
                            r = sB.conn.send_command(f"UPDATE rt SET v = {v + 1} WHERE id = {k}")
                            tc = time.perf_counter()
                            res2["rt-pair-update"].record(tc - tb, r)
                            res2["rt-pair"].record(tc - ta, r)
                            if not r.startswith("UPDATED 1"):
                                bad[("B", "rt-pair")] = bad.get(("B", "rt-pair"), 0) + 1
                        elif g == "rt-one":
                            tm(sB, res2["rt-one"], f"UPDATE rt SET v = v + 1 WHERE id = {k}", "UPDATED 1", "rt-one")
                        else:
                            ta = time.perf_counter()
                            r0 = sB.conn.send_command("BEGIN")
                            v, _ = rt_select(Phase("tmp"), k)
                            r = sB.conn.send_command(f"UPDATE rt SET v = {v + 1} WHERE id = {k}")
                            r2 = sB.conn.send_command("COMMIT")
                            res2["rt-txn"].record(time.perf_counter() - ta, r)
                            if not (r0.startswith("BEGIN") and r.startswith("UPDATED 1") and r2.startswith("COMMIT")):
                                bad[("B", "rt-txn")] = bad.get(("B", "rt-txn"), 0) + 1
                    res2[g].elapsed += time.perf_counter() - t0
            res2["rt-pair-select"].elapsed = res2["rt-pair-update"].elapsed = res2["rt-pair"].elapsed

            # ---- verification ----------------------------------------------------------
            verify = {}
            tot_i = rows * (rows + 1) // 2
            # point models: sums of v and w
            for g in xp:
                ev = sum(m[0] for m in model[g].values()); ew = sum(m[1] for m in model[g].values())
                got = scalar(sB.conn, f"SELECT SUM(v), SUM(w) FROM xp_{g}")
                verify[f"xp-{g}"] = bool(len(got) == 2 and int(got[0]) == ev and int(got[1]) == ew)
            M = args.blocks * per_block_r
            exp_range = {"lit": None,
                         "expr": (tot_i + M * rows, tot_i + 7 * rows),
                         "swap": ((tot_i, tot_i + 7 * rows) if M % 2 == 0 else (tot_i + 7 * rows, tot_i)),
                         "case": (tot_i, tot_i + 7 * rows),
                         "subq": (rows * rows, tot_i + 7 * rows)}
            for g in xr:
                got = scalar(sB.conn, f"SELECT SUM(v), SUM(w) FROM xr_{g}")
                if g == "lit":
                    verify["xr-lit"] = bool(len(got) == 2 and int(got[0]) == qBr["lit"] * rows)
                else:
                    verify[f"xr-{g}"] = bool(len(got) == 2 and (int(got[0]), int(got[1])) == exp_range[g])
            # case range: row 1 (odd) and row 2 (even) after M statements
            v1 = scalar(sB.conn, "SELECT v FROM xr_case WHERE id = 1")
            v2 = scalar(sB.conn, "SELECT v FROM xr_case WHERE id = 2")
            verify["xr-case-rows"] = bool(M % 2 == 0 and v1[0] == "1" and v2[0] == "2" or M % 2 == 1 and v1[0] == "0" and v2[0] == "3")
            got = scalar(sB.conn, "SELECT SUM(v) FROM rt")
            verify["rt-sum"] = bool(int(got[0]) == tot_i + rt_ops)
            for lab, s in S.items():
                # the two literal-range tables on each server: all rows equal one literal
                for t in ("p_r1", "p_r2"):
                    g2 = scalar(s.conn, f"SELECT MIN(v), MAX(v) FROM {t}")
                    verify[f"{lab}-{t}-uniform"] = bool(len(g2) == 2 and g2[0] == g2[1])
                cnt = scalar(s.conn, "SELECT COUNT(*) FROM p_i")
                verify[f"{lab}-insert-count"] = bool(int(cnt[0]) == rows + args.ops)
                cnt = scalar(s.conn, "SELECT COUNT(*) FROM p_d")
                verify[f"{lab}-delete-count"] = bool(int(cnt[0]) == rows)

        if args.p1_only:
            for lab, s in S.items():
                for t in ("p_r1", "p_r2"):
                    g2 = scalar(s.conn, f"SELECT MIN(v), MAX(v) FROM {t}")
                    verify[f"{lab}-{t}-uniform"] = bool(len(g2) == 2 and g2[0] == g2[1])
        meta_end = {s.label: bb.meta_all(s.conn) for s in servers}
        rss = {s.label: s.rss_kb() for s in servers}
        after = bb.host_state()
        cpu1 = cpu_times()
        summ = {}; raw = {}
        for l in S:
            summ[l] = {}; raw[l] = {}
            for n, p in res[l].items():
                d = p.summary()
                sl = sorted(p.latencies)
                d["p90_us"] = round(pct(sl, 90) * 1e6, 1)
                d["p999_us"] = round(pct(sl, 99.9) * 1e6, 1)
                d["bad"] = bad.get((l, n), 0)
                summ[l][n] = d
                raw[l][n] = [round(x * 1e6, 1) for x in p.latencies]
        summB = {}; rawB = {}
        for n, p in res2.items():
            d = p.summary()
            sl = sorted(p.latencies)
            d["p90_us"] = round(pct(sl, 90) * 1e6, 1)
            d["p999_us"] = round(pct(sl, 99.9) * 1e6, 1)
            d["bad"] = bad.get(("B", n), 0)
            summB[n] = d
            rawB[n] = [round(x * 1e6, 1) for x in p.latencies]
        payload = {"cores": args.cores, "rows": rows, "run": run, "tag": tag, "order_first": labels[0],
                   "frames": args.frames, "ops": args.ops, "range_ops": args.range_ops, "blocks": args.blocks,
                   "gate_wait_s": round(gate_wait, 1), "gate_other_busy_before": gate_other,
                   "t_start": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(tstart)),
                   "duration_s": round(time.time() - tstart, 1),
                   "host_before": before, "host_after": after,
                   "cpu_busy": busy_frac(cpu0, cpu1),
                   "mount_ckpt_us": {s.label: s.mount.get("recovery_checkpoint_us") for s in servers},
                   "rss_hwm_kb": rss, "verify": verify, "bad": {f"{k[0]}:{k[1]}": v for k, v in bad.items()},
                   "pool_end": {l: {k: v for k, v in m.items() if k.startswith("pool_")} for l, m in meta_end.items()},
                   "meta_end_all": meta_end,
                   "arms": summ, "arms_b_only": summB}
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, f"{tag}.json"), "w") as f:
            json.dump(payload, f, indent=1)
        with gzip.open(os.path.join(args.out, f"{tag}.raw.json.gz"), "wt") as f:
            json.dump({"ab": raw, "b": rawB}, f)
        errs = sum(p["errors"] for l in summ.values() for p in l.values()) + sum(p["errors"] for p in summB.values())
        nbad = sum(payload["bad"].values())
        okv = all(verify.values())
        line = " ".join(f"{n}:A={summ['A'][n]['p50_us']}/B={summ['B'][n]['p50_us']}" for n in ("lit-point", "lit-range", "ping"))
        print(f"{tag} load {before['loadavg'].split()[0]}->{after['loadavg'].split()[0]} gate {gate_wait:.0f}s/{gate_other} "
              f"dur {payload['duration_s']}s errs={errs} bad={nbad} verify={'ok' if okv else 'FAIL ' + str([k for k, v in verify.items() if not v])} {line}", flush=True)
    finally:
        for s in servers:
            s.stop()
        shutil.rmtree(wd, ignore_errors=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--rows", default="200,1000,10000")
    p.add_argument("--runs", type=int, default=16)
    p.add_argument("--ops", type=int, default=3000)
    p.add_argument("--range-ops", type=int, default=120)
    p.add_argument("--blocks", type=int, default=12)
    p.add_argument("--frames", type=int, default=65536)
    p.add_argument("--bin-a", required=True)
    p.add_argument("--bin-b", required=True)
    p.add_argument("--port-a", type=int, required=True)
    p.add_argument("--port-b", type=int, required=True)
    p.add_argument("--pin-server", required=True)
    p.add_argument("--pin-driver", default="4")
    p.add_argument("--cores", type=int, default=1)
    p.add_argument("--workdir", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--max-other-busy", type=float, default=0.10)
    p.add_argument("--max-wait", type=float, default=900)
    p.add_argument("--p1-only", action="store_true")
    p.add_argument("--tagpre", default="")
    args = p.parse_args()
    for rows in [int(x) for x in args.rows.split(",")]:
        for run in range(args.runs):
            run_one(args, rows, run)


if __name__ == "__main__":
    sys.exit(main())
