import json, statistics, sys
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/analysis")
RC = "/home/cdkbs/bench-runs/v3.3.0-38-ga40882af/rowcost"
SIZES = (1, 10, 100, 1000, 10000, 60480)


def load(f):
    by = {}
    for l in open(f"{RC}/{f}"):
        c = json.loads(l)
        by.setdefault((c["mode"], c["size"]), []).append(c)
    return by


NEW = load("cells-new.jsonl")
OLD = load("cells-old.jsonl")
NEW.update(load("cells-send-new.jsonl"))
OLD.update(load("cells-send-old.jsonl"))


def m(by, mode, size, key):
    return statistics.median(c[key] for c in by[(mode, size)])


def fit(by, mode, key, lo=1, hi=10000):
    xs = [s for s in SIZES if lo <= s <= hi]
    ys = [m(by, mode, s, key) for s in xs]
    mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
    sl = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
    return sl, my - sl * mx


def spr(by, mode, size, key="wall_us_per_stmt"):
    v = [c[key] for c in by[(mode, size)]]
    return 100 * (max(v) - min(v)) / statistics.median(v)


def pct(xs, p):
    xs = sorted(xs)
    k = (len(xs) - 1) * p / 100
    f = int(k)
    c = min(f + 1, len(xs) - 1)
    return xs[f] + (xs[c] - xs[f]) * (k - f)


W, S, C = "wall_us_per_stmt", "server_cpu_us_per_stmt", "client_cpu_us_per_stmt"
# PostgreSQL 18.6: the row-cost attribution file's figures (bench/v3.3.0/results-row-cost-attribution-v3.3.0-37-gf829964c.md
# section 3, branch row-cost-attribution at 64428271), measured 2026-10-20 06:19 to 06:26 UTC; NOT re-measured here.
PG = {"wall_values": 3.57, "server": 0.57, "client_total": 3.55, "decode": 2.59, "convert": 0.69, "idle": 0.01}


def comps():
    r = {}
    for lab, by in (("old", OLD), ("new", NEW)):
        r[lab] = dict(
            wall_values=fit(by, "values", W)[0], server=fit(by, "drain", S)[0], client_total=fit(by, "values", C)[0],
            decode=fit(by, "split", C)[0] - fit(by, "drain", C)[0], convert=fit(by, "values", C)[0] - fit(by, "split", C)[0],
            idle=fit(by, "values", W)[0] - fit(by, "values", C)[0], recv=fit(by, "drain", C)[0],
            wall_send=fit(by, "send", W)[0], client_send=fit(by, "send", C)[0], server_send=fit(by, "send", S)[0],
            wall_drain=fit(by, "drain", W)[0], wall_split=fit(by, "split", W)[0])
        r[lab]["render"] = r[lab]["client_send"] - fit(by, "split", C)[0]
        r[lab]["idle_send"] = r[lab]["wall_send"] - r[lab]["client_send"]
    return r


def table():
    r = comps()
    o, n = r["old"], r["new"]
    rows = [
        ("Wall slope, `values` (cells as Python ints)", o["wall_values"], n["wall_values"], PG["wall_values"]),
        ("Server CPU (`drain`, fixed 3 s windows)", o["server"], n["server"], PG["server"]),
        ("Client CPU, total (`values`)", o["client_total"], n["client_total"], PG["client_total"]),
        ("- receive (`drain` client CPU)", o["recv"], n["recv"], 0.27),
        ("- split cells into `bytes` (`split` minus `drain`)", o["decode"], n["decode"], PG["decode"]),
        ("- convert to int (`values` minus `split`)", o["convert"], n["convert"], PG["convert"]),
        ("Client idle = wall - client CPU (`values`)", o["idle"], n["idle"], PG["idle"]),
        ("Wall slope, `drain` (decode removed)", o["wall_drain"], n["wall_drain"], 0.57),
        ("**Wall slope, `send` (the scenario drivers' path: decode, render every cell to text, join)**", o["wall_send"], n["wall_send"], None),
        ("- client CPU, `send`", o["client_send"], n["client_send"], None),
        ("- of which render and join (`send` client CPU minus `split` client CPU)", o["render"], n["render"], None),
        ("- client idle, `send` (wall - client CPU)", o["idle_send"], n["idle_send"], None),
    ]
    s = "| component (µs per returned row, 12-column `daily_stats`, slope over 1 to 10,000 rows) | KDS, old client (this window) | KDS, new client | PostgreSQL 18.6 (attribution file, not re-measured) | new / old | new / PG |\n|---|---|---|---|---|---|\n"
    for lab, a, b, c in rows:
        s += (f"| {lab} | {a:.2f} µs | {b:.2f} µs | {'%.2f µs' % c if c is not None else '-'} | {b / a:.2f}x | {'%.2fx' % (b / c) if c and c >= 0.1 else '-'} |\n")
    return s


