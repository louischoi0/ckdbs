#!/usr/bin/env python3
import contextlib, io, json, os, re, statistics, sys

sys.path.insert(0, "/home/cdkbs/bench-runs/cc-close/harness")
import render as R
import final as F

B = "/home/cdkbs/bench-runs/cc-close"
DESC = "v3.2.0-184-gc547ef14"
OUTMD = f"/home/cdkbs/ckdbs/.claude/worktrees/cc-stats-planner/bench/v3.2.0/results-cc-close-{DESC}.md"


def J(p, d=None): return json.load(open(p)) if os.path.exists(p) else d


def cap(fn, *a):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf): fn(*a)
    return buf.getvalue().strip()


def dist(out, phase, arms, ctrl=None):
    g = R.groups(R.load(out, phase))
    t, S = R.dist_table(g, arms)
    return t, g, S


def delta(out, phase, arms, base, ctrl, test):
    t, g, S = dist(out, phase, arms)
    return t, R.delta_table(g, S, base, ctrl, test)


ov1_t, ov1_d = delta(f"{B}/out", "overhead", ["A", "A2", "B"], "A", "A2", "B")
ov2_t, ov2_d = delta(f"{B}/out", "overhead2", ["A", "A2", "B"], "A", "A2", "B")
ov3_t, ov3_d = delta(f"{B}/out", "overhead3", ["A", "A2", "B"], "A", "A2", "B")
ovR2_t, ovR2_d = delta(f"{B}/out2", "overhead", ["A", "A2", "B"], "A", "A2", "B")
pooled = cap(F.pooled_overhead)
host = '; '.join(l[2:] for l in cap(F.host_summary).splitlines() if l.startswith('- '))

g1_t, g1_d = delta(f"{B}/out", "gain", ["A", "A2", "B"], "A", "A2", "B")
g2_t, g2_d = delta(f"{B}/out2", "gain", ["A", "A2", "B"], "A", "A2", "B")
m1_t, m1_d = delta(f"{B}/out", "memo", ["A-same", "A2-same", "B-same", "A-vary", "B-vary"], "A-same", "A2-same", "B-same")
m2_t, m2_d = delta(f"{B}/out2", "memo", ["A-same", "A2-same", "B-same", "A-vary", "B-vary"], "A-same", "A2-same", "B-same")


def vary_lines(out):
    g = R.groups(R.load(out, "memo"))
    rows = ["| Cell | A-vary | B-vary | B-vary vs A-vary: CPU | B-vary vs A-vary: throughput |", "|---|---|---|---|---|"]
    for c, a in g.items():
        x, y = R.stats(a["A-vary"]), R.stats(a["B-vary"])
        rows.append(f"| {c} | {x['cpu']:.2f} µs | {y['cpu']:.2f} µs | {R.d(x['cpu'], y['cpu']):+.1f} % | {R.d(x['qps'], y['qps']):+.1f} % |")
    return "\n".join(rows)


vary1, vary2 = vary_lines(f"{B}/out"), vary_lines(f"{B}/out2")

# same plan two engines
ex = R.groups(J(f"{B}/out/extra.json"))
ex_t, ex_S = R.dist_table(ex, ["WA:drv", "WB:drv", "WA:swap", "WB:swap", "BP:drv"])
rows = ["| Cell | WB vs WA, driver order | WB vs WA, swapped order | Swapped vs driver order, A binary | BP (B planned) vs WB:swap = planning cost | BP vs WA:drv = realised flip |", "|---|---|---|---|---|---|"]
for c in ex:
    f = lambda a, b: R.d(ex_S[(c, a)]["cpu"], ex_S[(c, b)]["cpu"])
    rows.append(f"| {c} | {f('WA:drv','WB:drv'):+.1f} % | {f('WA:swap','WB:swap'):+.1f} % | {f('WA:drv','WA:swap'):+.1f} % | {f('WB:swap','BP:drv'):+.1f} % ({ex_S[(c,'BP:drv')]['cpu']-ex_S[(c,'WB:swap')]['cpu']:+.1f} µs) | {f('WA:drv','BP:drv'):+.1f} % |")
ex_d = "\n".join(rows)

uq = R.groups(J(f"{B}/out/extra-uniq.json"))
uq_t, uq_S = R.dist_table(uq, ["PA:uniq", "PA2:uniq", "BP:uniq"])
rows = ["| Cell | BP vs PA: CPU | BP vs PA: throughput | PA2 vs PA (noise floor) | Per-round BP vs PA CPU |", "|---|---|---|---|---|"]
for c, a in uq.items():
    ra = {x["round"]: x["server_cpu_us"] for x in a["PA:uniq"]}; rb = {x["round"]: x["server_cpu_us"] for x in a["BP:uniq"]}
    rows.append(f"| {c} | {R.d(uq_S[(c,'PA:uniq')]['cpu'], uq_S[(c,'BP:uniq')]['cpu']):+.1f} % ({uq_S[(c,'BP:uniq')]['cpu']-uq_S[(c,'PA:uniq')]['cpu']:+.1f} µs) | {R.d(uq_S[(c,'PA:uniq')]['qps'], uq_S[(c,'BP:uniq')]['qps']):+.1f} % | {R.d(uq_S[(c,'PA:uniq')]['cpu'], uq_S[(c,'PA2:uniq')]['cpu']):+.1f} % | {', '.join(f'{R.d(ra[k], rb[k]):+.1f}' for k in sorted(ra))} % |")
