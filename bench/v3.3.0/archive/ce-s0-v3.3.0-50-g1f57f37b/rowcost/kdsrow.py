#!/usr/bin/env python3
"""KDS-only re-run of the row-cost attribution's cells (rowcost.py, archived at
bench/v3.3.0/archive/row-cost-attribution-v3.3.0-37-gf829964c/ on branch row-cost-attribution), with the
PostgreSQL leg removed (PostgreSQL is not re-run; its figures are the attribution file's).

  kdsrow.py load --kds-port P
  kdsrow.py run  --kds-port P --kds-pid PID --out FILE.jsonl --progress FILE [--reps 3] [--target-s 3]
  kdsrow.py slopes FILE.jsonl
"""
import argparse, json, os, sys, time, statistics
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rowcost as rc
import kwp
import ckdbs_cli

_orig_run = rc.Kds.run


def _run(self, mode, sql):
    """mode 'send': the scenario drivers' own path - ServerConnection.send_command (decode, render every cell to text,
    join) then format_reply - on the connection this Kds object already holds."""
    if mode == "send":
        if not hasattr(self, "_sc"):
            sc = ckdbs_cli.ServerConnection.__new__(ckdbs_cli.ServerConnection)
            sc._conn = self.c
            self._sc = sc
        return ckdbs_cli.format_reply(self._sc.send_command(sql)).count("\n")
    return _orig_run(self, mode, sql)


rc.Kds.run = _run


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd")
    ap.add_argument("arg", nargs="?")
    ap.add_argument("--kds-port", type=int)
    ap.add_argument("--kds-pid", type=int)
    ap.add_argument("--rows", type=int, default=60480)
    ap.add_argument("--out")
    ap.add_argument("--progress")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--target-s", type=float, default=3.0)
    ap.add_argument("--modes", default="drain,split,values")
    a = ap.parse_args()
    if a.cmd == "load":
        k = kwp.Connection("127.0.0.1", a.kds_port, timeout=120)
        k.execute(f"CREATE TABLE daily_stats ({rc.SCHEMA_COLS}) BTREE")
        for s in rc.stmts(a.rows):
            k.execute(s)
        print("kds rows", k.execute("SELECT COUNT(*) FROM daily_stats")[1])
        return
    if a.cmd == "slopes":
        cells = [json.loads(l) for l in open(a.arg)]
        by = {}
        for c in cells:
            by.setdefault((c["mode"], c["size"]), []).append(c)

        def m(mode, size, key):
            return statistics.median(c[key] for c in by[(mode, size)])

        def fit(mode, key, lo=1, hi=10000):
            xs = [s for s in rc.SIZES if lo <= s <= hi]
            ys = [m(mode, s, key) for s in xs]
            mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
            sl = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
            return sl, my - sl * mx
        for mode in sorted({m_ for (m_, _) in by}, key=("drain", "split", "values", "send").index):
            for key in ("wall_us_per_stmt", "server_cpu_us_per_stmt", "client_cpu_us_per_stmt"):
                sl, ic = fit(mode, key)
                print(f"sizes 1..10000  kds {mode:6s} {key:24s} slope {sl:7.3f} us/row  intercept {ic:7.1f} us")
        print()
        for (mode, size), cs in sorted(by.items(), key=lambda t: (t[0][0], t[0][1])):
            w = [c["wall_us_per_stmt"] for c in cs]
            print(f"kds {mode:6s} {size:6d} n={cs[0]['n']:5d} | wall {statistics.median(w):10.1f} ({100 * (max(w) - min(w)) / statistics.median(w):.0f}%) | "
                  f"server {m(mode, size, 'server_cpu_us_per_stmt'):9.1f} | client {m(mode, size, 'client_cpu_us_per_stmt'):10.1f}")
        return
    rc.TARGET_S = a.target_s
    modes = a.modes.split(",")
    k = rc.Kds(a.kds_port, a.kds_pid)
    print("kds server pid", k.pid, flush=True)
    done = set()
    if os.path.exists(a.out):
        for line in open(a.out):
            d = json.loads(line)
            done.add((d["rep"], d["size"], d["mode"]))
    with open(a.out, "a") as out:
        for rep in range(1, a.reps + 1):
            for size in rc.SIZES:
                for mode in modes:
                    if (rep, size, mode) in done:
                        continue
                    d = rc.cell(k, mode, size, rc.nstmts(size))
                    d["rep"] = rep
                    out.write(json.dumps(d) + "\n")
                    out.flush()
                    with open(a.progress, "w") as pf:
                        pf.write(f"{time.time()} rep={rep} size={size} mode={mode}\n")
    print("FINISHED", flush=True)


if __name__ == "__main__":
    main()
