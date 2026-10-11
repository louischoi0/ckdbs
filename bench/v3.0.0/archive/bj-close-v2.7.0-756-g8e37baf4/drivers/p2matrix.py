"""Part 2 and 3 matrices in throughput terms (derived 1,000,000 / median per-run p50). usage: p2matrix.py <dir>"""
import glob, json, statistics, sys

D = sys.argv[1]
cells = {}
for f in sorted(glob.glob(f"{D}/c*-n*-r*.json")):
    j = json.load(open(f))
    sc = {2, 4} | ({3} if j["cores"] == 2 else set())
    o = [v for k, v in j["cpu_busy"].items() if int(k) not in sc]
    if sum(o) / len(o) > 0.30 or not all(j["verify"].values()) or j["bad"]:
        continue
    cells.setdefault((j["cores"], j["rows"]), []).append(j)
keys = sorted(cells)
N = {"lit": "`SET v = <lit>`", "expr": "`SET v = v + 1`", "swap": "`SET v = w, w = v`",
     "case": "`SET v = CASE WHEN v % 2 = 0 THEN v + 1 ELSE v - 1 END`", "subq": "`SET v = (SELECT COUNT(*) FROM t)`"}
hdr = "| SET value | " + " | ".join(f"cores {c}, {r:,} rows" for c, r in keys) + " |\n|---|" + "---|" * len(keys)


def med(js, k):
    return statistics.median(j["arms_b_only"][k]["p50_us"] for j in js)


print("POINT `WHERE id = k`: statements/s at p50 (derived), B\n" + hdr)
for g in ("lit", "expr", "swap", "case"):
    print(f"| {N[g]} | " + " | ".join(f"{1e6 / med(cells[k], 'xp-' + g):,.0f} /s" for k in keys) + " |")
print("\nRANGE `WHERE id >= 1`: statements/s at p50 (derived) = rows/s in brackets, B\n" + hdr)
for g in ("lit", "expr", "swap", "case", "subq"):
    print(f"| {N[g]} | " + " | ".join(f"{1e6 / med(cells[k], 'xr-' + g):,.1f} /s ({k[1] * 1e6 / med(cells[k], 'xr-' + g):,.0f} rows/s)" for k in keys) + " |")
print("\nROUND TRIP: operations/s at p50 (derived), B, one session\n" + hdr.replace("SET value", "spelling"))
for g, nm in (("rt-one", "one statement `UPDATE SET v = v + 1`"), ("rt-pair", "`SELECT v` then `UPDATE SET v = <v+1>` (2 round trips)"),
              ("rt-txn", "`BEGIN; SELECT; UPDATE; COMMIT` (4 round trips)")):
    print(f"| {nm} | " + " | ".join(f"{1e6 / med(cells[k], g):,.0f} /s" for k in keys) + " |")
