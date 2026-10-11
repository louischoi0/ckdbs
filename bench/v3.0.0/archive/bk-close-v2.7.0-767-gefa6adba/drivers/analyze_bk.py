#!/usr/bin/env python3
"""BK close analysis: one (directory, cores) over the three row counts.

usage: analyze_bk.py <dir> <cores> [--runs LO:HI] [--rows 200,1000,10000] [--null-json FILE] [--json OUT]
                     [--boots N] [--new-is-old] [--strict-subset]

Discard rules (declared before the first run; applied here, not by the driver):
  1. a real cc1plus / cmake / make / ninja / ld process in the run's before or after list (AMENDED: a foreign `ctest` or
     `kds_tests` process no longer discards on its own - it is a sleeping parent or one test, and the CPU rules below see its load);
  2. the CPUs outside {server, driver} and outside {6, 7} averaged more than 10 % busy over the run (quiet runs read
     1.7-3.2 %). AMENDED during the run: it first read "outside {server, driver}" (including 6 and 7); another
     session's benchmark then took CPUs 6 and 7 (one physical core, sharing nothing with 2-5 but the L3 and memory)
     for the rest of the session. CPUs 6 and 7 are recorded per run (`busy67`) and reported, not discarded on;
     every run kept before the amendment (all of c1 pass 1) satisfies both versions;
  2b. a hyperthread sibling of a pinned CPU (3 and 5 at cores = 1; 0 and 5 at cores = 2) more than 15 % busy over the
     run. AMENDED during the run with rule 2: at cores = 2 the engine pins its second reactor to CPU 1 itself, so the
     server occupies CPUs 1, 2 and 3 there (it first read 2 and 3), and CPU 1 is no "other" CPU;
  3. any error reply, a row count that is not the expected one, or a refusal probe that is not the expected one;
  4. the SHOW META control's p50 in either arm above 1.04 x the median of the cell's ping p50s. AMENDED during the
     run from 1.10 to 1.04 once a run that passed rules 1-3 read A 52.6 us / B 49.8 us on INSERT beside quiet runs'
     49.1-49.5 (the quiet runs' ping p50s span 49.8-51.2 us, so 1.04 x 50.4 = 52.4 us discards none of them: 0 runs of
     c1 and c2 move under either value).
A run that meets none is kept. `--strict-subset` additionally drops runs with other-CPU busy above 10 % or a
foreign benchmark server (anything but the autotrade instance) in the host list.

Delta = B - A. A 95 % CI is a bootstrap over the per-run deltas (4000 resamples, seed 1). A delta is READ as a
finding only if its CI excludes zero AND its median exceeds the floor: max(0.6 us, 3 x the larger robust sd of the
two replicate arms' (INSERT again - INSERT) per-run p50 difference, and - when --null-json is given - the null pair's
|median| and CI half-width for that shape and row count).
"""
import argparse, glob, gzip, json, os, random, statistics, sys

SHAPES = [
    ("insert", "INSERT, pk omitted"),
    ("insert-again", "INSERT again (replicate)"),
    ("insert-named", "INSERT, pk named (keys below 2^40)"),
    ("select-pk", "SELECT by pk"),
    ("update-pk", "UPDATE by pk"),
    ("update-hot", "UPDATE of one hot row"),
    ("delete-pk", "DELETE by pk"),
    ("range100", "COUNT/MIN/MAX over 100 ids"),
    ("ping", "SHOW META (control)"),
]
BONLY = [("named-lo", "INSERT, pk named, keys below 2^40 (named-pair arm)"),
         ("named-hi", "INSERT, pk named, keys 2^40 + i (named-pair arm)")]
HI = 1 << 40


