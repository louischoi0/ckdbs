#!/usr/bin/env python3
"""Analysis of bj_close_ab.py output. usage: analyze.py <out-dir-with-c*-n*-r*.json> [--max-other-busy X]

Prints markdown. Per (cores, rows):
  * runs used / discarded (other-CPU busy over the run above the threshold);
  * Part 1: A and B percentile tables (pooled), and the delta table
    (median per-run p50 delta B - A with bootstrap 95 % CI, mean delta, throughput delta);
  * the in-run noise floor: control (ping) delta, replicate (same binary, second relation)
    per-run difference;
  * Part 2 (B only), Part 3 (B only).
"""
import glob, gzip, json, math, os, random, statistics, sys

D = sys.argv[1]
THR = 0.10
if "--max-other-busy" in sys.argv:
    THR = float(sys.argv[sys.argv.index("--max-other-busy") + 1])
SHOW_DISCARDED = "--list" in sys.argv

random.seed(7)
ARMS1 = ["lit-point", "lit-point-again", "lit2-point", "lit-range", "lit-range-again", "insert", "select-pk", "delete-pk", "ping"]
NAMES1 = {"lit-point": "UPDATE SET v = lit, by pk", "lit-point-again": "UPDATE SET v = lit, by pk (replicate)",
          "lit2-point": "UPDATE SET v = lit, w = lit, by pk", "lit-range": "UPDATE SET v = lit, every row",
          "lit-range-again": "UPDATE SET v = lit, every row (replicate)", "insert": "INSERT, pk named",
          "select-pk": "SELECT by pk (control)", "delete-pk": "DELETE by pk", "ping": "SHOW META (control)"}
XP = ["lit", "expr", "swap", "case"]
XR = ["lit", "expr", "swap", "case", "subq"]
XPN = {"lit": "SET v = lit", "expr": "SET v = v + 1", "swap": "SET v = w, w = v",
       "case": "SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END", "subq": "SET v = (SELECT COUNT(*) FROM t)"}


