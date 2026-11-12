"""gen_tables.py <pass dir> - the results file's tables, from the driver's JSON.

Server kind is the file's prefix: m1-plain / m1-marked / ab-base (group),
m2-plain / m2-marked (strict), m4p-plain / m4p-marked (the pause).
"""
import collections
import glob
import json
import statistics
import sys

D = sys.argv[1]
cells = collections.defaultdict(list)   # (kind, dur, arm, k) -> [cell]
for f in sorted(glob.glob(D + "/*.json")):
    name = f.split("/")[-1]
    if any(s in name for s in (".server.", ".start.", ".stop.")):
        continue
    stem = name[:-5]
    fam, kind = stem.split("-")[0], stem.split("-")[1]
    dur = {"m1": "group", "ab": "group", "m4p": "pause", "m2": "strict"}[fam]
    for c in json.load(open(f))["cells"]:
        cells[(kind, dur, c["arm"], c["k"])].append(c)


def n(x):
    return f"{x:,.0f}"


def med(key, field=lambda c: c["tps"]):
    return statistics.median(field(c) for c in cells[key])


def spread(key):
    v = [c["tps"] for c in cells[key]]
    return f"`{n(min(v))}-{n(max(v))} tps`"


def refused(key):
    att = sum(c["committed"] + sum(c["refused"].values()) for c in cells[key])
    ref = collections.Counter()
    for c in cells[key]:
        ref.update(c["refused"])
    if not ref:
        return "none"
    return ", ".join(f"{k} `{v}` (`{100.0 * v / att:.1f} %` of attempts)" for k, v in sorted(ref.items()))


def cpu_us(key):
    return statistics.median(1e6 * c["server_cpu_s"] / c["committed"] for c in cells[key])


def oracle():
    allc = [c for v in cells.values() for c in v]
    return sum(1 for c in allc if c["oracle"]["ok"] and not c["fatal"]), len(allc)


ROWS = (("hot (debit first), today", "plain", "hot"),
        ("**reserved** (debit first, the column marked)", "marked", "hot"),
        ("hot-last piped with `COMMIT`", "plain", "hot-last-piped"),
        ("disjoint", "plain", "disjoint"))

print("### T1 throughput, `group`, 8 sessions (median of 3, and the range)\n")
print("| arm | `k = 8` | range | `k = 2` | range |")
print("|---|---|---|---|---|")
for label, kind, arm in ROWS:
    print(f"| {label} | `{n(med((kind, 'group', arm, 8)))} tps` | {spread((kind, 'group', arm, 8))} | "
          f"`{n(med((kind, 'group', arm, 2)))} tps` | {spread((kind, 'group', arm, 2))} |")
for k in (8, 2):
    hot = med(("plain", "group", "hot", k))
    top = med(("plain", "group", "hot-last-piped", k))
    res = med(("marked", "group", "hot", k))
    print(f"\nk={k}: reserved recovers {100 * (res - hot) / (top - hot):.1f} % of the gap "
          f"(hot {hot:.1f}, piped {top:.1f}, reserved {res:.1f}); reserved/hot = {res / hot:.2f}x")

print("\n### T2 refusals and latency, `group`, `k = 8` (all repetitions)\n")
print("| arm | refused | debit p50 | debit p99 | `COMMIT` p50 | `COMMIT` p99 | transaction p50 | transaction p99 |")
print("|---|---|---|---|---|---|---|---|")
for label, kind, arm in ROWS:
    key = (kind, "group", arm, 8)

    def p(what, q):
        vals = [c[what][q] for c in cells[key] if c.get(what) and c[what].get("n")]
        return f"`{n(statistics.median(vals))} us`" if vals else "not separate"
    print(f"| {label} | {refused(key)} | {p('debit', 'p50_us')} | {p('debit', 'p99_us')} | "
          f"{p('commit', 'p50_us')} | {p('commit', 'p99_us')} | {p('txn', 'p50_us')} | {p('txn', 'p99_us')} |")

print("\n### T3 `strict`, `k = 8` (2 repetitions: both values)\n")
print("| arm | committed | refused |")
print("|---|---|---|")
for label, kind, arm in ROWS:
    key = (kind, "strict", arm, 8)
    print(f"| {label} | " + ", ".join(f"`{n(c['tps'])} tps`" for c in cells[key]) + f" | {refused(key)} |")

print("\n### T4 a 1.2 s pause after the debit, `group`, `k = 0`, 20 s, one cell each\n")
print("| server | committed | refused | lock waits |")
print("|---|---|---|---|")
for label, kind in (("today", "plain"), ("**reserved**", "marked")):
    c = cells[(kind, "pause", "hot", 0)][0]
    print(f"| {label} | `{c['committed']}` transactions | "
          + (", ".join(f"{k} `{v}`" for k, v in c["refused"].items()) or "none")
          + f" | {c['lock_waits'].get('by_outcome')} |")

print("\n### T5 off the path: the spike's binary against the base's, no mark, `group` (median of 3)\n")
print("| arm, `k` | base | range | spike, unmarked | range | spike / base | server CPU a transaction, base | spike |")
print("|---|---|---|---|---|---|---|---|")
for arm in ("hot", "hot-last-piped", "disjoint"):
    for k in (8, 2):
        b, s = ("base", "group", arm, k), ("plain", "group", arm, k)
        if not cells[b]:
            continue
        print(f"| {arm}, `{k}` | `{n(med(b))} tps` | {spread(b)} | `{n(med(s))} tps` | {spread(s)} | "
              f"`{med(s) / med(b):.3f}` | `{cpu_us(b):.0f} us` | `{cpu_us(s):.0f} us` |")

print("\n### server CPU a transaction, `group`, `k = 8`")
for label, kind, arm in ROWS:
    key = (kind, "group", arm, 8)
    cores = statistics.median(c["server_cpu_s"] / c["seconds"] for c in cells[key])
    print(f"{label}: {cpu_us(key):.0f} us, {cores:.2f} cores")

ok, total = oracle()
print(f"\noracle exact in {ok} of {total} cells")
print("reserved lock waits k=8:", [c["lock_waits"].get("by_outcome") for c in cells[("marked", "group", "hot", 8)]],
      [c["lock_waits"].get("by_unit_mode") for c in cells[("marked", "group", "hot", 8)]])
print("hosts:", sorted({c["host"]["loadavg"].split()[0] for v in cells.values() for c in v})[:3], "...",
      "competing:", sorted({str(c["host"]["competing"]) for v in cells.values() for c in v}))
