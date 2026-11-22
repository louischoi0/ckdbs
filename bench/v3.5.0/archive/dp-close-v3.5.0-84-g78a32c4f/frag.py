import json, glob, statistics as st
R = "/home/louis/bench-runs/dp-close/archive"
F = {}
qps = lambda us: f"{1e6/us:,.0f}/s"

# ---- Q1 first pass, 3 runs, cores 1 and 2
rows = []
for k in (1, 2):
    for shape in ("point", "insert1", "pkmix"):
        d = {}
        for arm in "AB":
            d[arm] = [json.load(open(glob.glob(f"{R}/q1/k{k}-{shape}-{arm}-r{r}/*.json")[0])) for r in (1, 2, 3)]
        for arm in "AB":
            t = [j["statements_per_s"] for j in d[arm]]
            lat = [round(st.median([j[x] for j in d[arm]]), 1) for x in ("p0_us", "p25_us", "p50_us", "p95_us", "p99_us")]
            rows.append(f"| {k} | `{shape}` | {arm} | {st.median(t):,.0f} /s | {min(t):,.0f}..{max(t):,.0f} /s | {100*(max(t)-min(t))/st.median(t):.1f} % | {lat[0]} us | {lat[1]} us | {lat[2]} us | {lat[3]} us | {lat[4]} us | {sum(j['statements'] for j in d[arm]):,} | {sum(j['errors'] for j in d[arm])} errors |")
        ta = [j["statements_per_s"] for j in d["A"]]
        tb = [j["statements_per_s"] for j in d["B"]]
        rows.append(f"| {k} | `{shape}` | **B - A** | {st.median(tb)-st.median(ta):+,.0f} /s ({100*(st.median(tb)/st.median(ta)-1):+.1f} %) | | | | | | | | | |")
F["q1a"] = "\n".join(rows)

# ---- Q1 cpu
rows = []
d = json.load(open(f"{R}/q1cpu/dk_close_m1.json"))
for n, shapes in d["m1"].items():
    for s, v in shapes.items():
        c = v["cpu_us"]
        A, A2, B = c["A"], c["A2"], c["B"]
        ba = [b - a for a, b in zip(A, B)]
        a2a = [b - a for a, b in zip(A, A2)]
        K = v["stmts_per_block"]
        rows.append(f"| {int(n):,} | `{s}` | {K:,} | {st.median(A):.2f} us | {st.median(A2):.2f} us | {st.median(B):.2f} us | {st.median(a2a):+.2f} us [{min(a2a):+.1f}, {max(a2a):+.1f}] | {st.median(ba):+.2f} us [{min(ba):+.1f}, {max(ba):+.1f}] | {100*st.median(ba)/st.median(A):+.1f} % | {qps(st.median(A))} / {qps(st.median(B))} |")
F["q1cpu"] = "\n".join(rows)
bas = [st.median([b - a for a, b in zip(v["cpu_us"]["A"], v["cpu_us"]["B"])]) for sh in d["m1"].values() for v in sh.values()]
a2s = [st.median([b - a for a, b in zip(v["cpu_us"]["A"], v["cpu_us"]["A2"])]) for sh in d["m1"].values() for v in sh.values()]
F["q1cpu_sum"] = f"B-A medians: mean {st.mean(bas):+.2f} us, positive in {sum(1 for x in bas if x>0)} of 15, max {max(bas):+.2f}, min {min(bas):+.2f}; A2-A medians: mean {st.mean(a2s):+.2f} us, positive in {sum(1 for x in a2s if x>0)} of 15, max {max(a2s):+.2f}, min {min(a2s):+.2f}"

# ---- Q1b
rows = []
for shape in ("point", "insert1", "pkmix"):
    d = {a: [json.load(open(glob.glob(f"{R}/q1b/k1-{shape}-{a}-r{r}/*.json")[0])) for r in range(1, 7)] for a in ("A", "A2", "B")}
    t = {a: [j["statements_per_s"] for j in d[a]] for a in d}
    for a in ("A", "A2", "B"):
        lat = [round(st.median([j[x] for j in d[a]]), 1) for x in ("p0_us", "p25_us", "p50_us", "p95_us", "p99_us")]
        rows.append(f"| `{shape}` | {a} | {st.median(t[a]):,.0f} /s | {min(t[a]):,.0f}..{max(t[a]):,.0f} /s | {100*(max(t[a])-min(t[a]))/st.median(t[a]):.1f} % | {lat[0]} us | {lat[1]} us | {lat[2]} us | {lat[3]} us | {lat[4]} us | {sum(j['statements'] for j in d[a]):,} | {sum(j['errors'] for j in d[a])} errors |")
    pb = [100 * (b / a - 1) for a, b in zip(t["A"], t["B"])]
    pa = [100 * (b / a - 1) for a, b in zip(t["A"], t["A2"])]
    neg = sum(1 for x in pb if x < 0)
    rows.append(f"| `{shape}` | paired B/A - 1 | {st.median(pb):+.2f} % [{min(pb):+.2f}, {max(pb):+.2f}] | | | | | | | | {neg} of 6 runs negative | |")
    rows.append(f"| `{shape}` | paired A2/A - 1 (floor) | {st.median(pa):+.2f} % [{min(pa):+.2f}, {max(pa):+.2f}] | | | | | | | | {sum(1 for x in pa if x<0)} of 6 runs negative | |")
