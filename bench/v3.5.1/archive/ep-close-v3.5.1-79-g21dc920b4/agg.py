#!/usr/bin/env python3
import json, glob, os, re, statistics as S, sys
O = "/home/ukjin/workspaces/ckdbs/.claude/worktrees/ep-bench/out"
cells = {"s0txn": ("txn", "s0 txn=1 fk=1, 3 traders, group"), "s0g4": ("txn", "s0 txn=0, 4 traders, group"),
         "s5s4": ("mix", "s5 4 terminals, strict")}


def load(cell, arm):
    runs = []
    for f in sorted(glob.glob(f"{O}/{cell}-{arm}-*.json")):
        base = f[:-5]
        j = json.load(open(f))
        if j.get("failed"):
            runs.append({"tag": os.path.basename(base), "failed": True}); continue
        r = {"tag": os.path.basename(base), "j": j}
        err = open(base + ".stderr").read()
        r["stderr_refusal"] = len(re.findall(r"undo chain revisits", err))
        sl = base + ".serverlog"
        r["serverlog_refusal"] = len(re.findall(r"undo chain revisits", open(sl).read())) if os.path.exists(sl) else None
        r["rc"] = open(base + ".rc").read().split()[0]
        m = {}
        for line in open(base + ".meta.txt"):
            p = line.strip().split("|")
            if len(p) == 3 and p[0] == "undo":
                m[p[1]] = int(p[2])
        r["undo"] = m
        # errors
        ph = {p["name"]: p for p in j["phases"]}
        ev = 0; anyerr = False
        for n, p in ph.items():
            e = sum(p["errors"].values()) if p["errors"] else 0
            if e: anyerr = True
            if n not in ("txn", "mix"): ev += e
        main = ph[cells[cell][0]]
        e = sum(main["errors"].values()) if main["errors"] else 0
        if ev == 0: ev = e
        r["events"] = ev; r["anyerr"] = anyerr
        r["failed_checks"] = [c["name"] for c in j["checks"] if not c["ok"]]
        runs.append(r)
    return runs


def med(x): return S.median(x)


def spread(x): return (max(x) - min(x)) / med(x) * 100


def rate(r, cell):
    j = r["j"]
    if cell == "s5s4": return j["meta"]["mix"]["new_orders_per_minute"]
    return j["meta"]["tps"]


out = {}
for cell in cells:
    A = [r for r in load(cell, "A")]; B = [r for r in load(cell, "B")]
    print(f"\n=== {cell}: {cells[cell][1]}  runs A={len(A)} B={len(B)}")
    for arm, R in (("A", A), ("B", B)):
        ok = [r for r in R if not r.get("failed")]
        print(f" arm {arm}: runs {len(R)}, failed {len(R)-len(ok)}, rc!=0 {sum(1 for r in ok if r['rc']!='0')}, "
              f"failed checks {sum(1 for r in ok if r['failed_checks'])}")
        print(f"   refused runs (any error) {sum(1 for r in ok if r['anyerr'])}, stderr-revisit runs {sum(1 for r in ok if r['stderr_refusal'])}, "
              f"refusal events {sum(r['events'] for r in ok)}, serverlog revisit lines {sum(r['serverlog_refusal'] or 0 for r in ok)}")
        rt = [rate(r, cell) for r in ok]
        key = cells[cell][0]
        P = {k: [r["j"]["phases"][[p['name'] for p in r['j']['phases']].index(key)]["latency"][k] for r in ok] for k in ("p0_us", "p25_us", "p50_us", "p95_us", "p99_us", "mean_us", "max_us", "ops")}
        cpu = [r["j"]["phases"][[p['name'] for p in r['j']['phases']].index(key)]["server_cpu_us_per_op"] for r in ok]
        tp = [r["j"]["phases"][[p['name'] for p in r['j']['phases']].index(key)]["throughput_ops"] for r in ok]
        print(f"   rate (tps or NOPM) median {med(rt):.1f} min {min(rt):.1f} max {max(rt):.1f} spread {spread(rt):.1f}%  | phase ops/s median {med(tp):.1f}")
        print("   " + " ".join(f"{k} {med(v):.1f}(sp {spread(v):.0f}%)" for k, v in P.items()))
        print(f"   server cpu us/op median {med(cpu):.2f} spread {spread(cpu):.1f}%")
        ul = [r["undo"].get("undo_pages_live") for r in ok]; ur = [r["undo"].get("undo_pages_recycled") for r in ok]
        print(f"   undo live min/med/max {min(ul)}/{med(ul)}/{max(ul)}  recycled min/med/max {min(ur)}/{med(ur)}/{max(ur)}")
        out[(cell, arm)] = (ok, rt, P, cpu)
    # pairwise
    n = min(len(A), len(B))
    pr = []
    for a, b in zip(A[:n], B[:n]):
        if not a.get("failed") and not b.get("failed"):
            pr.append(rate(b, cell) / rate(a, cell))
    print(f" paired B/A rate: median {med(pr):.4f} min {min(pr):.3f} max {max(pr):.3f} n={len(pr)}")
    ra = out[(cell, "A")][1]; rb = out[(cell, "B")][1]
    print(f" medians B/A {med(rb)/med(ra):.4f}; within-arm spread A {spread(ra):.1f}% B {spread(rb):.1f}%; IQR-ish A {S.quantiles(ra,n=4)[0]:.0f}-{S.quantiles(ra,n=4)[2]:.0f} B {S.quantiles(rb,n=4)[0]:.0f}-{S.quantiles(rb,n=4)[2]:.0f}")
    # A first half vs second half (noise floor within arm: A_odd vs A_even)
    ao = [x for i, x in enumerate(ra) if i % 2 == 0]; ae = [x for i, x in enumerate(ra) if i % 2 == 1]
    bo = [x for i, x in enumerate(rb) if i % 2 == 0]; be = [x for i, x in enumerate(rb) if i % 2 == 1]
    print(f" control A-odd vs A-even median ratio {med(ae)/med(ao):.4f}; B-odd vs B-even {med(be)/med(bo):.4f}")