def pct(s, p):
    k = max(0, min(len(s) - 1, int(-(-p * len(s) // 100)) - 1))
    return s[k]


def boot(vals, fn, n=4000):
    b = sorted(fn(random.choices(vals, k=len(vals))) for _ in range(n))
    return b[int(0.025 * n)], b[int(0.975 * n)]


def rsd(v):
    """robust sd: 1.4826 x median absolute deviation"""
    m = statistics.median(v)
    return 1.4826 * statistics.median([abs(x - m) for x in v])


def fu(x):
    """a latency in us as text with sensible precision and its unit"""
    if x >= 10000:
        return f"{x / 1000:.2f} ms"
    if x >= 1000:
        return f"{x:,.0f} µs"
    return f"{x:.1f} µs"


def fd(x):
    s = "+" if x >= 0 else "-"
    x = abs(x)
    if x >= 10000:
        return f"{s}{x / 1000:.2f} ms"
    if x >= 1000:
        return f"{s}{x:,.0f} µs"
    return f"{s}{x:.2f} µs"


def pcts(allv):
    return [allv[0], pct(allv, 25), pct(allv, 50), pct(allv, 95), pct(allv, 99)]


cells = {}
for f in sorted(glob.glob(f"{D}/c*-n*-r*.json")):
    j = json.load(open(f))
    cells.setdefault((j["cores"], j["rows"]), []).append((f, j))


def other_busy(j):
    cores = j["cores"]
    self_cpus = {2, 4} | ({3} if cores == 2 else set())
    v = [x for c, x in j["cpu_busy"].items() if int(c) not in self_cpus]
    return sum(v) / len(v)


def get_raw(f):
    return json.load(gzip.open(f.replace(".json", ".raw.json.gz"), "rt"))


out = {}
S2 = {}
S3 = {}
for (cores, rows) in sorted(cells):
    items = cells[(cores, rows)]
    good = [(f, j) for f, j in items if other_busy(j) <= THR and all(j["verify"].values()) and not j["bad"]]
    bad_runs = [(f, j) for f, j in items if (f, j) not in good]
    summ = [j for f, j in good]
    raws = [get_raw(f) for f, j in good]
    n = len(summ)
    loads = [float(j["host_before"]["loadavg"].split()[0]) for j in summ]
    loads_a = [float(j["host_after"]["loadavg"].split()[0]) for j in summ]
    obs = [other_busy(j) for j in summ]
    comp = set()
    for j in summ:
        for c in j["host_before"]["competing"] + j["host_after"]["competing"]:
            comp.add(c.split(" ", 1)[0] + " " + c.split(" ", 1)[1][:60] if " " in c else c)
    errs = sum(a["errors"] for j in summ for l in j["arms"].values() for a in l.values()) + \
        sum(a["errors"] for j in summ for a in j["arms_b_only"].values())
    print(f"\n#### cores = {cores}, {rows:,} rows\n")
    print(f"{n} runs used of {len(items)} ({len(bad_runs)} discarded: other-CPU busy > {THR:.0%} or verify/bad flag); "
          f"loadavg(1 min) before each run {min(loads):.2f}-{max(loads):.2f}, after {min(loads_a):.2f}-{max(loads_a):.2f}; "
          f"other-CPU busy over the run {min(obs):.1%}-{max(obs):.1%} (median {statistics.median(obs):.1%}); "
          f"{errs} errors; gate wait median {statistics.median(j['gate_wait_s'] for j in summ):.0f} s; "
          f"processes matching cc1plus|cmake --build|ctest: {len(comp)} distinct\n")
    if SHOW_DISCARDED:
        for f, j in bad_runs:
            print("  discarded", os.path.basename(f), f"other={other_busy(j):.2f}", j["bad"], [k for k, v in j["verify"].items() if not v])
    if n < 2:
        continue
    # ---------------- part 1
    print("**Part 1 - the literal path and the controls, A and B** "
          "(pooled over the runs; `ops` per arm = runs x statements)\n")
    print("| shape | arm | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    rec = {}
    for a in ARMS1:
        for L in "AB":
            allv = sorted(x for r in raws for x in r["ab"][L][a])
            p = pcts(allv)
            rm = [j["arms"][L][a]["mean_us"] for j in summ]
            qps = statistics.median(j["arms"][L][a]["qps"] for j in summ)
            rec[(a, L)] = dict(p=p, mean=sum(allv) / len(allv), qps=qps)
            print(f"| {NAMES1[a]} | {L} | {len(allv):,} | " + " | ".join(fu(x) for x in p) +
                  f" | {fu(sum(allv) / len(allv))} (sd {statistics.stdev(rm):.1f}) | {qps:,.0f} stmt/s |")
    print()
    print("| shape | Δ p50, B - A (95 % CI) | Δ p50 as % of A | Δ mean (95 % CI) | Δ throughput (95 % CI) | reading |")
    print("|---|---|---|---|---|---|")
    d_rel = {}
    deltas = {}
    for a in ARMS1:
        d50 = [j["arms"]["B"][a]["p50_us"] - j["arms"]["A"][a]["p50_us"] for j in summ]
        r50 = [(j["arms"]["B"][a]["p50_us"] / j["arms"]["A"][a]["p50_us"] - 1) * 100 for j in summ]
        dm = [j["arms"]["B"][a]["mean_us"] - j["arms"]["A"][a]["mean_us"] for j in summ]
        dq = [(j["arms"]["B"][a]["qps"] / j["arms"]["A"][a]["qps"] - 1) * 100 for j in summ]
        c50 = boot(d50, statistics.median); cm = boot(dm, statistics.mean); cq = boot(dq, statistics.median); cr = boot(r50, statistics.median)
        deltas[a] = dict(d50=statistics.median(d50), c50=c50, rel=statistics.median(r50), crel=cr, dm=statistics.mean(dm), cm=cm,
                         dq=statistics.median(dq), cq=cq)
    # in-run noise floor on the relative scale
    floor_ping = max(abs(deltas["ping"]["crel"][0]), abs(deltas["ping"]["crel"][1]), abs(deltas["ping"]["rel"]))
    rep_pt = [(j["arms"]["B"]["lit-point-again"]["p50_us"] / j["arms"]["B"]["lit-point"]["p50_us"] - 1) * 100 for j in summ] + \
             [(j["arms"]["A"]["lit-point-again"]["p50_us"] / j["arms"]["A"]["lit-point"]["p50_us"] - 1) * 100 for j in summ]
    rep_rg = [(j["arms"]["B"]["lit-range-again"]["p50_us"] / j["arms"]["B"]["lit-range"]["p50_us"] - 1) * 100 for j in summ] + \
             [(j["arms"]["A"]["lit-range-again"]["p50_us"] / j["arms"]["A"]["lit-range"]["p50_us"] - 1) * 100 for j in summ]
    floor_pt = max(floor_ping, abs(statistics.median(rep_pt)) + 2 * rsd(rep_pt))
    floor_rg = max(floor_ping, abs(statistics.median(rep_rg)) + 2 * rsd(rep_rg))
    for a in ARMS1:
        d = deltas[a]
        fl = floor_rg if "range" in a else floor_pt
        excl = d["crel"][0] > 0 or d["crel"][1] < 0
        if a == "ping":
            reading = "control"
        elif excl and abs(d["rel"]) > fl:
            reading = f"**B {'slower' if d['rel'] > 0 else 'faster'}** ({d['rel']:+.1f} % at p50, floor {fl:.1f} %)"
        elif excl:
            reading = f"CI excludes 0, inside the floor ({fl:.1f} %): not resolved"
        else:
            reading = "no change resolved"
        base = statistics.median(j["arms"]["A"][a]["p50_us"] for j in summ)
        print(f"| {NAMES1[a]} | {fd(d['d50'])} [{fd(d['c50'][0])}, {fd(d['c50'][1])}] | {d['rel']:+.2f} % [{d['crel'][0]:+.2f}, {d['crel'][1]:+.2f}] | "
              f"{fd(d['dm'])} [{fd(d['cm'][0])}, {fd(d['cm'][1])}] | {d['dq']:+.2f} % [{d['cq'][0]:+.2f}, {d['cq'][1]:+.2f}] | {reading} |")
    print(f"\nIn-run noise floor (relative, p50): control `SHOW META` Δ {deltas['ping']['rel']:+.2f} % "
          f"[{deltas['ping']['crel'][0]:+.2f}, {deltas['ping']['crel'][1]:+.2f}]; replicate (second relation, same binary, per run, both arms pooled) "
          f"point median {statistics.median(rep_pt):+.2f} %, robust sd {rsd(rep_pt):.2f} % (plain sd {statistics.stdev(rep_pt):.2f} %); range median {statistics.median(rep_rg):+.2f} %, "
          f"robust sd {rsd(rep_rg):.2f} % (plain sd {statistics.stdev(rep_rg):.2f} %). Floor used: point {floor_pt:.2f} %, range {floor_rg:.2f} %.\n")
    out[(cores, rows, "p1")] = deltas
    # ---------------- part 2
    print("**Part 2 - the expression's own price, B only**\n")
    for grp, names, nm in (("xp", XP, "point (`WHERE id = k`)"), ("xr", XR, f"range (`WHERE id >= 1`, every one of the {rows:,} rows)")):
        print(f"{nm}:\n")
        print("| SET value | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs literal (95 % CI) | per row at p50 | Δ per row vs literal |")
        print("|---|---|---|---|---|---|---|---|---|---|---|---|")
        for g in names:
            key = f"{grp}-{g}"
            allv = sorted(x for r in raws for x in r["b"][key])
            p = pcts(allv)
            rm = [j["arms_b_only"][key]["mean_us"] for j in summ]
            qps = statistics.median(j["arms_b_only"][key]["qps"] for j in summ)
            if g == "lit":
                dtxt = "-"; drow = "-"
            else:
                d = [j["arms_b_only"][key]["p50_us"] - j["arms_b_only"][f"{grp}-lit"]["p50_us"] for j in summ]
                c = boot(d, statistics.median)
                dtxt = f"{fd(statistics.median(d))} [{fd(c[0])}, {fd(c[1])}]"
                drow = f"{statistics.median(d) / rows * 1000:+.0f} ns [{c[0] / rows * 1000:+.0f}, {c[1] / rows * 1000:+.0f}]" if grp == "xr" else "-"
                S2[(cores, rows, grp, g)] = (statistics.median(d), c[0], c[1])
            S2[(cores, rows, grp, g, "abs")] = (p[2], sum(allv) / len(allv), qps)
            per = f"{p[2] / rows * 1000:.0f} ns" if grp == "xr" else "-"
            print(f"| `{XPN[g]}` | {len(allv):,} | " + " | ".join(fu(x) for x in p) +
                  f" | {fu(sum(allv) / len(allv))} (sd {statistics.stdev(rm):.1f}) | {qps:,.0f} stmt/s" +
                  (f" = {qps * rows:,.0f} rows/s" if grp == "xr" else "") + f" | {dtxt} | {per} | {drow} |")
        print()
    # ---------------- part 3
    print("**Part 3 - the client's former round trip, B only, one session**\n")
    print("| spelling | n | p0 | p25 | p50 | p95 | p99 | mean (sd of run means) | throughput (median of runs) | Δ p50 vs `SET v = v + 1` (95 % CI) |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for key, nm in (("rt-one", "`UPDATE SET v = v + 1`, one statement"), ("rt-pair", "`SELECT v` then `UPDATE SET v = <v+1>`, two round trips"),
                    ("rt-pair-select", "  of which the `SELECT v`"), ("rt-pair-update", "  of which the `UPDATE SET v = <lit>`"),
                    ("rt-txn", "`BEGIN; SELECT; UPDATE; COMMIT`, four round trips")):
        allv = sorted(x for r in raws for x in r["b"][key])
        p = pcts(allv)
        rm = [j["arms_b_only"][key]["mean_us"] for j in summ]
        qps = statistics.median(j["arms_b_only"][key]["qps"] for j in summ)
        if key in ("rt-pair", "rt-txn"):
            d = [j["arms_b_only"][key]["p50_us"] - j["arms_b_only"]["rt-one"]["p50_us"] for j in summ]
            c = boot(d, statistics.median)
            dtxt = f"{fd(statistics.median(d))} [{fd(c[0])}, {fd(c[1])}]"
            S3[(cores, rows, key)] = (statistics.median(d), c[0], c[1])
        else:
            dtxt = "-"
        S3[(cores, rows, key, "abs")] = (p[2], 1e6 / p[2])
        thr = f"{qps:,.0f} /s" if key in ("rt-one", "rt-pair", "rt-txn") else "-"
        print(f"| {nm} | {len(allv):,} | " + " | ".join(fu(x) for x in p) +
              f" | {fu(sum(allv) / len(allv))} (sd {statistics.stdev(rm):.1f}) | {thr} | {dtxt} |")
    print()


if "--summary" in sys.argv:
    cs = sorted(cells)
    hdr = "| | " + " | ".join(f"cores {c}, {r:,} rows" for c, r in cs) + " |\n|---|" + "---|" * len(cs)
    print("\n##### SUMMARY point: extra cost per statement over `SET v = lit` (B)\n")
    print(hdr)
    for g in XP[1:]:
        print(f"| `{XPN[g]}` | " + " | ".join(
            (lambda v: f"{v[0]:+.2f} µs [{v[1]:+.2f}, {v[2]:+.2f}]")(S2[(c, r, 'xp', g)]) for c, r in cs if (c, r, 'xp', g) in S2) + " |")
    print("\n##### SUMMARY range: extra cost per row over `SET v = lit` (B), ns per row\n")
    print(hdr)
    for g in XR[1:]:
        print(f"| `{XPN[g]}` | " + " | ".join(
            (lambda v, r: f"{v[0] / r * 1000:+.0f} ns [{v[1] / r * 1000:+.0f}, {v[2] / r * 1000:+.0f}]")(S2[(c, r, 'xr', g)], r) for c, r in cs) + " |")
    print("\n##### SUMMARY range: extra cost per statement over `SET v = lit` (B)\n")
    print(hdr)
    for g in XR[1:]:
        print(f"| `{XPN[g]}` | " + " | ".join(
            (lambda v: f"{fd(v[0])} [{fd(v[1])}, {fd(v[2])}]")(S2[(c, r, 'xr', g)]) for c, r in cs) + " |")
    print("\n##### SUMMARY range: p50 per row, ns (B)\n")
    print(hdr)
    for g in XR:
        print(f"| `{XPN[g]}` | " + " | ".join(f"{S2[(c, r, 'xr', g, 'abs')][0] / r * 1000:.0f} ns" for c, r in cs) + " |")
    print("\n##### SUMMARY point: p50 (B)\n")
    print(hdr)
    for g in XP:
        print(f"| `{XPN[g]}` | " + " | ".join(f"{S2[(c, r, 'xp', g, 'abs')][0]:.1f} µs" for c, r in cs) + " |")
    print("\n##### SUMMARY round trip: p50, derived statements/s = 1e6 / p50\n")
    print(hdr)
    for k in ("rt-one", "rt-pair", "rt-txn"):
        print(f"| {k} | " + " | ".join(f"{S3[(c, r, k, 'abs')][0]:.1f} µs = {S3[(c, r, k, 'abs')][1]:,.0f} /s" for c, r in cs) + " |")
    for k in ("rt-pair", "rt-txn"):
        print(f"| {k} minus rt-one | " + " | ".join(
            (lambda v: f"{v[0]:+.1f} µs [{v[1]:+.1f}, {v[2]:+.1f}]")(S3[(c, r, k)]) for c, r in cs) + " |")