uq_d = "\n".join(rows)

g2x = R.groups(J(f"{B}/out/gain2.json")); g2x_t, g2x_S = R.dist_table(g2x, ["A", "A2", "B", "BL"])
g3x = R.groups(J(f"{B}/out/gain3.json")); g3x_t, g3x_S = R.dist_table(g3x, ["A", "B0", "B1", "B2", "B3"])
g4x = R.groups(J(f"{B}/out/gain4.json")); g4x_t, g4x_S = R.dist_table(g4x, ["WA0", "WA1", "WB0", "WB1"])


def dl(S, cells, base, arms):
    rows = ["| Cell | " + " | ".join(f"{a} vs {base}" for a in arms) + " |", "|---|" + "---|" * len(arms)]
    for c in cells:
        rows.append(f"| {c} | " + " | ".join(f"{R.d(S[(c, base)]['cpu'], S[(c, a)]['cpu']):+.1f} %" for a in arms) + " |")
    return "\n".join(rows)


g2x_d = dl(g2x_S, g2x, "A", ["A2", "B", "BL"])
g3x_d = dl(g3x_S, g3x, "A", ["B0", "B1", "B2", "B3"])
g4x_d = dl(g4x_S, g4x, "WA0", ["WA1", "WB0", "WB1"])


def cold_table(out):
    cold = J(f"{out}/cold.json")
    cells = []
    for r in cold:
        if r["cell"] not in cells: cells.append(r["cell"])
    rows = ["| Cell | Server | 1st execution: CPU | 1st execution: latency | 2nd-20th: mean CPU | 2nd-20th: median latency |", "|---|---|---|---|---|---|"]
    for c in cells:
        for lab in ("A", "B"):
            rs = sorted([r for r in cold if r["cell"] == c and r["server"] == lab], key=lambda r: r["i"])
            rest = rs[1:]
            rows.append(f"| {c} | {lab} | {rs[0]['cpu_us']:,.0f} µs | {rs[0]['lat_us']:,.0f} µs | {statistics.mean(r['cpu_us'] for r in rest):,.1f} µs | {statistics.median(r['lat_us'] for r in rest):,.0f} µs |")
    return "\n".join(rows)


cold1, cold2 = cold_table(f"{B}/out"), cold_table(f"{B}/out2")


def plans_table(out):
    p = J(f"{out}/plans.json")
    rows = ["| Statement | Server A (CB data-free) | Server B (statistics, steady state) |", "|---|---|---|"]
    for c, v in p.items():
        if c.startswith("c3-vary"): continue
        def chain(lines):
            steps = [re.sub(r"^step (\d+) (\w+) (\S+)( AS \w+)?( build on=\S+)?.*", lambda m: f"{m.group(2)} {m.group(3).rsplit('_', 1)[0]}" + (" +build" if m.group(5) else ""), l) for l in lines if l.startswith("step ") and "opens=" not in l]
            tag = [l for l in lines if l.startswith("planned order")]
            head = lines[0]
            ex = re.search(r"examined=(\d+) pages=(\d+) opens=(\d+)", head)
            return f"{' → '.join(steps)}; examined {ex.group(1)}, pages {ex.group(2)}, opens {ex.group(3)}" + (f"; `{tag[0].split(' est=')[0]}`" if tag else "")
        rows.append(f"| {c} | {chain(v['A'])} | {chain(v['B'])} |")
    return "\n".join(rows)


plans1 = plans_table(f"{B}/out")

