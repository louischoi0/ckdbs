import json, re
O = "/home/cdkbs/bench-runs/cc-close/out/"
J = lambda n: json.load(open(O + n))
print("STARTUP", J("startup.json"))
print("RESTART", json.dumps(J("restart.json"))[:1500])
print("IDLE", json.dumps(J("idle.json")))
st = J("stability.json")
print("STAB before", {k: v for k, v in st["meta_before"].items() if k.startswith("plan")})
print("STAB after", {k: v for k, v in st["meta_after"].items() if k.startswith("plan")})
print("ops", st["ops"], "cpu_us_per_op", st["cpu_us_per_op"], "wins", st["wins"])
print("samples", st["samples"])
by = {}
for o in st["orders"]:
    chain = [l for l in o["plan"] if l.startswith("step ") or l.startswith("planned order")]
    sig = " ; ".join(re.sub(r" gen=\d+| switch=[0-9.]+", "", l) if l.startswith("planned") else l for l in chain)
    by.setdefault(o["cell"], {}).setdefault(sig, []).append(o["t"])
for c, d in by.items():
    print(c, len(d), "distinct chains")
    for sig, ts in d.items(): print("   ", ts, sig[:230])
gens = {}
for o in st["orders"]:
    for l in o["plan"]:
        m = re.search(r"planned order=(\S+) gen=(\d+) switch=(\S+)", l)
        if m: gens.setdefault(o["cell"], []).append((o["t"], m.group(1), m.group(2)))
print("gens", gens)
pr = J("priors.json")
cols = pr["cols"]
print(cols)
for r in pr["rows"]:
    t = float(r["true_rows"]); e = float(r["est_rows"]) if r["est_rows"] not in (None, "") else 0
    print(r["name"], "h", r["height"], "leaves", r["leaves"], "est", r["est_rows"], "true", r["true_rows"], "gen", r["generation"], "ratio", round(e / t, 2) if t else None)
