#!/usr/bin/env python3
"""CZ-S4's tables (workorder-cz-checkpoint-roll-wait.md §5, the close), from
the driver's JSON and each cell's server log (`.s.log`).

  V1  isolated-B, levels-A, levels-B at relaxed: CZ-Q1 (a)'s bar on B, the
      median of 5 against the isolated median of 5, and each run against the
      isolated run interleaved with it in at least 4 of 5 pairs; p99.9 and
      the stall samples (>= 10 ms) beside it (CZ-Q11 (a)); BL's bar (v) is
      the same reading (CZ-Q6 (a)).
  C3  the bulk writer alone, A and B: B loses at most 3 % (CZ-Q2 (a)).
  G   the same V1 cell at group, no bar (CZ-Q5 (a)).
  The checkpoint: B's latch-site counters, wall time, recycle skips, and the
  live segments at the window's end (CZ-Q2 (a), reported with no bar).

Usage: cz_close.py OUT_DIR   (OUT_DIR holds v1/, c3/, g/, overhead/)
A copy of `cz_s1.py`'s loaders, so the archive stands alone.
"""
import glob, json, math, re, statistics, sys

OUT = sys.argv[1]
STALL_NS = 10_000_000
SEG = 64 * 1024 * 1024
KV = re.compile(r"(\w+)=(-?\d+)")


def fmt(us):
    return f"{us / 1e3:.1f} ms" if us >= 1e3 else f"{us:.0f} µs"


def load(path):
    hits = glob.glob(f"{path}/*.json")
    if not hits:
        return None
    d = json.load(open(hits[0]))
    d["_ck"], d["_rl"], d["_an"] = [], [], []
    for line in open(hits[0][:-5] + ".s.log"):
        kv = {k: int(v) for k, v in KV.findall(line)}
        if "[checkpoint] run:" in line:
            d["_ck"].append(kv)
        elif "[wal] roll" in line:
            d["_rl"].append(kv)
        elif "[checkpoint] anchor published:" in line:
            d["_an"].append(kv)
    return d


def cells(part, name):
    out = []
    for r in range(1, 6):
        d = load(f"{OUT}/{part}/r{r}/{name}")
        if d is not None:
            out.append((r, d))
    return out


def window(d):
    return d["measure_from_mono_ns"], d["stop_at_mono_ns"]


def inside(iv, w):
    return iv["start_ns"] < w[1] and iv["end_ns"] > w[0]


def readers(d):
    return [c for c in d["clients"] if c["role"] == "reader"]


def stalls(d):
    return [s for c in readers(d) for s in c["slow"] if s[1] >= STALL_NS]


def read_cores(d):
    return sorted(int(c) for c in d["meta_by_core"])


def live_segments(d, w):
    ends, seen, latest = {}, {}, {}
    for iv in d["_ck"]:
        ends.setdefault(iv["core"], []).append(iv["end_ns"])
    for a in d["_an"]:
        k = seen.get(a["core"], 0)
        seen[a["core"]] = k + 1
        mine = ends.get(a["core"], [])
        if k == 0 or (k <= len(mine) and mine[k - 1] <= w[1]):
            latest[a["core"]] = a["redo_start"]
    cur = max((iv["seg"] for iv in d["_rl"] if iv["end_ns"] <= w[1]), default=0)
    return cur - min(latest.values()) // SEG + 1 if latest else None


def p(d, k):
    return d["read_from_schedule"][k]