# SHOW META (B) subset
snaps = J(f"{B}/out/meta-snapshots.json")
keys = ["plan_stats_generation", "plan_stats_relations", "plan_stats_posteriors", "plan_stats_part_drops", "plan_stats_seeds", "plan_stats_seed_pages", "plan_stats_publishes", "plan_stats_steward_us_max", "plan_memo_hits", "plan_memo_misses"]
rows = ["| Snapshot (B) | UTC | generation | relations | posteriors | part_drops | seeds | seed_pages | publishes | steward_us_max | memo hits | memo misses | B server CPU so far |", "|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
for m in snaps:
    if m["server"] != "B": continue
    rows.append(f"| {m['label']} | {m['t'][11:19]} | " + " | ".join(str(m["meta"][k]) for k in keys) + f" | {m['cpu_ns']/1e9:.1f} s |")
meta_tbl = "\n".join(rows)
rows = ["| Snapshot | Server | pool_resident | pool_hits | pool_misses | pool_refused |", "|---|---|---|---|---|---|"]
for m in snaps:
    rows.append(f"| {m['label']} | {m['server']} | {m['meta']['pool_resident']} | {int(m['meta']['pool_hits']):,} | {int(m['meta']['pool_misses']):,} | {m['meta']['pool_refused']} |")
pool_tbl = "\n".join(rows)

idle = {n: J(f"{B}/{n}/idle.json") for n in ("out", "out2")}
rows = ["| Run | A | A2 (control) | B | B minus A | A2 minus A (floor) | B steward over the 30 s |", "|---|---|---|---|---|---|---|"]
for n, lab in (("out", "R1"), ("out2", "R2")):
    i = idle[n]; c = i["cpu_us"]
    rows.append(f"| {lab} | {c['A']/1e3:.1f} ms | {c['A2']/1e3:.1f} ms | {c['B']/1e3:.1f} ms | {(c['B']-c['A'])/1e3:+.1f} ms | {(c['A2']-c['A'])/1e3:+.1f} ms | seeds {int(i['B_seeds'][1])-int(i['B_seeds'][0])}, seed pages {int(i['B_seed_pages'][1])-int(i['B_seed_pages'][0])}, publishes {int(i['B_publishes'][1])-int(i['B_publishes'][0])}, steward_us_max {i['B_steward_us_max']} µs |")
idle_tbl = "\n".join(rows)

pr = J(f"{B}/out/priors.json")["rows"]
rows = ["| Relation | Height | Leaves | est_rows (published prior) | True COUNT(*) | est / true |", "|---|---|---|---|---|---|"]
ratios = {1: [], 2: [], 3: []}
for r in pr:
    if r["est_rows"] in (None, ""): continue
    e, t = float(r["est_rows"]), float(r["true_rows"])
    ratios[int(r["height"])].append(e / t)
    rows.append(f"| {r['name']} | {r['height']} | {r['leaves']} | {int(e):,} rows | {int(t):,} rows | {e/t:.2f} |")
priors_tbl = "\n".join(rows)
pr_sum = "; ".join(f"height {h}: {len(v)} relations, est/true {min(v):.2f}-{max(v):.2f}" for h, v in ratios.items() if v)
_np = sorted(r["name"] for r in pr if r["est_rows"] in (None, ""))
_kinds = sorted({n.rsplit("_", 1)[0] for n in _np})
noprior = str(len(_np)) + " relations, all scenario 2 side tables (" + ", ".join(_kinds) + ")"

st = {n: J(f"{B}/{n}/stability.json") for n in ("out", "out2")}
rows = ["| Run | Statements | Window throughput (10 s windows) | generation before → after | publishes | seeds / seed pages in 60 s | posteriors | part_drops | steward_us_max | memo hits / misses in 60 s |", "|---|---|---|---|---|---|---|---|---|---|"]
for n, lab in (("out", "R1"), ("out2", "R2")):
    s = st[n]; a, b = s["meta_before"], s["meta_after"]
    dd = lambda k: int(b[k]) - int(a[k])
    rows.append(f"| {lab} | {s['ops']:,} | {', '.join(str(w['ops']) for w in s['wins'])} | {a['plan_stats_generation']} → {b['plan_stats_generation']} | {a['plan_stats_publishes']} → {b['plan_stats_publishes']} | {dd('plan_stats_seeds')} / {dd('plan_stats_seed_pages')} | {b['plan_stats_posteriors']} | {b['plan_stats_part_drops']} | {b['plan_stats_steward_us_max']} µs | {dd('plan_memo_hits'):,} / {dd('plan_memo_misses'):,} |")
stab_tbl = "\n".join(rows)
stab_text = cap(F.stability, f"{B}/out")

rs = J(f"{B}/out/restart2.json")
rows = ["| Attempt | Mount to listening | Statements run before the first publish | First statement to first publish (generation > 0) | First statement to warm plan (`reordered=1`, gen > 0) | recovery_checkpoint_us |", "|---|---|---|---|---|---|"]
for r in rs:
    rows.append(f"| {r['attempt'] + 1} | {r['ready_s']*1e3:.0f} ms | {r['statements_before_publish']} | {r['first_publish_s']*1e3:,.0f} ms | {r['warm_plan_s']*1e3:,.0f} ms | {int(r['recovery_checkpoint_us'])/1e3:.1f} ms |")
restart_tbl = "\n".join(rows)

ova = R.groups(R.load(f"{B}/out", "overhead"))
rows = ["| Statement | p50 latency (A) | Server CPU / statement (A) | CPU share of p50 | Remainder of p50 (derived) |", "|---|---|---|---|---|"]
for c, a in ova.items():
    s = R.stats(a["A"])
    rows.append(f"| {c} | {s['p50']:,.0f} µs | {s['cpu']:.1f} µs | {s['cpu']/s['p50']*100:.0f} % | {s['p50']-s['cpu']:,.0f} µs |")
waits_tbl = "\n".join(rows)

doc = open("/home/cdkbs/bench-runs/cc-close/harness/doc_template.md").read()
vals = {k: v for k, v in globals().items() if isinstance(v, str)}
for k, v in vals.items():
    doc = doc.replace("{{" + k + "}}", v)
os.makedirs(os.path.dirname(OUTMD), exist_ok=True)
open(OUTMD, "w").write(doc)
left = re.findall(r"\{\{(\w+)\}\}", doc)
print("written", OUTMD, len(doc), "unfilled:", left)
