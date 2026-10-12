"""Per cell, the sampler's view of the host (1 s samples of per-CPU busy % and foreign processes >= 20 % of a CPU).
usage: hostwin.py <run-dir>   (reads cells.log and host-samples.jsonl)"""
import datetime, json, re, statistics as st, sys

R = sys.argv[1]
import glob
S = [json.loads(l) for f in sorted(glob.glob(f"{R}/host-samples*.jsonl")) for l in open(f)]
day = "2026-10-11"
wins = {}
for l in open(f"{R}/cells.log"):
    m = re.match(r"cell (\S+) (start|end) (\d\d:\d\d:\d\d)", l)
    if m:
        t = datetime.datetime.strptime(f"{day} {m.group(3)}", "%Y-%m-%d %H:%M:%S").replace(tzinfo=datetime.timezone.utc).timestamp()
        wins.setdefault(m.group(1), {})[m.group(2)] = t
print("| cell | UTC window | samples | 1-min load, min-max | mean busy, server CPUs / driver CPU 7 | mean busy, the other CPUs (0,1,2,3,6; 5 at cores = 1) | samples with a foreign process >= 20 % of a CPU | those processes |")
print("|---|---|---|---|---|---|---|---|")
for name, w in wins.items():
    if "end" not in w:
        continue
    win = [s for s in S if w["start"] <= s["t"] <= w["end"]]
    if not win:
        continue
    cores2 = "-c2" in name
    srv = [4, 5] if cores2 else [4]
    others = [0, 1, 2, 3, 6] + ([] if cores2 else [5])
    b = lambda cpus: round(st.mean(st.mean(s["busy"][str(c)] for c in cpus) for s in win))
    foreign = [f for s in win for f in s["foreign"] if "kds_server-" not in f]
    names = sorted({re.sub(r"^\d+@\d+ [\d.]+% ", "", f)[:60] for f in foreign})
    t0, t1 = (datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime("%H:%M:%S") for t in (w["start"], w["end"]))
    loads = [float(s["load"]) for s in win]
    print(f"| `{name}` | {t0}-{t1} | {len(win)} | {min(loads):.2f}-{max(loads):.2f} | {b(srv)} % / {b([7])} % | {b(others)} % | {sum(1 for s in win if any("kds_server-" not in f for f in s["foreign"]))} | {'; '.join(names) if names else '-'} |")