F["q1b"] = "\n".join(rows)

# ---- Q2
d = json.load(open(f"{R}/q234/dp_close.json"))
rows = []
for n, v in d["q2"].items():
    for a in ("N1", "N2", "V"):
        x = v["arms"][a]
        dl = x.get("delta_vs_N1")
        l = x["lat"]
        rows.append(f"| {int(n):,} | {a} | {x['cpu_us_median']:.2f} us | {qps(x['cpu_us_median'])} | " + (f"{dl['median_us']:+.2f} us [{dl['min_us']:+.1f}, {dl['max_us']:+.1f}]" if dl else "-") +
                    f" | {l['ops']:,} | {l['p0']} us | {l['p25']} us | {l['p50']} us | {l['p95']} us | {l['p99']} us |")
F["q2"] = "\n".join(rows)

# ---- Q3
rows = []
for n, shapes in d["q3"].items():
    for s, w in shapes.items():
        for a in ("H", "H2", "VIEW"):
            x = w["arms"][a]
            dl = x.get("delta_vs_H")
            l = x["lat"]
            rows.append(f"| {int(n):,} | `{s}` ({w['rows_per_stmt']} row{'s' if w['rows_per_stmt']!=1 else ''}) | {a} | {x['cpu_us_median']:.2f} us | {qps(x['cpu_us_median'])} | " +
                        (f"{dl['median_us']:+.2f} us [{dl['min_us']:+.1f}, {dl['max_us']:+.1f}] ({100*dl['median_us']/w['arms']['H']['cpu_us_median']:+.1f} %)" if dl else "-") +
                        f" | {l['ops']:,} | {l['p0']} us | {l['p25']} us | {l['p50']} us | {l['p95']} us | {l['p99']} us |")
F["q3"] = "\n".join(rows)

# ---- Q4
rows = []
v = d["q4"]["depths"]
for a, x in v["arms"].items():
    dl = x.get("delta_vs_H")
    l = x["lat"]
    rows.append(f"| {a} | {x['cpu_us_median']:.2f} us | {qps(x['cpu_us_median'])} | " + (f"{dl['median_us']:+.2f} us [{dl['min_us']:+.1f}, {dl['max_us']:+.1f}]" if dl else "-") +
                f" | {l['ops']:,} | {l['p0']} us | {l['p25']} us | {l['p50']} us | {l['p95']} us | {l['p99']} us |")
F["q4"] = "\n".join(rows)
dj = json.load(open(f"{R}/q234/dp_close_q4j.json"))["q4j"]["depths"]
c = dj["cpu_rounds"]
rows = []
for dd in (1, 2, 4, 8):
    dl = [m - h for m, h in zip(c[f"m{dd}"], c[f"H{dd}"])]
    for a in (f"H{dd}", f"m{dd}"):
        x = dj["arms"][a]
        l = x["lat"]
        rows.append(f"| {dd} | {a} | {dd+1}-way | {x['cpu_us_median']:.2f} us | {qps(x['cpu_us_median'])} | " + (f"{st.median(dl):+.2f} us [{min(dl):+.1f}, {max(dl):+.1f}] ({100*st.median(dl)/dj['arms']['H'+str(dd)]['cpu_us_median']:+.1f} %)" if a.startswith("m") else "-") +
                    f" | {l['ops']:,} | {l['p0']} us | {l['p25']} us | {l['p50']} us | {l['p95']} us | {l['p99']} us |")
x = dj["arms"]["H1b"]
fl = [h2 - h for h2, h in zip(c["H1b"], c["H1"])]
l = x["lat"]
rows.append(f"| 1 | H1b (floor, copy of H1's table) | 2-way | {x['cpu_us_median']:.2f} us | {qps(x['cpu_us_median'])} | {st.median(fl):+.2f} us [{min(fl):+.1f}, {max(fl):+.1f}] vs H1 | {l['ops']:,} | {l['p0']} us | {l['p25']} us | {l['p50']} us | {l['p95']} us | {l['p99']} us |")
F["q4j"] = "\n".join(rows)
json.dump(F, open("/home/louis/bench-runs/dp-close/frags.json", "w"), indent=1)
for k, v in F.items():
    print("##", k)
    print(v)