def bar_table(part, label):
    iso = dict(cells(part, "isolated-B"))
    arms = {"levels-A": dict(cells(part, "levels-A")), "levels-B": dict(cells(part, "levels-B"))}
    print(f"\n### {label}: reader schedule latency\n")
    print("| run | isolated-B p99 | levels-A p99 | levels-B p99 | A / isolated | B / isolated | isolated p99.9 | A p99.9 | B p99.9 | stall samples A | stall samples B | errors (iso, A, B) |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in sorted(iso):
        i, a, b = iso[r], arms["levels-A"].get(r), arms["levels-B"].get(r)
        if a is None or b is None:
            continue
        print(f"| r{r} | {fmt(p(i,'p99_us'))} | {fmt(p(a,'p99_us'))} | {fmt(p(b,'p99_us'))} | "
              f"{p(a,'p99_us')/p(i,'p99_us'):.2f}x | {p(b,'p99_us')/p(i,'p99_us'):.2f}x | "
              f"{fmt(p(i,'p99.9_us'))} | {fmt(p(a,'p99.9_us'))} | {fmt(p(b,'p99.9_us'))} | "
              f"{len(stalls(a))} | {len(stalls(b))} | {i['errors']}, {a['errors']}, {b['errors']} |")
    med = lambda xs, k: statistics.median(p(d, k) for d in xs.values())
    mi = med(iso, "p99_us")
    print(f"\n| reading | isolated-B | levels-A | levels-B |\n|---|---|---|---|")
    print(f"| p99, median | {fmt(mi)} | {fmt(med(arms['levels-A'],'p99_us'))} | {fmt(med(arms['levels-B'],'p99_us'))} |")
    print(f"| against the isolated median | 1.00x | {med(arms['levels-A'],'p99_us')/mi:.2f}x | {med(arms['levels-B'],'p99_us')/mi:.2f}x |")
    for arm in ("levels-A", "levels-B"):
        pairs = sum(1 for r, d in arms[arm].items() if r in iso and p(d, "p99_us") <= 2 * p(iso[r], "p99_us"))
        print(f"| {arm}: pairs within 2x | | {pairs} of {len(arms[arm])} | |" if arm == "levels-A"
              else f"| {arm}: pairs within 2x | | | {pairs} of {len(arms[arm])} |")
    print(f"| p99.9, median | {fmt(med(iso,'p99.9_us'))} | {fmt(med(arms['levels-A'],'p99.9_us'))} | {fmt(med(arms['levels-B'],'p99.9_us'))} |")
    print(f"| stall samples (>= 10 ms), all runs | {sum(len(stalls(d)) for d in iso.values())} | "
          f"{sum(len(stalls(d)) for d in arms['levels-A'].values())} | {sum(len(stalls(d)) for d in arms['levels-B'].values())} |")
    print(f"| inserter rows/s, median | | {statistics.median(d['inserter_rows_per_s'] for d in arms['levels-A'].values()):,.0f} | "
          f"{statistics.median(d['inserter_rows_per_s'] for d in arms['levels-B'].values()):,.0f} |")
    print("\n#### read cores' holds\n")
    print("| arm | run | read cores | system polls > 10 ms | longest system poll (since start) | in-run holds > 10 ms | stream-latch wait, read cores | roll holds in window | roll hold median |")
    print("|---|---|---|---|---|---|---|---|---|")
    for arm in ("levels-A", "levels-B"):
        for r, d in sorted(arms[arm].items()):
            rc = read_cores(d)
            mb, md = d["meta_by_core"], d["meta_delta"]
            s = lambda k: sum(mb[str(c)].get(k, 0) for c in rc)
            w = window(d)
            rolls = [iv for iv in d["_rl"] if inside(iv, w)]
            hold = statistics.median(iv["hold_us"] for iv in rolls) if rolls else 0
            print(f"| {arm} | r{r} | {','.join(map(str, rc))} | {int(s('sched_system_polls_over_10ms'))} | "
                  f"{fmt(max(mb[str(c)].get('sched_system_longest_poll_us', 0) for c in rc))} | "
                  f"{int(s('sched_hold_in_run_over_10ms'))} | "
                  f"{fmt(sum(md.get(f'load_c{c}_waited_wal_stream_us', 0) for c in rc))} | {len(rolls)} | {fmt(hold)} |")
    return arms


def checkpoint_table(arms, label):
    print(f"\n### {label}: B's checkpoint runs inside the window, every core\n")
    print("| run | runs | wall, median | wall, max | busy begin/end/flush | roll parks begin/end/flush | bound begin/end/flush | parked begin/end/flush | blocking | assertion visit | recycle skips | recycle blocked | live segments at window end (A, B) |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r, d in sorted(arms["levels-B"].items()):
        w = window(d)
        rs = [iv for iv in d["_ck"] if inside(iv, w)]
        g = lambda f: "/".join(str(sum(iv.get(f"latch_{s}_{f}", 0) for iv in rs)) for s in ("begin", "end", "flush"))
        gu = lambda f: "/".join(fmt(sum(iv.get(f"latch_{s}_{f}", 0) for iv in rs)) for s in ("begin", "end", "flush"))
        walls = [iv["wall_us"] for iv in rs] or [0]
        a = arms["levels-A"].get(r)
        print(f"| r{r} | {len(rs)} | {fmt(statistics.median(walls))} | {fmt(max(walls))} | {g('busy')} | {g('roll_parks')} | "
              f"{g('bound')} | {gu('park_us')} | {gu('blocking_us')} | {fmt(sum(iv.get('assertion_visit_us', 0) for iv in rs))} | "
              f"{sum(iv.get('recycle_skips', 0) for iv in rs)} | {sum(iv.get('recycle_blocked', 0) for iv in rs)} | "
              f"{live_segments(a, window(a)) if a else '-'}, {live_segments(d, w)} |")
    print("\n#### A's checkpoint runs inside the window, for the wall time\n")
    print("| run | runs | wall, median | wall, max |\n|---|---|---|---|")
    for r, d in sorted(arms["levels-A"].items()):
        w = window(d)
        walls = [iv["wall_us"] for iv in d["_ck"] if inside(iv, w)] or [0]
        print(f"| r{r} | {len(walls)} | {fmt(statistics.median(walls))} | {fmt(max(walls))} |")


def host_table():
    print("### 0. The host at each cell\n")
    print("| part | cell | run | driver wait | load at start | load at end | competing at start | competing at end |")
    print("|---|---|---|---|---|---|---|---|")
    for part in ("v1", "c3", "g"):
        for path in sorted(glob.glob(f"{OUT}/{part}/r*/*")):
            d = load(path)
            if d is None:
                continue
            b, a = d["host_before"], d["host_after"]
            print(f"| {part} | {path.split('/')[-1]} | {path.split('/')[-2]} | {d['wait_s']:.0f} s | {b['loadavg'].split()[0]} | "
                  f"{a['loadavg'].split()[0]} | {len(b['competing'])} | {len(a['competing'])} |")


host_table()
v1 = bar_table("v1", "1. V1, relaxed")
checkpoint_table(v1, "2. V1")
print("\n### 3. C3, the bulk writer alone (CZ-Q2 (a): B loses at most 3 %)\n")
print("| run | A rows/s | B rows/s | B / A | A log MB/s | B log MB/s |\n|---|---|---|---|---|---|")
ca, cb = dict(cells("c3", "bulk-A")), dict(cells("c3", "bulk-B"))
for r in sorted(ca):
    if r in cb:
        print(f"| r{r} | {ca[r]['inserter_rows_per_s']:,.0f} | {cb[r]['inserter_rows_per_s']:,.0f} | "
              f"{cb[r]['inserter_rows_per_s']/ca[r]['inserter_rows_per_s']:.3f} | {ca[r]['log_bytes_per_s']/1e6:.1f} | {cb[r]['log_bytes_per_s']/1e6:.1f} |")
if ca and cb:
    ma = statistics.median(d["inserter_rows_per_s"] for d in ca.values())
    mb = statistics.median(d["inserter_rows_per_s"] for d in cb.values())
    print(f"| median | {ma:,.0f} | {mb:,.0f} | {mb/ma:.3f} | | |")
g = bar_table("g", "4. The `group` cell (CZ-Q5 (a), no bar)")
checkpoint_table(g, "4.1 The `group` cell")
