#!/usr/bin/env python3
"""CE-S0 row-cost sweep, KDS only: returned rows 1, 100, 1,000, 10,000, 60,480 (key range) and one filtered
walk (WHERE symbol_id = 3, 7,560 rows of 60,480 scanned).

  ce_s0_rows.py load   --kds-port P
  ce_s0_rows.py run    --kds-port P --kds-pid PID --out F.jsonl --progress F [--reps 3] [--target-s 3] [--modes send,split,drain]
  ce_s0_rows.py verify --kds-port P

Built on the row-cost attribution's rowcost.py (unmodified, archived beside this file) and kdsrow.py's
`send` mode (the scenario drivers' own client path: send_command + format_reply). Differences from
kdsrow.py: statements are explicit per shape (so a filtered walk can be a shape), the statement count of a
cell is calibrated from the warm-up so each cell lasts about --target-s whatever the shape, and each cell
records its wall-clock window (t0/t1, epoch seconds) for the host-validity check.
"""
import argparse, json, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rowcost as rc
import kdsrow            # noqa: F401  (installs the 'send' mode on rc.Kds)
import kwp
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import cellvalid

SHAPES = [
    ("range-1", 1, "SELECT * FROM daily_stats WHERE id BETWEEN 1 AND 1"),
    ("range-100", 100, "SELECT * FROM daily_stats WHERE id BETWEEN 1 AND 100"),
    ("range-1000", 1000, "SELECT * FROM daily_stats WHERE id BETWEEN 1 AND 1000"),
    ("range-10000", 10000, "SELECT * FROM daily_stats WHERE id BETWEEN 1 AND 10000"),
    ("range-60480", 60480, "SELECT * FROM daily_stats WHERE id BETWEEN 1 AND 60480"),
    ("filter-sym3", 7560, "SELECT * FROM daily_stats WHERE symbol_id = 3"),
]
if os.environ.get("SHAPES_ONLY"):
    SHAPES = [x for x in SHAPES if x[0] in os.environ["SHAPES_ONLY"].split(",")]


def cell(eng, mode, label, rows, sql, target_s):
    pc, pw = time.process_time, time.perf_counter
    w = pw(); eng.run(mode, sql); warm = pw() - w          # warm-up, also the calibration
    eng.run(mode, sql)                                      # a second, so the first-use costs are out
    n = max(5, int(target_s / max(warm, 1e-6)))
    lat = []
    t0 = time.time()
    s0 = rc.cpu_ticks(eng.pid); c0 = pc(); w0 = pw(); last = w0
    for _ in range(n):
        eng.run(mode, sql)
        now = pw(); lat.append(now - last); last = now
    w1 = pw(); c1 = pc(); s1 = rc.cpu_ticks(eng.pid)
    t1 = time.time()
    return {"shape": label, "rows": rows, "mode": mode, "n": n, "t0": t0, "t1": t1,
            "wall_us_per_stmt": (w1 - w0) / n * 1e6,
            "server_cpu_us_per_stmt": (s1 - s0) / rc.TICK / n * 1e6,
            "client_cpu_us_per_stmt": (c1 - c0) / n * 1e6,
            "server_ticks": s1 - s0, "lat_us": [round(x * 1e6, 1) for x in lat]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd")
    ap.add_argument("--kds-port", type=int)
    ap.add_argument("--kds-pid", type=int)
    ap.add_argument("--out")
    ap.add_argument("--progress")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--target-s", type=float, default=3.0)
    ap.add_argument("--modes", default="send,split,drain")
    a = ap.parse_args()
    if a.cmd == "load":
        k = kwp.Connection("127.0.0.1", a.kds_port, timeout=120)
        k.execute(f"CREATE TABLE daily_stats ({rc.SCHEMA_COLS}) BTREE")
        for s in rc.stmts(60480):
            k.execute(s)
        print("kds rows", k.execute("SELECT COUNT(*) FROM daily_stats")[1])
        return
    if a.cmd == "verify":
        k = kwp.Connection("127.0.0.1", a.kds_port, timeout=120)
        for label, rows, sql in SHAPES:
            got = len(k.execute(sql)[1])
            print(label, "expected", rows, "got", got, "OK" if got == rows else "MISMATCH")
            if got != rows:
                sys.exit(1)
        return
    eng = rc.Kds(a.kds_port, a.kds_pid)
    done = set()
    if os.path.exists(a.out):
        for line in open(a.out):
            d = json.loads(line); done.add((d["rep"], d["shape"], d["mode"]))
    with open(a.out, "a") as out:
        for rep in range(1, a.reps + 1):
            for label, rows, sql in SHAPES:
                for mode in a.modes.split(","):
                    if (rep, label, mode) in done:
                        continue
                    d = cell(eng, mode, label, rows, sql, a.target_s)
                    d["rep"] = rep
                    # per-cell validity: the 1 s monitor must have seen no build/test and no foreign process >= 50 % CPU
                    # in the cell's window; if it did, the cell is set aside and the pass ends (exit 3) so the master
                    # waits for a quiet host again instead of spending the pass on contaminated cells
                    time.sleep(2)
                    n, b, h, mi, who = cellvalid.verdict(d["t0"], d["t1"])
                    if b or h or n == 0:
                        with open(a.out + ".contaminated", "a") as cf:
                            cf.write(json.dumps(d) + "\n")
                        print(f"CONTAMINATED {label} {mode} rep{rep}: samples={n} build/test={b} heavy={h} {who}", flush=True)
                        sys.exit(3)
                    out.write(json.dumps(d) + "\n"); out.flush()
                    with open(a.progress, "w") as pf:
                        pf.write(f"{time.time()} rep={rep} shape={label} mode={mode}\n")
    print("FINISHED", flush=True)


if __name__ == "__main__":
    main()
