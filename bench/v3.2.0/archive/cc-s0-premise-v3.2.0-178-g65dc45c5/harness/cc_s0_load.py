#!/usr/bin/env python3
"""CC-S0 loader: one server, every relation of every cell, then a clean stop.
s1 relations use the scenario driver's own schema (tools/scenario1_backtest.py SCHEMA)
with synthetic values; s2 relations are produced by tools/scenario2_freight.py itself."""
import argparse, json, os, random, subprocess, sys, time

TOOLS = "/home/cdkbs/ckdbs/.claude/worktrees/cc-stats-planner/tools"
sys.path.insert(0, TOOLS)
import kwp

S1 = {
    "symbols": "id int64, ticker varchar, exchange_id int64, sector int32, listed_session int32",
    "daily_bars": "id int64, symbol_id int64, session_id int64, session_no int32, open int64, high int64, low int64, close int64, volume int64",
    "daily_stats": "id int64, bar_id int64, symbol_id int64, session_no int32, ret_bp int64, mom5_bp int64, mom20_bp int64, mom60_bp int64, mom120_bp int64, vol10_bp int64, vol20_bp int64, vol60_bp int64",
    "models": "id int64, name varchar, family int32, lookback int32, top_k int32, param_bp int64",
    "model_results": "id int64, model_id int64, period_no int32, session_no int32, pnl_bp int64, equity_bp int64, positions int32, trades int32",
}
RESULT_SIZES = {"r200": 200, "r1k": 1000, "r2872": 2872, "r10k": 10000}
SESS = {"x200": 25, "x1k": 125, "x10k": 1250, "x60k": 7560}   # 8 symbols each


def must(conn, sql):
    tag, n, _b, err = conn.query(sql)
    if err is not None:
        sys.exit(f"{sql[:120]} -> {err}")
    return n


def create(conn, base, suffix):
    must(conn, f"CREATE TABLE {base}_{suffix} ({S1[base]}) BTREE")


def insert(conn, table, cols, rows):
    for i in range(0, len(rows), 500):
        must(conn, f"INSERT INTO {table} ({cols}) VALUES " + ", ".join(rows[i:i + 500]))


def cols_of(base):
    return ", ".join(c.split()[0] for c in S1[base].split(", "))


def load_results(conn, tag, n):
    rng = random.Random(f"res-{tag}")
    create(conn, "models", tag)
    create(conn, "model_results", tag)
    insert(conn, f"models_{tag}", cols_of("models"),
           [f"({i}, 'model{i}', {i % 3}, {5 * i}, 3, {rng.randint(0, 900)})" for i in range(1, 9)])
    eq = {m: 10000 for m in range(1, 9)}
    rows = []
    for i in range(1, n + 1):
        m = (i - 1) % 8 + 1
        pnl = rng.randint(-500, 500)
        eq[m] += pnl
        rows.append(f"({i}, {m}, {(i - 1) // 8 + 1}, {((i - 1) // 8 + 1) * 21}, {pnl}, {eq[m]}, 3, {rng.randint(0, 6)})")
    insert(conn, f"model_results_{tag}", cols_of("model_results"), rows)


def load_stats(conn, tag, nsess):
    rng = random.Random(f"stats-{tag}")
    for b in ("symbols", "daily_bars", "daily_stats"):
        create(conn, b, tag)
    insert(conn, f"symbols_{tag}", cols_of("symbols"),
           [f"({s}, 'SYM{s}', {1 + s % 2}, {s % 5}, 0)" for s in range(1, 9)])
    bars, stats = [], []
    n = 0
    for sess in range(nsess):
        for s in range(1, 9):
            n += 1
            c = 10000 + rng.randint(-300, 300)
            bars.append(f"({n}, {s}, {sess + 1}, {sess}, {c}, {c + 5}, {c - 5}, {c}, {rng.randint(1000, 90000)})")
            stats.append(f"({n}, {n}, {s}, {sess}, " + ", ".join(str(rng.randint(-300, 300)) for _ in range(8)) + ")")
    insert(conn, f"daily_bars_{tag}", cols_of("daily_bars"), bars)
    insert(conn, f"daily_stats_{tag}", cols_of("daily_stats"), stats)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int)
    p.add_argument("--s2", default="", help="json list of {suffix, args}")
    p.add_argument("--out")
    a = p.parse_args()
    conn = kwp.Connection("127.0.0.1", a.port, timeout=600, client_name="cc_s0_load")
    t0 = time.time()
    for tag, n in RESULT_SIZES.items():
        load_results(conn, tag, n)
    print("s1 results done", time.time() - t0, flush=True)
    for tag, ns in SESS.items():
        load_stats(conn, tag, ns)
    print("s1 stats done", time.time() - t0, flush=True)
    conn.close()
    for cfg in json.loads(a.s2) if a.s2 else []:
        cmd = [sys.executable, os.path.join(TOOLS, "scenario2_freight.py"), "--port", str(a.port),
               "--suffix", cfg["suffix"], "--no-manifest"] + cfg["args"] + ["--json", os.path.join(a.out, f"s2load-{cfg['suffix']}.json")]
        t1 = time.time()
        r = subprocess.run(cmd, capture_output=True, text=True, cwd=TOOLS)
        open(os.path.join(a.out, f"s2load-{cfg['suffix']}.stdout"), "w").write(r.stdout + r.stderr)
        print("s2", cfg["suffix"], "rc", r.returncode, round(time.time() - t1, 1), "s", flush=True)


if __name__ == "__main__":
    main()