def sizes_table():
    s = ("| rows returned | mode | KDS old client: wall / stmt (spread) | KDS new client: wall / stmt (spread) | new / old | KDS new client CPU / stmt | server CPU / stmt (new) |\n|---|---|---|---|---|---|---|\n")
    for size in SIZES:
        for mode in ("values", "send"):
            if (mode, size) not in NEW or (mode, size) not in OLD:
                continue
            a, b = m(OLD, mode, size, W), m(NEW, mode, size, W)
            s += (f"| {size:,} | {mode} | {a:,.1f} µs ({spr(OLD, mode, size):.0f} %) | {b:,.1f} µs ({spr(NEW, mode, size):.0f} %) | {b / a:.2f}x | "
                  f"{m(NEW, mode, size, C):,.1f} µs | {m(NEW, mode, size, S):,.1f} µs |\n")
    return s


def dist_table():
    s = "| mode | rows | client | n statements | p0 | p25 | p50 | p95 | p99 |\n|---|---|---|---|---|---|---|---|---|\n"
    for mode in ("values", "send"):
        for size in (1, 100, 1000, 10000):
            for lab, by in (("old", OLD), ("new", NEW)):
                lat = [x for c in by[(mode, size)] for x in c["lat_us"]]
                s += (f"| {mode} | {size:,} | {lab} | {len(lat):,} | {pct(lat, 0):,.0f} µs | {pct(lat, 25):,.0f} µs | {pct(lat, 50):,.0f} µs | {pct(lat, 95):,.0f} µs | {pct(lat, 99):,.0f} µs |\n")
    return s


def section():
    r = comps()
    o, n = r["old"], r["new"]
    gap_sym = 4.84 - 4.08
    s = (f"**Where the rest of the gap sits (a KDS-only re-run of the attribution's cells, `rowcost/` in the run archive; PostgreSQL is the attribution file's, not re-measured).** On the 12-column read the new client's wall slope is "
         f"{n['wall_values']:.2f} µs per row against {o['wall_values']:.2f} µs for the old client in the same window (the attribution measured 6.19), and against PostgreSQL's 3.57 µs: a like-for-like decode to Python ints is no longer where KDS is behind. "
         f"The scenario drivers do not stop at ints: `ServerConnection.send_command` renders every cell to text and joins it, and that path costs {n['wall_send']:.2f} µs per row on the new client ({o['wall_send']:.2f} on the old), which is the scenario's own figure "
         f"(`symbol-history` 4.84 µs per row at 7,560 rows). It breaks into the client's decode ({n['decode']:.2f} µs of CPU), the render and join ({n['render']:.2f} µs of CPU) and {n['idle_send']:.2f} µs of client idle, the "
         f"server's {n['server']:.2f} µs of CPU per row that is not overlapped because the reply leaves at statement completion. PostgreSQL's matching scenario figure is 4.08 µs per row. Of the {gap_sym:.2f} µs per row that remains between the two on `symbol-history` "
         f"(4.84 against 4.08), **{n['idle_send']:.2f} µs, {100 * n['idle_send'] / gap_sym:.0f} %, is the engine-side non-overlap and the remaining {gap_sym - n['idle_send']:.2f} µs, {100 * (gap_sym - n['idle_send']) / gap_sym:.0f} %, is client work "
         f"(the render step the newline-shaped reply needs and PostgreSQL's text rows do not)**; the split is by subtraction from this window's idle and PostgreSQL's attribution figures and was not measured on PostgreSQL's scenario path. "
         f"For `bar-range` (nine columns, 3.57 against 3.23 µs per row, a gap of 0.34 µs) the engine-side term was not measured at nine columns. Before the fix the gap on that symbol history was {9.54 - 4.08:.2f} µs per row; the fix removed {o['wall_send'] - n['wall_send']:.2f} µs of client work per row on the scenario path ("
         f"{100 * (o['wall_send'] - n['wall_send']) / (9.54 - 4.08):.0f} % of that gap), and the {o['idle_send']:.2f} µs of non-overlap was and is part of what remains.\n")
    return s


if __name__ == "__main__":
    for f in sys.argv[1:]:
        print(globals()[f]())