def pct(s, p):
    k = max(0, min(len(s) - 1, int(-(-p * len(s) // 100)) - 1))
    return s[k]


def f1(x):
    return f"{x:,.1f}"


def boot(vals, fn, n, rng):
    if len(vals) < 2:
        return (fn(vals), fn(vals))
    b = sorted(fn(rng.choices(vals, k=len(vals))) for _ in range(n))
    return b[int(0.025 * n)], b[int(0.975 * n)]


def load(D, cores, rows, lo, hi):
    out = []
    for f in glob.glob(f"{D}/c{cores}-n{rows}-r*.json"):
        base = os.path.basename(f)
        run = int(base.rsplit("-r", 1)[1].split(".")[0])
        if not (lo <= run < hi):
            continue
        j = json.load(open(f))
        j["_raw"] = f[:-len(".json")] + ".raw.json.gz"
        out.append(j)
    out.sort(key=lambda j: j["run"])
    return out


def swap_slots(j):
    """Flipped pair: slot A held the new engine. Return a copy with the slots exchanged, so that A is the old engine
    and B the new one and every delta below reads B - A = new - old."""
    k = dict(j)
    for key in ("arms", "verify", "mount_ckpt_us", "rss_hwm_kb", "pool_end"):
        if key in j:
            k[key] = {"A": j[key]["B"], "B": j[key]["A"]}
    k["_raw"] = j["_raw"] + "#swapped"
    return k


def pinned(j):
    """CPUs this run's own processes use. At cores = 2 the engine pins its second reactor thread to CPU 1 itself
    (probe: that thread's affinity list is `1` under `taskset -c 2,3`), so the server occupies 1, 2 and 3."""
    return ({1, 2, 3} if j["cores"] == 2 else {2}) | {4}


def other_busy_ex67(j):
    """Mean busy fraction of the CPUs that are neither the server's, the driver's, nor 6 and 7. Another session's
    benchmark occupied CPUs 6 and 7 (one physical core, no sibling of any CPU used here) for most of this run."""
    cs = [v for c, v in j["cpu_busy"].items() if int(c) not in pinned(j) | {6, 7}]
    return sum(cs) / len(cs)


def busy67(j):
    return (j["cpu_busy"]["6"] + j["cpu_busy"]["7"]) / 2


def foreign(j):
    out = []
    for k in ("host_before", "host_after"):
        for l in j[k].get("others", []):
            if "/home/cdkbs/autotrade/" not in l:
                out.append(l[:100])
    return out


def engines(mode):
    """Which slot holds the new engine (B of this order): main = (A old, B new), null = (old, old), flip = (new, old)."""
    if mode in (True, "null"):
        return {"A": False, "B": False}
    if mode == "flip":
        return {"A": True, "B": False}
    if mode == "nullnew":
        return {"A": True, "B": True}
    return {"A": False, "B": True}


def reasons(j, ping_ref, mode):
    r = []
    new = engines(mode)
    for k in ("host_before", "host_after"):
        real = [x for x in j[k].get("real", []) if not x.startswith("ctest:")]   # ctest is a sleeping parent: its tests show as CPU busy
        if real:
            r.append(f"{k}: {real}")
    ob = other_busy_ex67(j)
    if ob > 0.10:
        r.append(f"other-CPU busy (CPUs 6,7 excluded) {ob:.1%} > 10 %")
    # a hyperthread sibling of a pinned CPU (3 beside the server's 2 at cores = 1, 0 beside the second reactor's 1 at
    # cores = 2, 5 beside the driver's 4) shares its core
    for c in ((3, 5) if j["cores"] == 1 else (0, 5)):
        if j["cpu_busy"].get(str(c), 0) > 0.15:
            r.append(f"sibling CPU {c} busy {j['cpu_busy'][str(c)]:.0%} > 15 %")
    # the named-pair arm runs on slot B only; where B holds the old engine it refuses the upper-bits keys:
    # exactly ops error replies, in named-hi only
    errs = sum(a["errors"] for l in j["arms"].values() for n_, a in l.items() if not (not new["B"] and n_ == "named-hi"))
    if errs:
        r.append(f"{errs} error replies")
    if not new["B"] and j["arms"]["B"]["named-hi"]["errors"] != j["ops"]:
        r.append("old engine in slot B: named-hi did not draw its expected refusals")
    rows, n = j["rows"], j["ops"]
    for L in "AB":
        v = j["verify"][L]
        is_new = new[L]
        b_new = is_new  # the engine in this slot is the new one
        exp = {"a": rows + n, "b": rows + n, "n": rows + n, "r": rows, "u": rows, "o": rows, "d": rows,
               "m": rows + (n if L == "B" else 0), "h": rows + (n if (L == "B" and is_new) else 0)}
        for t, e in exp.items():
            if str(v.get(f"count_be_{t}")) != str(e):
                r.append(f"{L} count be_{t} {v.get(f'count_be_{t}')} != {e}")
        if str(v.get("hot_last_v")) != str(n):
            r.append(f"{L} hot row last v {v.get('hot_last_v')} != {n}")
        if b_new:
            if not v["probe_2_40"].startswith("INSERTED"):
                r.append("B probe 2^40 not placed")
            if not (v["probe_2_48"].startswith("ERR") and "281474976710655" in v["probe_2_48"]):
                r.append("B probe 2^48 not refused naming the ceiling")
            if not v["probe_2_48_minus_1"].startswith("INSERTED"):
                r.append("B probe 2^48-1 not placed")
            if "281474976710655,2,2" not in v["probe_2_48_minus_1_read"]:
                r.append("B probe 2^48-1 not read back")
            if L == "B" and (f"{HI + 1},1,1" not in v.get("hi_first", "") or f"{HI + n},{n},{n}" not in v.get("hi_last", "")):
                r.append("B upper-bits keys not read back")
        else:
            for k in ("probe_2_40", "probe_2_48", "probe_2_48_minus_1"):
                if not (v[k].startswith("ERR") and "1099511627775" in v[k]):
                    r.append(f"{L} probe {k} not refused naming 1099511627775")
        if j["arms"][L]["ping"]["p50_us"] > 1.04 * ping_ref:
            r.append(f"{L} ping p50 {j['arms'][L]['ping']['p50_us']} > 1.04 x {ping_ref:.1f}")
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("cores", type=int)
    ap.add_argument("--runs", default="0:1000")
    ap.add_argument("--rows", default="200,1000,10000")
    ap.add_argument("--null-json")
    ap.add_argument("--json")
    ap.add_argument("--boots", type=int, default=4000)
    ap.add_argument("--new-is-old", action="store_true", help="B is the old engine (null pair)")
    ap.add_argument("--null-new", action="store_true", help="the new engine in both slots (copy control)")
    ap.add_argument("--flipped", action="store_true", help="slot A holds the new engine, slot B the old (flipped pair): slots are swapped before the deltas")
    ap.add_argument("--strict-subset", action="store_true")
    ap.add_argument("--label", default="")
    ap.add_argument("--no-tables", action="store_true", help="skip the percentile tables")
    a = ap.parse_args()
    mode = "flip" if a.flipped else ("nullnew" if a.null_new else ("null" if a.new_is_old else "main"))
    lo, hi = (int(x) for x in a.runs.split(":"))
    null = json.load(open(a.null_json)) if a.null_json else {}
    rng = random.Random(1)
    summary = {}
    for rows in [int(x) for x in a.rows.split(",")]:
        allr = load(a.dir, a.cores, rows, lo, hi)
        if not allr:
            continue
        pings = [j["arms"][L]["ping"]["p50_us"] for j in allr for L in "AB"]
        ping_ref = statistics.median(pings)
        kept, dropped = [], []
        for j in allr:
            rs = reasons(j, ping_ref, mode)
            if a.strict_subset:
                if busy67(j) > 0.10:
                    rs.append(f"CPUs 6,7 busy {busy67(j):.0%} (another session's benchmark)")
                if foreign(j):
                    rs.append("foreign server on the host")
            (dropped if rs else kept).append((j, rs))
        recs = [swap_slots(j) for j, _ in kept] if mode == "flip" else [j for j, _ in kept]
        cell = f"cores = {a.cores}, {rows:,} rows"
        print(f"\n### {a.label}{cell}\n")
        loads = [float(j["host_before"]["loadavg"].split()[0]) for j in allr]
        loads_a = [float(j["host_after"]["loadavg"].split()[0]) for j in allr]
        ob = [other_busy_ex67(j) for j in recs] or [0]
        b67 = [busy67(j) for j in recs] or [0]
        fr = sum(1 for j in recs if busy67(j) > 0.10)
        print(f"{len(recs)} runs kept of {len(allr)} (runs {allr[0]['run']}-{allr[-1]['run']}); loadavg(1 min) before each run "
              f"{min(loads):.2f}-{max(loads):.2f}, after {min(loads_a):.2f}-{max(loads_a):.2f}; busy of the other CPUs (6, 7 excluded) in kept runs "
              f"{min(ob):.1%}-{max(ob):.1%} (median {statistics.median(ob):.1%}); CPUs 6,7 (another session's benchmark) busy over 10 % in {fr} of the kept runs "
              f"(range {min(b67):.0%}-{max(b67):.0%}); "
              f"gate wait median {statistics.median(j['gate_wait_s'] for j in allr):.0f} s; "
              f"ping p50 reference {ping_ref:.1f} us; errors in kept runs: "
              f"{sum(x['errors'] for j in recs for l in j['arms'].values() for n_, x in l.items() if not (mode != 'main' and n_ == 'named-hi'))}\n")
        if dropped:
            print("Discarded:")
            for j, rs in dropped:
                print(f"- run {j['run']}: {'; '.join(rs)[:300]}")
            print()
        if not recs:
            continue
        ck = [j["mount_ckpt_us"] for j in recs]
        print(f"`recovery_checkpoint_us` at mount (thermometer), median A {statistics.median(c['A'] for c in ck):,.0f} us, "
              f"B {statistics.median(c['B'] for c in ck):,.0f} us, max A {max(c['A'] for c in ck):,.0f} us, B {max(c['B'] for c in ck):,.0f} us\n")
        raws = [(lambda r: ({"A": r["B"], "B": r["A"]} if j["_raw"].endswith("#swapped") else r))(json.load(gzip.open(j["_raw"].replace("#swapped", ""), "rt"))) for j in recs]
        shapes = [s for s in SHAPES]
        bonly = [b for b in BONLY if not (mode != "main" and b[0] == "named-hi")]
        pooled = {}
        for L in "AB":
            for s, _ in shapes + bonly:
                vals = sorted(x for r in raws for x in r[L].get(s, []))
                if vals:
                    pooled[(L, s)] = vals
        cellsum = {}
        if not a.no_tables:
            print("| shape | arm | n | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs, ops/elapsed) | server CPU per statement (median of runs) |")
            print("|---|---|---|---|---|---|---|---|---|---|---|---|")
        for s, desc in shapes + bonly:
            for L in "AB":
                if (L, s) not in pooled:
                    continue
                v = pooled[(L, s)]
                rm = [j["arms"][L][s]["mean_us"] for j in recs]
                q = statistics.median(j["arms"][L][s]["qps"] for j in recs)
                c = statistics.median(j["arms"][L][s]["server_cpu_us_per_stmt"] for j in recs)
                if not a.no_tables:
                    print(f"| {desc} | {L} | {len(v):,} | {f1(v[0])} µs | {f1(pct(v,25))} µs | {f1(pct(v,50))} µs | {f1(pct(v,75))} µs | "
                          f"{f1(pct(v,95))} µs | {f1(pct(v,99))} µs | {sum(v)/len(v):.1f} µs (sd {statistics.stdev(rm) if len(rm)>1 else 0:.1f}) | "
                          f"{q:,.0f} /s | {c:.2f} µs |")
        print()
        # ---- deltas
        rep = {}  # replicate floor: the same shape on a second relation, same binary, per run
        for L in "AB":
            dd = [j["arms"][L]["insert-again"]["p50_us"] - j["arms"][L]["insert"]["p50_us"] for j in recs]
            med = statistics.median(dd)
            mad = statistics.median(abs(x - med) for x in dd) * 1.4826
            rep[L] = dict(med=med, rsd=mad, sd=statistics.stdev(dd) if len(dd) > 1 else 0.0)
        print("| shape | A p50 | B p50 | derived throughput at p50, A → B | Δ p50, B − A (95 % CI) | Δ mean (95 % CI) | Δ server CPU per statement (95 % CI) | Δ throughput ops/elapsed (95 % CI) | floor | reading |")
        print("|---|---|---|---|---|---|---|---|---|---|")
        for s, desc in shapes:
            d50 = [j["arms"]["B"][s]["p50_us"] - j["arms"]["A"][s]["p50_us"] for j in recs]
            dmn = [j["arms"]["B"][s]["mean_us"] - j["arms"]["A"][s]["mean_us"] for j in recs]
            dcp = [j["arms"]["B"][s]["server_cpu_us_per_stmt"] - j["arms"]["A"][s]["server_cpu_us_per_stmt"] for j in recs]
            dq = [(j["arms"]["B"][s]["qps"] / j["arms"]["A"][s]["qps"] - 1) * 100 for j in recs]
            dth = [(j["arms"]["A"][s]["p50_us"] / j["arms"]["B"][s]["p50_us"] - 1) * 100 for j in recs]
            c50 = boot(d50, statistics.median, a.boots, rng)
            cmn = boot(dmn, statistics.mean, a.boots, rng)
            ccp = boot(dcp, statistics.median, a.boots, rng)
            cq = boot(dq, statistics.median, a.boots, rng)
            cth = boot(dth, statistics.median, a.boots, rng)
            m50 = statistics.median(d50)
            pa = statistics.median(j["arms"]["A"][s]["p50_us"] for j in recs)
            pb = statistics.median(j["arms"]["B"][s]["p50_us"] for j in recs)
            floor = max(0.6, 3 * max(rep['A']['rsd'], rep['B']['rsd']))
            nk = f"{rows}|{s}"
            if nk in null:
                n_ = null[nk]
                floor = max(floor, abs(n_["dmed"]), (n_["ci"][1] - n_["ci"][0]) / 2)
            resolved = (c50[0] > 0 or c50[1] < 0) and round(abs(m50), 2) > round(floor, 2)
            if resolved:
                reading = f"**B {'slower' if m50 > 0 else 'faster'}** ({m50 / pa * 100:+.1f} % at p50)"
            elif c50[0] > 0 or c50[1] < 0:
                reading = "CI excludes 0, inside the floor: not resolved"
            else:
                reading = "no change resolved"
            thA, thB = 1e6 / pa, 1e6 / pb
            print(f"| {desc} | {pa:.1f} µs | {pb:.1f} µs | {thA:,.0f} → {thB:,.0f} /s ({statistics.median(dth):+.1f} % [{cth[0]:+.1f}, {cth[1]:+.1f}]) | "
                  f"{m50:+.2f} µs [{c50[0]:+.2f}, {c50[1]:+.2f}] | {statistics.mean(dmn):+.2f} µs [{cmn[0]:+.2f}, {cmn[1]:+.2f}] | "
                  f"{statistics.median(dcp):+.2f} µs [{ccp[0]:+.2f}, {ccp[1]:+.2f}] | {statistics.median(dq):+.1f} % [{cq[0]:+.1f}, {cq[1]:+.1f}] | {floor:.2f} µs | {reading} |")
            cellsum[s] = dict(pa=pa, pb=pb, dmed=m50, ci=list(c50), dmean=statistics.mean(dmn), cimean=list(cmn),
                              dcpu=statistics.median(dcp), cicpu=list(ccp), dth=statistics.median(dth), cith=list(cth),
                              sd_run=statistics.stdev(d50) if len(d50) > 1 else 0.0, floor=floor, resolved=resolved,
                              cpuA=statistics.median(j["arms"]["A"][s]["server_cpu_us_per_stmt"] for j in recs),
                              cpuB=statistics.median(j["arms"]["B"][s]["server_cpu_us_per_stmt"] for j in recs),
                              n_runs=len(recs), d50_runs=d50,
                              a50_runs=[j["arms"]["A"][s]["p50_us"] for j in recs], b50_runs=[j["arms"]["B"][s]["p50_us"] for j in recs])
        # ---- B only: hi vs lo
        if mode == "main" and all("named-hi" in j["arms"]["B"] for j in recs):
            dd = [j["arms"]["B"]["named-hi"]["p50_us"] - j["arms"]["B"]["named-lo"]["p50_us"] for j in recs]
            dc = [j["arms"]["B"]["named-hi"]["server_cpu_us_per_stmt"] - j["arms"]["B"]["named-lo"]["server_cpu_us_per_stmt"] for j in recs]
            ddm = [j["arms"]["B"]["named-hi"]["mean_us"] - j["arms"]["B"]["named-lo"]["mean_us"] for j in recs]
            c1_, c2_, c3_ = boot(dd, statistics.median, a.boots, rng), boot(dc, statistics.median, a.boots, rng), boot(ddm, statistics.mean, a.boots, rng)
            print(f"\nNamed-pair arm, B only, same blocks (hi - lo): Δ p50 {statistics.median(dd):+.2f} µs [{c1_[0]:+.2f}, {c1_[1]:+.2f}], "
                  f"Δ mean {statistics.mean(ddm):+.2f} µs [{c3_[0]:+.2f}, {c3_[1]:+.2f}], Δ server CPU per statement {statistics.median(dc):+.2f} µs "
                  f"[{c2_[0]:+.2f}, {c2_[1]:+.2f}]; lo p50 {statistics.median(j['arms']['B']['named-lo']['p50_us'] for j in recs):.1f} µs, "
                  f"hi p50 {statistics.median(j['arms']['B']['named-hi']['p50_us'] for j in recs):.1f} µs.")
            cellsum["named-hi-minus-lo"] = dict(dmed=statistics.median(dd), ci=list(c1_), dmean=statistics.mean(ddm), cimean=list(c3_),
                                                dcpu=statistics.median(dc), cicpu=list(c2_),
                                                lo=statistics.median(j['arms']['B']['named-lo']['p50_us'] for j in recs),
                                                hi=statistics.median(j['arms']['B']['named-hi']['p50_us'] for j in recs))
            # B's named-hi vs A's insert-named (cross-arm, same run)
            de = [j["arms"]["B"]["named-hi"]["p50_us"] - j["arms"]["A"]["insert-named"]["p50_us"] for j in recs]
            ce = boot(de, statistics.median, a.boots, rng)
            print(f"B hi-key insert against A's low-key named insert (cross-arm, same run): Δ p50 {statistics.median(de):+.2f} µs [{ce[0]:+.2f}, {ce[1]:+.2f}].")
            cellsum["named-hi-minus-A-named"] = dict(dmed=statistics.median(de), ci=list(ce))
        # ---- noise
        print("\nNoise floor from inside the runs: " +
              "; ".join(f"replicate arm {L} (INSERT again − INSERT p50 per run) median {rep[L]['med']:+.2f} µs, robust sd {rep[L]['rsd']:.2f} µs, sd {rep[L]['sd']:.2f} µs" for L in "AB") +
              f"; control SHOW META Δ p50 {cellsum['ping']['dmed']:+.2f} µs [{cellsum['ping']['ci'][0]:+.2f}, {cellsum['ping']['ci'][1]:+.2f}]; "
              f"run-to-run sd of the per-run Δ p50, INSERT {cellsum['insert']['sd_run']:.2f} µs, UPDATE {cellsum['update-pk']['sd_run']:.2f} µs, SELECT {cellsum['select-pk']['sd_run']:.2f} µs, ping {cellsum['ping']['sd_run']:.2f} µs.")
        cellsum["_replicate"] = rep
        cellsum["_n_runs"] = len(recs)
        for s in cellsum:
            if not s.startswith("_"):
                summary[f"{rows}|{s}"] = cellsum[s]
        summary[f"{rows}|_meta"] = dict(n_kept=len(recs), n_all=len(allr), ping_ref=ping_ref,
                                        dropped=[(j["run"], rs) for j, rs in dropped], replicate=rep)
    if a.json:
        json.dump(summary, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    sys.exit(main())
