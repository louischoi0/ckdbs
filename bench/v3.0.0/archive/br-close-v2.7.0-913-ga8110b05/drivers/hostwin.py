#!/usr/bin/env python3
"""Host record per cell. usage: hostwin.py <out dir holding the cell dirs> <host-samples.jsonl> <cells log> [cell ...]
Per cell: runs, UTC window of the run files, 1-min load before/after a run (min-max), runs with a live compiler/build/test
program, runs with any competing process, longest hold-back, errors; and the sampler's view over the cell's window."""
import glob, json, os, re, sys, time

OUT, SAMP, CLOG = sys.argv[1:4]
CELLS = sys.argv[4:]
LIVE = re.compile(r"(cc1plus|cmake|ctest|kds_tests|cargo|rustc|ninja|make )")
samples = [json.loads(l) for l in open(SAMP) if l.strip()]
windows = {}
for l in open(CLOG):
    m = re.match(r"cell (\S+) (start|end) (\d\d:\d\d:\d\d)", l)
    if m:
        windows.setdefault(m.group(1), {})[m.group(2)] = m.group(3)


def hms(t):
    return time.strftime("%H:%M:%S", time.gmtime(t))


print("| cell | runs | UTC window of the run files | 1-min load before a run | 1-min load after a run | runs with a live compiler/build/test program | runs with any competing process | longest hold-back | errors |")
print("|---|---|---|---|---|---|---|---|---|")
for cell in CELLS:
    fs = sorted(glob.glob(f"{OUT}/{cell}/n*-r*.json"))
    fs = [f for f in fs if not f.endswith(".raw.json.gz")]
    if not fs:
        continue
    d = [json.load(open(f)) for f in fs]
    bef = [float(x["host_before"]["loadavg"].split()[0]) for x in d]
    aft = [float(x["host_after"]["loadavg"].split()[0]) for x in d]

    def comp(x):
        return [c for c in x["host_before"]["competing"] + x["host_after"]["competing"]
                if "ctest" not in c.split("bash")[0] or True]

    def real(lst):
        out = []
        for c in lst:
            if "bench-runs" in c or "br_overhead" in c or "pgrep" in c:
                continue
            out.append(c)
        return out
    live = sum(1 for x in d if any(LIVE.search(c.split(None, 1)[1] if " " in c else c) for c in real(x["host_before"]["competing"] + x["host_after"]["competing"])))
    anyc = sum(1 for x in d if real(x["host_before"]["competing"] + x["host_after"]["competing"]))
    mt = sorted(os.path.getmtime(f) for f in fs)
    errs = sum(a["errors"] for x in d for l in "AB" for a in x["arms"][l].values())
    print(f"| `{cell}` | {len(d)} runs | {hms(mt[0])}-{hms(mt[-1])} UTC | {min(bef):.2f}-{max(bef):.2f} | {min(aft):.2f}-{max(aft):.2f} | {live} of {len(d)} runs | {anyc} of {len(d)} runs | {max(x['wait_s'] for x in d):.0f} s | {errs} errors |")

print("\nSampler (1 s, per-CPU busy from /proc/stat; foreign process = not under /home/cdkbs/bench-runs, >= 20 % of a CPU):\n")
print("| cell | UTC window | samples | 1-min load, min-max | mean busy, server CPUs | mean busy, driver CPU 7 | mean busy, the other CPUs | samples with a foreign process >= 20 % | those processes |")
print("|---|---|---|---|---|---|---|---|---|")
for cell in CELLS:
    w = windows.get(cell)
    if not w or "end" not in w:
        continue
    def tsec(s):
        h, m, sec = map(int, s.split(":"))
        return h * 3600 + m * 60 + sec
    lo, hi = tsec(w["start"]), tsec(w["end"])
    sel = [s for s in samples if lo <= (int(s["t"]) % 86400) <= hi]
    if not sel:
        continue
    c4 = cell.endswith("c4")
    srv = [0, 2, 4, 6] if c4 else [4]
    other = [c for c in range(8) if c not in srv and c != 7]
    def mean(cpus):
        v = [s["busy"][str(c)] for s in sel for c in cpus]
        return sum(v) / len(v)
    loads = [float(s["load"]) for s in sel]
    foreign = [s for s in sel if s["foreign"]]
    procs = sorted({f.split(None, 2)[2][:60] for s in foreign for f in s["foreign"]})
    print(f"| `{cell}` | {w['start']}-{w['end']} | {len(sel)} | {min(loads):.2f}-{max(loads):.2f} | {mean(srv):.0f} % | {mean([7]):.0f} % | {mean(other):.0f} % | {len(foreign)} | {'; '.join(procs[:3]) or '-'} |")
