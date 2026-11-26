"""Per-cell contamination report: foreign-process samples (monitor.log, about one per 2 s) that fall
inside a cell's driver window [t0, t1]. A sample counts when the foreign processes listed in it
(each above 10 % of one CPU, the monitor's own floor; `claude` and the scenario 2 driver's bookers
left out) SUM to 50 % or more of one CPU. Usage: python3 -I contam.py [--list]"""
import glob, os, re, sys, datetime

R = "/home/ukjin/bench-runs/scenarios-v3.4.17-159-g9f8f8b354"


def ts(s):
    return datetime.datetime.strptime(s.strip(), "%Y-%m-%dT%H:%M:%SZ")


samples = []
for line in open(R + "/run/monitor.log"):
    m = re.match(r"(\S+) load=(\S+) foreign=(.*)", line.rstrip("\n"))
    if not m:
        continue
    procs = []
    for item in m.group(3).split(" | "):
        mm = re.match(r"\s*(\d+):([\d.]+):(.*)", item)
        if mm:
            procs.append((float(mm.group(2)), mm.group(3).strip()))
    samples.append((ts(m.group(1)), procs))

rows = []
for f in sorted(glob.glob(R + "/out/logs/*.t0")):
    tag = os.path.basename(f)[:-3]
    if not os.path.exists(R + f"/out/logs/{tag}.t1"):
        continue
    t0, t1 = ts(open(f).read()), ts(open(R + f"/out/logs/{tag}.t1").read())
    n = heavy = 0
    cpu = 0.0
    names = set()
    for t, procs in samples:
        if t0 <= t <= t1:
            n += 1
            # not foreign: the claude CLI, and the scenario 2 driver's own booker processes (multiprocessing children, `python3 -c ...`)
            fp = [(c, a) for c, a in procs if not a.startswith("claude") and not a.startswith("/usr/bin/python3 -c")]
            tot = sum(c for c, _ in fp)
            if tot >= 50:
                heavy += 1
                cpu += tot
                for c, a in fp:
                    names.add(os.path.basename(a.split()[0]))
    rows.append((tag, t0, t1, n, heavy, cpu / heavy if heavy else 0.0, sorted(names)))

bad = [r for r in rows if r[4] > 0]
print(f"cells {len(rows)}; cells with one or more heavy foreign samples {len(bad)}")
if "--list" in sys.argv:
    for tag, t0, t1, n, heavy, avg, names in rows:
        if heavy:
            print(f"{tag} {t0:%H:%M:%S}-{t1:%H:%M:%S} samples={n} heavy={heavy} mean_foreign_cpu={avg:.0f}% {','.join(names)}")
if "--tags" in sys.argv:
    print(" ".join(r[0] for r in bad))
