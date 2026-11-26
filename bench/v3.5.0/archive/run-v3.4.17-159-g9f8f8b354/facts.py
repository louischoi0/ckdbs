"""Timeline and bookkeeping facts for the documents. Usage: python3 -I facts.py"""
import glob, os, re, json, datetime

R = "/home/ukjin/bench-runs/scenarios-v3.4.17-159-g9f8f8b354"


def ts(s):
    return datetime.datetime.strptime(s.strip(), "%Y-%m-%dT%H:%M:%SZ")


cells = {}
for f in glob.glob(R + "/out/logs/*.t0"):
    tag = os.path.basename(f)[:-3]
    t1 = R + f"/out/logs/{tag}.t1"
    if os.path.exists(t1):
        cells[tag] = (ts(open(f).read()), ts(open(t1).read()))


def group(tag):
    if tag.startswith("pin-"):
        return "pinned A/B"
    if tag.startswith("ab-") and tag.endswith("s4"):
        return "s4 old-binary control"
    if tag.startswith("ab-"):
        return "unpinned A/B"
    if tag == "s4":
        return "s4 run A"
    if tag == "cd2-s4":
        return "s4 run B"
    if tag.startswith(("rr-", "sat-b16")):
        return "s2 extras"
    for k in ("s1s-", "s1-"):
        if k in tag:
            return "s1 main"
    if "s2-" in tag:
        return "s2 main"
    if "s3-" in tag:
        return "s3 main"
    return "other"


g = {}
for tag, (a, b) in cells.items():
    k = group(tag)
    lo, hi, n, busy = g.get(k, (a, b, 0, 0.0))
    g[k] = (min(lo, a), max(hi, b), n + 1, busy + (b - a).total_seconds())
for k, (lo, hi, n, busy) in sorted(g.items(), key=lambda x: x[1][0]):
    print(f"{k}: {lo:%H:%M:%S} to {hi:%H:%M:%S} UTC, {n} cells, driver time {busy:.0f} s")

holds = [l.split() for l in open(R + "/run/hold") if l.strip()]
tot = sum(int(x[1]) for x in holds)
nz = [(x[0], int(x[1])) for x in holds if int(x[1]) > 0]
print(f"gates {len(holds)}, total hold {tot} s, gates that held {len(nz)}: " + ", ".join(f"{a} {b} s" for a, b in nz))
rcs = {os.path.basename(f)[:-3]: open(f).read().strip() for f in glob.glob(R + "/out/logs/*.rc")}
print("cells with rc:", len(rcs), "nonzero:", {k: v for k, v in rcs.items() if v != "0"})
ports = sorted({l.split("=")[1].strip() for f in glob.glob(R + "/out/conf/*.conf") for l in open(f) if l.startswith("port")})
print("ports:", ports)
gate2 = [os.path.basename(f)[:-9] for f in glob.glob(R + "/out/logs/*.precheck") if "gate v2" in open(f).read()]
print("cells under gate v2:", len(gate2), "of", len(glob.glob(R + "/out/logs/*.precheck")))
idle = [os.path.basename(f)[:-9] for f in glob.glob(R + "/out/logs/*.precheck")
        if re.search(r"present, idle or not: [1-9]", open(f).read())]
print("cells started with an idle build/test process present (gate v2):", len(idle))
sz = sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(R + "/out") for f in fs)
print(f"out/ size: {sz / 1e6:.1f} MB")
# scenario 4: is any lookup share non-NULL in any evidence snapshot?
for tag in ("s4", "cd2-s4", "ab-old1-s4"):
    p = R + f"/out/json/{tag}.json"
    if not os.path.exists(p):
        continue
    d = json.load(open(p))
    nn = 0; snaps = 0; ext = set(); pages = set()
    for e in d["evidence"]:
        on = e.get("on") or {}
        snaps += 1
        ext.add(on.get("header", {}).get("extends"))
        for x in on.get("entries", []):
            pages.add(x.get("pages"))
            if x.get("hint_fail_pct") != "NULL" or x.get("coverage_miss_pct") != "NULL":
                nn += 1
    fin = d["final"]["on"]["header"]
    print(f"{tag}: evidence snapshots {snaps}, entries with a non-NULL share {nn}, extends seen {sorted(ext, key=str)}, pages seen {sorted(pages, key=str)}, "
          f"final managed {fin.get('managed')} creates {fin.get('creates')} drops {fin.get('drops')} extends {fin.get('extends')}; "
          f"plans {json.dumps(d.get('plans'))[:200]}; started {d['meta'].get('started_utc')} elapsed {d['meta'].get('elapsed_s')}")
