import json, re, sys
runs = json.load(open(sys.argv[1]))["runs"]
ok = True
n = 0
for r in runs:
    if "bn" not in r.get("digest", {}):
        continue
    n += 1
    d = r["digest"]["bn"]
    a = r["analyze"]["bn"]
    sig = {k: re.findall(r"(examined=\d+|matched=\d+|sel=\d+%|pages=\d+|spills=\d+)", v) for k, v in a.items()}
    same_d = len(set(d.values())) == 1
    same_a = all(sig[k] == sig["A"] for k in sig)
    ok = ok and same_d and same_a
    print(r["run"], d, "digest identical" if same_d else "DIGEST DIFFERS", "counts identical" if same_a else "COUNTS DIFFER", sig["A"][:5])
print("runs checked:", n, "of", len(runs))
print("ALL IDENTICAL" if ok else "MISMATCH")
